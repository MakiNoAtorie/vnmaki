#include "engine/anim/timeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/byte_stream.hpp"

namespace vnmaki {

namespace {

using Values = std::array<float, kChannelCount>;

constexpr double kForever = std::numeric_limits<double>::infinity();

// --- Properties as channels -----------------------------------------------------------------

Values to_values(const DisplayProperties& properties) {
  return {
      properties.position.x, properties.position.y, properties.anchor.x,   properties.anchor.y,
      properties.scale.x,    properties.scale.y,    properties.rotation,   properties.alpha,
      properties.tint.r,     properties.tint.g,     properties.tint.b,     properties.tint.a,
      properties.crop.x,     properties.crop.y,     properties.crop.width, properties.crop.height};
}

// Opacity and colour stay in [0, 1], also where a back curve overshoots.
float unit(float value) { return std::clamp(value, 0.0F, 1.0F); }

void write_values(const Values& values, DisplayProperties& properties) {
  properties.position = {.x = values[slot(Channel::kX)], .y = values[slot(Channel::kY)]};
  properties.anchor = {.x = values[slot(Channel::kAnchorX)], .y = values[slot(Channel::kAnchorY)]};
  properties.scale = {.x = values[slot(Channel::kScaleX)], .y = values[slot(Channel::kScaleY)]};
  properties.rotation = values[slot(Channel::kRotation)];
  properties.alpha = unit(values[slot(Channel::kAlpha)]);
  properties.tint = {.r = unit(values[slot(Channel::kTintR)]),
                     .g = unit(values[slot(Channel::kTintG)]),
                     .b = unit(values[slot(Channel::kTintB)]),
                     .a = unit(values[slot(Channel::kTintA)])};
  properties.crop = {.x = values[slot(Channel::kCropX)],
                     .y = values[slot(Channel::kCropY)],
                     .width = values[slot(Channel::kCropWidth)],
                     .height = values[slot(Channel::kCropHeight)]};
}

// --- Times ----------------------------------------------------------------------------------

double cycle_of(double duration, bool yoyo) { return yoyo ? duration * 2 : duration; }

// Seconds from the start of `node` until it ends: infinite if a tween in it loops.
double span_of(const TweenNode& node) {
  switch (node.kind) {
    case TweenNode::Kind::kTween:
      return node.delay + (node.loop ? kForever : cycle_of(node.duration, node.yoyo));
    case TweenNode::Kind::kSequence: {
      double total = 0;
      for (const TweenNode& child : node.children) {
        total += span_of(child);
      }
      return node.delay + total;
    }
    case TweenNode::Kind::kParallel: {
      double longest = 0;
      for (const TweenNode& child : node.children) {
        longest = std::max(longest, span_of(child));
      }
      return node.delay + longest;
    }
  }
  return 0;
}

// Linear progress of a tween `local` seconds after it started, in [0, 1]: a yoyo goes up and back.
double progress_of(double duration, bool loop, bool yoyo, double local) {
  if (duration <= 0) {
    return 1;
  }
  const double cycle = cycle_of(duration, yoyo);
  const double time = loop ? std::fmod(local, cycle) : std::min(local, cycle);
  const double across = time / duration;
  return (yoyo && across > 1) ? 2 - across : across;
}

bool valid_seconds(double seconds) {
  return std::isfinite(seconds) && seconds >= 0 && seconds <= kMaxTweenSeconds;
}

}  // namespace

std::string Timeline::flatten(const TweenNode& node, double begin, bool outermost,
                              std::vector<Leaf>& leaves) {
  if (node.kind == TweenNode::Kind::kTween) {
    leaves.push_back(Leaf{.tag = node.tag,
                          .begin = begin,
                          .delay = node.delay,
                          .duration = node.duration,
                          .ease = node.ease,
                          .loop = node.loop,
                          .yoyo = node.yoyo,
                          .to = node.to});
    return {};
  }
  if (node.loop && !outermost) {
    return "only a tween or the outermost group can loop";
  }
  // The outermost group's delay is the delay of the whole timeline entry.
  double at = begin + (outermost ? 0 : node.delay);
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    std::string problem = flatten(node.children[i], at, false, leaves);
    if (!problem.empty()) {
      return problem;
    }
    if (node.kind == TweenNode::Kind::kSequence) {
      const double span = span_of(node.children[i]);
      if (!std::isfinite(span) && i + 1 != node.children.size()) {
        return "a looping tween must be the last item of a sequence";
      }
      at += span;
    }
  }
  return {};
}

std::string Timeline::start(const TweenNode& node, const Lookup& lookup, bool instant) {
  Running running;
  if (std::string problem = flatten(node, 0, true, running.leaves); !problem.empty()) {
    return problem;
  }
  if (running.leaves.empty()) {
    return {};
  }
  for (const Leaf& leaf : running.leaves) {
    if (running.base.contains(leaf.tag)) {
      continue;
    }
    const DisplayProperties* properties = lookup(leaf.tag);
    if (properties == nullptr) {
      return "no displayable '" + leaf.tag + "' is shown";
    }
    running.base.emplace(leaf.tag, to_values(*properties));
  }
  const bool group = node.kind != TweenNode::Kind::kTween;
  running.delay = group ? node.delay : 0;
  double body = 0;
  for (const Leaf& leaf : running.leaves) {
    body = std::max(body, leaf.begin + leaf.delay +
                              (leaf.loop ? kForever : cycle_of(leaf.duration, leaf.yoyo)));
  }
  if (group && node.loop && std::isfinite(body) && body > 0) {
    running.period = body;
  }
  running.length = running.period > 0 ? kForever : running.delay + body;
  if (instant && std::isfinite(running.length)) {
    running.elapsed = kForever;
    apply(running, lookup);
    return {};
  }
  running_.push_back(std::move(running));
  return {};
}

void Timeline::apply(Running& running, const Lookup& lookup) {
  double time = running.elapsed - running.delay;
  if (time < 0) {
    return;
  }
  if (running.period > 0) {
    time = std::fmod(time, running.period);
  }
  for (Leaf& leaf : running.leaves) {
    const double local = time - leaf.begin - leaf.delay;
    // A finished tween leaves its property alone, unless the group repeats it.
    if (local < 0 || (leaf.done && running.period <= 0)) {
      continue;
    }
    DisplayProperties* properties = lookup(leaf.tag);
    if (properties == nullptr) {
      continue;
    }
    Values values = to_values(*properties);
    if (!leaf.started) {
      leaf.from = values;
      leaf.started = true;
    }
    const double eased =
        apply_ease(leaf.ease, progress_of(leaf.duration, leaf.loop, leaf.yoyo, local));
    for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
      if (leaf.to.at(channel).has_value()) {
        const double from = leaf.from.at(channel);
        const double to = *leaf.to.at(channel);
        values.at(channel) = static_cast<float>(from + (to - from) * eased);
      }
    }
    write_values(values, *properties);
    leaf.done = !leaf.loop && local >= cycle_of(leaf.duration, leaf.yoyo);
  }
}

void Timeline::update(double seconds, const Lookup& lookup) {
  for (Running& running : running_) {
    running.elapsed += seconds;
    // An infinite elapsed time puts every tween that ends at its end value, with no rounding.
    const bool ends = running.elapsed >= running.length;
    if (ends) {
      running.elapsed = kForever;
    }
    apply(running, lookup);
  }
  std::erase_if(running_, [](const Running& running) { return std::isinf(running.elapsed); });
}

void Timeline::finish(std::string_view tag, const Lookup& lookup) {
  for (Running& running : running_) {
    const bool matches =
        tag.empty() ||
        std::ranges::any_of(running.leaves, [tag](const Leaf& leaf) { return leaf.tag == tag; });
    if (matches && std::isfinite(running.length)) {
      running.elapsed = kForever;
      apply(running, lookup);
    }
  }
  std::erase_if(running_, [](const Running& running) { return std::isinf(running.elapsed); });
}

void Timeline::prune(const Lookup& lookup) {
  for (Running& running : running_) {
    std::erase_if(running.leaves,
                  [&lookup](const Leaf& leaf) { return lookup(leaf.tag) == nullptr; });
  }
  std::erase_if(running_, [](const Running& running) { return running.leaves.empty(); });
}

bool Timeline::busy(std::string_view tag) const {
  return std::ranges::any_of(running_, [tag](const Running& running) {
    return std::isfinite(running.length) &&
           (tag.empty() || std::ranges::any_of(running.leaves, [tag](const Leaf& leaf) {
              return leaf.tag == tag;
            }));
  });
}

std::vector<std::string> Timeline::tags() const {
  std::set<std::string> found;
  for (const Running& running : running_) {
    for (const Leaf& leaf : running.leaves) {
      found.insert(leaf.tag);
    }
  }
  return {found.begin(), found.end()};
}

void Timeline::settle(const Lookup& lookup) {
  finish({}, lookup);
  for (const Running& running : running_) {
    for (const Leaf& leaf : running.leaves) {
      DisplayProperties* properties = lookup(leaf.tag);
      const auto base_found = running.base.find(leaf.tag);
      if (properties == nullptr || base_found == running.base.end()) {
        continue;
      }
      Values values = to_values(*properties);
      const Values& base = base_found->second;
      for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
        if (leaf.to.at(channel).has_value()) {
          values.at(channel) = base.at(channel);
        }
      }
      write_values(values, *properties);
    }
  }
}

// --- Snapshot form ----------------------------------------------------------------------------
// Little-endian. A u32 count of loops, then for each: f64 delay, f64 period, a u32 count of
// tweens and for each tween: tag, f64 begin, f64 delay, f64 duration, u8 easing, u8 flags (1 =
// loop, 2 = yoyo) and for each channel a u8 (1 = it has a target) and then the f32 target.

namespace {

constexpr std::uint8_t kLoopFlag = 1;
constexpr std::uint8_t kYoyoFlag = 2;

}  // namespace

std::vector<std::byte> Timeline::capture() const {
  const auto kept = [](const Running& running) { return !std::isfinite(running.length); };
  ByteWriter writer;
  writer.u32(static_cast<std::uint32_t>(std::ranges::count_if(running_, kept)));
  for (const Running& running : running_) {
    if (!kept(running)) {
      continue;
    }
    writer.f64(running.delay);
    writer.f64(running.period);
    writer.u32(static_cast<std::uint32_t>(running.leaves.size()));
    for (const Leaf& leaf : running.leaves) {
      writer.string(leaf.tag);
      writer.f64(leaf.begin);
      writer.f64(leaf.delay);
      writer.f64(leaf.duration);
      writer.u8(static_cast<std::uint8_t>(leaf.ease));
      writer.u8(
          static_cast<std::uint8_t>((leaf.loop ? kLoopFlag : 0) | (leaf.yoyo ? kYoyoFlag : 0)));
      for (const std::optional<float>& target : leaf.to) {
        writer.u8(target.has_value() ? 1 : 0);
        if (target.has_value()) {
          writer.f32(*target);
        }
      }
    }
  }
  return writer.take();
}

void Timeline::restore(std::span<const std::byte> state, const Lookup& lookup) {
  std::vector<Running> restored;
  if (!state.empty()) {
    ByteReader reader{state, "scene state"};
    const std::uint32_t count = reader.u32();
    for (std::uint32_t i = 0; i < count; ++i) {
      Running running;
      running.delay = reader.f64();
      running.period = reader.f64();
      if (!valid_seconds(running.delay) || !valid_seconds(running.period)) {
        throw std::invalid_argument{"scene state: a tween time is out of range"};
      }
      const std::uint32_t leaf_count = reader.u32();
      for (std::uint32_t j = 0; j < leaf_count; ++j) {
        Leaf leaf;
        leaf.tag = reader.string();
        leaf.begin = reader.f64();
        leaf.delay = reader.f64();
        leaf.duration = reader.f64();
        const std::uint8_t ease = reader.u8();
        const std::uint8_t flags = reader.u8();
        if (!valid_seconds(leaf.begin) || !valid_seconds(leaf.delay) ||
            !valid_seconds(leaf.duration) || ease > static_cast<std::uint8_t>(Ease::kInOutBounce)) {
          throw std::invalid_argument{"scene state: a tween is out of range"};
        }
        leaf.ease = static_cast<Ease>(ease);
        leaf.loop = (flags & kLoopFlag) != 0;
        leaf.yoyo = (flags & kYoyoFlag) != 0;
        for (std::optional<float>& target : leaf.to) {
          if (reader.u8() != 0) {
            const float value = reader.f32();
            if (!std::isfinite(value)) {
              throw std::invalid_argument{"scene state: a tween target is not a number"};
            }
            target = value;
          }
        }
        running.leaves.push_back(std::move(leaf));
      }
      restored.push_back(std::move(running));
    }
    if (!reader.at_end()) {
      throw std::invalid_argument{"scene state: unexpected data after the last tween"};
    }
  }
  for (Running& running : restored) {
    double body = 0;
    for (const Leaf& leaf : running.leaves) {
      body = std::max(body, leaf.begin + leaf.delay +
                                (leaf.loop ? kForever : cycle_of(leaf.duration, leaf.yoyo)));
      if (!running.base.contains(leaf.tag)) {
        const DisplayProperties* properties = lookup(leaf.tag);
        if (properties != nullptr) {
          running.base.emplace(leaf.tag, to_values(*properties));
        }
      }
    }
    running.length = running.period > 0 ? kForever : running.delay + body;
  }
  running_ = std::move(restored);
  prune(lookup);
}

}  // namespace vnmaki

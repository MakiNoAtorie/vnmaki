#pragma once

// The tween timeline (docs/SPEC.md section 10): tweens, sequences and parallel groups that change
// the DisplayProperties of tagged displayables over time. It needs no window or clock; the caller
// passes the elapsed seconds to update(), so a fake clock makes it deterministic.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/anim/easing.hpp"
#include "engine/scene/displayable.hpp"

namespace vnmaki {

/// The properties of a displayable that a tween changes, one float each.
enum class Channel : std::uint8_t {
  kX,
  kY,
  kAnchorX,
  kAnchorY,
  kScaleX,
  kScaleY,
  kRotation,
  kAlpha,
  kTintR,
  kTintG,
  kTintB,
  kTintA,
  kCropX,
  kCropY,
  kCropWidth,
  kCropHeight,
  kCount,
};

inline constexpr std::size_t kChannelCount = static_cast<std::size_t>(Channel::kCount);

[[nodiscard]] constexpr std::size_t slot(Channel channel) {
  return static_cast<std::size_t>(channel);
}

/// The value a tween moves each channel to; a channel without a value is left alone. Position is in
/// virtual pixels, alpha and the tint in [0, 1].
using ChannelTargets = std::array<std::optional<float>, kChannelCount>;

/// The longest delay or duration, in seconds, that a tween may have (one day).
inline constexpr double kMaxTweenSeconds = 86'400.0;

/// What a script asked for. A tween moves the properties of one displayable. A sequence runs its
/// children one after another and a parallel group runs them together; both are done when every
/// child is.
struct TweenNode {
  enum class Kind : std::uint8_t { kTween, kSequence, kParallel };

  Kind kind = Kind::kTween;
  std::string tag{};    ///< The displayable a tween changes.
  ChannelTargets to{};  ///< Where a tween goes.
  double duration = 0;  ///< Seconds from start to `to`.
  double delay = 0;     ///< Seconds before the node starts (a group delays its children).
  Ease ease = Ease::kLinear;
  /// A tween repeats forever; so does the outermost group, which then repeats as a whole.
  bool loop = false;
  /// A tween goes to `to` and back to where it started in each repeat. Groups ignore it.
  bool yoyo = false;
  std::vector<TweenNode> children{};
};

/// The tweens that are running. Displayables are found by tag through a `Lookup`, so the timeline
/// keeps no pointers to them: a tween of a tag the lookup does not know does nothing.
///
/// A tween starts after its delay. It reads the property it changes at that moment, so a
/// sequence of tweens on one property continues from where the last one ended. A looping group
/// reads each property once and goes back to that value in every repeat.
class Timeline {
 public:
  using Lookup = std::function<DisplayProperties*(std::string_view tag)>;

  /// Starts `node` at time 0. Returns why it cannot run, and starts nothing: a tag the lookup
  /// does not know, a loop on a group that is not the outermost, or a looping tween that is not
  /// the last of its sequence. Returns an empty string otherwise. With `instant`, a node that
  /// ends goes to its end value at once, and a loop starts as usual.
  [[nodiscard]] std::string start(const TweenNode& node, const Lookup& lookup,
                                  bool instant = false);

  /// Moves every tween on by `seconds` and writes the properties.
  void update(double seconds, const Lookup& lookup);

  /// Puts the tweens that end (not the loops) of `tag`, or of every tag when it is empty, at their
  /// end values and removes them.
  void finish(std::string_view tag, const Lookup& lookup);

  /// Removes the tweens of tags that the lookup no longer knows.
  void prune(const Lookup& lookup);

  /// True while a tween that ends, and changes `tag` (any tag when empty), runs. Loops never
  /// count.
  [[nodiscard]] bool busy(std::string_view tag) const;

  [[nodiscard]] bool empty() const { return running_.empty(); }

  /// Every tag a running tween changes.
  [[nodiscard]] std::vector<std::string> tags() const;

  /// Puts the properties in the state a snapshot keeps: every tween that ends is at its end
  /// value, and every property a loop changes is where it was when the loop started.
  void settle(const Lookup& lookup);

  /// The snapshot form of the loops (a group that contains a loop is kept whole). Tweens that end
  /// are not kept: settle() a copy first so the properties show their end values.
  [[nodiscard]] std::vector<std::byte> capture() const;
  /// Replaces all tweens with the loops in `state`, each restarted at time 0. Throws
  /// std::invalid_argument, changing nothing, if `state` is malformed.
  void restore(std::span<const std::byte> state, const Lookup& lookup);

 private:
  struct Leaf {
    std::string tag{};
    double begin = 0;  // seconds after the start of the group's body
    double delay = 0;
    double duration = 0;
    Ease ease = Ease::kLinear;
    bool loop = false;
    bool yoyo = false;
    ChannelTargets to{};
    std::array<float, kChannelCount> from{};
    bool started = false;  // `from` is read
    bool done = false;     // the end value is written
  };
  struct Running {
    double elapsed = 0;
    double delay = 0;   // seconds before the body starts
    double period = 0;  // above 0: the body repeats every `period` seconds
    double length = 0;  // seconds until it ends; infinite for a loop
    std::vector<Leaf> leaves{};
    // The properties of each tag as they were when the group started.
    std::map<std::string, std::array<float, kChannelCount>, std::less<>> base{};
  };

  // Appends the leaves of `node`, which starts `begin` seconds into the body. Returns why it
  // cannot run, or an empty string.
  [[nodiscard]] static std::string flatten(const TweenNode& node, double begin, bool outermost,
                                           std::vector<Leaf>& leaves);
  // Writes the properties for the time `running.elapsed`.
  static void apply(Running& running, const Lookup& lookup);

  std::vector<Running> running_;
};

}  // namespace vnmaki

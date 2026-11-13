#include "engine/ui/ui_layout.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>

#include <yoga/Yoga.h>

namespace vnmaki {

namespace {

constexpr float kNoLimit = std::numeric_limits<float>::infinity();
constexpr float kHalfEm = 0.5F;
constexpr unsigned char kUtf8ContinuationMask = 0xC0;
constexpr unsigned char kUtf8Continuation = 0x80;

struct MeasureContext {
  const UiNode* node = nullptr;
  const TextMeasure* measure = nullptr;
};

struct YogaDeleter {
  void operator()(YGNodeRef node) const { YGNodeFreeRecursive(node); }
};
using YogaTree = std::unique_ptr<std::remove_pointer_t<YGNodeRef>, YogaDeleter>;

// Yoga asks the node for the size of its content when it is not set by the style.
YGSize measure_node(YGNodeConstRef yoga, float width, YGMeasureMode width_mode, float height,
                    YGMeasureMode height_mode) {
  const auto* context = static_cast<const MeasureContext*>(YGNodeGetContext(yoga));
  // Vertical text wraps at its height: the column length.
  const bool vertical = context->node->style.vertical;
  const float limit = vertical ? height : width;
  const bool limited = (vertical ? height_mode : width_mode) != YGMeasureModeUndefined;
  MeasuredSize size = (*context->measure)(*context->node, limited ? limit : kNoLimit);
  if (context->node->kind == NodeKind::kToggle) {
    size.width += context->node->style.text_size * kToggleMarkEm;
  } else if (context->node->kind == NodeKind::kInput) {
    size.height = std::max(size.height, context->node->style.text_size);  // an empty input
  }
  const auto fit = [](float natural, float limit, YGMeasureMode mode) {
    if (mode == YGMeasureModeExactly) {
      return limit;
    }
    return mode == YGMeasureModeAtMost ? std::min(natural, limit) : natural;
  };
  return YGSize{.width = fit(size.width, width, width_mode),
                .height = fit(size.height, height, height_mode)};
}

YGAlign to_yoga(Align align) {
  switch (align) {
    case Align::kStart:
      return YGAlignFlexStart;
    case Align::kCenter:
      return YGAlignCenter;
    case Align::kEnd:
      return YGAlignFlexEnd;
    case Align::kStretch:
      break;
  }
  return YGAlignStretch;
}

YGJustify to_yoga(Justify justify) {
  switch (justify) {
    case Justify::kCenter:
      return YGJustifyCenter;
    case Justify::kEnd:
      return YGJustifyFlexEnd;
    case Justify::kSpaceBetween:
      return YGJustifySpaceBetween;
    case Justify::kStart:
      break;
  }
  return YGJustifyFlexStart;
}

// Mirrors `node` and its children as Yoga nodes. A leaf that shows text gets a measure function.
YGNodeRef build(const UiNode& node, std::deque<MeasureContext>& contexts,
                const TextMeasure& measure, TextDirection direction) {
  YGNodeRef yoga = YGNodeNew();
  const UiStyle& style = node.style;
  const YGFlexDirection row =
      direction == TextDirection::kRtl ? YGFlexDirectionRowReverse : YGFlexDirectionRow;
  YGNodeStyleSetFlexDirection(yoga, node.kind == NodeKind::kHbox ? row : YGFlexDirectionColumn);
  if (style.width) {
    YGNodeStyleSetWidth(yoga, *style.width);
  }
  if (style.height) {
    YGNodeStyleSetHeight(yoga, *style.height);
  }
  YGNodeStyleSetFlexGrow(yoga, style.grow);
  YGNodeStyleSetPadding(yoga, YGEdgeAll, style.padding);
  YGNodeStyleSetMargin(yoga, YGEdgeAll, style.margin);
  YGNodeStyleSetGap(yoga, YGGutterAll, style.gap);
  YGNodeStyleSetAlignItems(yoga, to_yoga(style.align));
  YGNodeStyleSetJustifyContent(yoga, to_yoga(style.justify));
  if (style.position) {
    YGNodeStyleSetPositionType(yoga, YGPositionTypeAbsolute);
    YGNodeStyleSetPosition(yoga, YGEdgeLeft, style.position->x);
    YGNodeStyleSetPosition(yoga, YGEdgeTop, style.position->y);
  }
  const bool shows_text = node.kind == NodeKind::kText || node.kind == NodeKind::kButton ||
                          node.kind == NodeKind::kToggle || node.kind == NodeKind::kInput;
  if (shows_text && node.children.empty()) {
    contexts.push_back({.node = &node, .measure = &measure});
    YGNodeSetContext(yoga, &contexts.back());
    YGNodeSetMeasureFunc(yoga, measure_node);
  }
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    YGNodeInsertChild(yoga, build(node.children[i], contexts, measure, direction), i);
  }
  return yoga;
}

// Copies the computed boxes back. Yoga gives each box relative to its parent.
void collect(UiNode& node, YGNodeRef yoga, PointF parent_origin) {
  const PointF origin{.x = parent_origin.x + YGNodeLayoutGetLeft(yoga),
                      .y = parent_origin.y + YGNodeLayoutGetTop(yoga)};
  node.rect = {.x = origin.x,
               .y = origin.y,
               .width = YGNodeLayoutGetWidth(yoga),
               .height = YGNodeLayoutGetHeight(yoga)};
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    collect(node.children[i], YGNodeGetChild(yoga, i), origin);
  }
}

}  // namespace

MeasuredSize estimate_text_size(const UiNode& node, float /*max_width*/) {
  std::size_t characters = 0;
  for (const char c : display_text(node)) {
    if ((static_cast<unsigned char>(c) & kUtf8ContinuationMask) != kUtf8Continuation) {
      ++characters;
    }
  }
  return {.width = static_cast<float>(characters) * node.style.text_size * kHalfEm,
          .height = node.style.text_size};
}

void layout_tree(UiNode& root, Size canvas, const TextMeasure& measure, TextDirection direction) {
  std::deque<MeasureContext> contexts;
  const YogaTree tree{build(root, contexts, measure, direction)};
  YGNodeStyleSetWidth(tree.get(), static_cast<float>(canvas.width));
  YGNodeStyleSetHeight(tree.get(), static_cast<float>(canvas.height));
  YGNodeCalculateLayout(tree.get(), static_cast<float>(canvas.width),
                        static_cast<float>(canvas.height), YGDirectionLTR);
  collect(root, tree.get(), {});
}

}  // namespace vnmaki

#include "engine/ui/ui_node.hpp"

namespace vnmaki {

UiNode make_node(NodeKind kind) {
  UiNode node;
  node.kind = kind;
  switch (kind) {
    case NodeKind::kText:
      node.role = NodeRole::kText;
      break;
    case NodeKind::kImage:
      node.role = NodeRole::kImage;
      break;
    case NodeKind::kButton:
      node.role = NodeRole::kButton;
      break;
    case NodeKind::kToggle:
      node.role = NodeRole::kToggle;
      break;
    case NodeKind::kSlider:
      node.role = NodeRole::kSlider;
      break;
    case NodeKind::kBar:
      node.role = NodeRole::kProgress;
      break;
    case NodeKind::kBox:
    case NodeKind::kHbox:
    case NodeKind::kVbox:
      node.role = NodeRole::kGroup;
      break;
  }
  return node;
}

bool is_focusable(const UiNode& node) {
  const bool takes_focus = node.kind == NodeKind::kButton || node.kind == NodeKind::kToggle ||
                           node.kind == NodeKind::kSlider;
  return takes_focus && !node.state.disabled;
}

}  // namespace vnmaki

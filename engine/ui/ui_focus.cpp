#include "engine/ui/ui_focus.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace vnmaki {

namespace {

constexpr float kCrossAxisWeight = 2;

void collect(UiNode& node, std::vector<UiNode*>& out) {
  if (is_focusable(node)) {
    out.push_back(&node);
  }
  for (UiNode& child : node.children) {
    collect(child, out);
  }
}

PointF center(const UiNode& node) {
  return {.x = node.rect.x + (node.rect.width / 2), .y = node.rect.y + (node.rect.height / 2)};
}

bool contains(const RectF& rect, PointF point) {
  return point.x >= rect.x && point.y >= rect.y && point.x < rect.x + rect.width &&
         point.y < rect.y + rect.height;
}

// How far `to` is from `from` in direction `move`; infinite if it does not lie that way.
float directional_cost(FocusMove move, PointF from, PointF to) {
  const float dx = to.x - from.x;
  const float dy = to.y - from.y;
  float along = 0;
  float across = 0;
  switch (move) {
    case FocusMove::kUp:
      along = -dy;
      across = dx;
      break;
    case FocusMove::kDown:
      along = dy;
      across = dx;
      break;
    case FocusMove::kLeft:
      along = -dx;
      across = dy;
      break;
    case FocusMove::kRight:
      along = dx;
      across = dy;
      break;
    case FocusMove::kNext:
    case FocusMove::kPrevious:
      break;
  }
  if (along <= 0) {
    return std::numeric_limits<float>::infinity();
  }
  return along + (kCrossAxisWeight * std::abs(across));
}

}  // namespace

std::vector<UiNode*> focusable_nodes(UiNode& root) {
  std::vector<UiNode*> nodes;
  collect(root, nodes);
  return nodes;
}

UiNode* focused_node(UiNode& root) {
  for (UiNode* node : focusable_nodes(root)) {
    if (node->state.focused) {
      return node;
    }
  }
  return nullptr;
}

bool set_focus(UiNode& root, UiNode* target) {
  const std::vector<UiNode*> nodes = focusable_nodes(root);
  const bool changed = std::ranges::any_of(
      nodes, [&](const UiNode* node) { return node->state.focused != (node == target); });
  for (UiNode* node : nodes) {
    node->state.focused = node == target;
  }
  return changed;
}

bool move_focus(UiNode& root, FocusMove move) {
  const std::vector<UiNode*> nodes = focusable_nodes(root);
  if (nodes.empty()) {
    return false;
  }
  const auto current =
      std::ranges::find_if(nodes, [](const UiNode* n) { return n->state.focused; });
  if (current == nodes.end()) {
    return set_focus(root, move == FocusMove::kPrevious ? nodes.back() : nodes.front());
  }
  UiNode* target = *current;
  const auto index = static_cast<std::size_t>(current - nodes.begin());
  if (move == FocusMove::kNext) {
    target = nodes[(index + 1) % nodes.size()];
  } else if (move == FocusMove::kPrevious) {
    target = nodes[(index + nodes.size() - 1) % nodes.size()];
  } else {
    float best = std::numeric_limits<float>::infinity();
    for (UiNode* candidate : nodes) {
      const float cost = directional_cost(move, center(**current), center(*candidate));
      if (cost < best) {
        best = cost;
        target = candidate;
      }
    }
  }
  return set_focus(root, target);
}

UiNode* focusable_at(UiNode& root, PointF point) {
  UiNode* found = nullptr;
  for (UiNode* node : focusable_nodes(root)) {
    if (contains(node->rect, point)) {
      found = node;  // later nodes are drawn on top
    }
  }
  return found;
}

void clear_hover(UiNode& root) {
  root.state.hovered = false;
  for (UiNode& child : root.children) {
    clear_hover(child);
  }
}

bool hover_at(UiNode& root, PointF point) {
  clear_hover(root);
  UiNode* under = focusable_at(root, point);
  if (under == nullptr) {
    return false;
  }
  under->state.hovered = true;
  return set_focus(root, under);
}

}  // namespace vnmaki

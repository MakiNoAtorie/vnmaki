#pragma once

#include <cstdint>

namespace vnmaki {

/// The direction a language is written in (project.toml `[text_direction]`). Right to left
/// reverses the order of the children of an `hbox` (see layout_tree).
enum class TextDirection : std::uint8_t { kLtr, kRtl };

}  // namespace vnmaki

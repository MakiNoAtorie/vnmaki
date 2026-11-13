#pragma once

#include <string_view>

namespace vnmaki {

/// The text of an engine shader header (shaders/*.hlsli) by its file name, e.g. "vnmaki.hlsli",
/// or an empty view for any other name. The build embeds them
/// (engine/render/shader_headers.cpp.in) so a shipped game needs no shader files of the engine.
[[nodiscard]] std::string_view hlsl_header(std::string_view name);

}  // namespace vnmaki

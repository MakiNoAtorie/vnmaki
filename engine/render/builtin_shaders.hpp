#pragma once

// The engine's own shaders (shaders/*.hlsl), compiled and embedded at build time.

#include "engine/render/shader.hpp"

namespace vnmaki::shaders {

/// shaders/sprite.vert.hlsl: every quad's vertex shader.
[[nodiscard]] ShaderBytecode sprite_vert();
/// shaders/sprite.frag.hlsl: the default fragment shader (texel times tint).
[[nodiscard]] ShaderBytecode sprite_frag();
/// shaders/sdf_text.frag.hlsl: effect text from a distance-field glyph atlas (see
/// sdf_text_shader() in engine/text/text.hpp).
[[nodiscard]] ShaderBytecode sdf_text_frag();

}  // namespace vnmaki::shaders

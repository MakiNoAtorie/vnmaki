// Vertex shader for every quad VNMaki draws. Positions arrive in the render target's pixels
// (origin top-left, y down) and leave in clip space. Vertex constant buffers live in register
// space1 (SDL GPU).

#include "sprite_fragment.hlsli"

cbuffer PassUniforms : register(b0, space1) {
  /// The render target's size in pixels.
  float2 target_size;
  float2 unused_padding;
};

struct SpriteVertex {
  float2 position : TEXCOORD0;
  float2 uv : TEXCOORD1;
  float4 color : TEXCOORD2;
};

SpriteFragment main(SpriteVertex input) {
  const float2 ndc = input.position / target_size * 2.0 - 1.0;
  SpriteFragment output;
  output.position = float4(ndc.x, -ndc.y, 0.0, 1.0);
  output.uv = input.uv;
  output.color = input.color;
  return output;
}

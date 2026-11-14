// A custom fragment shader for the renderer tests: desaturates the tinted texel by `amount`
// (0 = unchanged, 1 = gray). tools/generators/render_fixtures.py has the reference (grayscale()).

#include "vnmaki.hlsli"

cbuffer GrayscaleUniforms : register(b0, space3) {
  float amount;
  float3 unused_padding;
};

float4 main(SpriteFragment input) : SV_Target0 {
  const float4 color = sample_sprite(input) * input.color;
  const float gray = dot(color.rgb, float3(0.299, 0.587, 0.114));
  return float4(lerp(color.rgb, gray.xxx, amount), color.a);
}

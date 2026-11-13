#include "vnmaki.hlsli"

cbuffer SepiaUniforms : register(b0, space3) {
  float amount;
  float3 unused_padding;
};

static const float3 kLuma = float3(0.299, 0.587, 0.114);
static const float3 kTone = float3(1.0, 0.8, 0.6);

float4 main(SpriteFragment input) : SV_Target0 {
  const float4 color = sample_sprite(input) * input.color;
  const float3 toned = dot(color.rgb, kLuma) * kTone;
  return float4(lerp(color.rgb, toned, amount), color.a);
}

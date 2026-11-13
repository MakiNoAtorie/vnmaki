// Slide: the old frame moves out in `direction` and the new frame follows it in, edge to edge.

#include "transition.hlsli"
#include "transition_uniforms.hlsli"

float inside(float2 uv) {
  return (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) ? 1.0 : 0.0;
}

float4 main(SpriteFragment input) : SV_Target0 {
  const float2 from_uv = input.uv - direction * progress;
  const float2 to_uv = input.uv + direction * (1.0 - progress);
  return from_at(from_uv) * inside(from_uv) + to_at(to_uv) * inside(to_uv);
}

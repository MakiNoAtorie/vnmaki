// Wipe: an edge moves across the screen in `direction` and shows the new frame behind it.

#include "transition.hlsli"
#include "transition_uniforms.hlsli"

float4 main(SpriteFragment input) : SV_Target0 {
  // 0 on the side the edge starts from, 1 on the side it ends at.
  const float position = 0.5 + dot(input.uv - 0.5, direction);
  return lerp(from_at(input.uv), to_at(input.uv), reveal(position, progress, softness));
}

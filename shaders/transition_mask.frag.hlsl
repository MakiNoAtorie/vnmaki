// Image mask: the new frame shows first where the mask is dark and last where it is light.

#include "transition.hlsli"
#include "transition_uniforms.hlsli"

float4 main(SpriteFragment input) : SV_Target0 {
  const float position = mask_at(input.uv).r;
  return lerp(from_at(input.uv), to_at(input.uv), reveal(position, progress, softness));
}

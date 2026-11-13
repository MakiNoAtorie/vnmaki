// Fade through a colour: the old frame fades into the colour in the first half, and the colour
// into the new frame in the second.

#include "transition.hlsli"
#include "transition_uniforms.hlsli"

float4 main(SpriteFragment input) : SV_Target0 {
  const float4 through = float4(color.rgb * color.a, color.a);
  // Both halves are worked out first: a texture sample must not sit in a branch.
  const float4 going = lerp(from_at(input.uv), through, saturate(progress * 2.0));
  const float4 coming = lerp(through, to_at(input.uv), saturate(progress * 2.0 - 1.0));
  return progress < 0.5 ? going : coming;
}

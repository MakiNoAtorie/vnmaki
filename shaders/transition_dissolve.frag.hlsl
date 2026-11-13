// Dissolve: the old frame fades into the new one. tools/generators/transition_fixtures.py has the
// reference.

#include "transition.hlsli"
#include "transition_uniforms.hlsli"

float4 main(SpriteFragment input) : SV_Target0 {
  return lerp(from_at(input.uv), to_at(input.uv), progress);
}

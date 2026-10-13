// The default fragment shader: the texel tinted by the quad's colour (premultiplied alpha).

#include "vnmaki.hlsli"

float4 main(SpriteFragment input) : SV_Target0 {
  return sample_sprite(input) * input.color;
}

// Unlit: the base colour, with no light.

#include "mesh.hlsli"

cbuffer Uniforms : register(b0, space3) {
  MeshGlobals globals;
};

float4 main(MeshFragment input) : SV_Target0 {
  const float4 color = base_color_at(globals, input.uv);
  return premultiply(color.rgb, color.a);
}

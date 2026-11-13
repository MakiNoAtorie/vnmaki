// Toon: the light on the surface falls into three flat bands.

#include "mesh.hlsli"

cbuffer Uniforms : register(b0, space3) {
  MeshGlobals globals;
};

static const float kBrightBand = 1.0;
static const float kMiddleBand = 0.65;
static const float kDarkBand = 0.35;
static const float kBrightFrom = 0.6;
static const float kMiddleFrom = 0.2;

float4 main(MeshFragment input) : SV_Target0 {
  const float4 color = base_color_at(globals, input.uv);
  const float3 normal = normalize(input.normal);
  float3 lit = float3(0.0, 0.0, 0.0);
  [unroll]
  for (int i = 0; i < VNMAKI_MAX_LIGHTS; ++i) {
    float3 to_light;
    float3 radiance;
    light_at(globals, i, input.world_position, to_light, radiance);
    const float facing = max(dot(normal, to_light), 0.0);
    const float band = facing > kBrightFrom ? kBrightBand
                       : facing > kMiddleFrom ? kMiddleBand
                                              : kDarkBand;
    lit += radiance * (facing > 0.0 ? band : 0.0);
  }
  return premultiply(color.rgb * saturate(globals.ambient.rgb + lit), color.a);
}

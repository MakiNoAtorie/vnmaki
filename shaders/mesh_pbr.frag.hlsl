// Simple PBR: Lambert diffuse and a Blinn-Phong highlight from the metallic-roughness material.
// It is not energy-conserving or exact; it reacts to metallic and roughness the way glTF artists
// expect and nothing more.

#include "mesh.hlsli"

cbuffer Uniforms : register(b0, space3) {
  MeshGlobals globals;
};

static const float kDielectricReflectance = 0.04;
static const float kSmoothShininess = 128.0;
static const float kRoughShininess = 4.0;

float4 main(MeshFragment input) : SV_Target0 {
  const float4 color = base_color_at(globals, input.uv);
  const float metallic = saturate(globals.material.x);
  const float roughness = saturate(globals.material.y);
  const float3 normal = normalize(input.normal);
  const float3 to_camera = normalize(globals.camera_position.xyz - input.world_position);
  const float3 diffuse_color = color.rgb * (1.0 - metallic);
  const float3 dielectric = float3(kDielectricReflectance, kDielectricReflectance,
                                   kDielectricReflectance);
  const float3 specular_color = lerp(dielectric, color.rgb, metallic);
  const float shininess = lerp(kSmoothShininess, kRoughShininess, roughness);
  float3 result = diffuse_color * globals.ambient.rgb;
  [unroll]
  for (int i = 0; i < VNMAKI_MAX_LIGHTS; ++i) {
    float3 to_light;
    float3 radiance;
    light_at(globals, i, input.world_position, to_light, radiance);
    const float facing = max(dot(normal, to_light), 0.0);
    const float half_facing = max(dot(normal, normalize(to_light + to_camera)), 0.0);
    const float highlight = facing > 0.0 ? pow(half_facing, shininess) : 0.0;
    result += (diffuse_color + specular_color * highlight * (1.0 - roughness)) * radiance * facing;
  }
  return premultiply(saturate(result), color.a);
}

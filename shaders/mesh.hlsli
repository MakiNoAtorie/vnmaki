// What a mesh fragment shader, built-in or custom, can use (docs/SPEC.md section 12).
//
// A custom mesh shader includes this file and writes
//
//     cbuffer Uniforms : register(b0, space3) { MeshGlobals globals; float amount; };
//     float4 main(MeshFragment input) : SV_Target0 { ... }
//
// returning a colour with PREMULTIPLIED alpha. The first bytes of its constant buffer are the
// MeshGlobals (the engine fills them: material, camera and lights); the shader's own uniforms
// (`shader(name, { "amount" })`, tweened like those of a displayable) follow, each padded to
// 16 bytes after the struct as HLSL packs them. `sample_base(uv)` reads the model's base colour
// texture.
//
// The vertex stage (mesh.vert.hlsl) defines VNMAKI_MESH_VERTEX before including this file so that
// it does not declare the fragment textures.

#ifndef VNMAKI_MESH_HLSLI
#define VNMAKI_MESH_HLSLI

#ifndef VNMAKI_MESH_VERTEX
#include "vnmaki.hlsli"
#endif

/// The most lights a mesh shader receives.
#define VNMAKI_MAX_LIGHTS 4

struct MeshFragment {
  float4 position : SV_Position;
  /// Texture coordinates, (0, 0) at the texture's top-left corner. This comes first: Direct3D 12
  /// matches the inputs of a fragment shader that leaves out the later ones by their register,
  /// so a shader that reads only `uv` works, and one that reads `normal` must read the others.
  float2 uv : TEXCOORD0;
  /// World space.
  float3 world_position : TEXCOORD1;
  /// World space, not normalized (the rasterizer interpolates it).
  float3 normal : TEXCOORD2;
};

/// vector.xyz is the direction the light travels (a directional light, w = 0) or its position (a
/// point light, w = 1). color.rgb is the colour times the intensity. Unused lights have a zero
/// colour.
struct MeshLight {
  float4 vector;
  float4 color;
};

struct MeshGlobals {
  /// The material's base colour factor (straight alpha).
  float4 base_color;
  /// x: metallic, y: roughness, z and w: unused.
  float4 material;
  /// The camera's world position.
  float4 camera_position;
  /// The ambient light's colour times its intensity.
  float4 ambient;
  MeshLight lights[VNMAKI_MAX_LIGHTS];
};

#ifndef VNMAKI_MESH_VERTEX

/// The base colour texel at `uv` (premultiplied alpha), not yet times `globals.base_color`.
float4 sample_base(float2 uv) {
  return vnmaki_sprite_texture.Sample(vnmaki_sprite_sampler, uv);
}

/// The base colour with straight alpha: the texture times the factor.
float4 base_color_at(MeshGlobals globals, float2 uv) {
  const float4 texel = sample_base(uv);
  const float3 straight = texel.a > 0.0 ? texel.rgb / texel.a : float3(0.0, 0.0, 0.0);
  return float4(straight, texel.a) * globals.base_color;
}

/// The unit vector from the surface point to light `index`, and the light's colour there (a point
/// light weakens with the square of its distance).
void light_at(MeshGlobals globals, int index, float3 world_position, out float3 to_light,
              out float3 radiance) {
  const MeshLight light = globals.lights[index];
  if (light.vector.w < 0.5) {
    to_light = -normalize(light.vector.xyz);
    radiance = light.color.rgb;
  } else {
    const float3 offset = light.vector.xyz - world_position;
    to_light = normalize(offset);
    radiance = light.color.rgb / (1.0 + dot(offset, offset));
  }
}

/// A colour with straight alpha as the premultiplied colour a shader returns.
float4 premultiply(float3 rgb, float alpha) {
  return float4(rgb * alpha, alpha);
}

#endif

#endif

// Vertex shader for every mesh VNMaki draws: model space to world space to clip space. Vertex
// constant buffers live in register space1 (SDL GPU).

#define VNMAKI_MESH_VERTEX
#include "mesh.hlsli"

cbuffer MeshPass : register(b0, space1) {
  row_major float4x4 view_projection;
  row_major float4x4 model;
};

struct MeshVertex {
  float3 position : TEXCOORD0;
  float3 normal : TEXCOORD1;
  float2 uv : TEXCOORD2;
};

MeshFragment main(MeshVertex input) {
  const float4 world = mul(model, float4(input.position, 1.0));
  MeshFragment output;
  output.position = mul(view_projection, world);
  output.world_position = world.xyz;
  output.normal = mul((float3x3)model, input.normal);
  output.uv = input.uv;
  return output;
}

// What a VNMaki fragment shader, built-in or custom, can use.
//
// A custom fragment shader includes this file and writes
//
//     float4 main(SpriteFragment input) : SV_Target0 { ... }
//
// returning a colour with PREMULTIPLIED alpha. It may sample the drawn texture with
// sample_sprite(input), and may declare one constant buffer at register(b0, space3) whose bytes
// each draw supplies (DrawState::uniforms in engine/render/renderer.hpp).
//
// Register spaces follow SDL GPU's rules for SPIR-V and DXIL: fragment textures and samplers
// live in space2, fragment constant buffers in space3.

#ifndef VNMAKI_HLSLI
#define VNMAKI_HLSLI

#include "sprite_fragment.hlsli"

Texture2D<float4> vnmaki_sprite_texture : register(t0, space2);
SamplerState vnmaki_sprite_sampler : register(s0, space2);

/// The drawn texture's texel at the fragment (premultiplied alpha), not yet tinted.
float4 sample_sprite(SpriteFragment input) {
  return vnmaki_sprite_texture.Sample(vnmaki_sprite_sampler, input.uv);
}

#endif

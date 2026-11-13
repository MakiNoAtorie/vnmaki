// What a VNMaki fragment shader, built-in or custom, can use.
//
// A custom fragment shader includes this file and writes
//
//     float4 main(SpriteFragment input) : SV_Target0 { ... }
//
// returning a colour with PREMULTIPLIED alpha. It may sample the drawn texture with
// sample_sprite(input), and may declare one constant buffer at register(b0, space3) whose bytes
// each draw supplies (DrawState::uniforms in engine/render/renderer.hpp). A shader may read two
// more textures, sample_texture1 and sample_texture2 (DrawState::more_textures); a draw that
// names none binds the drawn texture there. Transitions use them (transition.hlsli).
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

Texture2D<float4> vnmaki_texture1 : register(t1, space2);
SamplerState vnmaki_sampler1 : register(s1, space2);
Texture2D<float4> vnmaki_texture2 : register(t2, space2);
SamplerState vnmaki_sampler2 : register(s2, space2);

/// The texel of DrawState::more_textures[0] at `uv`.
float4 sample_texture1(float2 uv) {
  return vnmaki_texture1.Sample(vnmaki_sampler1, uv);
}

/// The texel of DrawState::more_textures[1] at `uv`.
float4 sample_texture2(float2 uv) {
  return vnmaki_texture2.Sample(vnmaki_sampler2, uv);
}

#endif

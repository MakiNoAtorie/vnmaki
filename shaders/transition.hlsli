// What a transition shader can use, built-in or custom (docs/SPEC.md section 10). A transition
// draws one quad over the whole virtual screen with three textures, and the shader returns the
// mix, premultiplied:
//
//     #include "transition.hlsli"
//     float4 main(SpriteFragment input) : SV_Target0 { return lerp(from_at(input.uv), ...); }
//
// A custom transition is a game shader (shader(name, { "progress", ... })) that declares the
// uniform "progress" (0 at the start, 1 at the end). Its other uniforms come from the options of
// the transition (`transition("name", { uniforms = { amount = 2 } })`).

#ifndef VNMAKI_TRANSITION_HLSLI
#define VNMAKI_TRANSITION_HLSLI

#include "vnmaki.hlsli"

/// The old frame at `uv` (premultiplied). It is also the drawn texture of the quad.
float4 from_at(float2 uv) {
  return vnmaki_sprite_texture.Sample(vnmaki_sprite_sampler, uv);
}

/// The new frame at `uv` (premultiplied).
float4 to_at(float2 uv) {
  return sample_texture1(uv);
}

/// The mask image at `uv` (premultiplied; the image transition reads its red channel). A
/// transition that has no mask gets the new frame here.
float4 mask_at(float2 uv) {
  return sample_texture2(uv);
}

/// 1 where the new frame shows. `position` runs from 0 where the change starts to 1 where it ends;
/// the edge moves from before 0 to past 1 as `progress` goes from 0 to 1, and `softness` is how
/// much of that range the edge covers.
float reveal(float position, float progress, float softness) {
  static const float kTiny = 0.000001;
  if (progress <= 0.0) {
    return 0.0;
  }
  if (progress >= 1.0) {
    return 1.0;
  }
  return saturate((progress * (1.0 + softness) - position) / max(softness, kTiny));
}

#endif

// The constant buffer of the built-in transition shaders. The C++ side is TransitionUniforms in
// engine/anim/transition.hpp.

#ifndef VNMAKI_TRANSITION_UNIFORMS_HLSLI
#define VNMAKI_TRANSITION_UNIFORMS_HLSLI

cbuffer TransitionUniforms : register(b0, space3) {
  // 0 at the start of the transition, 1 at its end.
  float progress;
  // wipe and mask: the width of the soft edge, as a fraction of the way the edge moves.
  float softness;
  // slide and wipe: the way the content or the edge moves, in uv units.
  float2 direction;
  // fade: the colour to go through (straight alpha).
  float4 color;
};

#endif

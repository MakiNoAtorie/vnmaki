// What VNMaki's sprite vertex shader hands to a fragment shader. Shared by sprite.vert.hlsl and
// vnmaki.hlsli so both stages agree on the interface.

#ifndef VNMAKI_SPRITE_FRAGMENT_HLSLI
#define VNMAKI_SPRITE_FRAGMENT_HLSLI

struct SpriteFragment {
  float4 position : SV_Position;
  /// Texture coordinates, (0, 0) at the texture's top-left corner.
  float2 uv : TEXCOORD0;
  /// The quad's tint with premultiplied alpha.
  float4 color : TEXCOORD1;
};

#endif

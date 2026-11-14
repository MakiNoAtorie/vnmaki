// Effect text: draws a glyph from a signed-distance-field atlas (GlyphRaster::kDistance in
// engine/text/text.hpp) in the quad's tint, with an outline around it. The C++ side of the
// constant buffer is SdfTextUniforms. tools/generators/render_fixtures.py has the reference
// (sdf_coverage()).

#include "vnmaki.hlsli"

cbuffer SdfTextUniforms : register(b0, space3) {
  // Straight alpha.
  float4 outline_color;
  // In atlas pixels.
  float outline_width;
  // The pixels of distance that one step of the 8-bit atlas value stands for.
  float pixels_per_level;
  float2 unused_padding;
};

float4 main(SpriteFragment input) : SV_Target0 {
  // The atlas value is 128 on the glyph's edge and rises towards the inside.
  const float distance = (sample_sprite(input).a * 255.0 - 128.0) * pixels_per_level;
  // One pixel of soft edge, centred on the edge.
  const float fill = smoothstep(-0.5, 0.5, distance);
  const float shape = smoothstep(-0.5, 0.5, distance + outline_width);
  // The outline fades with the tint's alpha, as the glyph does.
  const float4 outline = float4(outline_color.rgb * outline_color.a, outline_color.a) * input.color.a;
  return lerp(outline, input.color, fill) * shape;
}

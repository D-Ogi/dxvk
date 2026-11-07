#version 450

#extension GL_EXT_samplerless_texture_functions : require

#define EXPORT_YUY2 (3u)
#define EXPORT_RGBA (0u)
#define EXPORT_Y (1u)
#define EXPORT_CbCr (2u)

// Can't use matrix types here since even a two-row
// matrix will be padded to 16 bytes per column for
// absolutely no reason
layout(std140, set = 0, binding = 0)
uniform ubo_t {
  vec4 color_matrix_r1;
  vec4 color_matrix_r2;
  vec4 color_matrix_r3;
  vec2 coord_matrix_c1;
  vec2 coord_matrix_c2;
  vec2 coord_matrix_c3;
  uvec2 src_offset;
  uvec2 src_extent;
  float y_min;
  float y_max;
  uint input_mode;
  uint export_mode;
};

layout(location = 0) in vec2 i_texcoord;
layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 1) uniform texture2D s_inputY;
layout(set = 0, binding = 2) uniform texture2D s_inputCbCr;

vec4 filter_color(vec2 texcoord) {
  // Transform input texture coordinates to
  // account for rotation and source rectangle
  mat3x2 coord_matrix = mat3x2(
    coord_matrix_c1,
    coord_matrix_c2,
    coord_matrix_c3);

  // Load color space transform
  mat3x4 color_matrix = mat3x4(
    color_matrix_r1,
    color_matrix_r2,
    color_matrix_r3);

  // Compute actual pixel coordinates to sample. We filter
  // manually in order to avoid bleeding from pixels outside
  // the source rectangle.
  vec2 abs_size_y = vec2(textureSize(s_inputY, 0));
  if (input_mode == 2u) abs_size_y.x *= 2.0f;
  vec2 abs_size_c = vec2(textureSize(s_inputCbCr, 0));

  vec2 coord = coord_matrix * vec3(texcoord, 1.0f);
  coord -= 0.5f / abs_size_y;

  vec2 size_factor = abs_size_c / abs_size_y;

  vec2 src_lo = vec2(src_offset);
  vec2 src_hi = vec2(src_offset + src_extent - 1u);

  vec2 abs_coord = coord * abs_size_y;
  vec2 fract_coord = fract(clamp(abs_coord, src_lo, src_hi));

  vec4 accum = vec4(0.0f, 0.0f, 0.0f, 0.0f);

  for (int i = 0; i < 4; i++) {
    ivec2 offset = ivec2(i & 1, i >> 1);

    // Compute exact pixel coordinates for the current
    // iteration and clamp it to the source rectangle.
    vec2 fetch_coord = clamp(abs_coord + vec2(offset), src_lo, src_hi);

    // Fetch actual pixel color in source color space
    vec4 color;

    if (input_mode == 2u) {
      ivec2 pixel = ivec2(fetch_coord);
      vec4 pair = texelFetch(s_inputY, ivec2(pixel.x / 2, pixel.y), 0);
      // The CSC matrix consumes Cr/Y/Cb. A pair contains Y0/Cb/Y1/Cr.
      color = vec4(pair.a, (pixel.x & 1) != 0 ? pair.b : pair.r, pair.g, 1.0f);
    } else if (input_mode == 1u) {
      color.g  = texelFetch(s_inputY, ivec2(fetch_coord), 0).r;
      color.rb = texelFetch(s_inputCbCr, ivec2(fetch_coord * size_factor), 0).gr;
      color.g  = clamp((color.g - y_min) / (y_max - y_min), 0.0f, 1.0f);
      color.a = 1.0f;
    } else {
      color = texelFetch(s_inputY, ivec2(fetch_coord), 0);
    }

    // Transform color space before accumulation
    color.rgb = vec4(color.rgb, 1.0f) * color_matrix;

    // Filter and accumulate final pixel color
    vec2 factor = fract_coord;

    if (offset.x == 0) factor.x = 1.0f - factor.x;
    if (offset.y == 0) factor.y = 1.0f - factor.y;

    accum += factor.x * factor.y * color;
  }

  return accum;
}

void main() {
  if (export_mode == EXPORT_YUY2) {
    // A destination texel covers two logical pixels. Sample at their centres,
    // transform both, then share their mean chroma while keeping both lumas.
    vec2 half_pixel = 0.25f * dFdx(i_texcoord);
    vec4 even_color = filter_color(i_texcoord - half_pixel);
    vec4 odd_color = filter_color(i_texcoord + half_pixel);
    o_color = vec4(even_color.g, 0.5f * (even_color.b + odd_color.b),
                   odd_color.g, 0.5f * (even_color.r + odd_color.r));
    return;
  }
  vec4 accum = filter_color(i_texcoord);
  if (export_mode == EXPORT_RGBA)
    o_color = accum;
  else if (export_mode == EXPORT_Y)
    o_color = vec4(accum.g, 0.0, 0.0, 1.0);
  else if (export_mode == EXPORT_CbCr)
    o_color = vec4(accum.br, 0.0, 1.0);
}

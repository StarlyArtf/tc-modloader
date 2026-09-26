#version 410 core

#define NUM_SYMBOL 32
#define BIG_ASS_NUMBER 64.0
#define TEXTURE_SHRINK 0.7
#define ALPHA_BOUNDARY 0.8

/* Same stride as word_watchee.vert: a >32-bit label is two lines of glyphs in
   one flat array, line 0 (low 32 bits, decimal) on top and line 1 (high bits,
   decimal) below.  A one-line label keeps using line 0 only. */
#define ROW_STRIDE 10

in vec2 frag_tex_coord;
flat in uint frag_label_value[2 * ROW_STRIDE];

out vec4 scrn_color;

uniform sampler2D u_texture;

float alpha(float frac) {
    float alpha = min(0.0, TEXTURE_SHRINK / 2 * ALPHA_BOUNDARY - abs(frac - 0.5));
    return exp(BIG_ASS_NUMBER * alpha);
}

void main() {
    float whole;
    float row;
    float frac = modf(frag_tex_coord.x, whole) * TEXTURE_SHRINK + (1 - TEXTURE_SHRINK) / 2;
    // The vertical coordinate selects the line, the glyph is sampled in its own
    // cell either way, so a one-line label (row always 0) is untouched.
    float glyph_row = modf(frag_tex_coord.y, row);
    whole = frag_label_value[int(row) * ROW_STRIDE + int(whole)];
    vec4 color = texture(u_texture, vec2((whole + frac) / NUM_SYMBOL, glyph_row));
    scrn_color = vec4(color.rgb, color.a * alpha(frac));
}

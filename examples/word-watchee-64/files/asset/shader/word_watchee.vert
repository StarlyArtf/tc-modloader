#version 410 core

#define SCALE (vec2(0.7, 1.8) * 0.85)

#define MAX_UINT 0xFFFFFFFFu
#define IDX_DIGIT_OFFSET 1u
#define IDX_SPACE 0u
#define IDX_MINUS 17u
#define IDX_DOT 18u

#define OVERRIDE_HIDDEN 1u
#define OVERRIDE_Z 2u
#define OVERRIDE_FORCE_SIGNED 3u

/* Cells one label line may use: ten decimal digits are the most a 32-bit
   unsigned number needs.  A value wider than 32 bits is drawn as two such
   lines - line 0 is the low 32 bits (top line), line 1 the high bits below -
   so the flat glyph array is two strides long and the fragment shader indexes
   it with this stride.  Both files carry the same #define. */
#define ROW_STRIDE 10

// Instance attributes
layout (location = 2) in vec2 inst_position;
layout (location = 3) in float inst_scale;
layout (location = 4) in int inst_rotation;
layout (location = 6) in uint inst_value_size;
layout (location = 7) in uvec2 inst_value;  // Little-endian (2 32-bit bytes) 64-bit integer
layout (location = 8) in uint inst_value_override;  // 0 for no override, 1 for no show, 2 for z-value

out vec2 frag_tex_coord;
flat out uint frag_label_value[2 * ROW_STRIDE];

layout (std140) uniform GlobalViewModel {
    vec2 u_translation;
    vec2 u_scale;
};

layout (std140) uniform GlobalTime {
    float u_time;
    float u_time_drone;
    int u_format;
};

struct Trigonometry {
    float cosine;
    float sine;
    float tangent;
    float dummy;
};

layout (std140) uniform GlobalComputedCache {
    Trigonometry u_trigs[16];
};

int label_length;
/* 1.0 for a one-line label, 2.0 when a >32-bit value is split into two lines. */
float label_rows = 1.0;

/* Divide a 64-bit value by ten in place and return the remainder.  The game's
   own divmod10 (the 2^67/10 inverse multiply) is off by one on some values - it
   returns 7 for 2^63, whose true remainder is 8 - which the 32-bit label path
   never noticed and this one cannot afford.  Four 16-bit chunks of plain long
   division are unrolled and exact: the running remainder stays below 10, so
   every step (remainder * 65536 + chunk <= 655359) fits a uint. */
uint divmod10(inout uvec2 value) {
    uint remainder = 0u;
    uint current = 0u;
    uint quotient_hi_hi = 0u;
    uint quotient_hi_lo = 0u;
    uint quotient_lo_hi = 0u;
    uint quotient_lo_lo = 0u;

    current = (value[1] >> 16) + remainder * 65536u;
    quotient_hi_hi = current / 10u;
    remainder = current % 10u;
    current = (value[1] & 0xFFFFu) + remainder * 65536u;
    quotient_hi_lo = current / 10u;
    remainder = current % 10u;
    current = (value[0] >> 16) + remainder * 65536u;
    quotient_lo_hi = current / 10u;
    remainder = current % 10u;
    current = (value[0] & 0xFFFFu) + remainder * 65536u;
    quotient_lo_lo = current / 10u;
    remainder = current % 10u;

    value = uvec2((quotient_lo_hi << 16) | quotient_lo_lo,
                  (quotient_hi_hi << 16) | quotient_hi_lo);
    return remainder;
}

bool is_zero(uvec2 value) {
    return value[0] == 0u && value[1] == 0u;
}

bool extend_sign(inout uvec2 value) {
    uint sign_bit;
    if (inst_value_size <= 32u) {
        uint mask = 1u << (inst_value_size - 1u);
        sign_bit = value[0] & mask;
        if (sign_bit == 0u) {
            value[0] &= mask - 1u;
            value[1] = 0u;
        } else {
            value[0] |= MAX_UINT - (sign_bit - 1u);
            value[1] = MAX_UINT;
        }
    } else {
        uint mask = 1u << (inst_value_size - 33u);
        sign_bit = value[1] & mask;
        if (sign_bit == 0u) {
            value[1] &= mask - 1u;
        } else {
            value[1] |= MAX_UINT - (sign_bit - 1u);
        }
    }
    return sign_bit != 0u;
}

void zero_extend(inout uvec2 value) {
    if (inst_value_size <= 32u) {
        uint mask = 0xFFFFFFFFu >> (32u - inst_value_size);
        value[0] &= mask;
        value[1] = 0u;
    } else {
        uint mask = 0xFFFFFFFFu >> (64u - inst_value_size);
        value[1] &= mask;
    }
}

uvec2 negate(uvec2 value) {
    value = ~value;
    value[0] += 1u;
    if (value[0] == 0u) {
        value[1] += 1u;
    }
    return value;
}

int repr10(uvec2 value, bool is_signed) {
    int lower = 0;
    if (is_signed && extend_sign(value)) {
        frag_label_value[lower] = IDX_MINUS;
        lower += 1;
        value = negate(value);
    } else if (!is_signed) {
        zero_extend(value);
    }

    int upper = lower;
    do {
        frag_label_value[upper] = divmod10(value) + IDX_DIGIT_OFFSET;
        upper += 1;
    } while (!is_zero(value));

    int sum = lower + upper;
    for (int i = lower; i * 2 < sum; i += 1) {
        uint tmp = frag_label_value[i];
        frag_label_value[i] = frag_label_value[sum - 1 - i];
        frag_label_value[sum - 1 - i] = tmp;
    }
    return upper;
}

int repr16(uvec2 value) {
    int index = 0;
    int ret = max((int(inst_value_size) + 3) / 4, 1);
    int shift = (int(ret) - 1) * 4;
    if (inst_value_size <= 32u) {
        value[0] &= MAX_UINT >> (32u - inst_value_size);
        do {
            frag_label_value[index] = ((value[0] >> shift) & 0xFu) + IDX_DIGIT_OFFSET;
            index += 1;
            shift -= 4;
        } while (shift >= 0);
    } else {
        value[1] &= MAX_UINT >> (64u - inst_value_size);
        shift -= 32;
        do {
            frag_label_value[index] = ((value[1] >> shift) & 0xFu) + IDX_DIGIT_OFFSET;
            index += 1;
            shift -= 4;
        } while (shift >= 0);

        shift = 28;
        do {
            frag_label_value[index] = ((value[0] >> shift) & 0xFu) + IDX_DIGIT_OFFSET;
            index += 1;
            shift -= 4;
        } while (shift >= 0);
    }
    return ret;
}

int repr_z() {
    frag_label_value[0] = IDX_MINUS;
    return 1;
}

/* A value wider than 32 bits, as two lines of decimal digits.  The digits are
   those of the value read unsigned over its own width (the wide label ignores
   the game's number format on purpose); the break falls after the tenth digit,
   so the top line keeps the leading ten digits and the bottom line the rest.
   2^63 = 9223372036854775808 therefore reads 9223372036 over 854775808, and a
   value that fits ten digits shows 0 on the second line. */
/* Cells of a line that carry no digit become spaces: the quad is as wide as the
   longer line, so the shorter one would otherwise show whatever its cells held
   (the game never reads past label_length in the one-line case, which is why it
   could leave them alone). */
void pad_row(int row, int used) {
    for (int i = used; i < ROW_STRIDE; i += 1) {
        frag_label_value[row * ROW_STRIDE + i] = IDX_SPACE;
    }
}

int repr_wide(uvec2 value) {
    value[1] &= MAX_UINT >> (64u - inst_value_size);

    // Count the digits first: the value is divided by ten in place, so no second
    // array is needed - and a dynamically indexed local array is exactly what
    // some drivers mis-compile here (measured on the pinned build: the second
    // line came out as its first digit only).
    uvec2 probe = value;
    int digits = 0;
    do {
        divmod10(probe);
        digits += 1;
    } while (!is_zero(probe) && digits < 2 * ROW_STRIDE);

    // The second line takes the digits that do not fit the first one.
    int bottom_cells = digits > ROW_STRIDE ? digits - ROW_STRIDE : 0;
    int top_cells = digits - bottom_cells;

    uvec2 rest = value;
    for (int k = 0; k < digits; k += 1) {
        uint glyph = divmod10(rest) + IDX_DIGIT_OFFSET;
        if (k < bottom_cells) {
            frag_label_value[ROW_STRIDE + (bottom_cells - 1 - k)] = glyph;
        } else {
            frag_label_value[top_cells - 1 - (k - bottom_cells)] = glyph;
        }
    }
    if (bottom_cells == 0) {
        // Ten digits or fewer: the second line is a plain zero.
        frag_label_value[ROW_STRIDE] = IDX_DIGIT_OFFSET;
        bottom_cells = 1;
    }
    pad_row(0, top_cells);
    pad_row(1, bottom_cells);
    return max(top_cells, bottom_cells);
}

vec2 vert_position() {
    return vec2(gl_VertexID % 2 - 0.5, gl_VertexID / 2 * label_rows - 0.5);
}

vec2 local_transform(vec2 position) {
    position *= inst_scale * SCALE;
    position.x *= label_length;

    // Translate scalar angle to rotation matrix
    Trigonometry trig = u_trigs[inst_rotation & 0xF];
    mat2 rotation_mat = mat2(trig.cosine, trig.sine, -trig.sine, trig.cosine);
    position = rotation_mat * position;
    return position + inst_position;
}

vec4 global_transform(vec2 position) {
    position *= u_scale;
    position += u_translation;
    return vec4(position, 0.0, 1.0);
}

// Calculate final position from model+global matrix and local position
vec4 transform(vec2 position) {
    position = local_transform(position);
    return global_transform(position);
}

vec2 tex_coord() {
    vec2 position = vec2(gl_VertexID % 2, gl_VertexID / 2);
    position.x *= label_length;
    position.y *= label_rows;
    return position;
}

void main() {
    if (inst_value_override == OVERRIDE_HIDDEN) {
        label_length = 0;
    } else if (inst_value_override == OVERRIDE_Z) {
        label_length = repr_z();
    } else if (inst_value_size > 32u) {
        // Wider than one line: split into low/high 32-bit lines of decimal
        // integers.  A label this wide does not follow the game's number
        // format; the <=32-bit path below keeps following it.
        label_rows = 2.0;
        label_length = repr_wide(inst_value);
    } else if (inst_value_override == OVERRIDE_FORCE_SIGNED) {
        label_length = repr10(inst_value, true);
    } else if (u_format >> 2 != 0) {
        label_length = repr10(inst_value, u_format >> 2 < 0);
    } else {
        label_length = repr16(inst_value);
    }

    gl_Position = transform(vert_position());

    frag_tex_coord = tex_coord();
}

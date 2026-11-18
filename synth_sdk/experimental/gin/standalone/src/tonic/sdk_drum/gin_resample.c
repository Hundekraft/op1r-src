/* Copyright (c) 2026 SquareWave Studio. SDK-derived portions (c) 2026 pcppcp.
 * GPL-3.0-or-later with OP1R additional permission; see LICENSE and NOTICE. */
#include "gin_resample.h"












static const int16_t odd_q14[8] = {
    -49, 134, -287, 541, -954, 1671, -3216, 10352
};

void gin_resample_2x_reset(gin_resample_2x_state *state)
{
    unsigned i;
    volatile int16_t *history = state->history;
    for (i = 0; i < GIN_RESAMPLE_HISTORY; ++i) history[i] = 0;
    state->write = 0;
}

static int16_t rounded_saturated(int32_t sum)
{
    int32_t value = sum >= 0 ? (sum + 8192) >> 14
                            : -(((-sum) + 8192) >> 14);
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    return (int16_t)value;
}

/* The samples of a whole call in one linear buffer (the 15 before it, then the call's
 * own), so the 33-tap reconstruction reads plain offsets instead of the masked 16-entry ring, with no call per sample.
 * Same integer sums (no term or partial sum can overflow: 2 * 32768 * 17204 < 2^31), same samples; the ring and
 * write index are kept as before for the next call and reset. */
static void reconstruct_block(gin_resample_2x_state *state, const int16_t *in,
                              int16_t *out, unsigned count)
{
    int16_t buf[GIN_RESAMPLE_HISTORY - 1u + GIN_RESAMPLE_CORE_BLOCK];
    unsigned w = state->write, i, j;
    for (j = 0; j < GIN_RESAMPLE_HISTORY - 1u; ++j)
        buf[j] = state->history[(w - (GIN_RESAMPLE_HISTORY - 1u) + j) & 15u];
    for (i = 0; i < count; ++i) buf[GIN_RESAMPLE_HISTORY - 1u + i] = in ? in[i] : 0;
#if defined(__bfin__) && !defined(GIN_PORTABLE_ARITHMETIC)
    /* sixteen 16 x 16 integer MACs into A0 (the same exact products and sum), the next pair of samples loaded
     * alongside; coefficient j in half j & 1 of word j >> 1 */
    const uint32_t c01 = (uint16_t)odd_q14[0] | (uint32_t)(uint16_t)odd_q14[1] << 16;
    const uint32_t c23 = (uint16_t)odd_q14[2] | (uint32_t)(uint16_t)odd_q14[3] << 16;
    const uint32_t c45 = (uint16_t)odd_q14[4] | (uint32_t)(uint16_t)odd_q14[5] << 16;
    const uint32_t c67 = (uint16_t)odd_q14[6] | (uint32_t)(uint16_t)odd_q14[7] << 16;
    for (i = 0; i < count; ++i) {
        const int16_t *x = buf + i;               /* x[15] is this input, x[15 - j] the one j before it */
        int32_t sum, b, c, d;
        __asm__ volatile(
            "%0 = 0;\n\t"
            "L0 = %0;\n\t"                   /* linear addressing, as the vendor's DAG asm does (the core's drum */
            "L1 = %0;\n\t"                   /* wrapper zeroes L0..L3 too) */
            "I0 = %4;\n\t"
            "I1 = %5;\n\t"
            "MNOP || %0.L = W[I0--] || %1.L = W[I1++];\n\t"
            "A0 = %0.L * %6.L (IS) || %2.L = W[I0--] || %3.L = W[I1++];\n\t"
            "A0 += %1.L * %6.L (IS);\n\t"
            "A0 += %2.L * %6.H (IS) || %0.L = W[I0--] || %1.L = W[I1++];\n\t"
            "A0 += %3.L * %6.H (IS);\n\t"
            "A0 += %0.L * %7.L (IS) || %2.L = W[I0--] || %3.L = W[I1++];\n\t"
            "A0 += %1.L * %7.L (IS);\n\t"
            "A0 += %2.L * %7.H (IS) || %0.L = W[I0--] || %1.L = W[I1++];\n\t"
            "A0 += %3.L * %7.H (IS);\n\t"
            "A0 += %0.L * %8.L (IS) || %2.L = W[I0--] || %3.L = W[I1++];\n\t"
            "A0 += %1.L * %8.L (IS);\n\t"
            "A0 += %2.L * %8.H (IS) || %0.L = W[I0--] || %1.L = W[I1++];\n\t"
            "A0 += %3.L * %8.H (IS);\n\t"
            "A0 += %0.L * %9.L (IS) || %2.L = W[I0--] || %3.L = W[I1++];\n\t"
            "A0 += %1.L * %9.L (IS);\n\t"
            "A0 += %2.L * %9.H (IS);\n\t"
            "A0 += %3.L * %9.H (IS);\n\t"
            "%0 = A0.w;"
            : "=&d"(sum), "=&d"(b), "=&d"(c), "=&d"(d)
            : "a"(x + 15), "a"(x), "d"(c01), "d"(c23), "d"(c45), "d"(c67), "m"(*(const int16_t (*)[16])x)
            : "I0", "I1", "L0", "L1", "A0", "ASTAT");
        out[2u * i] = x[15u - GIN_RESAMPLE_DELAY_CORE];
        out[2u * i + 1u] = rounded_saturated(sum);
    }
#else
    for (i = 0; i < count; ++i) {
        const int16_t *x = buf + i;               /* x[15] is this input, x[15 - j] the one j before it */
        int32_t sum = ((int32_t)x[15] + x[0]) * odd_q14[0]
                    + ((int32_t)x[14] + x[1]) * odd_q14[1]
                    + ((int32_t)x[13] + x[2]) * odd_q14[2]
                    + ((int32_t)x[12] + x[3]) * odd_q14[3]
                    + ((int32_t)x[11] + x[4]) * odd_q14[4]
                    + ((int32_t)x[10] + x[5]) * odd_q14[5]
                    + ((int32_t)x[9] + x[6]) * odd_q14[6]
                    + ((int32_t)x[8] + x[7]) * odd_q14[7];
        out[2u * i] = x[15u - GIN_RESAMPLE_DELAY_CORE];
        out[2u * i + 1u] = rounded_saturated(sum);
    }
#endif
    for (i = count > GIN_RESAMPLE_HISTORY ? count - GIN_RESAMPLE_HISTORY : 0; i < count; ++i)
        state->history[(w + i) & 15u] = buf[GIN_RESAMPLE_HISTORY - 1u + i];
    state->write = (w + count) & 15u;
}

void gin_resample_2x_process(gin_resample_2x_state *state,
                           const int16_t *in, int16_t *out,
                           unsigned input_count)
{
    while (input_count) {
        unsigned n = input_count < GIN_RESAMPLE_CORE_BLOCK ? input_count : GIN_RESAMPLE_CORE_BLOCK;
        reconstruct_block(state, in, out, n);
        in += n; out += 2u * n; input_count -= n;
    }
}

void gin_resample_2x_process64(gin_resample_2x_state *state,
                             const int16_t in[GIN_RESAMPLE_CORE_BLOCK],
                             int16_t out[GIN_RESAMPLE_NATIVE_BLOCK])
{
    reconstruct_block(state, in, out, GIN_RESAMPLE_CORE_BLOCK);
}

void gin_resample_2x_drain(gin_resample_2x_state *state, int16_t *out,
                         unsigned input_count)
{
    while (input_count) {
        unsigned n = input_count < GIN_RESAMPLE_CORE_BLOCK ? input_count : GIN_RESAMPLE_CORE_BLOCK;
        reconstruct_block(state, 0, out, n);
        out += 2u * n; input_count -= n;
    }
}

/* Brass synth engine: pitch-bent sine with timbre envelope.
 *   blue : starting interpolation position (sine → spread waveform), anywhere from 0% up to
 *          (100% - sweep); knob up = 0%, knob down = the highest start that still leaves room for the sweep
 *   green: sweep: how far the envelope pushes the interpolation up from the start (0%..100% of the whole range)
 *   white: vibrato depth
 *   red  : vibrato frequency (the orange knob)
 * The note starts flat and bends up into tune with the attack of the stock envelope; once the attack is over it
 * stays in tune (no droop on decay or release). The warp is limited on high notes (above B3) to keep aliasing
 * down, and the level drop the warp causes is partly made up (warp_gain, see gen_warp_table.py).
 * Copyright (C) 2026 nick (nvmsocool). GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"
#include "warp_table.h"

/* The stock envelope (OP1_VOICE_ENV) peaks at velocity x the firmware voice gain, about half scale on the device
 * (16256 at full velocity), not 32767. The timbre sweep and the pitch bend read it doubled (<< ENV_SHIFT, clamped),
 * so a full-velocity note reaches the full sweep and is in tune at the envelope's peak; softer notes stay darker. */
#define ENV_SHIFT         1

/* Pitch bend range: ~47 cents (Q15 fraction of the pitch increment). The note starts this far flat. */
#define BEND_AMOUNT       892
/* After the attack, any bend left (a soft note whose envelope peaked below half scale) settles into tune: it
 * loses 1/8 per block (~25 ms to within a cent), ramped across the block. */
#define BEND_SETTLE_SHIFT 3

/* Vibrato frequency range: red (orange) knob maps to VIBRATO_MIN_INC..VIBRATO_MAX_INC (0..32 Hz) on a squared
 * curve: slow speeds get most of the knob travel (knob 1/4 = 2 Hz, 1/2 = 8 Hz, 3/4 = 18 Hz, full = 32 Hz).
 * The LFO is advanced once per SAMPLE, so these are per-sample phase increments:
 * inc = f * 2^32 / 44100 (1 Hz = 97392). */
#define VIBRATO_MIN_INC   0          /*  0   Hz */
#define VIBRATO_MAX_INC   3116530    /* 32   Hz */

/* Vibrato depth range: white knob maps to 0..~90 cents (peak) on a squared curve for more control at the low end. */
#define VIBRATO_MAX_AMT   (BEND_AMOUNT * 2)

/* Vibrato gate: it fades in only once the stock envelope has reached sustain, i.e. it has stayed within
 * ENV_STEADY_TOL (Q15) of one value for ENV_STEADY_BLOCKS blocks in a row, and the key is held. The test spans
 * several blocks (not one) so the slow tail of an attack or decay curve is not mistaken for sustain.
 * Vibrato starts late: raise ENV_STEADY_BLOCKS or lower ENV_STEADY_TOL. Vibrato starts early: the opposite. */
#define ENV_STEADY_TOL    8
#define ENV_STEADY_BLOCKS 8          /* ~23 ms */
#define VIB_FADE_IN       1600       /* depth gained per block while steady (~20 blocks = ~60 ms to full) */
#define VIB_FADE_OUT      4096       /* depth lost per block otherwise */

/* Anti-aliasing: the full warp squeezes the sine cycle into a short burst with strong upper harmonics, which fold
 * back above about 300 Hz. From B3 up, the largest warp shrinks linearly with the note: WARP_CUT_PER_SEMI (Q15 of
 * the full range) per semitone, so it reaches a plain sine about 43 semitones up (F7). 768 = 1.5 of the 64 table
 * columns per semitone keeps the folded energy at the full warp below about -30 dB on every note (measured from the
 * table's spectrum); raise it for cleaner high notes, lower it for more bite. */
#define WARP_CUT_INC_B3   24050035   /* phase increment of B3 (246.94 Hz): 246.94 * 2^32 / 44100 */
#define WARP_CUT_RECIP    2857       /* 2^36 / WARP_CUT_INC_B3: (d >> 6) * this >> 15 = d / inc_B3 in Q15 */
#define WARP_CUT_PER_SEMI 768

/* Voice state (the firmware gives us 0x24 bytes: vtable + 32 bytes). */
typedef struct {
    uint32_t vtable;          /* firmware-owned */
    uint32_t ph;              /* oscillator phase (2^32 = one cycle) */
    int32_t  lp;              /* low-pass state */
    int32_t  interp_start;    /* blue/green: starting interp amount Q15, smoothed */
    uint32_t vibrato_phase;   /* vibrato LFO phase (2^32 = one cycle) */
    int16_t  vibrato_depth;   /* 0..32767: vibrato fade, rises while the envelope is steady */
    int16_t  prev_block_env;  /* stock envelope value of the previous block (Q15) */
    int16_t  width;           /* green: sweep width Q15, smoothed */
    int16_t  bend;            /* pitch bend at the end of the previous block (Q15 fraction, <= 0) */
    int16_t  env_anchor;      /* envelope value the steady test compares against (Q15) */
    uint8_t  steady_blocks;   /* blocks in a row the envelope stayed within ENV_STEADY_TOL of env_anchor (max 255) */
    uint8_t  flags;           /* BR_HELD, BR_ATTACK */
} brass_voice_t;
typedef char brass_voice_fits[sizeof(brass_voice_t) <= OP1_VOICE_SIZE ? 1 : -1];

#define BR_HELD   1           /* from note_on until note_off */
#define BR_ATTACK 2           /* the stock envelope is still rising: the bend follows it */

/* Starting interpolation position (Q15) from the blue and green knobs. Green is the sweep (the envelope adds up to
 * green on top of the start), so the start may use whatever is left: blue (inverted: up = 0%) picks anywhere in
 * 0 .. 100% - green. Green full: start is always 0%. Green zero: blue picks any position and nothing sweeps. */
static inline int32_t interp_start_for(const int16_t *params)
{
    return op1_mul16(32767 - params[0], 32767 - params[1]);
}

/* One-pole step toward target, rounded up when rising so the target is reached exactly (a plain >> 6 would stop
 * up to 63 short on the way up, and the top of the range must stay reachable). */
static inline int32_t track(int32_t cur, int32_t target)
{
    int32_t diff = target - cur;
    return cur + ((diff + (diff > 0 ? 63 : 0)) >> 6);
}

/* Largest interpolation amount (Q15) for this pitch: 32767 up to B3, then WARP_CUT_PER_SEMI less per semitone.
 * Semitones from log2(inc / inc_B3): whole octaves by halving, the rest by log2(1 + f) ~ f + 0.3466 f (1 - f)
 * (within 0.1 semitone). Once per block. */
static int32_t warp_limit(int32_t inc)
{
    if (inc <= WARP_CUT_INC_B3) return 32767;
    int32_t oct = 0;
    while (inc >= 2 * WARP_CUT_INC_B3) { inc >>= 1; oct++; }
    int32_t f = ((inc - WARP_CUT_INC_B3) >> 6) * WARP_CUT_RECIP >> 15;      /* 0..32767: d < 2^25, (d >> 6) * 2857 < 2^31 */
    int32_t l2 = f + ((((f * (32768 - f)) >> 15) * 11357) >> 15);           /* log2(1 + f), Q15 */
    int32_t cut = oct * (12 * WARP_CUT_PER_SEMI) + ((l2 * (12 * WARP_CUT_PER_SEMI)) >> 15);
    return cut >= 32767 ? 0 : 32767 - cut;
}

static void brass_note_on(void *self, const int16_t *params)
{
    brass_voice_t *v = (brass_voice_t *)self;
    v->interp_start = interp_start_for(params);
    v->width = params[1];
    v->ph = 0;
    v->lp = 0;
    v->vibrato_phase = 0;
    v->vibrato_depth = 0;
    v->prev_block_env = 0;
    v->bend = -BEND_AMOUNT;
    v->env_anchor = 0;
    v->steady_blocks = 0;
    v->flags = BR_HELD | BR_ATTACK;
}

static void brass_note_off(void *self)
{
    ((brass_voice_t *)self)->flags &= (uint8_t)~(BR_HELD | BR_ATTACK);
}

/* Warp table row s = 0..64 (64 = end of the cycle, 32768): rows past the middle mirror the first half. */
static inline int32_t warp_row(int32_t s, int32_t col)
{
    return s <= 32 ? warp_half[s][col] : 32768 - warp_half[64 - s][col];
}

/* Warped phase (Q15) at oscillator row s + row_frac_q15, column col + col_frac_q15, 4-point bilinear interpolation. */
static inline int32_t warp_lookup(int32_t s, int32_t row_frac_q15, int32_t col, int32_t col_frac_q15)
{
    int32_t w00 = warp_row(s, col);
    int32_t w10 = warp_row(s + 1, col);
    int32_t w01 = warp_row(s, col + 1);
    int32_t w11 = warp_row(s + 1, col + 1);
    /* Interpolate horizontally at top and bottom rows. */
    int32_t top = w00 + op1_mul16(w01 - w00, col_frac_q15);
    int32_t bottom = w10 + op1_mul16(w11 - w10, col_frac_q15);
    /* Interpolate vertically. */
    return top + op1_mul16(bottom - top, row_frac_q15);
}

static void brass_render(void *self, int16_t *out, const int16_t *params)
{
    brass_voice_t *v = (brass_voice_t *)self;
    const int32_t *inc = OP1_PITCH_INC;

    uint32_t ph = v->ph; int32_t lp = v->lp;
    uint32_t vibrato_phase = v->vibrato_phase;
    int32_t vg = op1_voice_vel_gain(self);             /* note velocity */

    /* ---- per-block control values ---- */

    /* Stock envelope: flat inside a block, steps between blocks. Ramp it linearly from the previous block's
     * value to this block's value, so nothing driven by it (timbre column, pitch bend) steps at a block edge.
     * env_q is the ramp in Q15 << 7; after 128 additions it equals env_end << 7 exactly. */
    int32_t env_prev = v->prev_block_env;
    int32_t env_end = OP1_VOICE_ENV[OP1_BLOCK - 1];
    int32_t env_q = env_prev << 7;
    int32_t env_step = env_end - env_prev;

    /* Pitch bend: flat at the start, up into tune as the envelope rises (in tune from half scale). The attack is
     * over once the envelope stops rising; from then on whatever bend is left settles to 0 and stays there. Ramped
     * across the block like the envelope (bend_q = Q15 << 7). */
    if ((v->flags & BR_ATTACK) && env_end > 0 && env_end <= env_prev) v->flags &= (uint8_t)~BR_ATTACK;
    int32_t bend_prev = v->bend, bend_end;
    if (v->flags & BR_ATTACK) {
        int32_t e2 = env_end << ENV_SHIFT;
        bend_end = op1_mul16(BEND_AMOUNT, e2 > 32767 ? 32767 : e2) - BEND_AMOUNT;
    } else {
        bend_end = bend_prev - (bend_prev >> BEND_SETTLE_SHIFT);      /* bend <= 0: >> rounds away from 0, reaches 0 */
    }
    int32_t bend_q = bend_prev << 7;
    int32_t bend_step = bend_end - bend_prev;

    /* Vibrato gate: sustain reached (envelope held still for ENV_STEADY_BLOCKS blocks) while the key is held
     * fades the vibrato in, anything else fades it out. */
    int32_t env_delta = env_end - v->env_anchor;
    if (env_delta < 0) env_delta = -env_delta;
    if (env_delta <= ENV_STEADY_TOL) {
        if (v->steady_blocks < 255) v->steady_blocks++;
    } else {
        v->env_anchor = (int16_t)env_end;
        v->steady_blocks = 0;
    }
    int32_t vibrato_depth = v->vibrato_depth;
    if ((v->flags & BR_HELD) && env_end > 0 && v->steady_blocks >= ENV_STEADY_BLOCKS) {
        vibrato_depth += VIB_FADE_IN;
        if (vibrato_depth > 32767) vibrato_depth = 32767;
    } else {
        vibrato_depth -= VIB_FADE_OUT;
        if (vibrato_depth < 0) vibrato_depth = 0;
    }

    /* Red/orange knob (0..32767) maps to vibrato frequency (per-sample LFO increment), squared: x^2 in Q15 gives
     * fine control at the slow end and squeezes the fast end into the last part of the knob. */
    int32_t knob_sq = (params[3] * params[3]) >> 15;
    /* 32-bit: (range >> 7) < 2^15, knob_sq < 2^15 (no 64-bit multiply, no libgcc import; error < 0.01 Hz) */
    int32_t vibrato_phase_inc = VIBRATO_MIN_INC + ((((VIBRATO_MAX_INC - VIBRATO_MIN_INC) >> 7) * knob_sq) >> 8);

    /* White knob (0..32767) maps to vibrato depth, squared: x^2 in Q15 gives fine control at the low end. */
    int32_t white_sq = (params[2] * params[2]) >> 15;
    int32_t vibrato_amt_q15 = (white_sq * VIBRATO_MAX_AMT) >> 15;
    vibrato_amt_q15 = op1_mul16(vibrato_amt_q15, vibrato_depth);

    /* Blue/green: the start position and the sweep width (green) both glide to their knob values. */
    int32_t interp_start = track(v->interp_start, interp_start_for(params));
    int32_t width = track(v->width, params[1]);

    /* Largest warp for this note (anti-aliasing above B3): scales the whole interpolation range, so the sweep
     * still moves on high notes, just over a smaller range. */
    int32_t limit = warp_limit(inc[0]);

    for (int n = 0; n < OP1_BLOCK; n++) {
        /* Envelope ramp, scaled up so its device peak reaches full scale (Q15, 0..32767, see ENV_SHIFT). */
        env_q += env_step;
        int32_t env_val = env_q >> (7 - ENV_SHIFT);
        if (env_val > 32767) env_val = 32767;

        /* Interpolation: start (blue, limited by green) + sweep (green) scaled by the envelope, then limited by pitch. */
        int32_t interp_amount = interp_start + op1_mul16(width, env_val);
        interp_amount = interp_amount > 32767 ? 32767 : (interp_amount < 0 ? 0 : interp_amount);
        if (limit < 32767) interp_amount = op1_mul16(interp_amount, limit);
        int32_t col_pos = interp_amount * 64;          /* column position */
        int32_t col = col_pos >> 15;                   /* 0..63 */
        int32_t col_frac = col_pos & 0x7FFF;

        /* Pitch bend (<= 0), ramped across the block. */
        bend_q += bend_step;
        int32_t bend_q15 = bend_q >> 7;

        /* Vibrato: per-sample LFO, scaled by the gate/fade above. */
        int32_t vibrato_q15 = (OP1_SINE_TABLE[vibrato_phase >> 19] * vibrato_amt_q15) >> 15;
        vibrato_phase += (uint32_t)vibrato_phase_inc;

        /* inc * (1 + mod): 32-bit only. |mod| <= 892 + 1784 < 2^12 and inc < 2^31, so (inc >> 12) * mod stays below 2^31. */
        int32_t mod_q15 = bend_q15 + vibrato_q15;
        int32_t effective_inc = inc[n] + (((inc[n] >> 12) * mod_q15) >> 3);

        /* Advance oscillator phase. */
        ph += (uint32_t)effective_inc;

        /* Phase warp: 4-point bilinear in (oscillator phase row, table column). */
        int32_t s = ph >> 26;
        int32_t row_frac_q15 = (ph & 0x3FFFFFF) >> 11;
        int32_t ph_f_warped = warp_lookup(s, row_frac_q15, col, col_frac);

        /* Convert warped phase (0..32768) to sine table index. */
        int32_t sine_idx = (ph_f_warped * 8191) >> 15;
        sine_idx = sine_idx > 8191 ? 8191 : (sine_idx < 0 ? 0 : sine_idx);

        /* Lookup sine value. */
        int32_t sample = OP1_SINE_TABLE[sine_idx];

        /* One-pole low-pass filter. */
        lp += ((sample - lp) * 16384) >> 15;

        /* Level compensation for the warp (Q14, 1.0 .. 1.88), interpolated like the warp column. */
        int32_t g = warp_gain[col] + op1_mul16(warp_gain[col + 1] - warp_gain[col], col_frac);

        /* Velocity, half scale as the other engines (six voices on the synth bus); the firmware applies the
         * gain envelope. |lp| <= 32768, g < 31000: (lp * g) >> 14 < 62000, times vg <= 32766 stays below 2^31. */
        out[n] = op1_sat16((((lp * g) >> 14) * vg) >> 16);
    }
    v->ph = ph; v->lp = lp;
    v->interp_start = interp_start;
    v->width = (int16_t)width;
    v->bend = (int16_t)bend_end;
    v->vibrato_phase = vibrato_phase;
    v->vibrato_depth = (int16_t)vibrato_depth;
    v->prev_block_env = (int16_t)env_end;
}

const op1_engine_t ENGINE_brass = { brass_note_on, brass_render, brass_note_off, { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE } };

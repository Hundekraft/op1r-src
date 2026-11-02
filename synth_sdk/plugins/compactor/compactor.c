/* "compactor" - a small, clean RMS compressor for the OP-1 FX slot.
 *
 *   blue  : ratio      1:1 .. 20:1 (log)
 *   green : rms        threshold, -60 .. 0 dBFS RMS (the level where compression starts)
 *   white : attack     0.5 .. 100 ms (log)
 *   red   : release    20 ms .. 2 s (log)
 *
 * The detector is a true RMS (mean square, 2 ms one-pole), linked across left / right in the master
 * position.  A 6 dB soft knee keeps the onset transparent.  Auto make-up adds back the full slow
 * average gain reduction (capped at +6 dB), so sustained compression keeps its level without boosting
 * transients the attack lets through.
 *
 * The screen labels the four knobs and shows their values, the current gain reduction and the auto
 * make-up.
 *
 * CPU: the level detector runs per sample; the gain computer runs every 16 samples (0.36 ms) and the
 * gain is linearly ramped across those 16 samples.  The make-up is folded into the ramped gain at the
 * same rate, so the audio loop is a square, a split one-pole update, a gain ramp and two multiplies.
 * No division, no float and no 64-bit arithmetic in the loop.
 *
 * Copyright (C) 2026 nvmsocool. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"
#include "seq/seq_text.h"

#define CTRL        16                  /* gain control block, samples (0.36 ms) */
#define DET_COEF    369                 /* RMS one-pole, 2 ms: (1 - exp(-1 / 88.2)) * 32768 */
#define KNEE        256                 /* soft knee, Q8 octaves of power: 256 = 1 octave = 3 dB each side */
#define T2_MIN      786                 /* threshold -60 dBFS RMS in log2(power) * 256, Q23 power */
#define T2_SPAN     5102                /* 0 .. 60 dB in the same units */
#define MAKEUP_COEF 40                  /* ~300 ms average of the gain reduction, per block */

typedef struct {
    int32_t ms;                         /* smoothed mean square, Q23 (0 .. 8388608) */
    int32_t res;                        /* fractional part of the detector update */
    int32_t gain;                       /* current gain, Q15 (0 .. 32768), before make-up */
    int32_t g0, g1;                     /* output gain ramp endpoints for the current control block */
    int32_t t2;                         /* threshold, log2(power) Q8 */
    int32_t k;                          /* (1 - 1/ratio) / 2, Q15 */
    int32_t atk, rel;                   /* one-pole coefficients per control block, Q15 */
    int32_t avg_gr;                     /* slow average gain reduction, log2(amplitude) Q8 */
    int32_t avg_res;                    /* fractional part of the average update */
    int32_t makeup;                     /* auto make-up, Q15 (32768 = 0 dB, 65535 = +6 dB) */
} comp_t;

static comp_t st[3];                    /* one per FX position: 0 synth, 1 drum, 2 master */

static inline int comp_pos(int pos) { return pos > 2 ? 2 : (pos < 0 ? 0 : pos); }

/* log2(1 + i/16) * 256, i = 0..16 */
static const uint16_t log2_tab[17] = {
    0, 22, 44, 63, 82, 100, 118, 134, 150, 165, 179, 193, 207, 220, 232, 244, 256
};
/* 2^(i/16) * 32768, i = 0..16 */
static const int32_t exp2_tab[17] = {
    32768, 34219, 35734, 37316, 38968, 40693, 42495, 44376, 46341, 48393,
    50535, 52773, 55109, 57549, 60097, 62757, 65536
};
/* (1 - 1/ratio) / 2 * 32768 for ratio = 20^(i/64) */
static const int16_t ratio_k[65] = {
    0, 749, 1464, 2146, 2798, 3419, 4012, 4578, 5117, 5633, 6124, 6593, 7041,
    7468, 7876, 8265, 8636, 8991, 9329, 9651, 9959, 10253, 10534, 10801, 11056,
    11300, 11532, 11754, 11966, 12168, 12361, 12545, 12720, 12888, 13048, 13200,
    13346, 13485, 13617, 13744, 13865, 13980, 14090, 14195, 14295, 14390, 14482,
    14569, 14652, 14731, 14806, 14879, 14947, 15013, 15076, 15136, 15193, 15247,
    15299, 15349, 15396, 15441, 15484, 15526, 15565
};
/* one-pole coefficient for a 16-sample control block: (1 - exp(-16 / (fs * t))) * 32768 */
static const int16_t attack_coef[65] = {   /* 0.5 ms .. 100 ms, log */
    16908, 15966, 15051, 14163, 13308, 12485, 11697, 10945, 10228, 9548, 8904,
    8295, 7721, 7181, 6673, 6196, 5750, 5333, 4943, 4579, 4240, 3924, 3630,
    3357, 3103, 2868, 2650, 2447, 2260, 2086, 1925, 1776, 1639, 1512, 1394,
    1286, 1185, 1093, 1007, 928, 856, 788, 727, 669, 617, 568, 523, 482, 444,
    409, 377, 347, 319, 294, 271, 249, 230, 212, 195, 179, 165, 152, 140, 129, 119
};
static const int16_t release_coef[65] = {  /* 20 ms .. 2 s, log */
    589, 549, 511, 476, 443, 412, 384, 357, 333, 310, 288, 268, 250, 232, 216,
    201, 187, 174, 162, 151, 141, 131, 122, 113, 106, 98, 91, 85, 79, 74, 69,
    64, 59, 55, 51, 48, 45, 41, 39, 36, 33, 31, 29, 27, 25, 23, 22, 20, 19,
    17, 16, 15, 14, 13, 12, 11, 11, 10, 9, 9, 8, 7, 7, 6, 6
};

/* log2(v) * 256 for a positive integer.  Full scale is log2(scale) * 256, e.g. 15 * 256 for Q15
 * amplitude and 23 * 256 for the Q23 detector power. */
static int32_t log2_q8(uint32_t v)
{
    if (!v) return -32768;
    int32_t e = 0;
    while (v >= 512) { v >>= 1; e++; }
    while (v < 256) { v <<= 1; e--; }
    uint32_t i = (v - 256) >> 4;                 /* 0 .. 15 */
    uint32_t f = v & 15;
    int32_t frac = log2_tab[i] + (((log2_tab[i + 1] - log2_tab[i]) * (int32_t)f) >> 4);
    return e * 256 + 2048 + frac;                /* e + 8 (the implicit 256) */
}

/* 2^(x/256) * 32768, capped to 65535 (+6 dB).  x <= 256, so the result never needs more than 2x. */
static int32_t exp2_q15(int32_t x)
{
    if (x > 256) x = 256;
    if (x < -3840) return 0;                     /* below -15 octaves */
    int32_t n = x >> 8;                          /* floor (arithmetic shift) */
    int32_t f = x & 255;
    int32_t i = f >> 4;
    int32_t v = exp2_tab[i] + (((exp2_tab[i + 1] - exp2_tab[i]) * (f & 15)) >> 4);
    int32_t sh = -n;
    if (sh > 0) {
        if (sh >= 16) return 0;
        v >>= sh;
    } else if (sh < 0) {
        v <<= -sh;
    }
    return v > 65535 ? 65535 : v;
}

/* target gain, Q15, for a smoothed mean-square value */
static int32_t target_gain(int32_t ms, int32_t t2, int32_t k)
{
    int32_t over = log2_q8((uint32_t)ms) - t2;   /* log2(power / threshold), Q8 */
    int32_t gr;                                  /* gain reduction, log2(amplitude) Q8 */
    if (over <= -KNEE) {
        gr = 0;
    } else if (over >= KNEE) {
        gr = (over * k) >> 15;
    } else {                                     /* 6 dB soft knee */
        int32_t x = over + KNEE;                 /* 0 .. 2 * KNEE */
        gr = (((x * x) >> 8) * k) >> 17;
    }
    if (gr <= 0) return 32768;
    int32_t g = exp2_q15(-gr);
    return g > 32768 ? 32768 : g;
}

static void comp_params(comp_t *s, const int16_t *p)
{
    int32_t q0 = op1_fx_param_q15(p, 0);
    int32_t q1 = op1_fx_param_q15(p, 1);
    int32_t q2 = op1_fx_param_q15(p, 2);
    int32_t q3 = op1_fx_param_q15(p, 3);
    s->t2 = T2_MIN + ((q1 * T2_SPAN) >> 15);
    s->k = ratio_k[(q0 * 64 + 16384) >> 15];
    s->atk = attack_coef[(q2 * 64 + 16384) >> 15];
    s->rel = release_coef[(q3 * 64 + 16384) >> 15];

    /* auto make-up: the full slow average gain reduction, capped at +6 dB.  A transient that the
     * attack lets through is not boosted (no clipping); sustained compression is restored. */
    int32_t gr = 3840 - log2_q8((uint32_t)s->gain);     /* 0 dBFS amplitude = 3840 */
    if (gr < 0) gr = 0;
    if (gr > 3840) gr = 3840;
    int32_t d = gr - s->avg_gr;
    s->avg_res += d * MAKEUP_COEF;                      /* exact slow average, no truncation at low levels */
    s->avg_gr += s->avg_res >> 15;
    s->avg_res &= 32767;
    int32_t mu = s->avg_gr;                             /* full slow gain reduction ... */
    if (mu > gr) mu = gr;                               /* ... but never boost above unity ... */
    if (mu > 256) mu = 256;                             /* ... and at most +6 dB */
    int32_t target = exp2_q15(mu);
    s->makeup += ((target - s->makeup) * 8192) >> 15;   /* ~5 ms, no zipper on knob moves */
}

/* one sample: update the RMS detector, move the gain at the 16-sample control rate, return the total gain */
static inline int32_t comp_gain(comp_t *s, int n, int32_t power)
{
    /* one-pole RMS update with a 15-bit fractional accumulator: the exact (power - ms) * DET_COEF / 32768
     * in two 32-bit pieces, so even very low levels keep moving and the state can use Q23. */
    int32_t d = power - s->ms;
    s->res += (d & 32767) * DET_COEF;
    s->ms += (d >> 15) * DET_COEF + (s->res >> 15);
    s->res &= 32767;
    if ((n & (CTRL - 1)) == 0) {
        int32_t target = target_gain(s->ms, s->t2, s->k);
        int32_t coef = target < s->gain ? s->atk : s->rel;
        s->gain += ((target - s->gain) * coef) >> 15;
        int32_t out = (s->gain * s->makeup) >> 15;      /* fold the make-up into the ramped gain */
        s->g0 = s->g1;
        s->g1 = out;
    }
    return s->g0 + (((s->g1 - s->g0) * ((n & (CTRL - 1)) + 1)) >> 4);
}

static void comp_init(int pos)
{
    comp_t *s = &st[comp_pos(pos)];
    s->ms = 0;
    s->res = 0;
    s->gain = 32768;
    s->g0 = s->g1 = 32768;
    s->avg_gr = 0;
    s->avg_res = 0;
    s->makeup = 32768;
}

static void comp_render(int pos, int16_t *buf, const int16_t *p)
{
    comp_t *s = &st[comp_pos(pos)];
    comp_params(s, p);
    for (int n = 0; n < OP1_FX_BLOCK; n++) {
        int32_t x = buf[n];
        int32_t power = (x * x) >> 7;               /* Q23 mean square, keeps ~60 dB of detector range */
        if (power > 8388608) power = 8388608;
        int32_t g = comp_gain(s, n, power);
        buf[n] = op1_sat16((x * g) >> 15);
    }
}

/* master position: link the detector across both channels, apply the same gain to left and right */
static void comp_render_stereo(int pos, int16_t *l, int16_t *r, const int16_t *p)
{
    comp_t *s = &st[comp_pos(pos)];
    comp_params(s, p);
    for (int n = 0; n < OP1_FX_BLOCK; n++) {
        int32_t xl = l[n], xr = r[n];
        int32_t power = (((xl * xl) >> 7) + ((xr * xr) >> 7)) >> 1;
        if (power > 8388608) power = 8388608;
        int32_t g = comp_gain(s, n, power);
        l[n] = op1_sat16((xl * g) >> 15);
        r[n] = op1_sat16((xr * g) >> 15);
    }
}

const op1_fx_t FX_compactor = {
    comp_init, comp_render, { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE },
    comp_render_stereo
};

/* ======================= screen (GUI thread) =======================
 * compactor.svg holds the frame and the two separators.  Drawn here: the title, the four knob
 * labels and values, a position bar under each label, the current gain reduction and the auto
 * make-up.  Everything is integer; the stroke font has no lowercase letters. */
#pragma GCC push_options
#pragma GCC optimize("Os")                             /* GUI code: size, not speed (code space is short) */
#define Q(v) OP1_Q10(v)

static const uint8_t c_blue[4]  = { 0x4D, 0x9E, 0xFF, 0xFF };
static const uint8_t c_green[4] = { 0x00, 0xED, 0x95, 0xFF };
static const uint8_t c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t c_red[4]   = { 0xFF, 0x3A, 0x5D, 0xFF };
static const uint8_t c_grey[4]  = { 0x6F, 0x71, 0x73, 0xFF };
static const uint8_t c_dim[4]   = { 0x3A, 0x3B, 0x3D, 0xFF };

static const char *const k_lab[4] = { "RATIO", "RMS", "ATTACK", "RELEASE" };
static const uint8_t *const k_col[4] = { c_blue, c_green, c_white, c_red };
static const int16_t k_x[4] = { 12, 88, 164, 240 };

/* display tables, one entry per 1/64 of the knob travel (same indices as the DSP tables) */
static const int16_t ratio_x10[65] = {
    10, 10, 11, 12, 12, 13, 13, 14, 15, 15, 16, 17, 18, 18, 19, 20, 21, 22, 23,
    24, 26, 27, 28, 29, 31, 32, 34, 35, 37, 39, 41, 43, 45, 47, 49, 51, 54, 57,
    59, 62, 65, 68, 71, 75, 78, 82, 86, 90, 95, 99, 104, 109, 114, 120, 125, 131,
    138, 144, 151, 158, 166, 174, 182, 191, 200
};
static const int16_t attack_x10[65] = {   /* 0.1 ms units, 0.5 .. 100 ms */
    5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 11, 12, 14, 15, 16, 17, 19, 20, 22, 24, 26,
    28, 31, 34, 36, 40, 43, 47, 51, 55, 60, 65, 71, 77, 83, 91, 98, 107, 116, 126,
    137, 149, 162, 176, 191, 207, 225, 245, 266, 289, 314, 341, 370, 402, 437, 475,
    516, 560, 609, 661, 718, 780, 847, 921, 1000
};
static const int16_t release_ms[65] = {   /* 20 ms .. 2 s */
    20, 21, 23, 25, 27, 29, 31, 33, 36, 38, 41, 44, 47, 51, 55, 59, 63, 68, 73,
    78, 84, 91, 97, 105, 112, 121, 130, 140, 150, 161, 173, 186, 200, 215, 231,
    248, 267, 287, 308, 331, 356, 382, 411, 441, 474, 510, 548, 589, 632, 680,
    730, 785, 843, 906, 974, 1047, 1125, 1209, 1299, 1396, 1500, 1612, 1732, 1861, 2000
};

static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *put_num(char *p, int32_t v)
{
    char t[10]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}

static void fmt_ratio(char *t, int32_t q)
{
    int32_t v = ratio_x10[(q * 64 + 16384) >> 15];
    char *p = put_num(t, v / 10);
    *p++ = '.';
    *p++ = (char)('0' + v % 10);
    put_str(p, ":1");
}

static void fmt_db(char *t, int32_t db)
{
    char *p = t;
    if (db < 0) { *p++ = '-'; db = -db; }
    p = put_num(p, db);
    put_str(p, " DB");
}

static void fmt_attack(char *t, int32_t q)
{
    int32_t v = attack_x10[(q * 64 + 16384) >> 15];
    char *p;
    if (v >= 100) {
        p = put_num(t, (v + 5) / 10);
    } else {
        p = put_num(t, v / 10);
        *p++ = '.';
        *p++ = (char)('0' + v % 10);
    }
    put_str(p, " MS");
}

static void fmt_release(char *t, int32_t q)
{
    int32_t v = release_ms[(q * 64 + 16384) >> 15];
    char *p;
    if (v >= 1000) {
        p = put_num(t, v / 1000);
        *p++ = '.';
        *p++ = (char)('0' + (v % 1000) / 100);
        put_str(p, " S");
    } else {
        p = put_num(t, v);
        put_str(p, " MS");
    }
}

static void fmt_gr(char *t, int32_t gain)
{
    int32_t x = 3840 - log2_q8((uint32_t)gain);         /* 0 dBFS amplitude = 3840 */
    if (x < 0) x = 0;
    if (x > 3840) x = 3840;
    int32_t db10 = (x * 60 + 128) >> 8;                 /* 6.02 dB per octave, tenths */
    char *p = put_str(t, "GR ");
    if (db10 > 0) *p++ = '-';
    p = put_num(p, db10 / 10);
    *p++ = '.';
    *p++ = (char)('0' + db10 % 10);
    put_str(p, " DB");
}

static void fmt_auto(char *t, int32_t makeup)
{
    int32_t x = log2_q8((uint32_t)makeup);
    if (x < 3840) x = 3840;
    int32_t db10 = ((x - 3840) * 60 + 128) >> 8;
    if (db10 > 60) db10 = 60;
    char *p = put_str(t, "AUTO ");
    if (db10 > 0) *p++ = '+';
    p = put_num(p, db10 / 10);
    *p++ = '.';
    *p++ = (char)('0' + db10 % 10);
    put_str(p, " DB");
}

static void comp_screen_init(void *scr) { (void)scr; }

static void comp_screen_update(void *scr, int dt)
{
    int pos = op1_fx_screen_pos() % 3;
    comp_t *s = &st[pos];
    int16_t k[4];
    op1_fx_knobs_live(k);
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    char t[24];

    /* title and detector type */
    sq_text(r, Q(12), Q(8), 2560, "COMPACTOR", 0x800, c_white);
    int32_t x = Q(308) - sq_width(1536, "RMS");
    sq_text(r, x, Q(10), 1536, "RMS", 0x500, c_grey);

    /* four knob labels and values */
    for (int i = 0; i < 4; i++) {
        int32_t kx = Q(k_x[i]);
        sq_text(r, kx, Q(40), 1536, k_lab[i], 0x500, k_col[i]);
        switch (i) {
        case 0: fmt_ratio(t, k[0]); break;
        case 1: fmt_db(t, -60 + ((k[1] * 60 + 16384) >> 15)); break;
        case 2: fmt_attack(t, k[2]); break;
        default: fmt_release(t, k[3]); break;
        }
        sq_text(r, kx, Q(56), 1792, t, 0x600, c_white);   /* 1.75 px: widest value "-60 DB" still clears the next column */

        /* knob position bar */
        int32_t v = k[i] < 0 ? 0 : k[i];
        int32_t w = Q(64) * v >> 15;
        sq_line(r, kx, Q(82), kx + Q(64), Q(82), 0x400, c_dim);
        if (w > Q(1)) sq_line(r, kx, Q(82), kx + w, Q(82), Q(3), k_col[i]);
        sq_line(r, kx + w, Q(78), kx + w, Q(86), 0x400, c_white);
    }

    /* gain reduction and auto make-up */
    int32_t gr_gain = s->gain > 0 ? s->gain : 32768;      /* before the first init: show 0 dB */
    int32_t auto_makeup = s->makeup > 0 ? s->makeup : 32768;
    fmt_gr(t, gr_gain);
    sq_text(r, Q(12), Q(114), 1536, t, 0x500, c_red);
    fmt_auto(t, auto_makeup);
    x = Q(308) - sq_width(1536, t);
    sq_text(r, x, Q(114), 1536, t, 0x500, auto_makeup > 32768 ? c_green : c_grey);
}

#pragma GCC pop_options
const op1_screen_t SCREEN_compactor = { comp_screen_init, comp_screen_update };

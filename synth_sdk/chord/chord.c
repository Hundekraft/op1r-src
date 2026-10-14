/* Chord mode (docs/chord-mod.md in hundekraft's OP1-Emulator repo): every poly synth note becomes a chord in a
 * chosen key and scale. Degree mode: the pressed note is the bass of its chord (shift: dominant 7th, iv <-> IV on
 * the 4th, half-diminished on #4). Step mode: each press plays the next chord of a progression, bass near the
 * pressed note, shift goes back one. Voice leading keeps the upper notes close to the last chord; per-note counts
 * keep shared notes of overlapping chords sounding. Page: clock (Shift + help), then T4 (dispatcher.c). */
#include "../op1_synth.h"
#include "../screens/stroke_font.h"

typedef struct { int32_t note, velocity; uint8_t send_midi, pad[3]; int32_t extra; } ch_note;
#ifdef OP1_HOST
extern void (*host_vc_note_on)(const ch_note *), (*host_vc_note_off)(const ch_note *);
extern int host_play_mode, host_shift;
#define VC_NOTE_ON(n)   host_vc_note_on(n)
#define VC_NOTE_OFF(n)  host_vc_note_off(n)
#define CH_PLAY_MODE()  host_play_mode
#define CH_SHIFT        host_shift
#else
#define VC_NOTE_ON(n)   (((void (*)(const ch_note *))0x01A84604)(n))   /* voicectl_synth_note_on */
#define VC_NOTE_OFF(n)  (((void (*)(const ch_note *))0x01A85058)(n))   /* voicectl_synth_note_off */
/* play mode of the current synth slot, read as voicectl_synth_note_on does: adsr word 4 >> 12 (0 = poly) */
#define CH_PLAY_MODE()  (((const volatile int16_t *)(0x01B60EF0 + (*(volatile uint32_t *)0xFF8016D4 & 7) * 16))[4] >> 12)
#define CH_SHIFT        (*(volatile uint8_t *)0x01DD7F78)
#endif

#define CHORD_MAX 4
#define WINDOW 23                   /* upper voices stay within this many semitones above the bass */

/* Intervals above the bass; root = semitones from the bass up to the chord root (slash chords), name suffix. */
typedef struct { uint8_t n, iv[CHORD_MAX]; uint8_t root; const char *suffix; } Shape;

/* Chords by semitone above the key root, per scale. In-scale keys play their diatonic triad; out-of-scale keys
 * play the most common chord on that bass: borrowed chords, or a slash chord (C/E, F/A, G/B, D/F#: intervals
 * 0, 3, 8) whose bass is the pressed note. */
enum { MAJOR, MINOR, DORIAN, MIXOLYDIAN, HAND_COUNT,       /* hand-tuned tables below */
       LYDIAN = HAND_COUNT, PHRYGIAN, LOCRIAN, HARM_MINOR, MEL_MINOR, PHRYG_DOM, MAJ_PENT, MIN_PENT, BLUES,
       SCALE_COUNT };
#define MAJ3 {3, {0, 4, 7}, 0, ""}
#define MIN3 {3, {0, 3, 7}, 0, "m"}
#define DIM3 {3, {0, 3, 6}, 0, "dim"}
#define SLASH {3, {0, 3, 8}, 8, ""}
static const Shape scale_shapes[HAND_COUNT][12] = {
    /*          C     Db    D     Eb    E      F     F#                   G     Ab    A      Bb    B */
    [MAJOR] = {MAJ3, MAJ3, MIN3, MAJ3, MIN3,  MAJ3, SLASH,               MAJ3, MAJ3, MIN3,  MAJ3, DIM3},
    [MINOR] = {MIN3, MAJ3, DIM3, MAJ3, SLASH, MIN3, {4, {0, 3, 6, 9}, 0, "dim7"}, MIN3, MAJ3, SLASH, MAJ3, SLASH},
    [DORIAN] = {MIN3, MAJ3, MIN3, MAJ3, SLASH, MAJ3, SLASH,              MIN3, MAJ3, DIM3,  MAJ3, SLASH},
    [MIXOLYDIAN] = {MAJ3, MAJ3, MIN3, MAJ3, DIM3, MAJ3, SLASH,           MIN3, MAJ3, MIN3,  MAJ3, SLASH},
};
/* minor: C/E = Picardy I, F#dim7 = vii7/V, F/A = dorian IV, G/B = V; major and modes: D/F# = V/V, G/B = V */

/* Shift + key in degree mode: dominant 7th on the pressed root (secondary, tritone-sub and backdoor dominants),
 * except on the 4th, which flips the triad's third (iv <-> IV), and the #4, which gives half-diminished. */
static const Shape DOM7_SHIFT = {4, {0, 4, 7, 10}, 0, "7"}, HALF_DIM = {4, {0, 3, 6, 10}, 0, "m7b5"};
static const Shape FOURTH_MAJ = MAJ3, FOURTH_MIN = MIN3;
static const Shape *shift_shape(int pc, const Shape *normal) {
    if (pc == 5) return normal->iv[1] == 3 ? &FOURTH_MAJ : &FOURTH_MIN;
    return pc == 6 ? &HALF_DIM : &DOM7_SHIFT;
}

/* bit n = n semitones above the root, per scale (keyboard dots and the generated tables) */
static const uint16_t scale_masks[SCALE_COUNT] = {0xab5, 0x5ad, 0x6ad, 0x6b5,          /* major minor dorian mixo */
    0xad5, 0x5ab, 0x56b, 0x9ad, 0xaad, 0x5b3,                                          /* lyd phr loc harm mel phrdom */
    0x295, 0x4a9, 0x4e9};                                                              /* maj pent, min pent, blues */

/* The other scales, generated from their notes: an in-scale key plays the chord stacked from every other scale
 * note (thirds in the seven-note scales, quartal/sus colours in the pentatonics), named by recognising it; an
 * out-of-scale key plays a borrowed major triad. */
static Shape gen_shapes[SCALE_COUNT - HAND_COUNT][12];
static int gen_ready;
static const struct { uint8_t a, b; const char *suffix; } triads[] = {
    {4, 7, ""}, {3, 7, "m"}, {3, 6, "dim"}, {4, 8, "aug"}, {2, 7, "sus2"}, {5, 7, "sus4"}};
static void name_shape(Shape *sh)                           /* root = the note that makes a known triad, bass first */
{
    for (int c = 0; c < 3; c++) {
        int r = c ? sh->iv[c] : 0, x = (sh->iv[(c + 1) % 3] - r + 12) % 12, y = (sh->iv[(c + 2) % 3] - r + 12) % 12;
        if (x > y) { int t = x; x = y; y = t; }
        for (unsigned k = 0; k < sizeof triads / sizeof triads[0]; k++)
            if (triads[k].a == x && triads[k].b == y) { sh->root = (uint8_t)r; sh->suffix = triads[k].suffix; return; }
    }
    sh->root = 0; sh->suffix = "";
}
static void gen_tables(void)
{
    for (int sc = HAND_COUNT; sc < SCALE_COUNT; sc++) {
        int notes[12], n = 0;
        for (int pc = 0; pc < 12; pc++) if (scale_masks[sc] >> pc & 1) notes[n++] = pc;
        for (int pc = 0; pc < 12; pc++) {
            Shape *sh = &gen_shapes[sc - HAND_COUNT][pc];
            sh->n = 3; sh->iv[0] = 0;
            int d = -1;
            for (int i = 0; i < n; i++) if (notes[i] == pc) d = i;
            if (d < 0) { sh->iv[1] = 4; sh->iv[2] = 7; sh->root = 0; sh->suffix = ""; continue; }
            int a = (notes[(d + 2) % n] - pc + 12) % 12, b = (notes[(d + 4) % n] - pc + 12) % 12;
            sh->iv[1] = (uint8_t)(a < b ? a : b); sh->iv[2] = (uint8_t)(a < b ? b : a);
            name_shape(sh);
        }
    }
    gen_ready = 1;
}
static const Shape *scale_shape(int sc, int pc)
{
    sc = (unsigned)sc < SCALE_COUNT ? sc : MAJOR;
    if (sc < HAND_COUNT) return &scale_shapes[sc][pc];
    if (!gen_ready) gen_tables();
    return &gen_shapes[sc - HAND_COUNT][pc];
}

/* Root-position qualities for progressions */
static const Shape MAJ = MAJ3, MIN = MIN3, DOM7 = {4, {0, 4, 7, 10}, 0, "7"},
                   MAJ7 = {4, {0, 4, 7, 11}, 0, "maj7"}, MIN7 = {4, {0, 3, 7, 10}, 0, "m7"};
typedef struct { uint8_t root; const Shape *shape; } Step;      /* bass in semitones above the key root */
typedef struct { uint8_t n; Step step[16]; } Progression;
static const Progression progressions[] = {
    {4, {{0, &MAJ}, {7, &MAJ}, {9, &MIN}, {5, &MAJ}}},                     /* pop I-V-vi-IV */
    {4, {{0, &MAJ}, {9, &MIN}, {5, &MAJ}, {7, &MAJ}}},                     /* 50s I-vi-IV-V */
    {3, {{2, &MIN7}, {7, &DOM7}, {0, &MAJ7}}},                             /* jazz ii7-V7-Imaj7 */
    {4, {{0, &MIN}, {10, &MAJ}, {8, &MAJ}, {7, &MAJ}}},                    /* Andalusian i-bVII-bVI-V */
    {12, {{0, &DOM7}, {0, &DOM7}, {0, &DOM7}, {0, &DOM7}, {5, &DOM7}, {5, &DOM7},
          {0, &DOM7}, {0, &DOM7}, {7, &DOM7}, {5, &DOM7}, {0, &DOM7}, {7, &DOM7}}},   /* 12-bar blues */
    {8, {{0, &MAJ}, {7, &MAJ}, {9, &MIN}, {4, &MIN}, {5, &MAJ}, {0, &MAJ}, {5, &MAJ}, {7, &MAJ}}},   /* Canon */
    {4, {{9, &MIN}, {5, &MAJ}, {0, &MAJ}, {7, &MAJ}}},                     /* sad vi-IV-I-V */
    {4, {{5, &MAJ}, {7, &MAJ}, {4, &MIN}, {9, &MIN}}},                     /* royal road IV-V-iii-vi */
    {4, {{0, &MAJ}, {10, &MAJ}, {5, &MAJ}, {0, &MAJ}}},                    /* rock I-bVII-IV-I */
    {4, {{9, &MIN}, {2, &MIN}, {7, &MAJ}, {0, &MAJ}}},                     /* circle vi-ii-V-I */
    {4, {{0, &MAJ7}, {9, &MIN7}, {2, &MIN7}, {7, &DOM7}}},                 /* turnaround Imaj7-vi7-ii7-V7 */
    {4, {{0, &MIN}, {8, &MAJ}, {3, &MAJ}, {10, &MAJ}}},                    /* epic i-bVI-bIII-bVII */
    {4, {{0, &MAJ}, {4, &MAJ}, {5, &MAJ}, {5, &MIN}}},                     /* creep I-III-IV-iv */
    {3, {{5, &MAJ}, {5, &MIN}, {0, &MAJ}}},                                /* soul IV-iv-I */
    {3, {{2, &HALF_DIM}, {7, &DOM7}, {0, &MIN}}},                          /* minor ii-V-i */
    {4, {{0, &MAJ}, {2, &MAJ}, {5, &MAJ}, {0, &MAJ}}},                     /* dreamy (lydian) I-II-IV-I */
};
#define PROGRESSION_COUNT (int)(sizeof progressions / sizeof progressions[0])
_Static_assert(sizeof progressions / sizeof progressions[0] <= 16, "progression index is 4 bits in the saved row");

enum { CHORD_DEGREE = 1, CHORD_STEP = 2 };
/* Settings (the T4 page writes them, op1rsettings keeps them). Chords are off until pushed on (owner). */
int chord_on, chord_mode = CHORD_DEGREE, chord_root, chord_scale, chord_progression, chord_voice_leading = 1;
int chord_colour;                   /* degree chords: 0 triad, 1 sus4, 2 sus2, 3 7th (from the scale), 4 add9 */
#define MINE PROGRESSION_COUNT      /* the recorded progression, after the built-in ones */

/* the recorded progression ("mine"): degree chords played while recording (push white in degree mode) */
static Shape rec_shape[16];
static Progression rec_prog;
int chord_rec_on;
static int rec_dirty;
static int clamp(int v, int n);
static const Progression *prog(int i) { return i == MINE ? &rec_prog : &progressions[clamp(i, PROGRESSION_COUNT)]; }

/* the chord colour applied to a degree chord (rk = its root in semitones above the key root). Sus leaves slash
 * chords alone: their bass is the third that sus would remove. */
static const char *const SUFFIXES[] = {"", "m", "dim", "aug", "sus2", "sus4", "7", "maj7", "m7", "m7b5", "dim7",
                                       "mmaj7", "add9", "madd9", "aug7", "augmaj7", "dimadd9", "augadd9"};
#define SUFFIX_COUNT (int)(sizeof SUFFIXES / sizeof SUFFIXES[0])
static const Shape *coloured(const Shape *base, int rk, Shape *out)
{
    if (!chord_colour || base->n != 3) return base;
    int R = base->root, third = 0, fifth = 7, set[4], n = 0;
    for (int i = 0; i < 3; i++) {
        int x = (base->iv[i] - R + 12) % 12;
        if (x == 3 || x == 4) third = x;
        if (x == 6 || x == 7 || x == 8) fifth = x;
    }
    if (chord_colour <= 2 && R) return base;
    set[n++] = 0;
    const char *suf;
    if (chord_colour <= 2) {                                /* sus4 / sus2: the third becomes the 4th or 2nd */
        set[n++] = chord_colour == 1 ? 5 : 2; set[n++] = 7;
        suf = SUFFIXES[chord_colour == 1 ? 5 : 4];
    } else {
        int q = third == 4 ? (fifth == 8 ? 3 : 0) : (fifth == 6 ? 2 : 1);   /* 0 maj, 1 min, 2 dim, 3 aug */
        set[n++] = third ? third : 4; set[n++] = fifth;
        if (chord_colour == 3) {                            /* the 7th the scale has on this root, else b7 */
            int mask = scale_masks[clamp(chord_scale, SCALE_COUNT)], inscale = mask >> (rk % 12) & 1;
            int sev = inscale && (mask >> ((rk + 11) % 12) & 1) ? 11 : inscale && !(mask >> ((rk + 10) % 12) & 1) && q == 2 ? 9 : 10;
            set[n++] = sev;
            static const uint8_t names[4][3] = {{7, 6, 6}, {11, 8, 8}, {9, 9, 10}, {15, 14, 14}};   /* [q][11, 10, 9] */
            suf = SUFFIXES[names[q][sev == 11 ? 0 : sev == 10 ? 1 : 2]];
        } else {                                            /* add9 */
            set[n++] = 2;
            static const uint8_t names[4] = {12, 13, 16, 17};
            suf = SUFFIXES[names[q]];
        }
    }
    for (int i = 0; i < n; i++) set[i] = (set[i] + R) % 12;      /* back to intervals above the bass */
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && set[j] < set[j - 1]; j--) { int t = set[j]; set[j] = set[j - 1]; set[j - 1] = t; }
    out->n = (uint8_t)n;
    for (int i = 0; i < n; i++) out->iv[i] = (uint8_t)set[i];
    out->root = (uint8_t)R; out->suffix = suf;
    return out;
}

static int step_pos, step_progression;                    /* next chord in step mode, of this progression */
static struct { int bass, n; const Shape *shape; int step; uint8_t tone[CHORD_MAX]; } last = {-1, 0, 0, -1, {0}};   /* page */
static Shape last_shape, colour_buf;
static void shape_copy(Shape *d, const Shape *src)          /* field by field: op1r links without memcpy */
{
    d->n = src->n; d->root = src->root; d->suffix = src->suffix;
    for (int i = 0; i < CHORD_MAX; i++) d->iv[i] = src->iv[i];
}
static struct { uint8_t n, tone[CHORD_MAX]; } held[128];   /* by pressed note; n = 0: not held */
static uint8_t count[128];                                 /* held chords using each sounding tone */
static int prev_upper[CHORD_MAX - 1], prev_n;              /* last chord's upper voices, for voice leading */

static int iabs(int x) { return x < 0 ? -x : x; }

/* Distance of a candidate voicing to the previous one: each voice to its nearest previous voice. */
static int distance(const int *v, int n) {
    int sum = 0;
    for (int i = 0; i < n; ++i) {
        int best = 128;
        for (int j = 0; j < prev_n; ++j)
            if (iabs(v[i] - prev_upper[j]) < best) best = iabs(v[i] - prev_upper[j]);
        sum += best;
    }
    return sum;
}

/* Chord tones: bass first, then the upper voices (the inversion closest to the last chord). */
static int voice(int note, const Shape *shape, uint8_t *tone) {
    int pcs[CHORD_MAX - 1], n = shape->n - 1;
    for (int i = 0; i < n; ++i) pcs[i] = shape->iv[i + 1];   /* ascending, 1..11 */
    int best[CHORD_MAX - 1], best_cost = -1;
    for (int r = 0; r < n; ++r) {                            /* rotation r: pcs[r] lowest, the rest stacked above */
        int v[CHORD_MAX - 1];
        for (int i = 0; i < n; ++i) v[i] = note + pcs[(r + i) % n] + (r + i >= n ? 12 : 0);
        if (v[n - 1] - note > WINDOW) continue;
        int cost = prev_n && chord_voice_leading ? distance(v, n) : r;   /* else root position */
        if (best_cost < 0 || cost < best_cost) {
            best_cost = cost;
            for (int i = 0; i < n; ++i) best[i] = v[i];
        }
    }
    int count_out = 0;
    tone[count_out++] = (uint8_t)note;
    for (int i = 0; i < n; ++i)
        if (best[i] <= 127) tone[count_out++] = (uint8_t)best[i];
    prev_n = count_out - 1;
    for (int i = 0; i < prev_n; ++i) prev_upper[i] = tone[i + 1];
    return count_out;
}

static int shift_held(void) { return CH_SHIFT; }

/* Next (or, with shift, previous) progression chord, its root in the octave nearest the pressed note. */
static int step(int note, uint8_t *tone) {
    const Progression *p = prog(chord_progression);
    if (chord_progression != step_progression || step_pos >= p->n) step_pos = 0;   /* a new progression starts at 1 */
    step_progression = chord_progression;
    if (shift_held())
        step_pos = (step_pos + 2 * p->n - 2) % p->n;            /* the chord before the last one played */
    const Step *s = &p->step[step_pos];
    last.step = step_pos;
    step_pos = (step_pos + 1) % p->n;
    int bass = note - (note - chord_root - s->root + 1200) % 12;  /* chord root at or below the pressed note */
    if (note - bass > 6) bass += 12;                            /* nearest octave, ties go down */
    if (bass > 127) bass -= 12;
    last.shape = s->shape;
    last.bass = bass;
    return voice(bass, s->shape, tone);
}

/* VoiceCtl key_down / key_up pass a 16-byte note record to voicectl_synth_note_on / _note_off (R0); the hooks
 * (make_firmware.py HOOKS 0x01A85828 / 0x01A858D0) send it here instead. Keyboard, MIDI in and every sequencer
 * reach the synth through these two calls. */
void chord_note_on(const ch_note *note)
{
    int n = note->note;
    if (n < 0 || n > 127 || held[n].n) { VC_NOTE_ON(note); return; }   /* out of range or already held: stock */
    uint8_t *tone = held[n].tone;
    if (!chord_on || CH_PLAY_MODE() != 0) { tone[0] = (uint8_t)n; held[n].n = 1; }
    else {
        if (chord_mode == CHORD_STEP && prog(chord_progression)->n) held[n].n = (uint8_t)step(n, tone);
        else {
            int pc = (n - chord_root + 1200) % 12;
            const Shape *shape = scale_shape(chord_scale, pc);
            shape = shift_held() ? shift_shape(pc, shape) : coloured(shape, (pc + shape->root) % 12, &colour_buf);
            shape_copy(&last_shape, shape);
            last.shape = &last_shape;
            last.bass = n;
            held[n].n = (uint8_t)voice(n, &last_shape, tone);
            if (chord_rec_on && rec_prog.n < 16) {          /* recording: append this chord (bass, shape) */
                shape_copy(&rec_shape[rec_prog.n], &last_shape);
                rec_prog.step[rec_prog.n].root = (uint8_t)pc;
                rec_prog.step[rec_prog.n].shape = &rec_shape[rec_prog.n];
                last.step = rec_prog.n++;
            }
        }
        last.n = held[n].n;                                 /* the voiced notes, for the page's keyboard */
        for (int i = 0; i < last.n; i++) last.tone[i] = tone[i];
    }
    ch_note copy = *note;
    for (int i = 0; i < held[n].n; i++)
        if (count[tone[i]]++ == 0) { copy.note = tone[i]; VC_NOTE_ON(&copy); }
}

void chord_note_off(const ch_note *note)
{
    int n = note->note;
    if (n < 0 || n > 127 || !held[n].n) { VC_NOTE_OFF(note); return; }
    ch_note copy = *note;
    for (int i = 0; i < held[n].n; i++)
        if (--count[held[n].tone[i]] == 0) { copy.note = held[n].tone[i]; VC_NOTE_OFF(&copy); }
    held[n].n = 0;
}

/* ---- T4 page on the clock screen (dispatcher.c routes T4, the pushes and the turns while it is shown) ----
 * Turn blue root, green scale, white progression, orange mode (degree / step). Push green voice leading on/off,
 * push white restart the progression, push orange chords on/off. */
#define DETENT 1024                 /* encoder delta per detent (as settings.c) */
static int wrap(int v, int n) { return ((v % n) + n) % n; }
static int clamp(int v, int n) { return v < 0 ? 0 : v >= n ? n - 1 : v; }

/* saved settings: one packed row in op1rsettings (as settings.c). Bits 0-3 mode, 4-7 root, 8-11 scale,
 * 12-15 progression, 16 voice leading, 17 off, 18 mine, 19-21 colour, 24 marker; the recording: key chordrec. Loaded at boot (screen init), written on change. */
#define CH_MARK (1 << 24)
static int32_t ch_pack(void)
{
    return CH_MARK | (chord_mode == CHORD_STEP ? CHORD_STEP : CHORD_DEGREE) | wrap(chord_root, 12) << 4 |
           clamp(chord_scale, SCALE_COUNT) << 8 | (chord_progression == MINE ? 0 : clamp(chord_progression, PROGRESSION_COUNT)) << 12 |
           !!chord_voice_leading << 16 | !chord_on << 17 | (chord_progression == MINE) << 18 | clamp(chord_colour, 5) << 19;
}
static void ch_unpack(int32_t v)
{
    if ((v & ~0x3fffff) != CH_MARK) return;
    chord_on = !(v >> 17 & 1);
    chord_mode = (v & 15) == CHORD_STEP ? CHORD_STEP : CHORD_DEGREE;
    chord_root = (v >> 4 & 15) % 12;
    chord_scale = clamp(v >> 8 & 15, SCALE_COUNT);
    chord_progression = v >> 18 & 1 ? MINE : clamp(v >> 12 & 15, PROGRESSION_COUNT);
    chord_colour = clamp(v >> 19 & 7, 5);
    chord_voice_leading = v >> 16 & 1;
}
#ifdef OP1_HOST
int32_t chord_host_saved = -1;
char chord_host_rec[128];
static void ch_load(void) { if (chord_host_saved >= 0) ch_unpack(chord_host_saved); }
static void ch_save(int32_t v) { chord_host_saved = v; }
static void rec_get(char *buf, int size) { int i = 0; for (; chord_host_rec[i] && i < size - 1; i++) buf[i] = chord_host_rec[i]; buf[i] = 0; }
static void rec_put(const char *str) { int i = 0; for (; str[i] && i < 127; i++) chord_host_rec[i] = str[i]; chord_host_rec[i] = 0; }
#else
#define DB_HANDLE ((void *(*)(void))0x0196DD44)
#define DB_EXEC   ((void (*)(void *, const char *))0x0196F950)
#define DB_GET    ((void (*)(void *, void *, const char *))0x0196FBD0)
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } ch_dstr;
static const char CH_CREATE[] = "CREATE TABLE IF NOT EXISTS op1rsettings (key TEXT PRIMARY KEY, val INTEGER NOT NULL)";
static void ch_load(void)
{
    void *db = DB_HANDLE();
    DB_EXEC(db, CH_CREATE);
    ch_dstr s = { 0 };
    DB_GET(&s, db, "SELECT val FROM op1rsettings WHERE key='chord'");
    const char *p = s.cap > 15 ? s.u.ptr : s.u.buf;
    int32_t v = 0;
    for (uint32_t i = 0; i < s.size && p[i] >= '0' && p[i] <= '9'; i++) v = v * 10 + (p[i] - '0');
    if (s.size) ch_unpack(v);
}
static void rec_get(char *buf, int size)                    /* the recording as text (op1rsettings is flexibly typed) */
{
    void *db = DB_HANDLE();
    DB_EXEC(db, CH_CREATE);
    ch_dstr s = { 0 };
    DB_GET(&s, db, "SELECT val FROM op1rsettings WHERE key='chordrec'");
    const char *p = s.cap > 15 ? s.u.ptr : s.u.buf;
    int i = 0;
    for (; (uint32_t)i < s.size && i < size - 1; i++) buf[i] = p[i];
    buf[i] = 0;
}
static void rec_put(const char *str)
{
    static const char head[] = "INSERT OR REPLACE INTO op1rsettings VALUES ('chordrec','";
    char q[160];
    int n = 0;
    for (; head[n]; n++) q[n] = head[n];
    for (int i = 0; str[i] && n < 156; i++) q[n++] = str[i];
    q[n++] = '\''; q[n++] = ')'; q[n] = 0;
    void *db = DB_HANDLE();
    DB_EXEC(db, CH_CREATE);
    DB_EXEC(db, q);
}
static void ch_save(int32_t v)
{
    char q[] = "INSERT OR REPLACE INTO op1rsettings VALUES ('chord',0000000000)", d[10];
    char *p = q + sizeof q - 12;                            /* the ten digit places */
    int n = 0;
    do d[n++] = (char)('0' + v % 10); while (v /= 10);
    for (int i = 0; i < 10; i++) p[i] = i < 10 - n ? ' ' : d[9 - i];
    void *db = DB_HANDLE();
    DB_EXEC(db, CH_CREATE);
    DB_EXEC(db, q);
}
#endif
/* recording as text: 'r', then 6 characters per chord: bass, the three upper intervals (0 = none) and the root
 * offset in hex, the suffix as 'a' + index (always a letter, so SQLite keeps it as text) */
static const char HEX[] = "0123456789abcdef";
static int unhex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
static void rec_save(void)
{
    char t[1 + 16 * 6 + 1];
    int k = 0;
    t[k++] = 'r';
    for (int i = 0; i < rec_prog.n; i++) {
        const Shape *sh = &rec_shape[i];
        int sx = 0;
        while (sx < SUFFIX_COUNT - 1 && SUFFIXES[sx] != sh->suffix) sx++;
        t[k++] = HEX[rec_prog.step[i].root % 12];
        for (int j = 1; j < 4; j++) t[k++] = HEX[j < sh->n ? sh->iv[j] : 0];
        t[k++] = HEX[sh->root % 12];
        t[k++] = (char)('a' + sx);
    }
    t[k] = 0;
    rec_put(t);
}
static void rec_load(void)
{
    char t[1 + 16 * 6 + 1];
    rec_get(t, sizeof t);
    rec_prog.n = 0;
    if (t[0] != 'r') return;
    for (int i = 0; i < 16; i++) {
        const char *c = t + 1 + 6 * i;
        int b = unhex(c[0]), a1 = unhex(c[1]), a2 = unhex(c[2]), a3 = unhex(c[3]), ro = unhex(c[4]), sx = c[5] - 'a';
        if (b < 0 || a1 <= 0 || a2 <= a1 || a3 < 0 || ro < 0 || sx < 0 || sx >= SUFFIX_COUNT || b > 11 || a2 > 11 || a3 > 11) break;
        Shape *sh = &rec_shape[i];
        sh->iv[0] = 0; sh->iv[1] = (uint8_t)a1; sh->iv[2] = (uint8_t)a2; sh->iv[3] = (uint8_t)a3;
        sh->n = a3 > a2 ? 4 : 3; sh->root = (uint8_t)ro; sh->suffix = SUFFIXES[sx];
        rec_prog.step[i].root = (uint8_t)b; rec_prog.step[i].shape = sh;
        rec_prog.n = (uint8_t)(i + 1);
    }
}

static int32_t ch_saved = -1;
void chord_sync(void)                                       /* KeyHandler thread: write once per change */
{
    int32_t v = ch_pack();
    if (ch_saved >= 0 && v != ch_saved) { ch_saved = v; ch_save(v); }
    if (rec_dirty) { rec_dirty = 0; rec_save(); }
}

void chord_push(int knob)                                   /* encoder push 0..3 while the page is shown */
{
    if (knob == 0) chord_colour = (chord_colour + 1) % 5;   /* blue: triad, sus4, sus2, 7th, add9 */
    else if (knob == 1) chord_voice_leading = !chord_voice_leading;
    else if (knob == 2) {
        if (chord_mode == CHORD_STEP && !chord_rec_on) step_pos = 0;   /* step mode: restart the progression */
        else if (chord_rec_on) {                            /* stop: the recording becomes "mine" */
            chord_rec_on = 0;
            if (rec_prog.n) { chord_progression = MINE; step_pos = 0; }
            rec_dirty = 1;
        } else if (CH_SHIFT) { rec_prog.n = 0; rec_dirty = 1; }        /* shift + white: clear */
        else { chord_rec_on = 1; rec_prog.n = 0; last.step = -1; }     /* degree mode: record */
    }
    else if (knob == 3) chord_on = !chord_on;
}

static int32_t knob_acc[4];
void chord_turn(int k, int delta)                           /* one value per detent, reset on a direction change */
{
    if (k < 0 || k > 3) return;
    if ((knob_acc[k] > 0 && delta < 0) || (knob_acc[k] < 0 && delta > 0)) knob_acc[k] = 0;
    knob_acc[k] += delta;
    if (knob_acc[k] > -DETENT && knob_acc[k] < DETENT) return;
    int st = knob_acc[k] > 0 ? 1 : -1;
    knob_acc[k] = 0;
    if (k == 0) chord_root = wrap(chord_root + st, 12);
    else if (k == 1) chord_scale = clamp(chord_scale + st, SCALE_COUNT);
    else if (k == 2) chord_progression = clamp(chord_progression + st, PROGRESSION_COUNT + 1);   /* + mine */
    else chord_mode = st > 0 ? CHORD_STEP : CHORD_DEGREE;
}

/* ---- drawing: layout B (owner, 2026-10-14) within the screen rules of hundekraft's engine-screen spec: art in
 * y 14..138; blue, green, white, red in the encoder lanes x 40/120/200/280; knob parts 2 px in full colour; scene
 * #7C7990/#383572, highlight #DFD9FF. Words in the firmware font (Text objects), the rest in raster lines. ---- */
static const char *const flat_names[12] = {"c", "db", "d", "eb", "e", "f", "f#", "g", "ab", "a", "bb", "b"};
static const char *const sharp_names[12] = {"c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b"};
static const char *const *note_names = flat_names;
static const char *const scale_names[SCALE_COUNT] = {"major", "minor", "dorian", "mixo", "lydian", "phryg",   /* <= 7 */
    "locrian", "h.minor", "m.minor", "phr.dom", "pent", "m.pent", "blues"};    /* characters: the lane is 70 px */
static const char *const progression_names[PROGRESSION_COUNT] = {"pop", "50s", "jazz", "andalus", "blues", "canon",
    "sad", "royal", "rock", "circle", "turn", "epic", "creep", "soul", "min251", "dreamy"};
static const char *const colour_labels[5] = {"", "SUS4", "SUS2", "7TH", "ADD9"};
static const char *const mode_names[3] = {"", "degree", "step"};
static const uint8_t c_black[4] = {0, 0, 0, 255}, c_text[4] = {0xdf, 0xd9, 0xff, 255},
    c_lavender[4] = {0xae, 0xb1, 0xdc, 255}, c_dark[4] = {0x38, 0x35, 0x72, 255}, c_grey[4] = {0x7c, 0x79, 0x90, 255},
    c_blue[4] = {0x69, 0x8e, 0xff, 255}, c_green[4] = {0x00, 0xed, 0x95, 255}, c_white[4] = {0xff, 0xff, 0xff, 255},
    c_red[4] = {0xff, 0x3a, 0x5d, 255}, c_fill[4] = {0x01, 0x01, 0x01, 255},
    /* the knob colours at about 40 %: a setting that does not apply keeps its knob's colour, dimmed (owner) */
    c_blue_dim[4] = {0x2a, 0x39, 0x66, 255}, c_green_dim[4] = {0x00, 0x5f, 0x3c, 255},
    c_white_dim[4] = {0x66, 0x66, 0x66, 255}, c_red_dim[4] = {0x66, 0x17, 0x25, 255};
#define STROKE 0x600                /* 1.5 px, the stock screens' stroke width */
#define PART 0x800                  /* 2 px knob parts */
static const int32_t ident[9] = {1024, 0, 0, 0, 1024, 0, 0, 0, 1024};

/* Text objects: x Q10, y px top; +0x60 alignment (1 = centred on x), as screens/oblique.c */
#ifdef OP1_HOST
void host_text_align(void *t, int a);
#define T_SET_ALIGN(t, a) host_text_align((t), (a))
#else
#define T_SET_ALIGN(t, a) (*(int32_t *)((uint8_t *)(t) + 0x60) = (a))
#endif
enum { T_TITLE, T_NAME, T_MSG, T_OFF, T_VAL, T_VALD = T_VAL + 4, T_COUNT = T_VALD + 4 };
static uint32_t ch_txt[T_COUNT][OP1_TEXT_SIZE / 4];
static char ch_shown[T_COUNT][16];                          /* what each Text holds: set only on change */
static const uint8_t *const lane_col[4] = {c_blue, c_green, c_white, c_red};
static const uint8_t *const lane_dim[4] = {c_blue_dim, c_green_dim, c_white_dim, c_red_dim};

static int same(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static void tx(int i, const char *s, void *r)               /* GUI thread: op1_text_set reallocates */
{
    if (!same(ch_shown[i], s)) {
        int k = 0;
        for (; s[k] && k < 15; k++) ch_shown[i][k] = s[k];
        ch_shown[i][k] = 0;
        op1_text_set(ch_txt[i], ch_shown[i]);
    }
    op1_text_draw(ch_txt[i], r, ident);
}

static void line(void *r, int x0, int y0, int x1, int y1, int w, const uint8_t *col)
{
    if (!r) return;                 /* measuring pass */
    op1_pt a = {x0, y0}, b = {x1, y1};
    op1_raster_line(r, &a, &b, w, col);
}

static void append(char *dst, const char *src, int size)
{
    int n = 0;
    while (dst[n]) n++;
    while (*src && n < size - 1) dst[n++] = *src++;
    dst[n] = 0;
}

/* spelling by letter: the chord root by its scale degree (flats for the lowered borrowed notes, sharps for #4 and
 * the leading tone), a slash bass by its interval from the chord root (C/E, A/C#), so names read as written */
static const uint8_t letter_pc[7] = {0, 2, 4, 5, 7, 9, 11};                 /* C D E F G A B */
static const int8_t degree_letter[12] = {0, 1, 1, 2, 2, 3, 3, 4, 5, 5, 6, 6};   /* semitones above the key: steps */
static const int8_t bass_letter[12] = {0, 1, 1, 2, 2, 3, 4, 4, 4, 5, 6, 6};     /* above the chord root: steps */
static int key_letter(void) { static const char L[] = "cdefgab"; int i = 0; while (L[i] != note_names[wrap(chord_root, 12)][0]) i++; return i; }
static void spell(char *out, int size, int letter, int pc)
{
    char t[4] = {0};
    int acc = (pc - letter_pc[letter % 7] + 18) % 12 - 6, k = 1;   /* -2..2 */
    t[0] = "CDEFGAB"[letter % 7];
    for (; acc > 0; acc--) t[k++] = '#';
    for (; acc < 0; acc++) t[k++] = 'b';
    append(out, t, size);
}
static void chord_name(char *out, int size, int bass, const Shape *shape)
{
    int key = wrap(chord_root, 12), root = (bass + shape->root) % 12, rl = key_letter() + degree_letter[(root - key + 12) % 12];
    out[0] = 0;
    spell(out, size, rl, root);
    append(out, shape->suffix, size);
    if (shape->root) { append(out, "/", size); spell(out, size, rl + bass_letter[(bass - root + 120) % 12], bass % 12); }
}

/* chord names: capital roots, real lowercase suffixes (Am, F#m7b5, Cmaj7), drawn in strokes on a grid with the
 * cap top at 0, the x-height at 8, the baseline at 20 and descenders to 26 (owner: the OP-1 font has one case) */
static const int8_t name_pts[] = {
    3, 0, 20, 6, 0, 12, 20, 2, 2, 14, 10, 14, 0,   /* A */
    7, 0, 0, 0, 20, 8, 20, 11, 17, 11, 13, 8, 10, 0, 10, 5, 0, 0, 7, 0, 10, 3, 10, 7, 7, 10, 0,   /* B */
    8, 12, 3, 9, 0, 3, 0, 0, 3, 0, 17, 3, 20, 9, 20, 12, 17, 0,   /* C */
    7, 0, 0, 0, 20, 7, 20, 12, 15, 12, 5, 7, 0, 0, 0, 0,   /* D */
    4, 11, 0, 0, 0, 0, 20, 11, 20, 2, 0, 10, 8, 10, 0,   /* E */
    3, 11, 0, 0, 0, 0, 20, 2, 0, 10, 8, 10, 0,   /* F */
    10, 12, 3, 9, 0, 3, 0, 0, 3, 0, 17, 3, 20, 9, 20, 12, 17, 12, 11, 7, 11, 0,   /* G */
    2, 4, 5, 4, 20, 2, 9, 5, 9, 20, 2, 1, 9, 12, 9, 2, 1, 15, 12, 15, 0,   /* # */
    2, 10, 0, 0, 20, 0,   /* / */
    2, 0, 20, 0, 8, 5, 0, 11, 2, 8, 4, 8, 6, 11, 6, 20, 5, 6, 11, 8, 8, 10, 8, 12, 11, 12, 20, 0,   /* m */
    2, 10, 8, 10, 20, 8, 10, 11, 7, 8, 3, 8, 0, 11, 0, 17, 3, 20, 7, 20, 10, 17, 0,   /* a */
    4, 4, 8, 4, 23, 2, 26, 0, 26, 2, 4, 4, 4, 5, 0,   /* j */
    2, 10, 0, 10, 20, 8, 10, 11, 7, 8, 3, 8, 0, 11, 0, 17, 3, 20, 7, 20, 10, 17, 0,   /* d */
    2, 1, 8, 1, 20, 2, 1, 4, 1, 5, 0,   /* i */
    10, 10, 9, 7, 8, 3, 8, 0, 10, 0, 13, 10, 15, 10, 18, 7, 20, 3, 20, 0, 19, 0,   /* s */
    5, 0, 8, 0, 17, 3, 20, 7, 20, 10, 17, 2, 10, 8, 10, 20, 0,   /* u */
    5, 10, 8, 10, 23, 7, 26, 3, 26, 0, 24, 8, 10, 11, 7, 8, 3, 8, 0, 11, 0, 17, 3, 20, 7, 20, 10, 17, 0,   /* g */
    2, 0, 0, 0, 20, 8, 0, 11, 3, 8, 7, 8, 10, 11, 10, 17, 7, 20, 3, 20, 0, 17, 0,   /* b */
    7, 0, 3, 3, 0, 9, 0, 12, 3, 12, 7, 0, 20, 12, 20, 0,   /* 2 */
    4, 9, 20, 9, 0, 0, 14, 12, 14, 0,   /* 4 */
    9, 12, 0, 1, 0, 0, 9, 8, 8, 12, 11, 12, 17, 9, 20, 3, 20, 0, 17, 0,   /* 5 */
    12, 11, 1, 7, 0, 3, 0, 0, 4, 0, 17, 3, 20, 9, 20, 12, 17, 12, 13, 9, 10, 3, 10, 0, 13, 0,   /* 6 */
    3, 0, 0, 12, 0, 4, 20, 0,   /* 7 */
    11, 12, 7, 9, 10, 3, 10, 0, 7, 0, 3, 3, 0, 9, 0, 12, 3, 12, 16, 8, 20, 2, 20, 0,   /* 9 */
};
static const struct { char c; uint8_t w; uint16_t at; } name_glyphs[] = {
    {'A', 12, 0}, {'B', 11, 13}, {'C', 12, 40}, {'D', 12, 58}, {'E', 11, 74}, {'F', 11, 89}, {'G', 12, 102}, {'#', 12, 124}, {'/', 10, 145}, {'m', 12, 151}, {'a', 10, 179}, {'j', 5, 202}, {'d', 10, 217}, {'i', 2, 240}, {'s', 10, 251}, {'u', 10, 273}, {'g', 10, 290}, {'b', 10, 319}, {'2', 12, 342}, {'4', 12, 358}, {'5', 12, 368}, {'6', 12, 388}, {'7', 12, 414}, {'9', 12, 422}};
static int name_glyph(char c)
{
    for (unsigned k = 0; k < sizeof name_glyphs / sizeof name_glyphs[0]; k++) if (name_glyphs[k].c == c) return (int)k;
    return -1;
}
static int name_width(const char *t, int u)                  /* u = px per grid unit, Q10 */
{
    int w = 0;
    for (; *t; t++) { int g = name_glyph(*t); if (g >= 0) w += (name_glyphs[g].w + 4) * u; }
    return w ? w - 4 * u : 0;
}
static void name_text(void *r, int x, int y, int u, const char *t, const uint8_t *col)   /* x, y top-left, Q10 */
{
    for (; *t; t++) {
        int g = name_glyph(*t);
        if (g < 0) continue;
        const int8_t *p = name_pts + name_glyphs[g].at;
        for (int n; (n = *p++) != 0; p += 2 * n)
            for (int i = 1; i < n; i++)
                line(r, x + p[2 * i - 2] * u, y + p[2 * i - 1] * u, x + p[2 * i] * u, y + p[2 * i + 1] * u, 0x700, col);
        x += (name_glyphs[g].w + 4) * u;
    }
}

static void box(void *r, int x, int y, int w, int h, const uint8_t *col)
{
    line(r, x, y, x + w, y, STROKE, col); line(r, x + w, y, x + w, y + h, STROKE, col);
    line(r, x + w, y + h, x, y + h, STROKE, col); line(r, x, y + h, x, y, STROKE, col);
}

/* 16-gon; a filled disc is one ring at half the radius with a stroke as wide as the radius (AGG has a per-frame
 * block budget: a runaway stroke halts the firmware, "AGGgg.cpp:339 alloc failed, too many blocks") */
static const int16_t ring_x[17] = {1024, 946, 724, 392, 0, -392, -724, -946, -1024, -946, -724, -392, 0, 392, 724, 946, 1024};
static void ring(void *r, int cx, int cy, int rad, int w, const uint8_t *col)
{
    for (int i = 0; i < 16; i++) {
        int j = (i + 4) % 16, k = (i + 5) % 16;             /* sin = cos a quarter turn on */
        line(r, cx + ring_x[i] * (rad >> 10), cy + ring_x[j] * (rad >> 10),
             cx + ring_x[i + 1] * (rad >> 10), cy + ring_x[k] * (rad >> 10), w, col);
    }
}
static void circle(void *r, int cx, int cy, int rad, int filled, const uint8_t *col)
{
    if (filled) ring(r, cx, cy, rad / 2, rad + (1 << 9), col);
    else ring(r, cx, cy, rad, STROKE, col);
}

/* roman numeral for a chord root `semi` semitones above the key root: capitals for major and dominant, small
 * (dotted i) for minor and diminished, then flat/sharp, degree, half-diminished, maj7 triangle, 7 and 6 (first
 * inversion) marks. x, y top-left, h capital height, all Q10. Returns the width (r == 0 only measures). */
static int roman(void *r, int x, int y, int h, int semi, const Shape *shape, const uint8_t *col)
{
    static const char *const numerals[12] = {"I", "bII", "II", "bIII", "III", "IV", "#IV", "V", "bVI", "VI", "bVII", "VII"};
    const char *suf = shape->suffix;
    int minor = suf[0] == 'm' && suf[1] != 'a', dim = suf[0] == 'd', small = minor || dim;
    int ch = small ? h * 2 / 3 : h, top = y + h - ch, x0 = x;
    for (const char *p = numerals[semi % 12]; *p; p++) {
        if (*p == 'b') {
            int sx = x + h / 10, by = y + h * 3 / 5;
            line(r, sx, y + h / 4, sx, y + h, STROKE, col);
            line(r, sx, by, sx + h / 4, by + h / 8, STROKE, col);
            line(r, sx + h / 4, by + h / 8, sx, y + h, STROKE, col);
            x += h * 9 / 20;
        } else if (*p == '#') {
            line(r, x + h / 8, y + h / 4, x + h / 8, y + h, STROKE, col);
            line(r, x + h * 3 / 10, y + h / 4, x + h * 3 / 10, y + h, STROKE, col);
            line(r, x, y + h / 2, x + h * 2 / 5, y + h / 2, STROKE, col);
            line(r, x, y + h * 3 / 4, x + h * 2 / 5, y + h * 3 / 4, STROKE, col);
            x += h * 11 / 20;
        } else if (*p == 'I') {
            line(r, x, top, x, y + h, STROKE, col);
            if (small) line(r, x, top - h / 5, x, top - h / 5 + (1 << 10), STROKE * 2, col);
            x += h * 3 / 10;
        } else {
            int w = ch * 3 / 5;
            line(r, x, top, x + w / 2, y + h, STROKE, col);
            line(r, x + w / 2, y + h, x + w, top, STROKE, col);
            x += w + h * 3 / 10;
        }
    }
    int m = h * 2 / 5, s = h / 12;                          /* marks: superscript; stroke font 6 units high */
    if (dim) { circle(r, x + m / 2, y + m / 2, m / 2, 0, col); x += m + h / 10; }
    if (suf[0] == 'm' && suf[1] == '7' && suf[2] == 'b') {
        circle(r, x + m / 2, y + m / 2, m / 2, 0, col);
        line(r, x, y + m, x + m, y, STROKE, col);
        x += m + h / 10;
    }
    if (suf[0] == 'm' && suf[1] == 'a') {
        line(r, x, y + m, x + m / 2, y, STROKE, col); line(r, x + m / 2, y, x + m, y + m, STROKE, col);
        line(r, x + m, y + m, x, y + m, STROKE, col);
        x += m + h / 10;
    }
    int has7 = 0;
    for (const char *p = suf; *p; p++) has7 |= *p == '7';
    if (has7) { if (r) sf_text(r, x, y, s, "7", STROKE, col); x += 5 * s + h / 10; }
    if (shape->root) { if (r) sf_text(r, x, y + h / 2, s, "6", STROKE, col); x += 5 * s; }
    return x - x0;
}

static void spell_for_key(void)     /* sharps when the key's relative major has sharps (G D A E B F#) */
{
    static const uint8_t to_major[SCALE_COUNT] = {0, 3, 10, 5, 7, 8, 1, 3, 3, 8, 0, 3, 3};   /* relative major */
    int sc = chord_mode == CHORD_STEP ? MAJOR : clamp(chord_scale, SCALE_COUNT);
    int major = (wrap(chord_root, 12) + to_major[sc]) % 12;
    note_names = major == 7 || major == 2 || major == 9 || major == 4 || major == 11 || major == 6 ? sharp_names : flat_names;
}

static int is_white(int pc) { return (0xab5 >> pc) & 1; }   /* C D E F G A B */
#define KEY_X 6
#define KEY_W 22
#define KEY_TOP 62
#define KEY_BOT 100
#define BLACK_BOT 86

static void fill(void *r, int x0, int y0, int x1, int y1, const uint8_t *col)   /* px, butt-capped band */
{
    if (x1 - x0 > y1 - y0) line(r, x0 << 10, ((y0 + y1) << 10) / 2, x1 << 10, ((y0 + y1) << 10) / 2, (y1 - y0) << 10, col);
    else line(r, ((x0 + x1) << 10) / 2, y0 << 10, ((x0 + x1) << 10) / 2, y1 << 10, (x1 - x0) << 10, col);
}

static void root_mark(void *r, int cx)
{
    line(r, (cx - 4) << 10, 108 << 10, cx << 10, 103 << 10, PART, c_blue);
    line(r, cx << 10, 103 << 10, (cx + 4) << 10, 108 << 10, PART, c_blue);
    line(r, (cx + 4) << 10, 108 << 10, (cx - 4) << 10, 108 << 10, PART, c_blue);
}

/* two octaves from `base`: lit = the voiced notes (bass white, upper lavender), green dots on the scale, blue
 * triangles under the root */
static void keyboard(void *r, int base, uint32_t lit, int bass, int active_scale)   /* lit: bit k = base + k */
{
    int mask = scale_masks[clamp(chord_scale, SCALE_COUNT)], root = wrap(chord_root, 12), x[25], n = 0;
    for (int k = 0; k < 25; k++) x[k] = is_white((base + k) % 12) && n < 15 ? n++ : -1;
    const uint8_t *dot = active_scale ? c_green : c_dark;
    for (int k = 0; k < 25; k++) {
        if (x[k] < 0 || x[k] > 13) continue;
        int x0 = KEY_X + x[k] * KEY_W, pc = (base + k) % 12;
        if (lit >> k & 1) fill(r, x0 + 2, KEY_TOP + 1, x0 + KEY_W - 2, KEY_BOT - 1, base + k == bass ? c_white : c_text);
        box(r, x0 << 10, KEY_TOP << 10, KEY_W << 10, (KEY_BOT - KEY_TOP) << 10, c_grey);
        if (mask >> ((pc - root + 12) % 12) & 1)
            circle(r, (x0 + KEY_W / 2) << 10, (KEY_BOT - 6) << 10, 2 << 10, 1, lit >> k & 1 ? c_fill : dot);
        if (pc == root) root_mark(r, x0 + KEY_W / 2);
    }
    for (int k = 1; k < 25; k++) {
        if (is_white((base + k) % 12) || x[k - 1] < 0 || x[k - 1] > 12) continue;
        int cx = KEY_X + (x[k - 1] + 1) * KEY_W, pc = (base + k) % 12;
        fill(r, cx - 7, KEY_TOP, cx + 7, BLACK_BOT, lit >> k & 1 ? (base + k == bass ? c_white : c_text) : c_fill);
        box(r, (cx - 7) << 10, KEY_TOP << 10, 14 << 10, (BLACK_BOT - KEY_TOP) << 10, c_grey);
        if (mask >> ((pc - root + 12) % 12) & 1)
            circle(r, cx << 10, (BLACK_BOT - 6) << 10, 2 << 10, 1, lit >> k & 1 ? c_fill : dot);
        if (pc == root) root_mark(r, cx);
    }
}

static void setting(void *r, int k, int filled, const char *value, const char *label, int active)
{
    int lane = 40 + 80 * k;
    const uint8_t *c = active ? lane_col[k] : lane_dim[k];
    if (filled) circle(r, (lane - 30) << 10, 120 << 10, 7 << 10, 1, c);
    else ring(r, (lane - 30) << 10, 120 << 10, 6 << 10, PART, c);
    tx(active ? T_VAL + k : T_VALD + k, value, r);
    sf_text(r, (lane - 19) << 10, 130 << 10, 1229, label, STROKE, c_grey);   /* 7 px capitals */
}

static void ch_init(void *scr)                              /* boot, GUI thread */
{
    (void)scr;
    for (int i = 0; i < T_COUNT; i++) ch_shown[i][0] = 0;   /* fresh Text objects hold nothing yet */
    op1_text_ctor(ch_txt[T_TITLE], OP1_Q10(8), 14, 12, c_lavender);
    op1_text_ctor(ch_txt[T_NAME], OP1_Q10(160), 12, 24, c_text);
    op1_text_ctor(ch_txt[T_MSG], OP1_Q10(160), 22, 12, c_grey);
    op1_text_ctor(ch_txt[T_OFF], OP1_Q10(160), 12, 24, c_dark);
    T_SET_ALIGN(ch_txt[T_NAME], 1); T_SET_ALIGN(ch_txt[T_MSG], 1); T_SET_ALIGN(ch_txt[T_OFF], 1);
    for (int k = 0; k < 4; k++) {
        op1_text_ctor(ch_txt[T_VAL + k], OP1_Q10(21 + 80 * k), 114, 12, lane_col[k]);
        op1_text_ctor(ch_txt[T_VALD + k], OP1_Q10(21 + 80 * k), 114, 12, lane_dim[k]);
    }
    gen_tables();
    ch_load();
    rec_load();
    if (chord_progression == MINE && !rec_prog.n) chord_progression = 0;
    ch_saved = ch_pack();
}

static void ch_update(void *scr, int dt)
{
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    spell_for_key();
    tx(T_TITLE, "chord", r);
    char name[16];
    int step_mode = chord_on && chord_mode == CHORD_STEP, degree = chord_on && chord_mode == CHORD_DEGREE;
    int key = wrap(chord_root, 12), playing = chord_on && last.bass >= 0;
    if (!chord_on) tx(T_OFF, "off", r);
    else if (!playing) tx(T_MSG, "play a key", r);
    else {                          /* the name in strokes: 18 px capitals (as the size-24 firmware text) */
        chord_name(name, sizeof name, last.bass, last.shape);
        int u = 18 * 1024 / 20;
        name_text(r, (160 << 10) - name_width(name, u) / 2, 13 << 10, u, name, c_text);
    }
    int recording = degree && chord_rec_on;
    if (step_mode || recording) {   /* the progression: playing = filled white (the white knob), next = lavender */
        const Progression *p = recording ? &rec_prog : prog(chord_progression);
        int cur = recording || chord_progression == step_progression, next = recording ? -1 : cur ? step_pos : 0;
        int at = cur && last.step >= 0 && playing ? last.step : next;   /* the chord whose page is shown */
        int first = at / 4 * 4, shown = p->n - first < 4 ? p->n - first : 4;   /* pages of four: 4-bar phrases */
        for (int i = 0; i < shown; i++) {
            int idx = first + i, x = 8 + i * 76;            /* fixed slots, so a short last page keeps its places */
            const Step *st = &p->step[idx];
            int semi = (st->root + st->shape->root) % 12;
            int on = cur && idx == last.step && playing, w = roman(0, 0, 0, 11 << 10, semi, st->shape, 0);
            if (on) fill(r, x, 38, x + 70, 54, c_white);    /* the only white: the chord playing (owner) */
            else if (idx == next) {                         /* next: a 2 px grey outline */
                line(r, x << 10, 38 << 10, (x + 70) << 10, 38 << 10, PART, c_grey);
                line(r, (x + 70) << 10, 38 << 10, (x + 70) << 10, 54 << 10, PART, c_grey);
                line(r, (x + 70) << 10, 54 << 10, x << 10, 54 << 10, PART, c_grey);
                line(r, x << 10, 54 << 10, x << 10, 38 << 10, PART, c_grey);
            } else box(r, x << 10, 38 << 10, 70 << 10, 16 << 10, c_dark);
            roman(r, ((x + 35) << 10) - w / 2, 41 << 10, 11 << 10, semi, st->shape, on ? c_black : c_grey);
        }
        if (p->n > 4) {             /* overview: one tick per chord; playing white, this page grey, the rest dark */
            int pitch = 304 / p->n;
            for (int idx = 0; idx < p->n; idx++) {
                int x = 8 + idx * pitch, on = cur && idx == last.step && playing;
                const uint8_t *c = on ? c_white : idx / 4 == at / 4 ? c_grey : c_dark;
                fill(r, x + 1, 57, x + pitch - 2, 59, c);
            }
        }
    } else if (degree && playing) {
        int semi = (last.bass + last.shape->root - key + 1200) % 12, w = roman(0, 0, 0, 13 << 10, semi, last.shape, 0);
        roman(r, (160 << 10) - w / 2, 42 << 10, 13 << 10, semi, last.shape, c_text);
    }
    uint32_t lit = 0;               /* two octaves from the C at or below the bass, or from the bass if needed */
    int bass = playing ? last.bass : 60, base = bass - bass % 12;
    if (playing) {
        int top = bass;
        for (int i = 0; i < last.n; i++) top = last.tone[i] > top ? last.tone[i] : top;
        if (top - base > 23) base = is_white(bass % 12) ? bass : bass - 1;
        for (int i = 0; i < last.n; i++) if (last.tone[i] >= base && last.tone[i] - base < 25) lit |= 1u << (last.tone[i] - base);
    }
    keyboard(r, base, lit, playing ? bass : -1, degree);
    setting(r, 0, chord_colour != 0, note_names[key], "ROOT", chord_on);
    if (chord_colour) sf_text(r, 52 << 10, 130 << 10, 1229, colour_labels[clamp(chord_colour, 5)], STROKE, degree ? c_blue : c_blue_dim);
    setting(r, 1, chord_voice_leading, scale_names[clamp(chord_scale, SCALE_COUNT)], "SCALE", degree);
    setting(r, 2, recording, recording ? "rec" : chord_progression == MINE ? "mine" : progression_names[clamp(chord_progression, PROGRESSION_COUNT)],
            "PROG", step_mode || recording);
    setting(r, 3, chord_on, mode_names[chord_mode == CHORD_STEP ? CHORD_STEP : CHORD_DEGREE], "MODE", 1);
}

const op1_screen_t SCREEN_chord = { ch_init, ch_update };

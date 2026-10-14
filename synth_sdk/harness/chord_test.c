/* Host test + preview frames for chord/chord.c (chord mode, T4 page): chords per scale, shift chords, voice
 * leading, shared notes of overlapping chords, step mode, settings and their saved row, and every page stroke
 * on screen. "render": JSON frames for harness/render_usbout.py (render_usbout.main(name='chord')). */
#include "../op1_synth.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../chord/chord.c"

int host_play_mode, host_shift;
static int sounding[128], ons, offs;
static void vc_on(const ch_note *n) { if (!sounding[n->note]) { sounding[n->note] = 1; ++ons; } }   /* stock ignores held */
static void vc_off(const ch_note *n) { assert(sounding[n->note]); sounding[n->note] = 0; ++offs; }
void (*host_vc_note_on)(const ch_note *) = vc_on, (*host_vc_note_off)(const ch_note *) = vc_off;

static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } L[4096]; static int nl;
typedef struct { int32_t x, y, align, size; uint8_t c[4]; char s[32]; } txt_t;
static txt_t *drawn[32]; static int nt;
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c)
{
    (void)r; const op1_pt *a = p0, *b = p1;   /* every stroke on the 320x160 screen, sane width (AGG budget) */
    assert(a->x >= -(8 << 10) && a->x <= 328 << 10 && a->y >= -(8 << 10) && a->y <= 168 << 10);
    assert(b->x >= -(8 << 10) && b->x <= 328 << 10 && b->y >= -(8 << 10) && b->y <= 168 << 10);
    assert(w > 0 && w <= 64 << 10 && nl < 4096);
    L[nl].x0 = a->x; L[nl].y0 = a->y; L[nl].x1 = b->x; L[nl].y1 = b->y; L[nl].w = w; memcpy(L[nl].c, c, 4); nl++;
}
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t sz, const uint8_t *c)
{ txt_t *t = o; t->size = sz; t->x = x; t->y = y; t->align = 0; memcpy(t->c, c, 4); t->s[0] = 0; return o; }
void host_text_align(void *o, int a) { ((txt_t *)o)->align = a; }
void host_text_set(void *o, const char *s) { snprintf(((txt_t *)o)->s, 32, "%s", s); }
void host_text_draw(void *o, void *r, const int32_t *m) { (void)r; (void)m; assert(nt < 32); drawn[nt++] = o; }
void *host_raster(void) { return (void *)1; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }

static void on(int note) { ch_note n = {note, 127, 1, {0}, 0}; chord_note_on(&n); }
static void off(int note) { ch_note n = {note, 64, 1, {0}, 0}; chord_note_off(&n); }
static int is(const int *notes)
{
    int want[128] = {0};
    for (; *notes; ++notes) want[*notes] = 1;
    return !memcmp(want, sounding, sizeof want);
}
static int chord_is(int bass, const int *pcs)                /* bass lowest, sounding pitch classes = pcs */
{
    int want = 0, have = 0, low = -1;
    for (; *pcs >= 0; ++pcs) want |= 1 << *pcs;
    for (int i = 0; i < 128; ++i) if (sounding[i]) { have |= 1 << (i % 12); if (low < 0) low = i; }
    return low == bass && have == want;
}
static int render;
static void frame(const char *label)
{
    nl = nt = 0; ch_update(0, 1);
    assert(nl > 50);
    if (!render) return;
    printf("{\"label\": \"%s\", \"lines\": [", label);
    for (int i = 0; i < nl; i++) printf("%s[%d,%d,%d,%d,%d,[%d,%d,%d,%d]]", i ? "," : "", L[i].x0, L[i].y0, L[i].x1, L[i].y1, L[i].w, L[i].c[0], L[i].c[1], L[i].c[2], L[i].c[3]);
    printf("], \"text\": [");
    for (int i = 0; i < nt; i++) printf("%s[%d,%d,[%d,%d,%d],\"%s\",%d,%d]", i ? "," : "", drawn[i]->x, drawn[i]->y, drawn[i]->c[0], drawn[i]->c[1], drawn[i]->c[2], drawn[i]->s, drawn[i]->align, drawn[i]->size);
    printf("]}\n");
}
static int lit_on_page(int note)                            /* a white or lavender key fill covers this note */
{
    (void)note; int found = 0;
    for (int i = 0; i < nl; i++) if (L[i].w >= 12 << 10 && (L[i].c[0] == 0xff || L[i].c[0] == 0xdf) && L[i].y0 != L[i].y1) found++;
    return found;
}

int main(int argc, char **argv)
{
    render = argc > 1 && !strcmp(argv[1], "render");
    ch_init(0);
    assert(!chord_on);                                                   /* fresh install: chords off */
    on(60); assert(is((int[]){60, 0})); off(60);
    chord_on = 1;
    frame("empty");
    on(60); assert(is((int[]){60, 64, 67, 0})); off(60);                 /* C: root position */
    on(62); assert(is((int[]){62, 65, 69, 0})); off(62);                 /* Dm, voice-led F-A */
    on(66); assert(is((int[]){66, 69, 74, 0})); off(66);                 /* D/F# */
    on(71); assert(is((int[]){71, 74, 77, 0})); frame("b dim (vii)"); off(71);
    on(60); assert(is((int[]){60, 67, 76, 0}));                          /* C after B dim: G-E closer */
    nl = 0; ch_update(0, 1); assert(lit_on_page(0) == 3);                /* the keyboard shows the 3 voiced notes */
    on(64); assert(is((int[]){60, 64, 67, 71, 76, 0}));                  /* Em on top: shared E and G */
    off(60); assert(is((int[]){64, 67, 71, 0}));                         /* still sounding for the held E */
    off(64); assert(is((int[]){0}) && ons == offs);
    host_shift = 1;
    on(62); assert(chord_is(62, (int[]){2, 6, 9, 0, -1})); frame("shift d: d7 (II7)"); off(62);
    on(65); assert(chord_is(65, (int[]){5, 8, 0, -1})); off(65);         /* iv */
    on(66); assert(chord_is(66, (int[]){6, 9, 0, 4, -1})); frame("shift f#: f#m7b5"); off(66);
    host_shift = 0;
    on(66); frame("f#: d/f# (II6)"); off(66);
    {                                                                    /* spelling */
        char nm[16]; spell_for_key();
        chord_root = 9; chord_scale = 1; spell_for_key();                 /* A minor */
        chord_name(nm, 16, 61, &scale_shapes[MINOR][4]); assert(!strcmp(nm, "A/C#"));
        chord_name(nm, 16, 63, &scale_shapes[MINOR][6]); assert(!strcmp(nm, "D#dim7"));
        chord_root = 0; chord_scale = 0; spell_for_key();                 /* C major */
        chord_name(nm, 16, 63, &scale_shapes[MAJOR][3]); assert(!strcmp(nm, "Eb"));
        chord_name(nm, 16, 66, &scale_shapes[MAJOR][6]); assert(!strcmp(nm, "D/F#"));
        chord_name(nm, 16, 70, &scale_shapes[MAJOR][10]); assert(!strcmp(nm, "Bb"));
        chord_root = 2; spell_for_key();                                  /* D major: B minor, F# minor */
        chord_name(nm, 16, 71, &scale_shapes[MAJOR][9]); assert(!strcmp(nm, "Bm"));
        chord_name(nm, 16, 66, &scale_shapes[MAJOR][4]); assert(!strcmp(nm, "F#m"));
        chord_root = 0;
    }
    chord_root = 9; chord_scale = 1;                                     /* A minor */
    on(63); assert(chord_is(63, (int[]){3, 6, 9, 0, -1})); off(63);      /* D#dim7 */
    on(61); assert(chord_is(61, (int[]){1, 4, 9, -1})); frame("a minor: a/c#"); off(61);
    chord_root = 0;
    chord_scale = HARM_MINOR;                                            /* C harmonic minor */
    on(63); assert(chord_is(63, (int[]){3, 7, 11, -1})); off(63);        /* III+: Eb aug */
    on(67); assert(chord_is(67, (int[]){7, 11, 2, -1})); frame("c harm min: g (V)"); off(67);
    chord_scale = LYDIAN;
    on(62); assert(chord_is(62, (int[]){2, 6, 9, -1})); off(62);         /* II major: the lydian sound */
    chord_scale = MAJ_PENT;                                              /* C D E G A: every other note */
    on(62); assert(chord_is(62, (int[]){2, 7, 0, -1})); frame("c maj pent: csus2/d"); off(62);
    chord_scale = BLUES;
    on(66); off(66); on(61); assert(chord_is(61, (int[]){1, 5, 8, -1})); off(61);   /* out of scale: major */
    for (int sc = 0; sc < SCALE_COUNT; sc++)                             /* every key of every scale: 3+ notes */
        for (int n = 60; n < 72; n++) { chord_scale = sc; on(n); int c = 0; for (int i = 0; i < 128; i++) c += sounding[i]; assert(c >= 3); off(n); }
    chord_scale = 0;
    host_play_mode = 1; on(60); assert(is((int[]){60, 0})); off(60); host_play_mode = 0;   /* mono */
    chord_mode = CHORD_STEP; chord_progression = 0;                      /* pop */
    on(60); off(60); on(60); assert(chord_is(55, (int[]){7, 11, 2, -1})); frame("step: pop, V playing"); off(60);
    host_shift = 1; on(60); assert(chord_is(60, (int[]){0, 4, 7, -1})); off(60); host_shift = 0;   /* back: I */
    chord_progression = 2; on(62); assert(chord_is(62, (int[]){2, 5, 9, 0, -1})); off(62);          /* jazz ii7 */
    chord_progression = 4;                                               /* blues: 12 chords in pages of four */
    for (int i = 0; i < 5; i++) { on(60); off(60); }                     /* bars 1-5 played: page 2 */
    on(60); frame("step: blues, bar 6 (page 2 of 3)");
    { int tabs = 0, ticks = 0;
      for (int i = 0; i < nl; i++) { if (L[i].y0 == 38 << 10 && L[i].y1 == 38 << 10) tabs++;
                                     if (L[i].w == 2 << 10 && L[i].y0 == 58 << 10) ticks++; }
      assert(ticks == 12); (void)tabs; }
    off(60);
    for (int m = 1; m <= 2; m++)                                         /* every chord and progression draws */
        for (int sc = 0; sc < 4; sc++)
            for (int sh = 0; sh < 2; sh++)
                for (int n = 53; n <= 76; n++) {
                    chord_mode = m; chord_scale = sc; host_shift = sh; chord_progression = (n + sc) % PROGRESSION_COUNT;
                    on(n); nl = nt = 0; ch_update(0, 1); off(n);
                }
    host_shift = 0; chord_scale = 0;
    chord_progression = 14; on(62); assert(chord_is(62, (int[]){2, 5, 8, 0, -1})); off(62);   /* min251: Dm7b5 */
    for (int pr = 0; pr < PROGRESSION_COUNT; pr++) {                     /* every chord of every progression */
        chord_progression = pr;
        for (int i = 0; i < progressions[pr].n; i++) {
            on(64); int c = 0; for (int k = 0; k < 128; k++) c += sounding[k];
            assert(c >= 3); nl = nt = 0; ch_update(0, 1); off(64);
        }
    }
    for (int i = 0; i < 20; i++) chord_turn(2, 1024);
    assert(chord_progression == MINE);                                   /* white stops at "mine" */
    on(64); assert(chord_is(64, (int[]){4, 7, 11, -1})); off(64);        /* mine still empty: degree chords */
    chord_mode = CHORD_DEGREE; chord_progression = 0;
    assert(is((int[]){0}) && ons == offs);
    /* page controls and saved row */
    chord_turn(0, 1024); assert(chord_root == 1);
    chord_turn(0, 512); chord_turn(0, -512); chord_turn(0, -512); assert(chord_root == 0);   /* direction change */
    chord_turn(1, 1024); chord_turn(1, 1024); chord_turn(1, 9000); chord_turn(1, 1024); chord_turn(1, 1024);
    assert(chord_scale == 5);                                            /* one per detent */
    for (int i = 0; i < 20; i++) chord_turn(1, 1024);
    assert(chord_scale == SCALE_COUNT - 1);                              /* stops at the last (blues) */
    for (int i = 0; i < 7; i++) chord_turn(1, -1024);
    assert(chord_scale == 5);
    chord_turn(3, 1024); assert(chord_mode == CHORD_STEP);
    chord_push(1); assert(!chord_voice_leading); chord_push(1);
    chord_push(3); assert(!chord_on); on(60); assert(is((int[]){60, 0})); frame("off"); off(60); chord_push(3);
    chord_sync(); assert((chord_host_saved & 15) == CHORD_STEP && (chord_host_saved >> 8 & 15) == 5);
    int32_t saved = chord_host_saved;
    chord_sync(); assert(chord_host_saved == saved);
    chord_mode = CHORD_DEGREE; chord_scale = 0; ch_init(0);              /* boot: the saved row comes back */
    assert(chord_mode == CHORD_STEP && chord_scale == 5 && chord_on);
    /* chord colour (blue push): degree chords in C major */
    chord_on = 1; chord_mode = CHORD_DEGREE; chord_scale = 0; chord_root = 0; chord_colour = 0;
    chord_push(0); assert(chord_colour == 1);
    on(60); assert(chord_is(60, (int[]){0, 5, 7, -1})); frame("colour sus4: csus4"); off(60);
    on(66); assert(chord_is(66, (int[]){6, 9, 2, -1})); off(66);         /* D/F#: sus leaves the slash chord */
    chord_push(0); on(62); assert(chord_is(62, (int[]){2, 4, 9, -1})); off(62);       /* sus2: Dsus2 */
    chord_push(0);                                                       /* 7th, from the scale */
    on(60); assert(chord_is(60, (int[]){0, 4, 7, 11, -1})); off(60);     /* Cmaj7 */
    on(62); assert(chord_is(62, (int[]){2, 5, 9, 0, -1})); off(62);      /* Dm7 */
    on(67); assert(chord_is(67, (int[]){7, 11, 2, 5, -1})); frame("colour 7th: g7"); off(67);
    on(71); assert(chord_is(71, (int[]){11, 2, 5, 9, -1})); off(71);     /* Bm7b5 */
    on(70); assert(chord_is(70, (int[]){10, 2, 5, 8, -1})); off(70);     /* out of scale Bb: Bb7 */
    chord_push(0); on(69); assert(chord_is(69, (int[]){9, 0, 4, 11, -1})); off(69);   /* add9: Amadd9 */
    chord_push(0); assert(chord_colour == 0);
    /* recording: push white in degree mode, play, push white again */
    chord_progression = 0; chord_push(2); assert(chord_rec_on);
    on(60); off(60); on(69); off(69); chord_push(0); chord_push(0); chord_push(0);   /* 7th for the next two */
    on(65); off(65); on(67); frame("recording: c am fmaj7 g7"); off(67);
    chord_push(0); chord_push(0);
    chord_push(2); assert(!chord_rec_on && chord_progression == MINE && rec_prog.n == 4);
    chord_sync(); assert(chord_host_rec[0] == 'r' && strlen(chord_host_rec) == 1 + 4 * 6);
    chord_mode = CHORD_STEP;                                             /* step through it */
    on(72); assert(chord_is(72, (int[]){0, 4, 7, -1})); off(72);
    on(72); assert(chord_is(69, (int[]){9, 0, 4, -1})); off(72);
    on(72); assert(chord_is(77, (int[]){5, 9, 0, 4, -1})); frame("step: mine, fmaj7"); off(72);
    on(72); assert(chord_is(67, (int[]){7, 11, 2, 5, -1})); off(72);
    chord_root = 2; on(72); assert(chord_is(74, (int[]){2, 6, 9, -1})); off(72);   /* follows a new root: D */
    chord_root = 0;
    rec_prog.n = 0; ch_init(0);                                          /* boot: recording and choice come back */
    assert(rec_prog.n == 4 && chord_progression == MINE && rec_shape[2].n == 4);
    chord_mode = CHORD_DEGREE; host_shift = 1; chord_push(2); host_shift = 0;   /* shift + white: clear */
    assert(rec_prog.n == 0); chord_sync(); assert(!strcmp(chord_host_rec, "r"));
    assert(is((int[]){0}) && ons == offs);
    if (!render) printf("ok\n");
    return 0;
}

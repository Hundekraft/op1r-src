/* Host test of the peal sequencer (seq/peal.c).
 *   checks: plain courses come round after the right number of rows with no row repeated (Plain Hunt on 3..8,
 *   Plain Bob Minimus..Major, Grandsire Doubles / Triples (+ cover), Stedman Doubles / Triples), every row a
 *   permutation; calls: Bob / Single change the lead heads, true 120-row extents of Plain Bob and Grandsire Doubles
 *   exist with these calls (exhaustive search over callings); keys: a held chord of 3..8 rings (sorted, treble =
 *   highest; 1-2 keys silent; the rest of a chord joins the first row), keys added while held join (rounds at the
 *   next row), the last key up finishes the row and stops with nothing hanging, Shift latches (on a chord key or at
 *   release), a chord without Shift takes over, a same-size chord just changes pitches; Shift + C# / Shift + red with
 *   the last bells; timing at tempo with the handstroke gap, rough striking bounds, "That's all", persistence.
 *   "render": JSON screen frames for harness/render_peal.py. */
#include "../seq/peal.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4];

static int n_on[2], active[2][128], seq_log[4096], n_seq, vel_min = 999, vel_max = -1;
static long sample_now, on_time[4096];
void host_pl_note_on(int pos, int n, int v)
{
    n_on[pos]++; active[pos][n]++;
    if (n_seq < 4096) { on_time[n_seq] = sample_now; seq_log[n_seq++] = n; }
    if (v < vel_min) vel_min = v;
    if (v > vel_max) vel_max = v;
}
void host_pl_note_off(int pos, int n) { active[pos][n]--; }

#define MAXL 4096
static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } lines[MAXL]; static int nlines;
void *host_find_node(void *s, const char *id) { (void)s; (void)id; return 0; }
void host_node_set_transform(void *n, const int32_t *m) { (void)n; (void)m; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; nlines = 0; }
void *host_raster(void) { return &nlines; }
void host_raster_line(void *r, const void *a, const void *b, int32_t w, const uint8_t *c)
{ (void)r; const op1_pt *p = a, *q = b; if (nlines < MAXL) { lines[nlines].x0 = p->x; lines[nlines].y0 = p->y; lines[nlines].x1 = q->x; lines[nlines].y1 = q->y; lines[nlines].w = w; memcpy(lines[nlines].c, c, 4); nlines++; } }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static void run(int pos, long samples, uint32_t bpm) { for (long s = 0; s < samples; s += 64) { pl_tick(pos, 64, bpm); sample_now += 64; } }
static int hanging(int pos) { int a = 0; for (int i = 0; i < 128; i++) a += active[pos][i]; return a; }
/* set the bells as a player would (hold the chord, let go) and stop again at once: a quiet start state */
static void set_bells(int pos, const int *notes, int n)
{
    for (int k = 0; k < n; k++) pl_key_down(pos, notes[k], k, 0);
    for (int k = 0; k < n; k++) pl_key_up(pos, k, 0);
    pl_stop(pos, &pl[pos]);
}
static void frame(const char *label)
{
    pl_screen_update(0, 16, 0);
    printf("{\"label\": \"%s\", \"nodes\": {}, \"lines\": [", label);
    for (int i = 0; i < nlines; i++) printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", lines[i].x0, lines[i].y0, lines[i].x1, lines[i].y1, lines[i].w, lines[i].c[0], lines[i].c[1], lines[i].c[2], lines[i].c[3]);
    printf("]}\n");
}

static int row_code(const uint8_t *row, int n) { int c = 0; for (int i = 0; i < n; i++) c = c * 9 + row[i]; return c; }
static uint8_t seen[43046721 / 8 + 1];                  /* 9^8 bits */
static void seen_clear(void) { memset(seen, 0, sizeof seen); }
static int seen_add(int c) { int r = (seen[c >> 3] >> (c & 7)) & 1; seen[c >> 3] |= (uint8_t)(1 << (c & 7)); return r; }
static int is_perm(const uint8_t *row, int n) { int m = 0; for (int i = 0; i < n; i++) { if (row[i] < 1 || row[i] > n) return 0; m |= 1 << row[i]; } return m == ((1 << (n + 1)) - 2); }

/* plain course: rows until rounds; -1 if a row repeats / not a permutation / > 50000 rows */
static int course(int method, int n, char *pn_text)
{
    pl_bank_t *b = &pl[0];
    memset(b, 0, sizeof *b); pl_bank_init(b);
    b->c.n = (uint8_t)n; b->c.method = (uint8_t)method; b->c.callp = 0;
    pl_rounds(b);
    if (pn_text) {                                      /* the lead's place notation, for the report */
        char *p = pn_text;
        for (int i = 0; i < b->m.len; i++) {
            if (i) *p++ = '.';
            if (!b->m.pn[i]) *p++ = 'x';
            for (int k = 0; k < 16; k++) if ((b->m.pn[i] >> k) & 1) *p++ = (char)('1' + k);
        }
        *p = 0;
    }
    seen_clear(); seen_add(row_code(b->row, n));
    for (int r = 1; r <= 50000; r++) {
        pl_next_row(b);
        if (!is_perm(b->row, n)) return -1;
        for (int i = b->m.w; i < n; i++) if (b->row[i] != i + 1) return -1;   /* cover stays behind */
        if (pl_is_rounds(b->row, n)) return r;
        if (seen_add(row_code(b->row, n))) return -1;
    }
    return -1;
}

/* a touch: calls[k] for lead (call position) k = 0 plain, 1 bob, 2 single; returns rows until rounds or -1 if false */
static int touch(int method, int n, const uint8_t *calls, int ncalls, int maxrows)
{
    pl_bank_t *b = &pl[0];
    memset(b, 0, sizeof *b); pl_bank_init(b);
    b->c.n = (uint8_t)n; b->c.method = (uint8_t)method; b->c.callp = 0;
    pl_rounds(b);
    seen_clear(); seen_add(row_code(b->row, n));
    int k = 0;
    for (int r = 1; r <= maxrows; r++) {
        if (!b->call_left && (b->ci == b->m.cp[0] || b->ci == b->m.cp[1])) b->call_req = k < ncalls ? calls[k++] : 0;
        pl_next_row(b);
        if (pl_is_rounds(b->row, n)) return r;
        if (seen_add(row_code(b->row, n))) return -1;
    }
    return -1;
}
/* exhaustive search for a true extent (all w! rows) of a method on 5 bells with bobs and singles */
static int find_extent(int method, int leads, char *out)
{
    int total = 1; for (int i = 0; i < leads; i++) total *= 3;
    uint8_t calls[16];
    for (int v = 0; v < total; v++) {
        int x = v; for (int i = 0; i < leads; i++) { calls[i] = (uint8_t)(x % 3); x /= 3; }
        if (touch(method, 5, calls, leads, 200) == 120) {
            for (int i = 0; i < leads; i++) out[i] = "pBS"[calls[i]];
            out[leads] = 0;
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    int render = argc > 1 && !strcmp(argv[1], "render");
    char pn[80];

    /* plain courses */
    for (int n = 3; n <= 8; n++) { int r = course(PL_HUNT, n, pn); CHECK(r == 2 * n, "Plain Hunt on %d: %d rows (expect %d)", n, r, 2 * n); }
    static const struct { int method, n, rows; const char *name; } pc[] = {
        { PL_BOB, 4, 24, "Plain Bob Minimus" }, { PL_BOB, 5, 40, "Plain Bob Doubles" }, { PL_BOB, 6, 60, "Plain Bob Minor" },
        { PL_BOB, 7, 84, "Plain Bob Triples" }, { PL_BOB, 8, 112, "Plain Bob Major" },
        { PL_GRANDSIRE, 5, 30, "Grandsire Doubles" }, { PL_GRANDSIRE, 6, 30, "Grandsire Doubles + cover" },
        { PL_GRANDSIRE, 7, 70, "Grandsire Triples" }, { PL_GRANDSIRE, 8, 70, "Grandsire Triples + cover" },
        { PL_STEDMAN, 5, 60, "Stedman Doubles" }, { PL_STEDMAN, 6, 60, "Stedman Doubles + cover" },
        { PL_STEDMAN, 7, 84, "Stedman Triples" }, { PL_STEDMAN, 8, 84, "Stedman Triples + cover" },
        { PL_BOB, 3, 6, "Plain Bob on 3 (= plain hunt)" }, { PL_GRANDSIRE, 4, 6, "Grandsire on 4 (hunt on 3 + cover)" } };
    for (unsigned i = 0; i < sizeof pc / sizeof pc[0]; i++) {
        int r = course(pc[i].method, pc[i].n, pn);
        CHECK(r == pc[i].rows, "%s: %d rows (expect %d)", pc[i].name, r, pc[i].rows);
        if (!render) printf("  %-36s %4d rows  %s\n", pc[i].name, r, pn);
    }
    /* calls: a bob / single changes the course; touches stay permutations */
    {
        uint8_t bob1[1] = { 1 }, s1[1] = { 2 };
        int pb = course(PL_BOB, 6, 0), b1 = touch(PL_BOB, 6, bob1, 1, 2000), s = touch(PL_BOB, 6, s1, 1, 2000);
        CHECK(pb == 60 && b1 != 60 && s != 60, "Plain Bob Minor: a call changes the course (plain %d, bob %d, single %d)", pb, b1, s);
        uint8_t bbb[3] = { 1, 1, 1 };
        int bc = touch(PL_BOB, 6, bbb, 3, 2000);
        CHECK(bc == 36, "Plain Bob Minor B B B: %d rows (expect 36, true)", bc);
        uint8_t st[2] = { 1, 1 };
        CHECK(touch(PL_STEDMAN, 7, st, 2, 5000) != 84, "Stedman Triples: bobs change the course");
        char ext[16];
        CHECK(find_extent(PL_GRANDSIRE, 12, ext), "no true 120 of Grandsire Doubles with these calls");
        if (!render) printf("  Grandsire Doubles 120 (12 leads, p/B/S): %s\n", ext);
        CHECK(find_extent(PL_BOB, 12, ext), "no true 120 of Plain Bob Doubles with these calls");
        if (!render) printf("  Plain Bob Doubles 120 (12 leads, p/B/S): %s\n", ext);
    }
    /* automatic calls keep every row a permutation, cover stays last, no crash over long runs */
    {
        pl_bank_t *b = &pl[0];
        for (int mth = 0; mth < PL_METHODS; mth++) for (int n = 3; n <= 8; n++) {
            memset(b, 0, sizeof *b); pl_bank_init(b);
            b->c.n = (uint8_t)n; b->c.method = (uint8_t)mth; b->c.callp = 50; pl_rounds(b);
            int ok = 1;
            for (int r = 0; r < 5000; r++) { pl_next_row(b); ok &= is_perm(b->row, n); for (int i = b->m.w; i < n; i++) ok &= b->row[i] == i + 1; }
            CHECK(ok, "auto calls method %d on %d: bad row", mth, n);
        }
    }

    /* keys: a held chord of 3..8 rings while held, highest = treble */
    memset(pl, 0, sizeof pl);
    {
        n_seq = 0;
        CHECK(pl_key_down(0, 60, 7, 0) == 1 && pl_key_down(0, 67, 14, 0) == 1, "keys consumed");
        run(0, 44100 / 2, 12000);
        CHECK(!pl[0].running && pl[0].c.n == 0 && n_seq == 0, "two keys: silent");
        long t0 = sample_now;
        pl_key_down(0, 64, 11, 0);                      /* third key: rings */
        CHECK(pl[0].running && pl[0].c.n == 3 && pl[0].c.note[0] == 67 && pl[0].c.note[2] == 60, "3 keys ring, sorted");
        run(0, 44100 / 100, 12000);                     /* 10 ms later (inside PL_CHORD_TICKS) a 4th key */
        CHECK(n_seq == 0, "struck before the chord time");
        pl_key_down(0, 72, 19, 0);
        run(0, 44100 / 2 + 64, 12000);
        double d = (on_time[0] - t0) / 44100.0;
        CHECK(d > 1.0 / 48 - 0.002 && d < 2.0 / 48 + 0.003, "first blow after %.4f s (expect 1..2 ticks)", d);
        CHECK(n_seq >= 4 && seq_log[0] == 72 && seq_log[1] == 67 && seq_log[2] == 64 && seq_log[3] == 60, "first row: the whole chord, treble first");
        pl_key_up(0, 7, 0); pl_key_up(0, 14, 0);
        run(0, 44100 / 2, 12000);
        CHECK(pl[0].running && pl[0].c.n == 4 && !pl[0].rel_pend, "keys partly up: rings on, no bell dropped");
        pl_key_down(0, 74, 21, 0);                      /* a 5th key joins: rounds on 5 at the next row */
        CHECK(pl[0].c.n == 4 && pl[0].chord_pend, "added key: 5 bells from the next row");
        run(0, 44100, 12000);
        CHECK(pl[0].run_n == 5, "ringing on 5");
        /* last key up mid-row: the row is finished, then it stops */
        while (pl[0].pos != 2) run(0, 64, 12000);
        int s0 = n_seq;
        pl_key_up(0, 11, 0); pl_key_up(0, 19, 0); pl_key_up(0, 21, 0);
        CHECK(pl[0].running && pl[0].rel_pend, "released: last row");
        run(0, 44100 * 2, 12000);
        CHECK(!pl[0].running && n_seq - s0 == 3 && hanging(0) == 0, "row finished (%d more blows), stopped, %d hanging", n_seq - s0, hanging(0));
        pl_key_up(0, 5, 0);                             /* stray key up: nothing */
        CHECK(pl_shift_key(0, 0) == 0, "shift + F not taken");
    }
    /* latch: Shift on a chord key; Shift + a new chord of the same size changes pitches only; plain chord takes over */
    {
        static const int ch1[4] = { 60, 64, 67, 72 }, ch2[4] = { 62, 65, 69, 74 };
        pl_key_down(0, ch1[0], 7, 0); pl_key_down(0, ch1[1], 11, 1); pl_key_down(0, ch1[2], 14, 0); pl_key_down(0, ch1[3], 19, 0);
        pl_key_up(0, 7, 0); pl_key_up(0, 11, 0); pl_key_up(0, 14, 0); pl_key_up(0, 19, 0);
        run(0, 44100 * 2, 12000);
        CHECK(pl[0].running && pl[0].latched && pl[0].c.n == 4, "Shift + chord latches");
        uint32_t rows = pl[0].rowabs;
        for (int k = 0; k < 4; k++) pl_key_down(0, ch2[k], k, 1);
        CHECK(pl[0].chord_pend && pl[0].c.note[0] == 72 && pl[0].latched, "same-size chord: waits for the row end");
        for (int k = 0; k < 4; k++) pl_key_up(0, k, 0);
        while (pl[0].chord_pend) run(0, 64, 12000);
        CHECK(!pl[0].reset_pending && pl[0].c.note[0] == 74 && pl[0].latched && pl[0].pos <= 1, "same-size chord: new pitches at the row end, method goes on");
        int s0 = n_seq;
        run(0, 44100, 12000);
        int ok = 1; for (int i = s0; i < n_seq; i++) ok &= seq_log[i] == 62 || seq_log[i] == 65 || seq_log[i] == 69 || seq_log[i] == 74;
        CHECK(ok && n_seq > s0 && pl[0].running && pl[0].rowabs > rows, "latched on the new chord");
        pl_key_down(0, 50, 0, 0);                       /* one key without Shift: takes over */
        CHECK(!pl[0].latched && pl[0].c.n == 4, "plain key unlatches (bells kept)");
        pl_key_up(0, 0, 0);
        run(0, 44100 * 2, 12000);
        CHECK(!pl[0].running && hanging(0) == 0, "released: stopped after the row");
        for (int k = 0; k < 3; k++) pl_key_down(0, ch1[k], k, 0);
        run(0, 44100 / 2, 12000);
        for (int k = 0; k < 3; k++) pl_key_up(0, k, k == 2);   /* Shift held at the last release */
        run(0, 44100 * 2, 12000);
        CHECK(pl[0].running && pl[0].latched, "Shift at release latches");
        CHECK(pl_shift_key(0, 20) == 1 && !pl[0].running && !pl[0].latched && hanging(0) == 0, "Shift+C# stops a latched peal");
        CHECK(pl_shift_key(0, 20) == 2 && pl[0].running && pl[0].latched && pl[0].c.n == 3, "Shift+C#: last bells, latched");
        pl_turn(0, 3, -1024, 1);
        CHECK(!pl[0].running && !pl[0].latched && hanging(0) == 0, "Shift+red left stops");
    }
    /* timing: 6 bells, 1/16 at 120 bpm (8 blows / s), gap: 13 steps per hand+back pair */
    {
        memset(pl, 0, sizeof pl);
        static const int notes[6] = { 72, 71, 69, 67, 65, 64 };
        set_bells(0, notes, 6);
        pl_turn(0, 0, 1024 * 3, 0); pl_turn(0, 0, -1024 * 2, 0);   /* Plain Bob */
        CHECK(pl[0].c.method == PL_BOB, "method");
        n_seq = 0;
        pl_turn(0, 3, 1024, 1);
        CHECK(pl[0].running && pl[0].latched, "Shift+red starts (latched)");
        run(0, 44100 * 26 / 8 + 44100 / 16, 12000);       /* 26 steps = 2 pairs of rows with gaps */
        CHECK(n_seq == 24, "blows %d (expect 24 = 4 rows of 6)", n_seq);
        int rounds_ok = 1; for (int i = 0; i < 6; i++) rounds_ok &= seq_log[i] == notes[i];
        CHECK(rounds_ok, "first row = rounds (treble first)");
        double blow = (on_time[1] - on_time[0]) / 44100.0, gap = (on_time[12] - on_time[11]) / 44100.0;
        CHECK(fabs(blow - 0.125) < 0.003 && fabs(gap - 0.25) < 0.003, "blow %.4f s, after backstroke %.4f s", blow, gap);
        /* second row: x on 6 -> 2 1 4 3 6 5 */
        static const int r2[6] = { 71, 72, 67, 69, 64, 65 };
        int ok = 1; for (int i = 0; i < 6; i++) ok &= seq_log[6 + i] == r2[i];
        CHECK(ok, "second row 214365");
        pl_press(0, 0, 0);                              /* gap off */
        int s0 = n_seq; run(0, 44100 * 2, 12000);
        CHECK(n_seq - s0 == 16 || n_seq - s0 == 15, "no gap: %d blows in 2 s", n_seq - s0);
        /* rough striking: blows late by at most half a step, velocity spread */
        pl_turn(0, 3, 20 * 1024, 0);
        CHECK(pl[0].c.rough == 100, "rough 100");
        vel_min = 999; vel_max = -1; s0 = n_seq;
        run(0, 44100 * 4, 12000);
        int late_ok = 1;
        long step = 44100 / 8;
        for (int i = s0 + 1; i < n_seq; i++) { long d = on_time[i] - on_time[i - 1]; if (d < step / 2 - 128 || d > step * 3 / 2 + 128) late_ok = 0; }
        CHECK(late_ok, "rough: blow spacing outside 0.5..1.5 steps");
        CHECK(vel_max - vel_min >= 20 && vel_min >= 70 && vel_max <= 127, "rough velocity %d..%d", vel_min, vel_max);
        pl_turn(0, 3, -20 * 1024, 0);
        /* manual Bob: made at the next lead end, then flashes */
        pl_press(0, 2, 0);
        CHECK(pl[0].call_req == PL_CALL_BOB, "bob requested");
        int seen_bob = 0;
        for (int i = 0; i < 400 && !seen_bob; i++) { run(0, 64 * 8, 12000); seen_bob = pl[0].flash == PL_CALL_BOB; }
        CHECK(seen_bob && pl[0].call_req == 0, "bob made");
        pl_press(0, 2, 1);
        CHECK(pl[0].call_req == PL_CALL_SINGLE, "Shift: single requested");
        pl_press(0, 3, 0);                              /* look to: rounds at the next row */
        int back = 0;
        for (int i = 0; i < 400 && !back; i++) { run(0, 64 * 4, 12000); back = pl[0].rowabs == 0 && pl[0].pos > 0; }
        CHECK(back && pl[0].call_req == 0, "look to -> rounds");
        /* that's all: Plain Bob Minor plain course comes round after 60 rows, then stops */
        pl_press(0, 1, 0);
        CHECK(pl[0].stand == 1, "that's all armed");
        int stopped = 0;
        for (int i = 0; i < 4000 && !stopped; i++) { run(0, 64 * 8, 12000); stopped = !pl[0].running; }
        CHECK(stopped && pl[0].last_course == 60 && hanging(0) == 0, "that's all: stopped %d after %u rows", stopped, pl[0].last_course);
        /* method change while running: rounds at the next row */
        pl_shift_key(0, 20);
        CHECK(pl[0].running, "Shift+C# starts");
        run(0, 44100, 12000);
        pl_turn(0, 0, 1024, 0);
        CHECK(pl[0].c.method == PL_GRANDSIRE && pl[0].reset_pending, "method change pending");
        run(0, 44100, 12000);
        CHECK(!pl[0].reset_pending && pl[0].m.w == 5, "Grandsire on 6 = Doubles + cover (w %d)", pl[0].m.w);
        pl_midi_start(); pl_advance(0, 1u << 14);
        CHECK(pl[0].pos == 1 && pl[0].rowabs == 0, "MIDI START: rounds from the first blow");
        CHECK(pl_shift_key(0, 20) == 1 && !pl[0].running && hanging(0) == 0, "Shift+C# stops");
    }
    /* persistence */
    {
        char buf[200]; pl_cfg_t saved = pl[0].c;
        pl[0].c.callp = 35; saved.callp = 35;
        int len = pl_serialize(0, buf, sizeof buf);
        memset(&pl[0], 0, sizeof pl[0]);
        CHECK(pl_parse(0, buf, len) && !memcmp(&pl[0].c, &saved, sizeof saved) && !pl[0].running, "round trip: %s", buf);
        CHECK(!pl_parse(0, "1,2", 3) && !pl_parse(0, "x", 1), "bad text accepted");
        CHECK(host_pl_changed > 0, "changes not reported");
    }
    /* drum bank independent */
    {
        static const int dn[3] = { 53, 54, 55 };
        set_bells(1, dn, 3);
        CHECK(pl[1].c.n == 3 && pl[0].c.n == 6 && !pl[0].running && hanging(1) == 0, "drum bank");
    }
    if (!render) { printf(fails ? "%d FAILED\n" : "OK: all peal checks pass\n", fails); return fails != 0; }

    /* ---- screen frames ---- */
    memset(pl, 0, sizeof pl);
    frame("start: no bells");
    static const int notes8[8] = { 84, 83, 81, 79, 77, 76, 74, 72 };
    for (int k = 0; k < 6; k++) pl_key_down(0, notes8[k + 2], k, 0);
    run(0, 44100 * 6, 12000);
    frame("6 keys held: Plain Bob Minor ringing (HOLD, treble's blue line)");
    for (int k = 0; k < 6; k++) pl_key_up(0, k, k == 5);
    pl_press(0, 2, 0); run(0, 44100 / 4, 12000);
    frame("Bob! called (dim until the lead end); latched (Shift at release)");
    for (int i = 0; i < 400 && pl[0].flash != PL_CALL_BOB; i++) run(0, 64 * 8, 12000);
    frame("BOB made at the lead end");
    pl_turn(0, 3, -1024, 1);
    set_bells(0, notes8, 8);
    pl_turn(0, 0, 1024, 0); pl_turn(0, 2, 4 * 1024, 0); pl_turn(0, 3, 6 * 1024, 0);
    pl_turn(0, 3, 1024, 1); run(0, 44100 * 9, 12000);
    frame("8 bells Grandsire Triples + cover, call 20 %, rough 30 %");
    pl_turn(0, 3, -1024, 1);
    set_bells(0, notes8, 7);
    pl_turn(0, 0, 1024, 0); pl_turn(0, 2, -4 * 1024, 0); pl_turn(0, 3, 1024, 1);
    for (int i = 0; i < 20000 && pl[0].flash != PL_ROUNDS; i++) run(0, 64 * 4, 12000);
    frame("Stedman Triples comes round: ROUNDS 84/5040");
    run(0, 44100 * 3, 12000); pl_press(0, 1, 0); pl_press(0, 0, 0);
    frame("That's all armed, gap off");
    pl_press(0, 1, 0);
    for (int k = 0; k < 7; k++) pl_key_down(0, notes8[k], k, 0);
    for (int k = 0; k < 7; k++) pl_key_up(0, k, 0);
    frame("keys released: the bells finish the row (LAST ROW)");
    return 0;
}

/* Host test of the oracle sequencer (seq/oracle.c).
 *   checks: figure table, chart derivation (Daughters, Nieces, Witnesses, Judge, Reconciler), Judge parity over all
 *   65536 casts; playing from keys: a key starts at once, transposes (lanes = intervals above it), a second key
 *   re-transposes in phase, release stops at the next step with nothing hanging, a short tap still plays, Shift + key
 *   and Shift at release latch, a key without Shift takes a latched pattern over; voicings, lane EDIT (interval above
 *   the root / drum sound), drum mode (any key, own sounds); step / lane hits at tempo, gate, chords, mute, patterns,
 *   clipboard, Shift + C# / Shift + red, persistence, MIDI start.
 *   "render": JSON screen frames for harness/render_oracle.py. */
#include "../seq/oracle.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4];

static int n_on[2], active[2][128], hits_per_note[128], last_vel, max_active, n_accent;
static long sample_now, first_on = -1, first_off = -1, last_on = -1;
void host_ora_note_on(int pos, int n, int v)
{
    n_on[pos]++; active[pos][n]++; hits_per_note[n]++; last_vel = v; n_accent += v == 120; last_on = sample_now;
    if (first_on < 0) first_on = sample_now;
    int a = 0; for (int i = 0; i < 128; i++) a += active[pos][i]; if (a > max_active) max_active = a;
}
void host_ora_note_off(int pos, int n) { active[pos][n]--; if (first_off < 0) first_off = sample_now; }

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
static void run(int pos, long samples, uint32_t bpm) { for (long s = 0; s < samples; s += 64) { ora_tick(pos, 64, bpm); sample_now += 64; } }
static int hanging(int pos) { int a = 0; for (int i = 0; i < 128; i++) a += active[pos][i]; return a; }
static void frame(const char *label, int dt, int pos)
{
    ora_screen_update(0, dt, pos);
    printf("{\"label\": \"%s\", \"nodes\": {}, \"lines\": [", label);
    for (int i = 0; i < nlines; i++) printf("%s[%d, %d, %d, %d, %d, [%d, %d, %d, %d]]", i ? ", " : "", lines[i].x0, lines[i].y0, lines[i].x1, lines[i].y1, lines[i].w, lines[i].c[0], lines[i].c[1], lines[i].c[2], lines[i].c[3]);
    printf("]}\n");
}
/* dots per row of a figure, Fire first, e.g. Laetitia "1222" */
static void dots(int fig, char *s) { int o = ora_odd(fig); for (int r = 0; r < 4; r++) s[r] = (o >> r) & 1 ? '1' : '2'; s[4] = 0; }
/* expected hits per lane over one bar */
static void lane_hits(const uint8_t m[4], int exp[4])
{
    uint8_t c[16]; ora_chart(m, c);
    for (int l = 0; l < 4; l++) exp[l] = 0;
    for (int i = 0; i < 16; i++) for (int l = 0; l < 4; l++) exp[l] += (c[i] >> l) & 1;
}

int main(int argc, char **argv)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    int render = argc > 1 && !strcmp(argv[1], "render");

    /* figure table (Fire Air Water Earth, dots per row), standard geomantic figures */
    static const struct { const char *name, *rows; } ref[] = {
        { "VIA", "1111" }, { "CAUDA DRACONIS", "1112" }, { "PUER", "1121" }, { "FORTUNA MINOR", "1122" },
        { "PUELLA", "1211" }, { "AMISSIO", "1212" }, { "CARCER", "1221" }, { "LAETITIA", "1222" },
        { "CAPUT DRACONIS", "2111" }, { "CONJUNCTIO", "2112" }, { "ACQUISITIO", "2121" }, { "RUBEUS", "2122" },
        { "FORTUNA MAJOR", "2211" }, { "ALBUS", "2212" }, { "TRISTITIA", "2221" }, { "POPULUS", "2222" } };
    for (int f = 0; f < 16; f++) {
        char s[5]; dots(f, s);
        CHECK(!strcmp(s, ref[f].rows) && !strcmp(ora_names[f], ref[f].name), "figure %d %s = %s", f, ora_names[f], s);
        CHECK(ora_fig(ora_odd(f)) == f, "fig/odd round trip %d", f);
    }
    /* chart derivation, by hand: Mothers Puer, Amissio, Via, Populus */
    {
        uint8_t m[4] = { 2, 5, 0, 15 }, c[16];
        ora_chart(m, c);
        char s[5];
        /* D1 = Fire rows of M1..M4 = 1,1,1,2 = Cauda Draconis; D2 = Air rows 1,2,1,2 = Amissio;
         * D3 = Water 2,1,1,2 = Conjunctio; D4 = Earth 1,2,1,2 = Amissio */
        CHECK(ora_fig(c[4]) == 1 && ora_fig(c[5]) == 5 && ora_fig(c[6]) == 9 && ora_fig(c[7]) == 5, "daughters %d %d %d %d", ora_fig(c[4]), ora_fig(c[5]), ora_fig(c[6]), ora_fig(c[7]));
        /* N1 = Puer + Amissio: 1+1,1+2,2+1,1+2 = 2,1,1,1 = Caput Draconis; N2 = Via + Populus = Via */
        CHECK(ora_fig(c[8]) == 8 && ora_fig(c[9]) == 0, "nieces 1 2: %d %d", ora_fig(c[8]), ora_fig(c[9]));
        dots(ora_fig(c[14]), s);
        int odd = 0; for (int r = 0; r < 4; r++) odd += s[r] == '1';
        CHECK(!(odd & 1), "judge %s not even", s);
        CHECK(c[15] == (c[14] ^ c[0]), "reconciler");
    }
    /* every cast: Judge even; exactly the 8 even figures can judge */
    {
        int seen[16] = { 0 }, bad = 0;
        for (int v = 0; v < 65536; v++) {
            uint8_t m[4] = { (uint8_t)(v & 15), (uint8_t)(v >> 4 & 15), (uint8_t)(v >> 8 & 15), (uint8_t)(v >> 12) }, c[16];
            ora_chart(m, c);
            int o = c[14], p = (o & 1) + (o >> 1 & 1) + (o >> 2 & 1) + (o >> 3 & 1);
            if (p & 1) bad++;
            seen[ora_fig(o)]++;
        }
        int nj = 0; for (int f = 0; f < 16; f++) nj += seen[f] > 0;
        CHECK(!bad && nj == 8, "judge parity: %d odd judges, %d judge figures", bad, nj);
        CHECK(seen[0] && seen[15] && seen[9] && seen[6] && seen[12] && seen[3] && seen[10] && seen[5], "judges: via populus conjunctio carcer fortunae acquisitio amissio");
    }

    /* defaults: OPEN voicing (Earth root, Water fifth, Air octave, Fire tenth), all lanes on, stopped */
    ora_bank_init(&ora[0]); ora_bank_init(&ora[1]);
    {
        const ora_pat_t *q = &ora[0].pats[0];
        CHECK(q->ivl[3] == 0 && q->ivl[2] == 7 && q->ivl[1] == 12 && q->ivl[0] == 16 && q->used[0] && q->used[3], "default voicing");
        CHECK(ora[1].pats[0].key[3] == 0 && ora[1].pats[0].key[2] == 2 && ora[1].pats[0].key[1] == 4 && ora[1].pats[0].key[0] == 6, "default drum keys");
        run(0, 44100, 12000);
        CHECK(n_on[0] == 0 && !ora[0].running, "started by itself");
    }
    /* a key starts at once, transposed: one bar at 120 bpm, 1/16, held on D4 (62) */
    {
        memset(hits_per_note, 0, sizeof hits_per_note); n_on[0] = 0; first_on = first_off = -1;
        long t0 = sample_now;
        CHECK(ora_key_down(0, 62, 9, 0) == 1 && ora[0].running && !ora[0].latched, "key starts (consumed, not latched)");
        while (ora[0].abs < 15) run(0, 64, 12000);      /* exactly one bar: steps 0..15 */
        run(0, 44100 / 16, 12000);
        int exp[4], got[4] = { hits_per_note[62 + 16], hits_per_note[62 + 12], hits_per_note[62 + 7], hits_per_note[62] };
        lane_hits(ora[0].pats[0].m, exp);
        CHECK(got[0] == exp[0] && got[1] == exp[1] && got[2] == exp[2] && got[3] == exp[3],
              "lane hits %d %d %d %d, expected %d %d %d %d", got[0], got[1], got[2], got[3], exp[0], exp[1], exp[2], exp[3]);
        CHECK(hits_per_note[60] == 0 && hits_per_note[67] == 0, "untransposed notes heard");
        CHECK(first_on - t0 <= 64 * 2, "first step late: %ld samples", first_on - t0);
        while (hanging(0)) run(0, 64, 12000);           /* gate of a later (whole) step: from a silent moment */
        first_on = first_off = -1;
        while (first_off < 0) run(0, 64, 12000);
        double gate = (first_off - first_on) / 44100.0;
        CHECK(fabs(gate - 0.0625) < 0.003, "gate %.4f s (expect 0.0625)", gate);
        CHECK(max_active <= 4, "max simultaneous %d", max_active);
        /* a second key re-transposes in phase; releasing the first changes nothing */
        int32_t a0 = ora[0].abs;
        CHECK(ora_key_down(0, 67, 14, 0) == 1 && ora[0].root == 67, "second key transposes");
        memset(hits_per_note, 0, sizeof hits_per_note);
        run(0, 44100 / 2, 12000);
        CHECK(ora[0].abs - a0 == 4, "re-transposing kept the phase (%d steps in 1/2 s)", ora[0].abs - a0);
        CHECK(hits_per_note[62] == 0 && hits_per_note[62 + 16] == 0 && hits_per_note[67] + hits_per_note[74] + hits_per_note[79] + hits_per_note[83] > 0, "notes follow the new root");
        ora_key_up(0, 9, 0);
        CHECK(ora[0].running && !ora[0].stop_pend, "one key still held: runs");
        /* release: stops at the next step boundary, no notes after, nothing hanging */
        run(0, 44100 / 40, 12000);                       /* into a step */
        ora_key_up(0, 14, 0);
        CHECK(ora[0].running && ora[0].stop_pend, "release: stop pending");
        int on0 = n_on[0];
        run(0, 44100 / 8 + 128, 12000);                  /* past the next boundary */
        CHECK(!ora[0].running && n_on[0] == on0 && hanging(0) == 0, "release stops at the next step (run %d, %d new notes, %d hanging)", ora[0].running, n_on[0] - on0, hanging(0));
        ora_key_up(0, 14, 0);                            /* stray key up: nothing */
    }
    /* a short tap still plays its first step */
    {
        int on0 = n_on[0];
        ora_key_down(0, 60, 7, 0); ora_key_up(0, 7, 0);
        run(0, 44100 / 4, 12000);
        CHECK(n_on[0] > on0 && !ora[0].running && hanging(0) == 0, "tap: %d notes, running %d", n_on[0] - on0, ora[0].running);
    }
    /* latch: Shift + key; a key without Shift takes over; Shift at release latches */
    {
        ora_key_down(0, 60, 7, 1); ora_key_up(0, 7, 0);
        run(0, 44100, 12000);
        CHECK(ora[0].running && ora[0].latched, "Shift + key latches");
        ora_key_down(0, 65, 12, 1); ora_key_up(0, 12, 0);
        run(0, 44100 / 2, 12000);
        CHECK(ora[0].running && ora[0].latched && ora[0].root == 65, "Shift + key re-transposes, stays latched");
        ora_key_down(0, 62, 9, 0);
        CHECK(!ora[0].latched && ora[0].root == 62, "plain key takes over");
        run(0, 44100 / 2, 12000);
        ora_key_up(0, 9, 0); run(0, 44100 / 4, 12000);
        CHECK(!ora[0].running && hanging(0) == 0, "released after take over: stopped");
        ora_key_down(0, 62, 9, 0); run(0, 44100 / 4, 12000); ora_key_up(0, 9, 1);
        run(0, 44100, 12000);
        CHECK(ora[0].running && ora[0].latched, "Shift held at release latches");
    }
    /* Judge's step accented */
    {
        uint8_t c[16]; ora_chart(ora[0].pats[0].m, c);
        n_accent = 0;
        run(0, 44100 * 2, 12000);
        CHECK(!c[14] || n_accent > 0, "judge accent not heard");
    }
    /* Shift + C#: stop (unlatched) / start latched with the last root, at once */
    {
        CHECK(ora_shift_wants(20) && ora_shift_wants(22) && ora_shift_wants(19) && !ora_shift_wants(21)
              && !ora_shift_wants(23) && !ora_shift_wants(24), "Shift shortcut keys");
        CHECK(ora_shift_key(0, 20) == 1 && !ora[0].running && !ora[0].latched && hanging(0) == 0, "Shift+C# stops");
        CHECK(ora_shift_key(0, 20) == 2 && ora[0].running && ora[0].latched && ora[0].root == 62
              && ora[0].start == (uint32_t)(ora[0].tick_q16 >> 16), "Shift+C# starts now, latched, last root");
        ora_shift_turn(0, 3, -1024);
        CHECK(!ora[0].running && !ora[0].latched && hanging(0) == 0, "Shift+red left stops");
        ora_shift_turn(0, 3, 1024);
        CHECK(ora[0].running && ora[0].latched, "Shift+red right starts latched");
    }
    /* voicings: Shift + red press cycles (synth); drum mode: no voicing */
    {
        ora_press(0, 3, 1);
        CHECK(ora[0].voicing == 1 && ora[0].pats[0].ivl[0] == 12 && ora[0].pats[0].ivl[2] == 4 && ora[0].touched == 4 && ora[0].show_ms > 0, "voicing MAJOR");
        for (int k = 0; k < ORA_VOICINGS - 1; k++) ora_press(0, 3, 1);
        CHECK(ora[0].voicing == 0 && ora[0].pats[0].ivl[0] == 16, "voicings wrap");
        int bad = 0;
        for (int v = 0; v < ORA_VOICINGS; v++) for (int l = 0; l < 4; l++) bad |= ora_voicing[v][l] < ORA_IVL_MIN || ora_voicing[v][l] > ORA_IVL_MAX;
        CHECK(!bad, "voicing out of range");
        int m0 = ora[1].pats[0].key[0];
        ora_press(1, 3, 1);
        CHECK(ora[1].voicing == 0 && ora[1].pats[0].key[0] == m0, "drum: no voicing");
    }
    /* lane EDIT: Shift + white press; a key sets the selected lane's interval above the root */
    {
        ora_press(0, 2, 1);
        CHECK(ora_editing(0), "EDIT on");
        ora_shift_turn(0, 2, 2048);                     /* lane Water */
        ora_assign(0, 62 + 3, 10);                      /* root 62: minor third */
        CHECK(ora[0].pats[0].ivl[2] == 3, "interval set: %d", ora[0].pats[0].ivl[2]);
        ora_assign(0, 127, 23); CHECK(ora[0].pats[0].ivl[2] == ORA_IVL_MAX, "interval clamps high");
        ora_assign(0, 0, 0); CHECK(ora[0].pats[0].ivl[2] == ORA_IVL_MIN, "interval clamps low");
        ora_assign(0, 62 + 7, 14);
        ora_press(0, 2, 1);
        CHECK(!ora_editing(0) && ora[0].sel == 2, "EDIT off");
        ora_press(0, 0, 0); ora_press(0, 1, 0);         /* plain presses still re-cast, Shift + blue / green nothing */
        int b4 = ora[0].pats[0].m[0]; ora_press(0, 0, 1); ora_press(0, 1, 1);
        CHECK(ora[0].pats[0].m[0] == b4 && !ora_editing(0), "Shift + blue / green press do nothing");
        ora[0].sel = 0;
    }
    /* turning a Mother changes the chart, wraps 0..15; re-cast */
    {
        int m0 = ora[0].pats[0].m[1];
        ora_turn(0, 1, -1024);
        CHECK(ora[0].pats[0].m[1] == ((m0 + 15) & 15), "turn green: %d -> %d", m0, ora[0].pats[0].m[1]);
        ora[0].pats[0].m[2] = 15; ora_turn(0, 2, 1024);
        CHECK(ora[0].pats[0].m[2] == 0, "wraps 15 -> 0");
        ora_turn(0, 2, 512); CHECK(ora[0].pats[0].m[2] == 0, "half detent ignored");
        int diff = 0;
        for (int k = 0; k < 20; k++) { int b4 = ora[0].pats[0].m[3]; ora_press(0, 3, 0); diff += ora[0].pats[0].m[3] != b4; }
        CHECK(diff >= 12, "re-cast changed the figure only %d / 20 times", diff);
    }
    /* mute lane, mute all; no hanging notes (latched on root 62: Fire = 78) */
    {
        CHECK(ora_shift_key(0, 14) == 1 && ora[0].pats[0].muted[0], "Shift+G mutes Fire");
        memset(hits_per_note, 0, sizeof hits_per_note);
        run(0, 44100 * 2, 12000);
        CHECK(hits_per_note[78] == 0 && (hits_per_note[74] + hits_per_note[69] + hits_per_note[62]) > 0, "mute Fire");
        ora_shift_key(0, 22);
        int all = 1; for (int l = 0; l < 4; l++) all &= ora[0].pats[0].muted[l];
        CHECK(all && hanging(0) == 0, "mute all");
        ora_shift_key(0, 22);
        int none = 1; for (int l = 0; l < 4; l++) none &= !ora[0].pats[0].muted[l];
        CHECK(none, "unmute all");
        CHECK(ora_shift_key(0, 21) == 0 && ora_shift_key(0, 1) == 0, "other keys left for latching");
    }
    /* rate 1/8: 8 steps per 2 s */
    {
        ora_shift_turn(0, 0, -2048);
        CHECK(ora[0].rate == 1 && ora[0].running, "rate 1/8");
        run(0, 44100 / 2, 12000);
        int32_t a0 = ora[0].abs; run(0, 44100 * 2, 12000);
        CHECK(ora[0].abs - a0 == 8, "1/8: %d steps in 2 s", ora[0].abs - a0);
        ora_shift_turn(0, 0, 2048);
        ora_shift_turn(0, 1, 10 * 1024);
        CHECK(ora[0].gate == 100, "gate clamps at 100");
        ora_shift_turn(0, 1, -10 * 1024);
    }
    /* patterns: Shift+G (key 2) -> pattern 2: own mothers / lanes; running continues; back */
    {
        uint8_t m1 = ora[0].pats[0].m[0];
        CHECK(ora_shift_key(0, 2) == 1 && ora[0].pat == 1 && ora[0].running, "pattern 2");
        CHECK(ora[0].pats[1].ivl[2] == 7 && ora[0].pats[1].used[0], "pattern 2 default lanes");
        ora_turn(0, 0, 3 * 1024);
        ora_shift_key(0, 0);
        CHECK(ora[0].pat == 0 && ora[0].pats[0].m[0] == m1 && hanging(0) == 0, "back to P1");
    }
    /* clipboard */
    {
        ora_clip(0, 0, 0);
        ora_shift_key(0, 4);                            /* P3 */
        ora_clip(0, 1, 0);
        CHECK(ora[0].pats[2].ivl[2] == 7 && ora[0].pats[2].m[0] == ora[0].pats[0].m[0], "paste pattern");
        ora[0].sel = 1; ora_clip(0, 2, 0);
        CHECK(!ora[0].pats[2].used[1] && ora[0].pats[2].used[0], "scissors clears selected lane");
        ora_clip(0, 2, 1);
        CHECK(ora[0].pats[2].used[1] && ora[0].pats[2].m[0] == 12 && ora[0].pats[2].ivl[0] == 16, "Shift+scissors resets pattern");
        ora_shift_key(0, 0); ora[0].sel = 0;
    }
    /* MIDI START, stop */
    {
        run(0, 44100 / 3, 12000);
        ora_midi_start(); ora_advance(0, 1u << 14);
        CHECK(ora[0].step == 0, "MIDI START: step 0 on the next tick (%d)", ora[0].step);
        ora_shift_key(0, 20);
        CHECK(!ora[0].running && hanging(0) == 0, "stopped");
    }
    /* persistence */
    {
        char buf[600]; ora_bank_t saved = ora[0];
        int len = ora_serialize(0, buf, sizeof buf);
        CHECK(len > 150 && len < 500, "text length %d", len);
        memset(&ora[0], 0, sizeof ora[0]);
        CHECK(ora_parse(0, buf, len), "parse: %s", buf);
        int same = ora[0].pat == saved.pat && ora[0].rate == saved.rate && ora[0].gate == saved.gate && ora[0].root == saved.root
                   && ora[0].voicing == saved.voicing && !ora[0].running && !ora[0].latched;
        same &= !memcmp(ora[0].pats, saved.pats, sizeof saved.pats);
        CHECK(same, "round trip differs");
        CHECK(!ora_parse(0, "1,2,3", 5) && !ora_parse(0, "a", 1), "bad text accepted");
        CHECK(host_ora_changed > 0, "changes not reported");
    }
    /* drum mode: any key plays the lanes' own sounds; EDIT sets a lane's sound; two lanes on one sound -> one note */
    {
        memset(hits_per_note, 0, sizeof hits_per_note);
        ora_key_down(1, 53 + 13, 13, 0);                /* F#: does not matter */
        run(1, 44100 * 2, 12000);
        int other = 0; for (int n = 0; n < 128; n++) if (n != 53 && n != 55 && n != 57 && n != 59) other += hits_per_note[n];
        CHECK(hits_per_note[53] > 0 && hits_per_note[59] > 0 && !other, "drum lanes play F G A B (other notes %d)", other);
        ora_key_up(1, 13, 0); run(1, 44100 / 4, 12000);
        CHECK(!ora[1].running && hanging(1) == 0, "drum release stops");
        ora_press(1, 2, 1); ora[1].sel = 0; ora_assign(1, 53, 0); ora_press(1, 2, 1);   /* Fire = F, as Earth */
        CHECK(ora[1].pats[0].key[0] == 0 && ora[1].pats[0].note[0] == 53, "drum lane assign");
        ora[1].pats[0].m[0] = 0; ora[1].pats[0].m[1] = 0;   /* step 1 = Via: all rows odd */
        n_on[1] = 0; memset(active[1], 0, sizeof active[1]);
        ora_shift_key(1, 20); ora_advance(1, 1u << 14);
        CHECK(n_on[1] == 3 && active[1][53] == 1, "duplicate lanes: %d note-ons", n_on[1]);
        run(1, 44100, 12000); ora_shift_key(1, 20);
        CHECK(hanging(1) == 0 && ora[0].pats[0].ivl[3] == 0, "drum bank independent / hanging");
    }
    /* synth: two lanes on one interval -> one note */
    {
        ora_bank_t *b = &ora[0];
        b->pats[0].ivl[0] = b->pats[0].ivl[1] = 12; b->pats[0].m[0] = 0; b->pats[0].m[1] = 0;
        n_on[0] = 0;
        ora_key_down(0, 60, 7, 0); ora_advance(0, 1u << 14);
        CHECK(n_on[0] == 3 && active[0][72] == 1, "synth duplicate interval: %d note-ons", n_on[0]);
        ora_key_up(0, 7, 0); run(0, 44100, 12000);
        CHECK(!b->running && hanging(0) == 0, "stopped, nothing hanging");
    }
    if (!render) { printf(fails ? "%d FAILED\n" : "OK: all oracle checks pass\n", fails); return fails != 0; }

    /* ---- screen frames ---- */
    memset(ora, 0, sizeof ora);
    frame("stopped: default Mothers, OPEN voicing from C4", 16, 0);
    ora_key_down(0, 62, 9, 0); run(0, 44100 * 7 / 10, 12000);
    frame("D4 held: playing (HOLD), lanes = notes above D4", 2000, 0);
    ora_turn(0, 1, 3 * 1024);
    frame("green turned: Mother 2 name shown", 16, 0);
    ora_press(0, 3, 1); ora_press(0, 3, 1); ora_press(0, 3, 1); ora_press(0, 3, 1); ora_press(0, 3, 1);
    frame("Shift + red pressed 5x: voicing MIN7", 16, 0);
    ora_key_down(0, 65, 12, 1); ora_key_up(0, 9, 0); ora_key_up(0, 12, 0); ora[0].show_ms = 0;
    while (ora[0].step != 14) run(0, 64, 12000);
    frame("latched on F4 (Shift + key), Judge step (accent)", 2000, 0);
    ora_press(0, 2, 1); ora_shift_turn(0, 2, 1024); ora_shift_key(0, 18);
    frame("EDIT (Shift + white press): intervals, Air selected, Water muted", 2000, 0);
    ora_press(0, 2, 1); ora_shift_key(0, 20);
    ora_key_down(1, 60, 7, 1); ora_key_up(1, 7, 0); run(1, 44100 / 3, 12000);
    frame("drum mode, latched: lanes = drum keys F G A B", 2000, 1);
    ora_shift_key(0, 6); ora[0].pats[3].m[0] = 0; ora[0].pats[3].m[1] = 15; ora[0].pats[3].m[2] = 7; ora[0].pats[3].m[3] = 12;
    ora_shift_turn(0, 0, 1024);
    frame("stopped, pattern 4 (Via Populus Laetitia Fortuna Major), 1/16T", 2000, 0);
    return 0;
}

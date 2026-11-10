/* Host test of the sequencers' note output (seq/seq_core.c) and the synth octave:
 *   the note-on adds the live synth octave to a stored synth pitch (op1_seq_key_note stores octave 0), so a running
 *   sequence follows the octave arrows; a held note keeps its number across an octave step (the queued renumbering
 *   is dropped while one of ours is the synth sequencer), so its note-off still finds it; drums keep their pitch.
 *   cc -DOP1_HOST -fsanitize=address,undefined -I.. seqcore_test.c -o build/seqcore_test */
#include "../seq/seq_core.c"
#include <stdio.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* VoiceCtl stand-in: the held note numbers, and R246's queued renumbering (0x01A860C0: every held note + 12 a step) */
int host_synth_oct, host_drum_oct;
int32_t host_oct_queue;
static int ours;                                       /* one of our sequencers is the synth sequencer */
int seq_ours_live(void) { return ours; }
static int held[128], last_param;
void host_vc_down(int32_t *n, int param) { held[n[0]]++; last_param = param; }
void host_vc_up(int32_t *n) { if (n[0] >= 0 && n[0] < 128 && held[n[0]]) held[n[0]]--; }
static void step(int steps)                            /* an octave arrow while the sequencer runs */
{
    host_synth_oct += steps;
    eu_fw_octave_queue(steps);
}
static void seq_tick(void)                             /* R246 applies the queue before the next player tick */
{
    int d = host_oct_queue * 12, h[128] = { 0 };
    host_oct_queue = 0;
    for (int i = 0; i < 128; i++) if (held[i] && i + d >= 0 && i + d < 128) h[i + d] = held[i];
    for (int i = 0; i < 128; i++) held[i] = h[i];
}
static int any_held(void) { for (int i = 0; i < 128; i++) if (held[i]) return 1; return 0; }

int main(void)
{
    /* a running sequence follows the octave: the stored pitch (octave 0) plays at the current synth octave */
    ours = 1;
    int note = op1_seq_key_note(0, 7, 1, 0x019AC100);                    /* key 7 recorded at octave +1 */
    CHECK(note == 0x35 + 7, "stored at octave 0: %d", note);
    host_synth_oct = 1;
    int id = eu_fw_note_on(0, note, 7, 100);
    CHECK(id == 0x35 + 7 + 12 && held[id], "plays as recorded: %d", id);
    eu_fw_note_off(id);
    step(1); seq_tick();
    id = eu_fw_note_on(0, note, 7, 100);
    CHECK(id == 0x35 + 7 + 24 && held[id], "next note an octave up: %d", id);
    eu_fw_note_off(id);
    CHECK(!any_held(), "nothing left");

    /* a USB MIDI note sounds as played at the octave it was recorded at (PR #29) */
    host_synth_oct = 2;
    note = op1_seq_key_note(0, 60 - 0x35, 2, 0x01A85BF4);
    id = eu_fw_note_on(0, note, 0, 100);
    CHECK(id == 60, "MIDI note plays its pitch: %d", id);
    eu_fw_note_off(id);

    /* a held note keeps its number across an octave step, so its note-off finds it (before: stuck in POLY) */
    host_synth_oct = 0;
    id = eu_fw_note_on(0, 0x35 + 7, 7, 100);
    step(1); seq_tick();
    CHECK(held[id] && host_oct_queue == 0, "held note not renumbered");
    eu_fw_note_off(id);
    CHECK(!any_held(), "held note released after the step");
    step(-2); seq_tick();                                                 /* down two: same */
    id = eu_fw_note_on(0, 0x35 + 7, 7, 100);
    CHECK(id == 0x35 + 7 - 12, "octave -1: %d", id);
    step(1); seq_tick();
    eu_fw_note_off(id);
    CHECK(!any_held(), "released after a step up");

    /* a stock sequencer: R246 keeps its queue (its players release by key at the current octave) */
    ours = 0; host_synth_oct = 0;
    step(1); step(-2);
    CHECK(host_oct_queue == -1, "stock: queued %d", host_oct_queue);
    host_oct_queue = 0;

    /* out of range after the octave add: not played */
    ours = 1; host_synth_oct = 4;
    CHECK(eu_fw_note_on(0, 0x35 + 23 + 12, 23, 100) == -1 && !any_held(), "past 127 not played");
    host_synth_oct = 0;

    /* drums: the pad at its own pitch, the synth octave does not matter (PR #28) */
    host_synth_oct = 3; host_drum_oct = 1;
    note = op1_seq_key_note(1, 5, 1, 0);                                  /* pad 5 at drum octave +1 */
    id = eu_fw_note_on(1, note, 5, 100);
    CHECK(id == 0x35 + 5 && last_param == 0, "drum pad: id %d param %d", id, last_param);
    host_drum_oct = 0;
    eu_fw_note_off(eu_fw_note_on(1, note, 5, 100));
    CHECK(last_param == 12, "drum pad one up at drum octave 0: %d", last_param);
    eu_fw_note_off(id);
    CHECK(!any_held(), "drums released");

    if (fails) { printf("seqcore: %d failed\n", fails); return 1; }
    printf("seqcore: ok\n");
    return 0;
}

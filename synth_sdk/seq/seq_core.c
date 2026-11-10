/* Core part of the sequencers (the sequencers themselves are plugin files, docs/44): what they and other core code
 * share. The one firmware copy of seq_text.h (stroke text, lines, oracle / peal DB persistence; exported to plugins),
 * the VoiceCtl note output of euclid / dots / oracle / peal, and the "start with REC held" flag dispatcher.c turns
 * into a PLAY press. */
#include "../op1_synth.h"
#define OCC_TEXT_IMPL
#include "seq_text.h"

volatile uint8_t eu_rec_play;                           /* Shift + C# started a sequencer with REC held */

/* notes go through VoiceCtl like the stock sequencers (ok_player). The synth octave is a live setting, as on the
 * stock sequencers: a slot stores a synth pitch at octave 0 (op1_seq_key_note) and the note-on adds the current synth
 * octave, so the octave arrows (and a slot change) move what a sequencer plays from its next note on. Drum slots store
 * the absolute pitch (key + 0x35 + drum octave * 12).
 *  synth: record {midi = note + synth octave * 12, vel, send midi = 1, 0} -> key_down 0x01A85750 -> note_on_synth
 *         (uses midi as is). The returned id is that midi: the note-off releases the number the note-on started.
 *  drums: note_on_drum takes the SOUND from record midi - 0x35 (key 0..23) and the PITCH from the param: the pad
 *         plays 2^(param / 12) times its own pitch, and key_down adds the current drum octave * 12 to the param. A
 *         pad key sends param 0 (the stock players send octave offsets only), so key + 0x35 + octave * 12 is the
 *         pad's unshifted pitch -> param = note - (key + 0x35) - current drum octave * 12 (0 at the octave the note
 *         was recorded at). */
#ifdef OP1_HOST                                                    /* harness/seqcore_test.c */
void host_vc_down(int32_t *n, int param);
void host_vc_up(int32_t *n);
extern int host_synth_oct, host_drum_oct;
extern int32_t host_oct_queue;
#define VC_DOWN   host_vc_down
#define VC_UP     host_vc_up
#define DRUM_OCT()  host_drum_oct
#define SYNTH_OCT() host_synth_oct
#define OCT_QUEUE host_oct_queue
#else
#define VC_DOWN   ((void (*)(int32_t *, int))0x01A85750)
#define VC_UP     ((void (*)(int32_t *))0x01A85840)
#define DRUM_OCT()  (((const int *)0x01DFE500)[*(volatile int *)0xFF8016D8 & 7])
#define SYNTH_OCT() (((const int *)0x01DFE4E0)[*(volatile int *)0xFF8016D4 & 7])
#define OCT_QUEUE (*(volatile int32_t *)0x018B6208)                /* PlayerManager: queued synth octave steps */
#endif
int eu_fw_note_on(int pos, int note, int key, int vel)
{
    int midi = pos ? key + 0x35 : note + SYNTH_OCT() * 12, param = pos ? note - (key + 0x35) - DRUM_OCT() * 12 : 0;
    if (midi < 0 || midi > 127) return -1;
    int32_t n[4] = { midi, vel, 0, 0 };
    ((uint8_t *)n)[8] = 1;
    VC_DOWN(n, param);
    return midi;                                                   /* note-off uses this */
}
void eu_fw_note_off(int midi)
{
    int32_t n[4] = { midi, 0, 1, 0 };
    ((uint8_t *)n)[8] = 1;
    VC_UP(n);
}

/* Synth octave and slot steps while the sequencer runs (synth_octave_step 0x01A86158 and slot select, each a CALL
 * 0x01A0CEFC at 0x01A86236 / 0x01A86578; hook.S hook_seqoct0 / 1). R246 adds the steps to 0x018B6208 and the
 * sequencer thread renumbers every held VoiceCtl note by 12 a step before the next player tick (0x01A860C0: MIDI out
 * off / on, the voice keeps its pitch). The stock players release by key at the current octave, so they find the new
 * number; our sequencers release by the number they started (eu_fw_note_off), so in POLY a renumbered note was never
 * released. With one of ours as the synth sequencer the step is dropped: held notes keep their numbers until their
 * note-off, and the next notes get the new octave (eu_fw_note_on). A keyboard key held through the step is not
 * renumbered either, and the stock key_up releases it at the new octave (0x01A85014), so it keeps sounding. */
int seq_ours_live(void);                                           /* dispatcher.c */
void eu_fw_octave_queue(int steps)
{
    if (!seq_ours_live()) OCT_QUEUE += steps;
}

# brassophone

A brass synth engine based on a video by Eric Bowman. Brass instruments sweep up in timbre and pitch during their
attack, and skilled players add vibrato during the sustain. Graphics hand drawn, inspired by Dr Seuss.

![brassophone](../../display/brassophone_preview_single.png)

## Knobs

| encoder | function |
|---|---|
| blue | starting timbre: up = pure sine, down = the brightest start that still leaves room for the sweep |
| green | timbre sweep: how far the attack of the envelope pushes the timbre up from the start |
| white | vibrato depth, 0 .. ~90 cents (squared curve) |
| red | vibrato rate, 0 .. 32 Hz (squared curve) |

For the sultriest horns, slow the attack down a little (T2) and add a fat reverb.

## How it works

- **Oscillator.** A sine whose phase is warped by an exponential curve (`gen_warp_table.py`). With no warp it is a
  pure sine; with more warp the whole cycle is squeezed into a short burst in the middle of the period, which
  sounds brighter and buzzier. The warp amount is `start + sweep * envelope`, read from a 33 x 65 table (half of
  the point-symmetric warp) with bilinear interpolation, then a fixed one-pole low-pass (~5 kHz).
- **Pitch.** The note starts ~47 cents flat and bends up into tune with the attack of the stock envelope (in tune
  from half scale). Once the envelope stops rising the note stays in tune: a soft note whose envelope peaks lower
  settles into tune within ~25 ms, and decay and release do not bend the pitch.
- **Vibrato.** Fades in only after the envelope has held still for ~23 ms (sustain) while the key is down, and
  fades out on release.
- **High notes.** The full warp aliases on high notes. From B3 up, the largest warp shrinks linearly with the note
  (`WARP_CUT_PER_SEMI` in `brass.c`: 1.5 of the 64 warp steps per semitone, a plain sine from about F7). The sweep
  still moves on high notes, over a smaller range.
- **Level.** The warp keeps the peak but lowers the RMS (by up to 11 dB). `warp_gain` makes up half of that drop in
  dB (`LEVEL_COMP` in `gen_warp_table.py`); full compensation would clip, and a brighter tone already sounds
  louder at equal RMS.

Regenerate the tables after changing `gen_warp_table.py`: `python3 gen_warp_table.py` (it writes `warp_table.h`
itself).

## Measurements (host build)

- Tuning: sustain within 0.1 cent at any envelope level; release unchanged.
- Folded (aliased) energy at full warp, sustained note: below -37 dB from C3 to C8 (worst around D#6). Before the
  high-note limit it reached -4 dB at C7.
- Level at full warp: -6 dB RMS relative to the pure sine (was -11 to -17 dB). Peak per voice at full velocity up
  to about -1 dBFS for heavy warp, half scale for a pure sine.
- Device build: 0 libgcc imports; engine code 5.8 KB.

`brassophone.wav`: the `audio.py` phrase with the default knobs.

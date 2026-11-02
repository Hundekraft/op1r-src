# compactor

A clean RMS compressor for the OP-1 FX slot. The screen labels the four knobs, shows their values,
the current gain reduction and the automatic make-up. A rendered demo with the default settings is in
`compactor_audio.wav`.

| knob | parameter | range |
|---|---|---|
| blue | ratio | 1:1 .. 20:1 (log) |
| green | rms | threshold, -60 .. 0 dBFS RMS |
| white | attack | 0.5 .. 100 ms (log) |
| red | release | 20 ms .. 2 s (log) |

The green knob is the **RMS level where compression starts**. The detector is a true mean-square
(RMS) one-pole with a 2 ms window, so it responds to energy rather than individual peaks. A 6 dB soft
knee around the threshold keeps the onset transparent.

Auto make-up adds back the full slow (about 300 ms) average gain reduction, capped at +6 dB and
never above the current gain reduction, so it cannot push the signal past unity when only a little
compression is happening. A transient that the attack lets through is therefore not boosted, while
sustained compression keeps its level. There is no separate make-up knob; the screen shows the
current value as `AUTO`.

In the master FX position the detector is linked across left and right (the same gain is applied to
both channels). In the synth and drum FX positions it is mono.

## Screen

`compactor.svg` holds the frame and the two separators. The plugin draws the title, the four knob
labels (`RATIO`, `RMS`, `ATTACK`, `RELEASE`), their values, a position bar under each label, the
current gain reduction (`GR`) and the auto make-up (`AUTO`). The stroke font has no lowercase
letters, so the labels are uppercase.

## CPU

The RMS detector and gain ramp run per sample; the logarithm / exponential gain computer runs every
16 samples (0.36 ms), and the knob conversion runs once per 64-sample block. The make-up is folded
into the ramped gain at the control rate, so the audio loop has no division, no float and no 64-bit
arithmetic. On the Blackfin the DSP is about 2 KB of code and 144 bytes of state for all three FX
positions; the screen adds about 2 KB of code.

## Testing on a PC

```sh
cd synth_sdk
python3 harness/audio.py compactor                       # -> harness/out/audio_compactor.wav
python3 harness/audio.py compactor --knobs 0,32767,16384,16384   # 1:1, no compression
python3 harness/audio.py compactor --knobs 32767,16000,8000,10000 # 20:1, heavy
python3 harness/render.py compactor                      # -> display/compactor_preview.png
```

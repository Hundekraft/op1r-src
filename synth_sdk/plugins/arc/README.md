# arc: Duddell's singing arc

Carbon arc street lamps in 1890s London hissed and hummed. In 1899 William Duddell shunted an arc lamp with a
capacitor and an inductor. The arc's negative resistance made the circuit oscillate, and with a keyboard switching
the capacitors he played tunes on it: the "singing arc", one of the first electronic instruments. `arc` models each
voice as a Van der Pol oscillator (a "lamp"), with plasma noise, a striking transient and a second lamp coupled to
the first.

## Knobs

| page | knob | function |
|---|---|---|
| 1 | blue | current μ: clean sine → reedy buzz → screaming relaxation oscillation (ε = 0.04 … 7) |
| 1 | green | carbon: asymmetric electrode, adds a 2nd harmonic / even harmonics (the DC it creates is blocked) |
| 1 | white | hiss: noise injected into the plasma. The pitch wanders at high values (up to ~±25 cents slow wander), and broadband hiss rides on the arc current |
| 1 | red | strike + sputter: pitch sag on attack (up to −12 semitones, recovery 50 ms … 1 s) plus an ignition noise burst; while a key is held, the lamp re-strikes at random (0 … 6 per second): a pitch dip, crackle and a short dropout |
| 2 (press) | blue | 2nd lamp ratio 1/2 … 2, detents at 1/2 2/3 3/4 1 5/4 4/3 3/2 5/3 2 (unset = 1:1) |
| 2 (press) | green | coupling: 0 = 2nd lamp off; weak = beats / locking; strong (with high μ) = chaos |
| 2 (press) | white | brightness: key-tracked tilt, dark … bright (unset = neutral) |

Screen: a phase portrait of the most recent voice is drawn behind the two carbons, as a blue comet trail. It is a
circle for a sine and turns into the squarish relaxation loop as μ rises (shown in Liénard coordinates when μ is
large). The arc between the tips grows and flickers with the output level and gets jagged with hiss. The tips glow
green with carbon, and the positive tip burns a crater. A strike throws red sparks. While coupling is on, the 2nd
lamp's portrait is drawn in green and its ratio appears top right. Dials that are on page 2 get a ring.

## How it stays in tune and stable

The oscillator runs in its own time τ = ωt, so the waveform is the same on every key. It is integrated with
semi-implicit Euler, in fixed point: x is Q29 and v is Q27, both saturating, and every product is a 16×16 MAC.
The limit-cycle period grows with μ, and at high notes the discretisation shifts it again. `gen_tables.py`
therefore runs this very C code (`arc_calib.c`) and solves for the step factor G on a 33 (μ) × 97 (1/8 octave)
grid. This builds `ARC_GP` in `arc_tables.h` and a loudness table, so the table matches the integer code exactly.

At high notes h·ε is capped at 0.3, so the lamp gets cleaner instead of unstable, which also limits aliasing. The
per-step damping is clamped at −0.75. Coupling is dissipative, k(v2 − v1). It adds no stiffness, so the played
note keeps its pitch at every ratio, and it pulls the lamps into phase. A spring coupling would lock relaxation
oscillators in anti-phase, where the mix cancels. The 2nd lamp runs at half rate to save CPU, with its own
calibrated table lookup.

Measured on the host (FFT of the real output, off-grid notes and μ values):
- tuning is within ±2.4 cents for MIDI 24–96 at all μ; residuals at the table nodes are ≤ 0.8 cents up to 5.5 kHz
- the 2nd lamp is within ±0.2 cents at weak coupling
- loudness across μ stays within 1.3 dB
- peaks are −6 dBFS at defaults and −2.4 dBFS with every knob at max; nothing saturates in 400 random fuzz runs
  (UBSan, MIDI −10…135, vibrato, negative page words)

## CPU (Blackfin, `bfin-elf-gcc -O2`, instructions per sample from objdump ≈ cycles)

| configuration | pass 1 (lamps) | pass 2 (shaping) | per-block share | total per voice-sample | 6 voices at 400 MHz |
|---|---|---|---|---|---|
| coupling 0 (one lamp) | 50 | 39 | ~12 | ~101 | ~6.7 % |
| coupling > 0 (two lamps) | 87.5 (175 per 2 samples) | 39 | ~12 | ~139 | ~9.2 % |

Divisions (`__divsi3`) only happen per block: voice index, ratio, gain, brightness and portrait scale. There is
no division or 64-bit maths in any sample loop.

## Known limitations

- Above ~6 kHz the step factor saturates (h ≤ 0.9), so those notes go flat. High notes are also cleaner by design,
  because μ is capped there.
- Strong coupling pulls the pitch (by up to a few hundred cents) and can go chaotic. This is intended, but it
  is not "in tune".
- The strike sag and pitch bends are applied per 128-sample block. The steps are phase-continuous and stay under
  10 cents.
- With 2 lamps the worst case is ~9 % CPU, a little above the 8 % target.

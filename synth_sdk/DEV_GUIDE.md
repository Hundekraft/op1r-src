# Plugin development guide: lessons from the stock OP-1 engines/effects and op1r plugins

This builds on `PLUGINS.md`, which covers the API, `plugin.json` and the rules. Read that first. This file
collects the techniques worth copying, the tricks, and the traps. The material comes from three places:

| Source | Where | What it is |
|---|---|---|
| Stock TE effects (readable, bit-exact verified C) | `D:\Projects\op1_246\decomp\src\effects\readable\*.c`, `fx_common.h` | delay, filter, cwo, grid, nitro, phone, punch, spring |
| Stock TE synth/drum engines (Ghidra C) | `D:\Projects\op1_246\decomp\src\engines\*.c`, overview in `ENGINES.md` | fm, digital, string, phase, pulse, voltage, iter, cluster, dna, drwave, dsynth, sampler, drum, dbox |
| op1r plugins | `synth_sdk/plugins/*` | supersaw, fmbell, mixtur, arc, aether, reedie, brassophone, palme, sitting, reverb, tape, clouds, compactor, steve, scope-b, snake, ... |

References look like `filter.c:189` (firmware effects folder), `string.c:375` (firmware engines folder) or
`plugins/mixtur/mixtur.c:170` (this repo). The engine files are raw Ghidra output; the lifted assembly in
`op1_246\decomp\src\asm\code\` is the ground truth when the C is unclear.

---

## 1. Platform cheat sheet

| Thing | Value |
|---|---|
| CPU | Blackfin BF52x, one core shared by instruments, FX, tape and UI. No FPU. 16x16 MAC is fast; 32x32 multiply is several instructions; division and 64-bit math are libgcc calls |
| Sample rate | 44100 Hz |
| Synth block | 128 samples, `render(self, out[128], params)`, 6 voices, called once per voice |
| FX block | 64 samples in place, `render(pos, buf[64], params)`; `pos` 0 synth, 1 drum, 2 master |
| Synth state | 32 bytes per voice (bytes 4..0x23 of a 0x24 object; word 0 is the firmware's). Shared with stock iter when switching engines |
| FX memory | `op1_fx_scratch(pos)`: 176460 bytes per position, **starts with foreign data** |
| More memory | static arrays (bss): up to 2 MB per plugin, 4 MB for all plugins |
| Pitch | `OP1_PITCH_INC[n]` is a **per-sample array** (128 entries), 2^32 = one cycle. Read `[n]` in the loop to get sample-accurate glide/bend/vibrato |
| Knobs | synth `params[0..3]` 0..32767; FX raw words, convert with `op1_fx_param_q15(p, i)`; `params[5..7]` page-2 words |
| Tables | `OP1_SINE_TABLE`: 8192 x int16, one cycle |
| Envelope | `OP1_VOICE_ENV[n]`: the stock ADSR of this voice (velocity and level included), valid only during `render`; the firmware still applies it afterwards |

What stock TE code relies on that **plugins don't get**: the boot-built coefficient tables (`0x0106a5ec`,
`0xff801f50`), the 7x32 sinc kernel at `0x0106a730`, L1 scratch buffers (`0xff801b50`, `0xff801e50`),
the shared LCG seed at `0xff802bc8`, and anything in encrypted L1. Generate your own tables (section 9).

---

## 2. Workflow

1. `cp -r template/synth plugins/<name>` (or `template/fx`); rename the file, symbol and `name`.
2. Prototype hard DSP in Python/numpy first if it is a model you don't know well (`harness/aether_proto.py`).
   Write a short spec with the knob map and a decision log for anything non-trivial (`plugins/reedie/REEDIE_SPEC.md`).
3. Iterate with `python3 harness/audio.py <name> --knobs a,b,c,d`. It builds with ASan + UBSan, so
   out-of-bounds writes and signed overflow show up on the PC instead of locking the OP-1.
4. Add a `harness/<name>_test.c` for what `audio.py` doesn't cover (section 14).
5. Preview the screen: `python3 harness/render.py <name> out.png`.
6. Build the file: `python3 make_plugin.py plugins/<name>`. **Read the import list it prints**: any libgcc
   import (`__divsi3`, `__muldi3`, `__divdi3`, soft-float) is either a per-sample cost you missed or a link
   failure (the linker refuses imports `plugload.c` doesn't export).
7. Claim a number in `plugins/ORDERS.md` only when shipping as a built-in. Never renumber or reuse one.
8. Commit a WAV from `audio.py`, a preview PNG if there's a screen, and a README (knobs, how it works,
   measured CPU, known limitations).

Plugins that are good to read first: `template/` (minimal), `supersaw` (synth + screen), `tape` (FX + screen),
`mixtur` (polyBLEP, SVF, tables, voice-stomp detection), `arc` (inline MAC asm, compressed tables, calibration),
`palme` (incremental scratch clear, Karplus-Strong), `sitting` (beat clock, fuzz test), `compactor` (stereo master).

---

## 3. State and memory

### Synth voices

- **Assert the size at compile time** (`plugins/arc/arc.c:42`, `plugins/reedie/reedie.c:68`):
  ```c
  typedef struct { uint32_t vtable; uint32_t ph1, ph2; int32_t lp; /* ... */ } my_voice_t;
  typedef char my_voice_fits[sizeof(my_voice_t) <= OP1_VOICE_SIZE ? 1 : -1];
  ```
- **Pack slow state narrow.** `uint8_t` counters and gains, `int16_t` filter states (saturate on write-back), a
  spare bit as a flag (`plugins/mixtur/mixtur.c:36-42`). The stock engines repeat one 8-byte pattern per oscillator:
  `{uint32 phase; int16 last_out; int16 spare}`. digital fits 3 oscillators in 0x20 bytes.
- **Anything bigger goes in a static array indexed by voice** (`plugins/aether/aether.c:34`,
  `plugins/reedie/reedie.c:70`). Stock engines do the same: string has a 48 KB delay line per voice, drwave a
  per-voice wavetable.
  ```c
  static int16_t ring[OP1_VOICES][4096];
  int vi = op1_voice_index(self); if (vi < 0 || vi >= OP1_VOICES) vi = 0;
  ```
- **Voice memory gets stomped** by the stock iter and other engines. Either reset everything in `note_on`, or
  keep a static signature per voice and start clean on a mismatch (`plugins/mixtur/mixtur.c:57-64,109-119`). The
  signature lets a retrigger keep phase and filter state when the state is still yours.
- **Copy state to locals, run the loop, write back** (`template/synth/mysynth.c`). GCC keeps locals in
  registers; `v->x` in the loop costs a load/store per access.

### FX positions and scratch

- `static state_t st[3]` and `&st[pos % 3]` everywhere. The same FX can run on synth, drum and master at once.
- `op1_fx_scratch(pos)` can be null: check it (`plugins/reverb/reverb.c:58`).
- **Never trust the scratch contents, and don't clear it all in one call.** `init` can run inside the audio
  interrupt, and 176 KB of writes is a long block. Three patterns that work:
  1. **Track what's valid.** Keep a `filled` count and never read further back than it (`plugins/tape/tape.c:22,81,97`).
     The stock cwo delay outputs 0 until its write head has wrapped once (cwo.c:574-576).
  2. **Clear incrementally from render** while passing dry audio: palme clears 1/8 per block
     (`plugins/palme/palme.c:194-212`), snake 4096 samples per block, stock spring a branch per call, stock
     string 1.5 K samples per block over 16 blocks (string.c:232-247).
  3. **Clear only what you're about to use** and let a cursor run ahead of the write head (stock grid's clear
     cursor, grid.c:266-279).
  Stock grid memsets 270 KB in attach. Don't copy that.
- **Lay out several delay lines from one base pointer** with a `const` table of `{offset, length, tap}`
  (stock spring, spring.c:109-113). palme uses a cache-line-aware stride.
- Put large buffers for the master in bss if the scratch is too small (`plugins/steve/steve.c:14-15`).

---

## 4. Per-block vs per-sample

The rule every stock engine/effect follows: **per sample only MACs, adds, shifts, compare-and-subtract and table
reads.** Everything else happens per block:

- reading knobs and mapping them through curves/tables;
- coefficient computation (one division, one 64-bit op, a Newton reciprocal, `exp2` polynomials);
- pitch-derived values: stock string and dna compute the period `2^48 / PITCH_INC[0]` with one 64-bit divide
  per block; mixtur computes its polyBLEP reciprocal once per block (`plugins/mixtur/mixtur.c:170-173`);
- envelope steps, peak decay, meter writes, LFO seeding;
- expensive regeneration spread over blocks: stock drwave rebuilds the wavetable for **one voice per block**
  (`voice == frame % 6`), so the cost is constant.

**Two-pass blocks** keep each inner loop tight: write an intermediate block into a local `int32_t tmp[64]`, then
run the second stage over it (stock nitro runs filter 1 then filter 2; stock digital builds a per-sample
increment array first; dbox/dsynth precompute a 128-entry pitch-swept increment array). Stock fm renders
**one operator per pass** over the whole block, and cluster renders one oscillator per pass and accumulates
into `out` with saturation, so each pass has its state in registers.

**Budget.** About 7-9 % of the CPU for 6 voices is what the op1r synths aim for (arc README, mixtur README,
palme ~4 %). Estimate by counting instructions per sample in `bfin-elf-objdump` at -O2, then check the resource
monitor on hardware (clock screen + T1). Measure before optimising: a 16x16 MAC rewrite of the reverb gained
nothing because it was memory-bound (commit 271d09e).

---

## 5. Fixed-point toolkit

### Formats in use

- Audio: Q15 int16. Filter states and accumulators: Q31 int32 (or int32 with extra fractional bits).
- Phases: uint32, 2^32 = one cycle. Read/write positions: 16.16 samples (stock delay, phone, string).
- Coefficients: Q15, or Q31 for low-cutoff one-poles that need the precision.

### Multiplies

```c
/* Q15 x Q15 */
static inline int32_t mul15(int32_t a, int32_t b) { return (a * b) >> 15; }   /* = op1_mul16: truncates, no -1*-1 sat */

/* Q31-ish 32-bit a times Q15 b without 64-bit math (stock digital, cluster):
 * exact except the a = b = most-negative edge */
static inline int32_t mul32x15(int32_t a, int32_t b)
{ return (a >> 16) * b * 2 + (((a & 0xffff) * b) >> 15); }
```
The simpler form you'll see in plugins is `(i >> 15) * k` for scaling a phase increment by a Q15 factor
(template: `ph2 += i + (i >> 15) * (detune - 32768)`; aether: `((i>>16)*m<<2) + ((i&0xFFFF)*m>>14)`). Write the
bound next to each multiply that could overflow ("|x| < 2^16, g < 2^15: 32 bit"), as the op1r plugins do.

- **Use the 16x16 MAC on device.** Plain C often makes GCC use the multi-cycle 32-bit multiply. arc and mixtur use
  inline asm with a C fallback for host builds (`plugins/arc/arc.c:47-67`); palme uses the fract builtins
  (`plugins/palme/palme.c:101-123`). Do this only in the hot loop and only after measuring.
- **Count-leading-zeros** (Blackfin `norm` builtin) replaces log loops (`plugins/arc/arc.c:80-85`) and seeds
  reciprocals (stock drwave: CLZ-indexed 8-entry seed + two Newton steps `y = y*(2 - x*y)`).
- 64-bit products: if you need them per sample, split them (clouds does an exact 32-bit split and a host
  `CLOUDS_CHECK` build asserts it matches int64, `plugins/clouds/clouds_dsp.c:24-36`).

### Saturation rules

- **Saturate every add in a feedback path** (stock code uses saturating adds everywhere). Plain wrapping is
  only for phases and indices.
- `op1_sat16` before any int16 store.
- Pre-halve for headroom before summing (`>>1`, `>>2`), then make up gain with a saturating shift at the end.
  Stock nitro feeds its ladders x/4; stock cwo feeds its loop x/2.
- `abs` of -32768 is -32768: stock phone/punch get this wrong. Use `x == -32768 ? 32767 : (x < 0 ? -x : x)`.

### Rounding in feedback loops

`>>` rounds toward minus infinity. In a recirculating loop that biases toward a DC limit cycle; rounding to
nearest leaves a low-level limit cycle. **Truncate toward zero** so tails die (`plugins/reverb/reverb.c:29-31`):
```c
static inline int32_t mul15_tz(int32_t x, int32_t g) { int32_t p = x * g; return p >= 0 ? p >> 15 : -((-p) >> 15); }
```
For nearly lossless loops (strings, combs), also force decay at low level or clear the history of a dying voice
(`plugins/palme/palme.c:285-299`). Stock cwo snaps values to their target when close to stop endless creep.

### Avoiding division

- Ring wrap: power-of-2 length and `& mask` (stock cwo `& 0xffff`), or compare-and-subtract (stock delay).
- Constant divisors compile to multiplies; `/ 3` per sample is fine, `/ n` with variable `n` is not.
- Variable divisors: reciprocal per block, per event (granular divides only when a grain spawns), or a table.
- In the GUI thread, division and int64 are fine.

---

## 6. Knob mapping

- **FX knobs are raw words.** Always `op1_fx_param_q15(p, i)` first (custom FX run in the filter slot, whose raw
  range is not 0..32767).
- **Perceptual curves without tables:**
  - `k*k >> 15` for drive, depth, damping, gain (stock filter, grid, spring, phase, iter, digital).
  - `k^3` for decay and envelope amounts (stock spring `1 - k^3`: fine control near the top; nitro `k^3`).
  - Partly quadratic, so the low end stays usable and the top still reaches full scale:
    `(k >> 1) + (k * k >> 16)` (`plugins/reedie/reedie.c:113-115`).
  - Dead zone then curve: stock fm cubes `max(k - 0x2000, 0)`.
- **Exponential (frequency, time):** a polynomial `exp2` per block (`plugins/aether/aether.c:51-63`, cubic, under
  1 cent) or a table.
- **Tables:** `table[k >> 7]` into 129 entries (stock), or 257 entries with the low 7 bits as the interpolation
  fraction (stock cwo.c:330-344), or a 33-point curve with linear interpolation (`plugins/mixtur` `curve()`).
  **Clamp the index.** Stock filter, punch and spring read past their tables for some knob values; stock
  nitro.c:222-226 clamps properly.
- **Discrete choices:**
  - steps `k >> 11` (16), `k >> 10` (32), `k >> 13` (4 segments with a crossfade inside each: cluster, drwave, dsynth);
  - several bit fields from one knob (stock fm splits one knob into three 3-bit ratio indices);
  - detents at musical values (`plugins/arc`, ratio breakpoints; `plugins/fmbell` half steps; steve centre ±512).
- **Page-2 words** (`params[5..7]`): an unset word reads 0; treat 0 as 8192, where the firmware starts turning
  it (`plugins/mixtur/mixtur.c:82`). FX page words can be negative: clamp. White and red share word 7, so only
  one of them can have a page 2.
- **Dead zone with a rest state:** stock cwo turns the frequency shift off below a threshold and pulls the
  oscillator phase back to rest the short way round, so a stopped shifter leaves no offset (cwo.c:364-375,743-746).

---

## 7. Smoothing (zipper noise)

Most stock engines don't smooth knob values: they step at block boundaries (2.9 ms). That is acceptable for
slow timbre knobs. It is not for gain, delay time, cutoff with resonance, or pitch-like parameters. Options,
cheapest first:

1. **One-pole per block:** `c += (target - c) >> 3` (stock punch.c:217-219; cluster gains use 0.995/0.005 for
   ~0.6 s). Round toward the target so it arrives (`plugins/brassophone/brass.c:139-143`).
2. **Linear ramp across the block** (stock cwo.c:347-360; aether, arc). Copy this one:
   ```c
   int32_t step = (target - cur) >> 6;           /* >> 7 for 128-sample synth blocks */
   if (step == 0) cur = target;                  /* snap when close */
   for (n = 0; n < OP1_FX_BLOCK; n++) { cur += step; /* use cur */ }
   ```
3. **Adaptive:** stock cwo runs each knob through a 2-pole smoother whose cutoff grows with the size of the
   move: fast for big turns, gentle for small ones (cwo.c:311-326).
4. **Fade for discrete changes:** fade out, switch, fade back in over ~10 ms (`plugins/mixtur/mixtur.c:149-165`).

Stock filter and nitro compute a smoothed cutoff and then use the raw one. Smooth it and use it.

---

## 8. Oscillators and pitch

- **Phase convention:** reset phases to `0x80000000` on note-on and read the sine as
  `OP1_SINE_TABLE[((int32_t)ph >> 19) + 4096]`. Index 0 is sin(0), so the first sample is 0 and there's no click.
  (This is -sin, inaudible. For screen geometry use `(uint32_t)ph >> 19`.)
- **Phase modulation:** add an int16 modulator to the top 16 bits before the shift:
  ```c
  int16_t h = (int16_t)((ph >> 16) + ((depth * mod) >> 15));
  s = OP1_SINE_TABLE[(h >> 3) + 4096];
  ```
- **Feedback sine = cheap saw** (stock cluster, fm, dbox, voltage): use the oscillator's own previous output as
  `mod`. Feedback 0 is a sine; around 0.5 it is saw-like. One multiply, one lookup, 2 bytes of state.
- **Detuned bank** (stock cluster): per block `off_i = mul32x15(PITCH_INC[0], d_i)`, per sample
  `ph_i += PITCH_INC[n] + off_i`. Animate detune with a leaky random walk at block rate:
  `d_i = mul15(d_i, decay) + mul15(noise, amount)`. For a symmetric log spread, stock drwave uses `inc*r^k`.
- **Detune without 64-bit math:** `ph2 += i + (i >> 15) * (detune - 32768)` (template).
- **Wrap detection** for sync, resets and per-cycle events:
  `uint32_t old = ph; ph += inc; if (ph < old) { /* new cycle */ }`.
  Stock digital hard-syncs osc 2 here; stock phase restarts a windowed slave sine (CZ-style phase distortion) and
  counts slave cycles for a burst; stock voltage halves a decaying value on every cycle.
- **Aliasing:** no stock engine band-limits its oscillators. They are sine-based, and the edge comes from
  FM, feedback, sync, XOR and cubic shaping. Their only mitigation is a 2-tap average `(x + x_prev) >> 1` at the
  output (a zero at Nyquist), and stock phase hides its sync reset with a window that reaches 0 there. If you
  want saws or squares, use a polyBLEP. mixtur does one 2-sample polyBLEP at the master wrap that covers four
  phase-locked dividers, with its reciprocal computed per block (`plugins/mixtur/mixtur.c:170-211`). The other
  op1r engines use naive saws plus a one-pole low-pass.
- **Pitch is per sample.** Use `OP1_PITCH_INC[n]` inside the loop. Stock fm and string read only `[0]` per block
  and lose sample-accurate bends. Stock fm also keeps only the top 16 bits of the increment (a few cents of
  error). Keep the full 32 bits.
- **Pitch isn't known in `note_on`** (`plugins/mixtur/mixtur.c:126`). Do pitch-dependent setup in the first render.
- **Digital grit, on purpose:** stock digital XORs `|sin|` into the mix; stock dsynth's combine modes include
  `a & b`, `a ^ b`, `min(|a|,|b|)` and a saturating difference. Cheap, distinctive, and it creates DC (section 10).

---

## 9. Noise and tables

- **LCG** (stock string, cluster, dna; fxsim):
  `seed = seed * 1664525u + 1013904223u; int16_t n = (int16_t)(seed >> 16);` Use the high bits; the low bits of
  an LCG are poor. Give each voice/position its own seed unless you want them correlated.
- **xorshift32** (13, 17, 5), seed 0x92d68ca2 (stock cwo.c:385-391).
- **Coloured noise:** two cascaded one-poles. Stock string pre-warms the filter states with noise on note-on so
  the first block is already coloured.
- **Making your own tables:** a `gen_*.py` writes `*_tables.h` with a "generated, do not edit" header. Write the
  file from Python with `open(..., 'w')`, never through a shell redirect: a PowerShell redirect wrote reedy's
  header as UTF-16LE (commit e5f21e1).
- **Calibrate tables against the real integer code:** arc builds `arc_calib.c` with the plugin source under
  `OP1_HOST` and iterates each table entry until the note is in tune (`plugins/arc/gen_tables.py`).
- **Code space is short.** Compress large tables and expand them once into bss at init: arc uses an int8 residual
  stream; palme stores `2^(-k/256)` as 2-bit second differences; brassophone shrank its table by interpolating
  bilinearly between fewer rows and columns.
- **Share geometry and constants between the SVG generator and C** through a generated `*_geom.h` (supersaw, tape,
  aether, palme).

---

## 10. Filters

- **One-pole low-pass with 32-bit state.** Small coefficients on 16-bit state stall (limit cycles, a dead low
  end). Keep extra fractional bits:
  ```c
  /* s: sample << 15 (int32); x: Q15; k: Q15 coefficient. |x - y| <= 65535, k <= 32767: fits in 32 bits */
  s += (x - (s >> 15)) * k;
  y = s >> 15;
  ```
  Stock code writes it as `s + k*x - k*s` with two saturating adds to avoid overflowing the difference
  (filter.c:189-196).
- **Low-pass inside a delay line:** stock string writes the filtered value back into the line at the read
  position, so the loop filter costs no extra buffer (string.c:375-390).
- **DC blocker:** `y = x - x1 + mul15(y, R); x1 = x;` with R about 0.995 (stock phone, dbox). Add one after any
  rectifying, XOR or `x + offset` stage. Stock digital, voltage and phase create DC and don't remove it.
  Inside a nearly lossless comb, an integrating DC blocker makes a high-Q mode near 30 Hz: use a proportional DC
  servo instead (`plugins/palme/palme.c:387-392`).
- **Resonant ladder** (stock filter, nitro, punch): 4 one-poles in Q31, feedback `u = in - 4*res*(S(y4) - in)`
  with a saturating shift, a soft clipper inside the loop, and **each stage's coefficient clamped** so it stays
  stable even when modulated by its input (filter.c:189-196).
- **Auto-limiting resonance** (stock punch.c:222-227): once per block,
  `fb = max(0, res - sum_of_|stage outputs|)`. The louder the ladder, the less resonance. Self-oscillation
  stays bounded without a hard clipper.
- **Input-dependent resonance/cutoff:** stock filter adds `peak_in * k1` to resonance; stock nitro moves its
  cutoffs with a decaying peak follower (`env = k^3 * peak`). Envelope-following filters are cheap this way.
- **Multimode from one ladder:** mix the taps binomially. LP4 = s3, BP = s0 - s1, HP4 = (1,-4,6,-4,1)/6 over
  (in, s0..s3). Crossfade between table rows with one knob (filter.c:341-355).
- **State-variable filter:** mixtur uses a Chamberlin SVF with Q13 coefficients and L1-norm bounds, so the loop
  needs no clamps (`plugins/mixtur/mixtur.c:212-235`); sitting recomputes its SVF coefficients only when a knob
  changes.
- **All-pass (Hilbert, phasers):** stock cwo uses all-passes in z^-2, `y = c*(x + y[n-2]) - x[n-2]`, with a toggle
  bit picking the delay slot instead of shifting state (cwo.c:514-524).
- No stock code uses biquads.

---

## 11. Delays, interpolation and physical models

- **Power-of-2 ring + mask** for the cheapest wrap. 64 Ki int16 = 128 KB fits in the scratch (stock cwo, tape).
- **Linear fractional read** (16.16 position):
  ```c
  int32_t i = pos >> 16, f = (pos >> 1) & 0x7fff;
  int32_t a = buf[i & M], b = buf[(i + 1) & M];
  y = a + (((b - a) * f) >> 15);
  ```
- **Polyphase windowed sinc** for quality, in two sizes:
  - 7 taps x 32 phases, with the coefficients linearly interpolated between neighbouring phases (stock delay,
    phone, string, dna). Good for modulated delays and resampling.
  - 16 taps x 64 phases, stored `[tap][phase]` so each tap is a constant stride, with a low-pass baked into the
    kernel (stock sampler/drum, cutoff about fs/4 so upward transposition stays clean).
  You need your own kernel table (generate it). Keep a guard region past every loop and sample end equal to the
  kernel length: `OP1_SAMPLE_GUARD` (8) covers a 7-tap kernel, not 16.
- **Variable-speed resampling** (stock phone.c:371-373): run a 16.16 phase; per output sample, feed
  `((phase + inc) >> 16) - (phase >> 16)` new input samples into a small ring (clamped), and interpolate from it.
- **Write-by-splat** (stock delay, the clever one): the heads move at a knob-set speed. Instead of interpolating
  the read, *add* the input into 7 cells with the kernel, and clear the cell just ahead of the write head. Pitch
  bends like tape when the time changes, and the write is scaled by the speed to keep the level.
- **Modulated all-pass interpolation:** stock cwo switches to a "re-prime" mode, rebuilding the all-pass state
  from a few samples back when the delay time moves fast, to avoid its transient (cwo.c:571-596).
- **Waveguide / Karplus-Strong** (stock string, palme, reedie):
  - period per block: `L = 2^48 / PITCH_INC[0]` (16.16 samples), clamped; wrap by subtracting L so the loop
    length is fractional;
  - a second "pickup" tap at `pos + L * knob` shapes the comb (stock string);
  - an exciter mixed into the line at the write position (stock string adds coloured noise whose amplitude decays
    per block; release scales the loop filter down so the note damps faster);
  - palme folds interpolation, averaging and decay into one 3-tap kernel, with sigma-delta quantised taps for
    precise loop gain (`plugins/palme/palme.c:1-9,382-389`);
  - reedie folds the phase compensation of its filters into the delay length (`plugins/reedie/reedie.c:130-146`).
- **Waveguide mesh** (stock grid): updating only 2 cells per audio sample gives a time-smeared, granular texture
  for nearly no CPU. It's a nice idea to borrow, but its layout needs more memory than the scratch.
- **Reverb tanks:** stock spring is 9 int16 lines in 3 branches with a damping one-pole per cell and negative
  feedback around branches and the tank, plus a stereo matrix of ± line outputs for class B. op1r reverb and
  snake use a Dattorro/Griesinger tank. Keep the input gain low enough not to clip 16-bit lines (reverb dropped
  0.5 to 0.2, `plugins/reverb/reverb.c:80-81`).

---

## 12. Nonlinearity, gain staging and level

- **Soft clippers:**
  - `x - x^3/3` without division: `x -= (((x*x >> 15) * x >> 15) * 10923) >> 15` (template; clamp x to ±32767 first).
  - `0.95x - x^3` (stock dna, constant 0x7999); `x - 0.6x^3` (stock voltage, 0x4cce).
  - Sine shaper `sin(pi/2 * x)` through `OP1_SINE_TABLE` (stock filter, nitro, punch). Interpolate between idx and
    idx+1 the right way round: the stock code has it backwards.
  - Soft knee near the ceiling: reedie limits at 28000 with slope 1/8 above it.
- **Hard limit / sign:** `op1_sat16(x << 15)`.
- **Feedback gain above 1 is OK when every add saturates**: stock delay allows 2x feedback, which clips instead of
  exploding.
- **Output level:** run synths at about half scale so six voices don't clip the bus (supersaw x0.75, fmbell and
  brass `>> 16`; brassophone's review flagged 0 dBFS output). Use velocity:
  `gain = mul15(level, op1_voice_vel_gain(self))`, read in `render`, not `note_on`.
- **Velocity can shape timbre too** (reedie maps it to breath, brightness and a few cents).

---

## 13. Envelopes, note on/off

- **Click-free start:** phases at 0x80000000, filter and feedback states zeroed or pre-warmed (stock string warms
  its noise filters; arc starts on its limit cycle with output 0 and presets its DC; reedie ramps the reflection
  in while the bore fills).
- **`note_off`** only sets a gate flag; the firmware keeps calling `render` through the release. Fade
  excitation instead of breaking a loop (reedie fades the reed drive over 100 ms because "that step is a click").
  Stock string damps its loop filter, stock cluster cuts its feedback and drift, and stock voltage starts a pitch
  drift ("Slump") on release.
- **Internal block-rate envelopes** (stock iter, string, dbox): a Q31 level multiplied per block by a decay factor
  from a table indexed by the knob; square the factor for longer ranges. Attack:
  `env += mul(1 - env, rate) * gate`. Stock iter closes its filter as its internal envelope decays.
- **`OP1_VOICE_ENV` as a modulator:** it's flat within a block and steps between blocks. Ramp from the previous
  block's last value (`plugins/brassophone/brass.c:104-109`). Stock drum squares it into its gain curve. No other
  stock engine reads it; it's an op1r addition worth using for envelope-driven cutoff, FM depth or fold.
- **Drum-style pitch sweep** (stock dbox): per block, fill an increment array
  `inc[n] = PITCH_INC[n] + mul(PITCH_INC[n], env * depth)`, then run the oscillator from it.
- **Sample playback** (stock sampler): 20.12 position, loop while held with a hard jump
  (`if (looping && pos >= loop_end) pos = loop_start;`), reverse by mirroring the comparisons; after release the
  tail plays to the end. Treat recordings shorter than ~256 samples as empty (`plugins/granular/granular.c:101-105`).
  Watch index/phase bit widths: stock drum's index and phase overlap by a bit.

---

## 14. FX specifics

- **Meter:** stock FX write a peak to `op1_fx_meter(pos)` once per block: decay by `mul15(peak, 0x7f00)`, then
  take the max over the block. (Some read only `buf[0]`; take the whole block.)
- **Mono vs stereo:** stock class B is either two mono instances with the same knobs, or one tank fed with L+R
  that builds stereo from line combinations. In the SDK, `render_stereo` + `"stereo": true` gives the master the
  real L/R. Link the detector across channels in dynamics processors (`plugins/compactor/compactor.c:205-223`).
- **Mix:** `y = dry + mul15(wet - dry, mix)` (template). Stock grid keeps full dry up to half the mix knob
  (`dry = sat(2*(1 - k))`).
- **Beat clock:** read it once per block. Validate it (`len >= 256 << 14`, a sane `step`, `next` within range)
  and fall back to 120 BPM (`plugins/sitting/sitting.c`, `plugins/steve/steve.c:29-34`).
- **Latch as an event:** store the last-seen `op1_fx_page[pos][k]` and act on a change. Record it in `init`, so
  creating the FX doesn't fire (`plugins/steve/steve.c:62-68,82`).

---

## 15. Screens

- `init`: look up nodes once with `op1_find_node`; null-check them before every use. Get recolour pointers with
  `op1_node_line_rgba/fill_rgba` here (the id must exist and have that style, or the firmware asserts).
- `update`, in order:
  1. read live knobs (`op1_synth_knobs_live` / `op1_fx_knobs_live`, which include LFO modulation) and page
     words;
  2. set transforms and visibility;
  3. `op1_screen_draw(scr, dt)`;
  4. **then** draw raster lines. Lines drawn before the SVG are lost (brassophone fix, commit 2fb0be6).
- Dial: `a = (k * 6144 >> 15) - 3072` (±135°), then `op1_node_rotate` (`plugins/supersaw/supersaw.c:173-176`).
- Show DSP state via `volatile` arrays the render writes and the screen reads, as stock engines do with their
  display buffers (stock voltage copies every 4th sample for its scope; dbox stores 4 samples per block). Read
  voice state with `(my_voice_t *)OP1_VOICE(i)`. Use a render counter so the screen ignores voices that stopped
  rendering (`plugins/granular/granular.c`, commit 6d983e0). Audio code must never write screen/game state that
  the screen also writes (`plugins/snake/snake.c`).
- `#pragma GCC optimize("Os")` around GUI code: code space is short.
- SVG: 320x160, keep content above y about 140, round all coordinates (a `72.03999999999999` crashed the parser at
  boot, commit f8622dc), no CSS / dasharray / rotate() / text; draw dots in code. Don't reuse a stock file name.
- Text: the stock font (`op1_text_*`) is lower case only (upper case are icons); build it lazily on the GUI thread
  and only call `op1_text_set` when the string changes. `seq/seq_text.h` has a shared stroke font.
- Colours: blue #4D9EFF, green #00ED95, white #FFFFFF, red #FF3A5D, grey #6F7173.

---

## 16. Testing

`audio.py` alone has gaps: synths play on **voice 0 only**, FX run at **pos 0 with `render` only**, the scratch
is clean, and page words are always 0. Cover the rest in a `harness/<name>_test.c` that `#include`s your
plugin source with `OP1_HOST`:

- **Dirty memory:** fill the scratch and voice memory with random bytes, put guard words (0x5A5A5A5A) after
  them, run, and check the guards (`harness/sitting_test.c`).
- **Fuzz:** random knobs, page words (including negative), presses, tempo, clock jumps, note on/off patterns,
  all voice indices and all three positions; hundreds of runs under UBSan (mixtur ran 80 with garbage voice
  memory, arc 400).
- **Stability:** full-scale input at max feedback/resonance for many seconds; check the output stays bounded
  and silence decays to exactly 0 (no limit cycle).
- **Measurements:** tuning in cents over the keyboard (FFT), RT60, click metrics at loop/retrigger boundaries
  (sitting cut a 2nd-difference spike from 10.0 to 0.35).
- **Against a reference:** `harness/clouds_compare.py` compares a port to the original on SNR or envelope
  correlation. The firmware effects in `op1_246` were verified bit-exact the same way in a Blackfin simulator
  (`tools/fxsim`); if you port a stock idea, you can test it there too. Don't trust that simulator on saturating
  left shifts of negative values or on parallel bundles.
- **Device build:** `make_plugin.py` with zero libgcc imports is the proof there's no hidden division or 64-bit
  math (reedie: "0 imports").

---

## 17. Don't copy from the stock code

- Firmware addresses that aren't in `op1_plugin.h`: boot tables, L1 scratch buffers, the shared LCG seed, fixed
  SDRAM buffers, anything in encrypted L1 (pulse's render, parts of cwo).
- Reads past table ends (filter, punch, spring), stock punch's crash for 0 stages, delay class B's infinite loop
  at length 0. Clamp every index derived from a knob.
- Big clears in a constructor or attach (grid's 270 KB memset).
- The shared feedback buffer across voices in stock iter (each voice reads what the previous voice wrote).
- Smoothed values that are then not used (filter, nitro), the backwards sine-shaper interpolation, meters from
  `buf[0]`.
- Bit-exact emulation details (`round16` round-half-to-even, sim-bug switches): only needed to match firmware.

---

## 18. Pre-submit checklist

- [ ] Voice struct size asserted; big state in `[OP1_VOICES]` arrays; FX state in `st[pos % 3]`.
- [ ] Nothing assumes clean scratch or voice memory; no large clear in one call.
- [ ] No division, modulo, float or 64-bit per sample; `make_plugin.py` shows no libgcc imports.
- [ ] Every knob-derived index clamped; FX knobs through `op1_fx_param_q15`; page words: 0 means 8192, clamp negatives.
- [ ] Saturating adds in all feedback paths; feedback rounding toward zero; silence decays to 0.
- [ ] Gain, delay time and cutoff smoothed or ramped.
- [ ] DC blocked after any rectifying/XOR/offset stage.
- [ ] Output about -6 dB with velocity applied in `render`; no clicks on note-on, note-off or retrigger.
- [ ] Pitch from `OP1_PITCH_INC[n]` per sample; nothing pitch-dependent in `note_on`.
- [ ] Screen: SVG drawn before raster lines, nodes null-checked, coordinates rounded, content above y 140.
- [ ] `audio.py` WAV, preview PNG, README with knobs and CPU estimate, copyright line, licence.

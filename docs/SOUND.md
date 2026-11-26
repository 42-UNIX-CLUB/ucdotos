# Sound

State on 2026-11-26. The design is DECISIONS.md 20 (two paths that meet in the browser)
and 21 (Doom's music: our own OPL synthesizer, through audiod),
built to cost as little as possible on the one thread the whole system runs on. How the
pieces work inside is ARCHITECTURE.md section 15; measurements are PERFORMANCE.md
section 12.

## 1. The picture

```
Doom effects ── /dev/dsp, its own stream, non-blocking, ~64 ms buffer ──────┐
                                                                            ├─ browser audio thread:
play, oplplay ── /run/audiod.sock ── audiod ── /dev/dsp, ~340 ms ───────────┘  boot/audio-worklet.js sums
  (Doom's music: oplplay,  (any format; converted, resampled,                   every stream -> speakers
   started and steered      volume, mixed)
   by Doom through a pipe)
```

- **The kernel does not mix.** `/dev/dsp` (`kernel/audio.c`) is OSS-shaped: each open
  descriptor is a stream; `write` hands the samples to the host in one copy; the kernel
  only counts what each stream has queued and not yet played. That count is the stream's
  buffer, and its size (`SNDCTL_DSP_SETFRAGMENT`) is the stream's delay.
- **The browser mixes, on its audio thread.** `boot/audio.mjs` sends each write to an
  AudioWorklet (`boot/audio-worklet.js`) as one message with the buffer transferred. The
  worklet sums the streams with their volumes, converts to float, and every ~11 ms tells
  the kernel how much of each stream has played, which frees room and wakes writers.
- **One format on the device**: 16-bit stereo at the browser's own rate (usually 48 kHz;
  `SNDCTL_DSP_SPEED` answers with it), so every sample is resampled at most once, by the
  program that has it.
- **Games** write to `/dev/dsp` directly, never blocking: once per frame they ask for the
  free room (`SNDCTL_DSP_GETOSPACE`), mix exactly that and write it. A frame can never
  wait for sound. Doom does this (`bin/doom/i_ucossound.c`): 8 effect channels, the WAD's
  8-bit 11 kHz samples resampled straight to the device rate in the mixing pass.
- **Everything else** goes through `audiod` (`bin/audiod.c`), started by `init` at boot.
  Programs connect to `/run/audiod.sock`, send a 20-byte header (format, rate, volume,
  length) and their samples; audiod converts, mixes and writes 20 ms chunks into a large
  buffer, so music does not skip when the system is busy. Idle, it holds no stream and
  sleeps in `poll()`.
- **Gaps are counted.** A stream that runs dry while it is being fed (more data arrives
  within 0.5 s) counts an underrun, readable with `SNDCTL_DSP_GETERROR`; the page keeps
  the total in `window.ucosAudio`.

- **Doom's music** is played by `oplplay` (`bin/oplplay/`), our own OPL2 FM synthesizer
  (the chip in AdLib and Sound Blaster cards, which Doom's music was written for), with the
  WAD's own `GENMIDI` instruments. Doom writes the song and the instruments to
  `/tmp/doom/`, starts `oplplay`, and steers it through a pipe on its stdin (`pause`,
  `resume`, `volume N`, `stop`). Doom's process synthesizes nothing; `oplplay` renders
  only ~60 ms ahead of real time and sends 16-bit stereo to audiod like any client.

## 2. Using it

- `play FILE.wav` plays PCM WAV (8 or 16 bit, mono or stereo, any rate); `play -t 440 -s 2`
  plays a 2-second tone; `-v 0..100` sets the volume. `play` returns when audiod has taken
  all of its sound.
- **The browser stays silent until the first click or key press on the page**: a browser
  rule. Until then nothing plays and blocking writers wait.
- Doom plays its sound effects and music when its game data is present (none is in the
  image yet). Music needs audiod, which `init` starts; Doom started as the first process
  (`--init=/bin/doom`) has effects but no music.
- `oplplay -w FILE.wad SONG` (or `-g GENMIDI`) plays a MUS or MIDI file; `-l` loops,
  `-v 0..127`, `-o FILE -s SECONDS` renders to a raw file instead.

## 3. Web constraints and how the design meets them

| Constraint | What it would cost | What the design does |
|---|---|---|
| The whole system shares the page's main thread | mixing, conversion and pacing there would cost Doom frames | mixing and conversion run on the audio thread; the kernel only counts bytes; Doom writes without waiting |
| The main thread can stall (an 8 ms time slice, a Doom frame, a busy program) | gaps in the sound | each stream has its own buffer: 64 ms for Doom (enough for normal stalls, short enough for instant gunshots), 340 ms for audiod (music) |
| Shared memory between threads needs special HTTP headers (COOP and COEP) | ties sound to the open hosting decision | messages with transferred buffers instead: works on any static host |
| Autoplay: silent until a user gesture | sound before the first input is lost | the page resumes the audio context on the first click or key |
| Node has no Web Audio | tests could not run | `boot/audio.mjs` has a stand-in that "plays" in real time and can save the mix to a file, so `make test` checks sound sample by sample |

## 4. What is measured

In headless Chromium, Doom playing its demo, 3 runs each way: **34.8 fps with and
without sound, the same 95th and 99th percentile frame times (33.6 to 34.0 ms), 0
underruns**. Doom's recorded output under Node: the demo's effects at a peak of 24,635
(no clipping), left and right different 69% of the time (stereo separation works).
`audiod` mixing two clients for about 1.3 s used 8 to 10 ms of CPU. With music (Doom on the
desktop, 3 runs each way): 35.8 fps with and without music, the same 95th percentile,
0 underruns. The synthesizer renders 10 s of 9 voices in about 0.1 s. Details and the
benchmarks behind the design: PERFORMANCE.md section 12.

`audiotest` (37 checks, in `make test`, run by `tools/tests/audio.mjs`) checks the
device's calls, non-blocking and blocking writes, pacing in real time, underrun counting,
3000 random calls, 8 streams, audiod's protocol and mixing; then the saved recording:
1 s written to `/dev/dsp` must appear unchanged, and audiod's mix of a 16-bit 24 kHz
client and an 8-bit 48 kHz client at volume 50 must come out as the exact expected value.
For the synthesizer it builds a one-instrument GENMIDI (a pure sine) and checks that MIDI
note 69 comes out at 440 Hz from both a MIDI and a MUS file, centred, silent after its
release; that 10 s of 9 voices render in under 0.5 s; and the control pipe.

## 5. Not built yet

- **The terminal bell** (owner decision: tone, flash, both, or silent).
- **A volume control**: per-stream volume exists (`SNDCTL_DSP_SETPLAYVOL`, and audiod's
  header), but there is no master volume, mixer program or bar indicator.
- **Recording** (the microphone, through a permission prompt): not built; nothing needs it.
- **Shared memory** between the page and the audio thread, if the hosting ever sends
  COOP and COEP: would save one message per write (about 5 µs).
- Firefox and Safari are untested.
- Music: OPL3 (18 voices, 4-operator voices) and the chip's rhythm mode are not emulated;
  Doom's GENMIDI uses 2-operator voices, which OPL2 covers. Pausing and stopping are heard
  after ~0.4 s (audiod's buffer). Music is quieter than the effects at Doom's default
  music volume (8 of 15), as on the original; the menu slider raises it.

## 6. Options that were not taken (for the pull request review)

The owners chose decisions 20 and 21 from these; they are recorded so a reviewer can see
what was weighed.

**Doom's music synthesizer** (decision 21 chose our own OPL emulator in C):

| Option | For | Against |
|---|---|---|
| Nuked OPL3 | cycle-exact, the most faithful OPL emulator | third-party (LGPL-2.1): needs an exception to "no third-party code", like Doom's engine; known to be heavy on CPU (not measured here) |
| DOSBox's DBOPL | what chocolate-doom uses: very close to the chip, light | third-party (GPL-2.0-or-later): needs the same exception |
| Our own simple synth (not FM) | smallest, lightest | does not sound like Doom's soundtrack |

**Where the synthesizer runs** (decision 21 chose a separate program sending its sound
through audiod):

| Option | For | Against |
|---|---|---|
| On the browser's audio thread: the synthesizer (C compiled to wasm) inside the AudioWorklet, notes sent through an OSS-style `/dev/midi` | costs the main thread, and so Doom's frames, nothing | a new device, wasm inside the worklet, event timing across threads; more code in `boot/` |
| Inside Doom's own process, mixed into its `/dev/dsp` stream | simplest | all synthesis inside Doom's frames, on the main thread |

Measured with the chosen design: ~10 ms of main-thread CPU per second of music (~1%), and
no difference in Doom's frame rate (PERFORMANCE.md section 12). If that ever matters, the
audio-thread option above is the way to remove it.

**The sound system itself** (decision 20): one path for everything; mixing in the kernel
(~11 µs per Doom frame on the main thread, measured); one program at a time; an
ALSA-shaped device (needs `mmap`); our own `/dev/audio`; scheduling buffers from the main
thread (gaps whenever it stalls); shared memory with the worklet (needs COOP and COEP
headers, so it waits for the hosting decision).


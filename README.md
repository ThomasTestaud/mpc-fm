# mpc-fm

An FM synth add-on for the Akai MPC Sample (firmware 1.3.0.12). It runs next
to [SampQOL OS](https://github.com/snarebes/SampQOL-OS), but it is a separate
project. It does not contain, change or redistribute any SampQOL file: the
user installs SampQOL OS from its official releases.

Plan: FM as a pad's sound source. A pad plays an FM patch instead of a
sample, and everything after the sound source stays stock: filter, envelopes,
Pad EQ, inserts, mixer, 16 Levels, Keys, export and resample.

**Status: 0.1.1, FM pads prototype.** 0.1.0 ran on the device without a
problem but never played FM: it hooked a voice class the drum pads do not use.
0.1.1 hooks the right one (not yet tested on the device).

- **FM carriers.** `make samples` writes WAV files that are recordings of FM
  patches with the patch hidden in the lowest 4 bits of the first 336 frames
  (at most 15 LSB, -66 dB, CRC-checked; `src/fm/fm_carrier.c`). On a stock
  MPC they play as recordings. With mpc-fm, a pad with such a file plays the
  live FM engine; the patch travels with the sample into projects.
- **Voice hook** (`src/voice_hook.c`). The drum voice renders each layer
  through a `DefaultSampleGenerator`; three of its vtable entries (start,
  stop, render) are swapped, after a check that all 16 are exactly stock for
  app build `1dac7f48` (fw 1.3.0.12). For a carrier, the stock render runs
  (position, loops, end, gain ramp stay stock), then its output is replaced
  by the FM engine x the same gain ramp; pitch = how far the stock read
  position moved. Everything after the source is stock: filter, amp
  envelope, pan, Pad EQ, inserts, mixer, 16 Levels, Keys, export.
- **Discovery** (`src/discover.c`). Where the app keeps a sample's frames
  is found at run time: the low-priority thread searches memory reachable
  from the generator for a carrier header with `process_vm_readv` (a bad
  pointer cannot crash), staying inside each heap block. The path it learns
  is then followed at each note start (a few reads).
- **FM engine** (`src/fm/`): 4 operators, 8 algorithms, an envelope per
  operator, feedback on operator 4. No allocation, no locks; output
  bit-exact whatever the render block sizes.
- **Probe**: audio block timing (window and since start), xruns, thread CPU
  and priorities, CPU clocks, FM voice counters and cost.
- A line under the SampQOL splash shows `mpc-fm 0.1.0  FM on`, or why not.

## How it loads

The MPC Sample runs `autorun/autorun.sh` from the SD card (the stock script
runner checks `autorun.check`). Our launcher is that entry point:

```
autorun/autorun.sh (ours)
  -> <SampQOL folder>/autorun.sh     official, unchanged: inside ours or at the card root
    -> qol-run.sh                    SampQOL watchdog + project safety net
      -> setarch                     OUR static stand-in, first in PATH:
                                     LD_PRELOAD += /run/mpcfm/fm.so,
                                     status line over SampQOL's splash
        -> real setarch -R -> /usr/bin/MPC Sample
             LD_PRELOAD="/tmp/mpcqol.so /run/mpcfm/fm.so"
```

Safety:
- Our files are pinned by SHA256 in `autorun.sh` (and `autorun.check` covers
  `autorun.sh`). If they do not match, SampQOL starts without FM.
- Crash guard: if the app stops less than 10 minutes after a start with FM
  (or after 3 starts with FM), FM stays off until the next boot. SampQOL keeps
  working.
- `autorun/mpcfm/DISABLE` turns FM off. Without the card, the MPC is stock.
- `fm.so` stays passive in every process except `/usr/bin/MPC Sample`.

SD layout and install steps: [launcher/INSTALL.txt](launcher/INSTALL.txt).

## Build

Needs only `make`, a host C compiler and `curl`. No root.

```
make toolchain   # zig 0.16.0 into .tools/ (pinned, SHA256 checked)
make check       # host build + tests: launcher, autorun chain, probe
make arm         # aarch64 build for the device + static checks
make dist        # SD card folder: dist/autorun/
make bench       # engine benchmark here (device: build/aarch64/fmbench)
make wavs        # every preset as WAV in build/wav/
```

`zig cc` cross-compiles for aarch64 and links against old glibc symbol
versions (target 2.30; the device has at least that, as SampQOL needs it).
CPU tuning: `-mcpu=cortex_a55` (Rockchip RK3566).

| Path | What |
|---|---|
| `src/fm.c` | entry point, status/log thread |
| `src/alsa_probe.c` | timing wrappers of the ALSA PCM calls |
| `src/fm/` | FM engine, presets, carrier header |
| `src/voice_hook.c` | FM as the sound source of a pad (stock generator hooks) |
| `src/discover.c` | run-time search for the sample frames |
| `launcher/` | `autorun.sh` template, `autorun.json`, `setarch` stand-in, status strip |
| `tools/` | toolchain fetch, `autorun.check`, SD folder, aarch64 checks, `fmbench`, `fmrender`, font generator |
| `tests/` | engine unit tests; launcher tests with a fake ALSA library, fake app and fake framebuffer |

The status strip font (`launcher/font.h`) is generated from the X11
misc-fixed 6x13 font (public domain) by `tools/gen-font.py`.

## Rules

- Never commit SampQOL files (`mpcqol.so`, its scripts, its manual). Its
  license forbids redistribution.
- Code on the audio thread: no locks, no malloc, no syscalls, no file or
  display I/O.

Not affiliated with inMusic, Akai Professional or SampQOL OS. MPC and MPC
Sample are trademarks of inMusic Brands, Inc.

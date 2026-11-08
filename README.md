# mpc-fm

An FM synth add-on for the Akai MPC Sample (firmware 1.3.0.12). It runs next
to [SampQOL OS](https://github.com/snarebes/SampQOL-OS), but it is a separate
project. It does not contain, change or redistribute any SampQOL file: the
user installs SampQOL OS from its official releases.

Plan: FM as a pad's sound source. A pad plays an FM patch instead of a
sample, and everything after the sound source stays stock: filter, envelopes,
Pad EQ, inserts, mixer, 16 Levels, Keys, export and resample.

**Status: phase 0.** `fm.so` makes no sound yet. It measures the real audio
budget on the device (block timing, xruns, CPU use and priority of each
thread, CPU clocks) so the FM engine can be sized from real numbers.

## How it loads

The MPC Sample runs `autorun/autorun.sh` from the SD card (the stock script
runner checks `autorun.check`). Our launcher is that entry point:

```
autorun/autorun.sh (ours)
  -> autorun/sampqol/autorun.sh      official SampQOL, unchanged (its own checks)
    -> qol-run.sh                    SampQOL watchdog + project safety net
      -> setarch                     OUR static stand-in, first in PATH:
                                     LD_PRELOAD += /run/mpcfm/fm.so
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
```

`zig cc` cross-compiles for aarch64 and links against old glibc symbol
versions (target 2.30; the device has at least that, as SampQOL needs it).
CPU tuning: `-mcpu=cortex_a55` (Rockchip RK3566).

| Path | What |
|---|---|
| `src/fm.c` | entry point, status/log thread |
| `src/alsa_probe.c` | timing wrappers of the ALSA PCM calls |
| `launcher/` | `autorun.sh` template, `autorun.json`, `setarch` stand-in |
| `tools/` | toolchain fetch, `autorun.check`, SD folder, aarch64 checks |
| `tests/` | host tests with a fake ALSA library and fake app |

## Rules

- Never commit SampQOL files (`mpcqol.so`, its scripts, its manual). Its
  license forbids redistribution.
- Code on the audio thread: no locks, no malloc, no syscalls, no file or
  display I/O.

Not affiliated with inMusic, Akai Professional or SampQOL OS. MPC and MPC
Sample are trademarks of inMusic Brands, Inc.

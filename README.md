# mpc-fm

An FM synth add-on for the Akai MPC Sample (firmware 1.3.0.12). It runs next
to [SampQOL OS](https://github.com/snarebes/SampQOL-OS), but it is a separate
project. It does not contain, change or redistribute any SampQOL file: the
user installs SampQOL OS from its official releases.

Plan: FM as a pad's sound source. A pad plays an FM patch instead of a
sample, and everything after the sound source stays stock: filter, envelopes,
Pad EQ, inserts, mixer, 16 Levels, Keys, export and resample.

**Status: 0.2.0, prototype.** FM pads play live FM on the device (tested
up to 0.1.6: amp and filter envelopes, Keys, streamed samples). 0.1.7 keeps
the patch when a project save rewrites the sample (the patch library).
0.2.0 adds the FM page (not yet tested on the device): SHIFT + NOTE REPEAT
opens it, B1-B3 pick the page, K1-K3 edit, and the edits are heard at once
on the pads, then saved on the card.

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
- **FM page** (`src/ui/`). `surface.c` defines the rawmidi input functions
  the app reads the control surface with (SampQOL does not), and
  `surface_filter.c` takes K1-K3, B1-B3, the encoder and -/+ while the page
  is open; SysEx (the surface's authentication, pad colors) always passes
  untouched. `display.c` defines `drmModePageFlip` / `drmModeSetCrtc`: the
  page has its own DRM buffer, shown instead of the app's frame while it is
  open (the app keeps its flip events; its buffers are never written), and
  sent to the SPI panel with DIRTYFB on the changed rectangle only.
  `fm_page.c` is the page (pure: tested and drawn on the computer,
  `build/host/page-*.ppm`). The voices of the edited sample play a shared
  edit patch (`voice_hook.c`), so edits are live; they are saved as text in
  `MPC-FM-Patches/` (`src/patchfile.c`), which wins over the file's patch.
- A line under the SampQOL splash shows `mpc-fm 0.2.0  FM on`, or why not.

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
| `src/sampleinfo.c`, `src/patchfile.c` | sample names, WAV headers, the patch library, edited patches |
| `src/ui/` | the FM page: surface filter, screen, page model and drawing, its thread |
| `launcher/` | `autorun.sh` template, `autorun.json`, `setarch` stand-in, status strip |
| `tools/` | toolchain fetch, `autorun.check`, SD folder, aarch64 checks, `fmbench`, `fmrender`, font generator |
| `tests/` | engine unit tests; launcher tests with a fake ALSA library, fake app and fake framebuffer |

The fonts (`launcher/font.h`, `src/ui/fonts.h`) are generated from the X11
misc-fixed 6x13 and 9x18 fonts (public domain) by `tools/gen-font.py`.

## Rules

- Never commit SampQOL files (`mpcqol.so`, its scripts, its manual). Its
  license forbids redistribution.
- Code on the audio thread: no locks, no malloc, no syscalls, no file or
  display I/O.

Not affiliated with inMusic, Akai Professional or SampQOL OS. MPC and MPC
Sample are trademarks of inMusic Brands, Inc.

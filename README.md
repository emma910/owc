# overwatch aim

Native Windows aim-assist based on real-time pixel detection, written in C++20. It duplicates the desktop via
DXGI, searches for the closest magenta-colored cluster inside a configurable region, and moves the mouse toward
it via a kernel-level input driver.

This project is self-contained: sources, the vendored input library and the build scripts all live in this folder.

> **Disclaimer.** Screen-detection aim tools can be considered cheating by game services and may lead to account
> bans. Use it only where permitted — on your own servers, in offline/private lobbies, or for research.

---

## Features

- **GPU-accelerated scanning** — default path captures with DXGI Desktop Duplication and runs the color search on
  the GPU via a D3D11 compute shader. CPU cost is near zero.
- **CPU fallback** — a row-parallel scanner (persistent worker pool) distributes the search across cores when no
  GPU path is available or when forced with `OWC_CPU=1`.
- **Driver-level input** — relative mouse movement is injected through the Interception driver. The mouse device is
  auto-detected from its hardware ID; if the driver is missing, `SendInput` is used instead.
- **Precise, tunable aim loop** — integer dead-zone, per-step movement cap, randomized pacing, aim-point offset,
  max-snap guard, optional human-like jitter.
- **Two aim modes** — `TRACKING` (smooth follow) and `FLICKING` (snap-and-shoot with a configurable fire threshold
  and cooldown).
- **Low footprint** — ~0.02–0.04 CPU cores while idle at 30 fps in the development environment; `SPIN_WAIT` pacing
  bulk-sleeps and only spins a short tail, so aiming stays sub-millisecond-accurate without pinning a core.
- **Configuration-only tuning** — all parameters are read from `owc.cfg` at startup; no recompilation.

## Requirements

- Windows 10/11 x64
- Visual Studio 2022 with the *Desktop development with C++* workload (x64 toolchain)
- The [Interception driver](http://www.oblita.com/interception) installed, if you want driver-level input
  (not required to build)

## Building

### Script (recommended)

```bat
build.bat
```

The script locates `vcvars64.bat`, compiles the sources, links against the vendored Interception library and places
everything into `out\`:

| File            | Purpose                                       |
|-----------------|-----------------------------------------------|
| `out\Discord.exe` | The application (name is a neutral placeholder, rename freely) |
| `out\interception.dll` | Copied from the vendored driver library        |
| `out\owc.cfg`    | Settings file, copied from the parent directory and ready to edit |

Intermediate object files (.obj) go to `build\obj\`.

### CMake

```bat
cmake -S . -B cmake-build
cmake --build cmake-build --config Release
```

Output goes to `out\` as well; the executable is named `Overwatcheat.exe` in this configuration.

## Running

1. Install the Interception driver as administrator:
   `Interception\command line installer\install-interception.exe`, then reboot if requested.
2. Run `out\Discord.exe`.
3. Configure `out\owc.cfg` (or any copy named `owc.cfg` next to the executable).
4. Hold the configured `aim_key` (default: mouse button 1) — the bot aims at the detected target.

Console logs startup status (capture backend, input device, box geometry, detected colors). Press `Ctrl+C` to stop.

## Configuration (`owc.cfg`)

| Key                            | Default         | Description                                          |
|--------------------------------|-----------------|------------------------------------------------------|
| `aim_key`                      | `1`             | Virtual-key code held to activate aiming              |
| `aim_mode`                     | `0`             | `0` = TRACKING, `1` = FLICKING                        |
| `sensitivity`                  | `10.0`          | In-game sensitivity; defines the aim dead zone        |
| `fps`                          | `30`            | Captured frames per second (raise for more precision) |
| `box_width` / `box_height`     | `512` / `512`   | Scanned region size, centered on the screen           |
| `target_colors`                | magenta set     | Base colors to match, hex `RRGGBB`, comma-separated   |
| `target_color_tolerance`       | `8`             | Allowed per-channel color distance                    |
| `aim_min_target_width/height`  | `8` / `8`       | Minimum detected region size to count as a target     |
| `aim_offset_x` / `aim_offset_y`| `1.0` / `0.75`  | Aim-point position relative to the target region      |
| `aim_max_move_pixels`          | `3`             | Maximum movement per aim step (raise for flicking)    |
| `aim_duration_millis`          | `3.5`           | Base step pacing in milliseconds                      |
| `aim_duration_multiplier_base` / `max` | `1.0` / `2.0` | Randomized pacing window                   |
| `aim_jitter_percent`           | `0`             | Random aim offset, 0–100, for a more human profile    |
| `max_snap_divisor`             | `2.0`           | `box / divisor` caps the maximum allowed snap         |
| `aim_precise_sleeper_type`     | `1`             | `0` YIELD, `1` SPIN_WAIT, `2` SLEEP                   |
| `aim_cpu_thread_affinity_index`| `2`             | Pins the aim thread to a physical core; `-1` disables |
| `flick_shoot_pixels`           | `5`             | FLICKING: distance threshold at which to fire         |
| `flick_pause_duration`         | `200`           | FLICKING: cooldown between shots, ms                  |
| `toggle_in_game_ui` / `toggle_key_codes` | `true` / `12,5A` | Sends toggle keys (ALT+Z by default) before aiming |
| `mouse_id` / `keyboard_id`     | `11` / `1`      | Interception device IDs (mouse is normally auto-picked) |

**Sleeper trade-off**

| Type        | Timing | CPU while aiming |
|-------------|--------|------------------|
| `YIELD`     | good   | moderate         |
| `SPIN_WAIT` | best   | low (bulk sleep + short pause tail) |
| `SLEEP`     | coarse | near zero        |

## Environment variables

| Variable            | Effect                                       |
|---------------------|----------------------------------------------|
| `OWC_CPU=1`         | Force the CPU scan path instead of the GPU one |
| `OWC_SCAN_THREADS=N`| Thread count for the parallel CPU scanner     |

## How aiming works

1. Grab the latest frame inside the configured box; scan it for clusters matching `target_colors ± tolerance`.
2. Take the closest / largest cluster and compute the aim point from `aim_offset_x/y`.
3. Compute the per-axis movement:

```
step = min(aim_max_move_pixels, (delta / sensitivity))   // integer division creates the dead zone
```

4. Pace each step with a random duration in `[base, max] × aim_duration_millis` so the crosshair glides.
5. Respect the max-snap guard and the aim-key state; in FLICKING mode, trigger a shot and cool down once the
   crosshair is within `flick_shoot_pixels` of the target.

## Project structure

```
cpp/
├── build.bat               MSVC build script
├── CMakeLists.txt          CMake build
├── src/
│   ├── main.cpp            entry point, capture + input wiring
│   ├── config.cpp/.h       owc.cfg loader
│   ├── capture.cpp/.h      capture interface, CPU scanner, parallel worker pool
│   ├── gpu_capture.cpp/.h  DXGI duplication + D3D11 compute scan
│   ├── aim_bot.cpp/.h      aim loop, pacing, tracking/flicking logic
│   ├── native.cpp/.h       Interception / SendInput input backends
│   ├── state.h             lock-free frame snapshot exchange
│   ├── util.h              clock / precise-sleep helpers
│   └── color_lut.h         color lookup table
├── owc.cfg		           	config
├── Interception/           vendored driver library, installer and licenses
└── out/                    build output (generated)
```

## Third-party components

| Component    | License  | Source                                                  |
|--------------|----------|---------------------------------------------------------|
| Interception | LGPL-3.0 | prebuilt binaries vendored under `Interception/` together with its license texts |

Windows SDK headers/libraries (DirectX, DXGI, GDI, WinMM) are provided by Visual Studio.
This project is released under the **GNU Affero General Public License v3** (see `LICENSE.txt` in the parent
directory).

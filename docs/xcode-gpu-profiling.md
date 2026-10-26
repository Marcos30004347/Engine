# Xcode GPU Profiling

The engine renders through Vulkan, which on macOS runs on MoltenVK on top of
Metal. Vulkan timestamp queries measure Vulkan pass boundaries, not Metal
encoders, so a stall inside the translation layer gets billed to whichever pass
follows it. Reading a Metal-level capture is the only way to tell a long
compute encoder apart from a gap between encoders.

There are two ways to capture. The headless one needs no Xcode project and is
usually the faster loop; the project is for interactive, frame-by-frame work.

## Headless capture

```bash
make gputrace
open build/tests/frame.gputrace
```

This drives the binary already built in `build/`. Xcode is only needed to view
the result.

`MTL_CAPTURE_ENABLED=1` is mandatory — Metal refuses programmatic capture
without it. The trace is flushed on clean exit, so the run has to be bounded
(`--frames`).

Traces are large: three full-pipeline frames at eight rays per pixel is about
2.7 GB, against roughly 300 MB for five depth-only frames. Keep the frame count
low and delete traces once read. Capture also slows the run by more than an
order of magnitude, so frame times reported during a capture mean nothing.

Override the target or the run:

```bash
make gputrace TRACE_ARGS="--test-camera-roof-down --rays-per-pixel 8 --frames 3"
make gputrace TRACE=/tmp/other.gputrace
```

`MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE` selects what is captured
(`MoltenVK/mvk_private_api.h`):

| value | scope |
| ----- | ----- |
| `1`   | the whole `VkDevice` lifetime |
| `2`   | the first frame only — the default here |
| `3`   | on demand, signalled through a temporary named pipe |

## Xcode project

```bash
make xcode-debug     # or: make xcode-release
make xcode-open
```

`make xcode` alone just generates. Xcode is a multi-config generator, so one
generate serves both configurations and the choice happens at build time.

Two flags matter and are already applied by the Makefile:

- `-DENGINE_BUILD_SAMPLES=ON` — defaults to `OFF`, and without it the
  `SparseVoxelRendererSample` target does not exist in the project at all.
- `-DCMAKE_CONFIGURATION_TYPES="Debug;Release"` — trims the default four.

The Xcode tree is independent of `build/`; neither forces the other to
reconfigure, and `build-xcode/` is gitignored.

### Scheme setup

In Xcode: scheme selector → **SparseVoxelRendererSample**, then
**Product → Scheme → Edit Scheme… → Run**.

- **Arguments → Arguments Passed On Launch**, for example
  `--test-camera-roof-down --rays-per-pixel 8 --ray-resolution-divisor 2 --disable-vsync --skip-output-signature`.
  Leave `--frames` off for interactive capture so the app does not exit before
  you click the capture button.

  Avoid `--depth-only` for profiling. It drops the entire shading and ray
  chain, runs at roughly 8.5ms GPU against ~28ms for a full frame, and the
  deferred cost settles on a different pass than it does in a full frame. It is
  useful for isolating the raster and culling floor, and misleading for
  anything else.
- **Options → GPU Frame Capture** → set to **Metal**. It is under *Options*,
  not *Diagnostics*. Because the app reaches Metal through MoltenVK rather than
  linking Metal directly, the *Automatically Enabled* setting may not detect it;
  selecting **Metal** explicitly is what makes the capture button appear.
- **Diagnostics → Metal API Validation** is a separate control. It catches API
  misuse but costs a lot of time — leave it off for anything you intend to
  measure.
- **Options → Working Directory** does *not* need to be set. Assets resolve
  relative to the executable, and the sample's `RUNTIME_OUTPUT_DIRECTORY_<CONFIG>`
  properties pin the binary next to the generated `assets/` tree in
  `build-xcode/tests`.

Run with ⌘R, then click the camera icon in the debug bar to capture a frame.

If **GPU Frame Capture** is missing from the Options tab entirely, use the
headless path above — it does not depend on Xcode's Metal detection.

## Reading a capture

Open the **Counters** view and the encoder timeline. The question a capture
answers that Vulkan timestamps cannot:

- a pass showing as a **long compute encoder** is real GPU occupancy, and the
  Vulkan timestamp was accurate
- a pass showing as a **gap, or a wait on an `MTLEvent`/drawable**, is
  synchronization, and the Vulkan timestamp has attributed someone else's stall
  to it

The second case is common here: cost defers to whichever node establishes the
next dependency boundary, so it migrates between passes as the graph changes.
Confirming which of the two is happening should come before optimising the pass
the timestamps blame.

## Toolchain notes

`cmake -G Xcode` needs the full Xcode toolchain, not the Command Line Tools.
If `xcode-select -p` prints `/Library/Developer/CommandLineTools`, CMake reports
`No CMAKE_C_COMPILER could be found` — it probes by running `xcodebuild`, which
fails before it can answer.

```bash
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
sudo xcodebuild -runFirstLaunch
```

`-runFirstLaunch` matters on its own: Xcode installs shared components into
`/Library/Developer/PrivateFrameworks`, and a copy left behind by an older
Xcode or Command Line Tools makes `xcodebuild` fail to load
`IDESimulatorFoundation` with a missing-symbol error.

The Makefile passes `DEVELOPER_DIR` explicitly, so it keeps working if
`xcode-select` is later pointed back at the Command Line Tools:

```bash
sudo xcode-select -s /Library/Developer/CommandLineTools
```

Note that switching the global setting changes the compiler for `build/` too,
which will generally force a rebuild there.

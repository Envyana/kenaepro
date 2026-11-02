# KenaePRO

A Linux-first, Qt 6 application for frame-by-frame video review: scrub and play clips, take notes
and draw annotations per frame, bookmark frames and ranges, and organise material in a project.
It is meant to be a native Linux counterpart to existing review tools for animators. (It was called
framereview up to version 0.2; settings from that name are copied over on the first start.)

**Status (0.3.4, 2026-11-02): built on Linux as an AppImage (Ubuntu 22.04 base). Since 0.3.1 it
has run in clean Ubuntu 22.04 and Debian 13 containers and on a Fedora Kinoite workstation.** All C++ test suites
pass (`test_core`, `test_runtime`, `test_features`, `test_ui`), plus the plain-Python tests of the
Blender add-on and of the Python client. The Client API was driven end to end over a socket against
the AppImage with `python/kenaepro_client.py`.

Not exercised by hand yet: sound through speakers, the new viewer gestures (rotate, vertical flip,
right-drag scrub), stepped playback while watching, the hotkey editor, and the Maya and Blender
scripts inside Maya and Blender.

## Features

Every row of the Keyframe Pro feature comparison has a counterpart:

| Area | What KenaePRO has |
|---|---|
| Playback | Play, step, loop, ping-pong, play once; adjustable speed and range (I / O); **stepped playback, bookmarks only** (Shift+S, or Stepped under the viewer): only bookmarked frames show, each held until the next |
| Scrubbing | Timeline scrubbing and single-frame stepping with audio scrub; **variable mouse scrubbing**: right-drag in the viewer (speed in Preferences, Shift ten times faster) |
| Media | Videos (FFmpeg), image sequences, stills. Image sequences decode on several threads; playback waits for each frame instead of showing a nearby one, and a looping range that fits in the RAM cache (default: an eighth of the memory per viewer) plays from memory after the first pass. Measured: 1080p PNG (4 MB a frame) at a full 24 fps, every frame exact; UHD PNG (17 MB) at 23–24 fps |
| View | Pan and zoom, **rotate 90°** (Ctrl+R / Ctrl+Shift+R), **flip horizontal and vertical** (Shift+F / Shift+V), overlays, ghosting, **always on top** (Ctrl+T) |
| Compare | A and B: right-click a source → Show as A / Show as B. Layout buttons in the status bar and Alt+1…5: one viewer (A), side by side, stacked, **split A \| B** and **split A / B** (a wipe in one picture, drag the divider). "A"/"B" labels in the pictures and in the Project list; Alt+0 swaps them; an offset for B |
| Project | Folders, sources, timelines (edit in/out, several sources, reorder, seamless playback) |
| Bookmarks | Frame, range and annotation bookmarks (list filter), next/previous, Alt+Left/Right to frames with drawings or notes |
| Annotation | Pencil, arrow, ellipse, rectangle, text, eraser, select/move, eyedropper, laser; notes per frame |
| Output | Export through ffmpeg; snapshot to file or clipboard |
| Notes | Type while paused. While playing the box is read-only and Space stops playback; letters typed into the box never fire shortcuts; Esc or a click on the picture gives the keys back |
| Audio | Volume slider under the viewer (Ctrl+Up / Ctrl+Down), mute (M), audio scrub |
| Interface | Dockable panels whose arrangement is remembered (View → Reset Panel Layout); **customisable hotkeys** (Help → Keyboard Shortcuts, F1); **Global Timeline and Range Timeline** under the viewer; Project and Timeline views; Info panel with resolution, fps, total frames and duration |
| Scripting | Client API over TCP, **Python client** (`python/kenaepro_client.py`), **Maya script** (`maya/kenaepro_maya.py`: playblast into KenaePRO, frame sync), Blender add-on (`blender/kenaepro_sync.py`) |

## How it was designed

This is an independent implementation. It was designed from observed behaviour of a macOS
application (feature list, command names of its scripting API, key names of its project file) and
from no copied code, assets, shaders, icons or text. Things taken from the original and why:

| Taken | Why |
|---|---|
| Names of the scripting commands | Scripts written for the original can keep working |
| Key names of the project file | Projects may be exchangeable (unverified, see below) |
| Feature set | It is the point of a Linux port |

Not taken and not to be added: decompiled code, icons, shaders, strings, the licence mechanism.
Check the original's EULA before shipping anything that reads or writes its file format.

Libraries: Qt 6 (LGPL), FFmpeg (LGPL, build without GPL parts). Audio time-stretch is not
implemented; the original uses Rubber Band, which is GPL or commercial. If you add it, decide the
licence first, or use SoundTouch (LGPL).

## Layout

```
src/core/    Project model, .kpro reader/writer, playback, Session (no widgets), strokes,
             frame numbering, export (ffmpeg child process), preferences, audio controller
src/media/   FrameSource interface, still images, image sequences, FFmpeg decoder (optional),
             ClipSequenceSource (several runs back to back), AudioTrack, and AsyncFrameSource
             (worker thread + frame cache around any FrameSource)
src/api/     Client API: dispatcher (transport independent) and a local TCP server
src/ui/      Qt Widgets: main window, OpenGL viewer, viewer area (1 or 2 viewers), timeline panel,
             export and preferences dialogs
blender/     Blender add-on that keeps Blender's frame and the app's playhead together
python/      Python client for the Client API, with its tests
maya/        Maya script: playblast into KenaePRO and frame sync (uses python/kenaepro_client.py)
tests/       Qt Test suite for core/, media/, api/ and the widgets
```

`Session` is the single place that holds state. The GUI and the Client API both go through it, so
behaviour cannot drift between them. Up to two viewers each show a source or a timeline; they
share one playhead (the second can be shifted by an offset), and edits always go to the active
viewer at the playhead.

## Building

Needs CMake 3.21+, a C++20 compiler and Qt 6.5+. FFmpeg 6 or 7 is optional; without it only still
images and image sequences open, and video export and sound are unavailable. Sound also needs
Qt Multimedia (without it the app is silent). Video export runs the `ffmpeg` executable, found on
`PATH` or set in Preferences. Package names below are untested.

```bash
# Debian / Ubuntu
sudo apt install cmake ninja-build pkg-config qt6-base-dev libqt6opengl6-dev \
    libgl1-mesa-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
    libswresample-dev qt6-multimedia-dev ffmpeg
```

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/kenaepro [--api-port 4545] [project.kpro | video.mp4]
```

On macOS the same CMake file works with Homebrew's `cmake`, `qt`, `ffmpeg` and `pkg-config`
(`cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"`). On Linux the builds
that have been verified are the container builds below.

On an older distribution (such as Ubuntu 22.04, whose Qt and FFmpeg are too old) build in a
container with Podman. `scripts/Containerfile.debian13` gives Debian 13's Qt 6.8 and FFmpeg 7.1:

```bash
podman build -t framereview-build -f scripts/Containerfile.debian13 scripts
podman run --rm -v "$PWD":/src:Z -w /src framereview-build scripts/build-in-container.sh
```

### AppImage

`scripts/Containerfile.appimage` is Ubuntu 22.04 (glibc 2.35) with the official Qt 6.8 binaries and
FFmpeg 7.1 built as LGPL with only its built-in codecs, so the AppImage runs on 22.04 and newer and
carries no GPL code. `build-appimage.sh` builds, runs the tests and writes
`build-appimage/KenaePRO-<version>-x86_64.AppImage` (about 48 MB):

```bash
podman build -t framereview-appimage -f scripts/Containerfile.appimage scripts
podman run --rm -v "$PWD":/src:Z -w /src framereview-appimage scripts/build-appimage.sh
```

`scripts/test-appimage-clean.sh` runs it in a container without Qt or FFmpeg. Like every AppImage
it expects from the system what any desktop has: the OpenGL/EGL driver, fontconfig, X11 and
`fusermount` (package `fuse3`; without it, run with `--appimage-extract-and-run`). Sound uses
PulseAudio (PipeWire's PulseAudio layer works).

Two command-line aids exist for testing: `--frame <n>` seeks after opening a file, and
`--screenshot <png>` saves a picture of the window and exits. The screenshot needs a real display:
Qt's `offscreen` platform has no OpenGL on macOS, so the viewer cannot be created there.
`test_runtime` generates its own clip with the `ffmpeg` executable and skips that test if it is
not on `PATH`. `test_ui` needs OpenGL and skips itself without it (ctest still reports a skip as a
pass, so run `build/tests/test_ui` directly and look for "0 skipped"). Set
`FR_TEST_SCREENSHOT=/path/out.png` to save a picture of the window after its drawing test.
`FR_BENCH=1 build/tests/test_runtime benchmark_1080p_blocking_versus_async` runs the decoding
benchmark (it generates a 1080p clip with `ffmpeg`).

For a thread check, configure a second build directory with
`-DCMAKE_CXX_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread` and run with
`TSAN_OPTIONS=abort_on_error=0` (on macOS the default is to abort, which shows up as a crash
report). Qt's own `QMutex` and `QWaitCondition` confuse ThreadSanitizer and produce false reports,
which is why `AsyncFrameSource` uses `std::mutex`, `std::condition_variable` and `std::thread`.

## What works, what does not

"Written" means the code builds and its logic is covered by tests where the text says so, but it has not been tried by hand on real material.

| Area | State |
|---|---|
| Project model, folders/sources/timelines, remove rules | Done, tested (before the latest round; the model gained fields since: fill opacity, pan, hold, cut range) |
| `.kpro` read/write, path resolution, bad-input handling | Done, tested, **format provisional**. Image sequences are stored as a printf path such as `/dir/shot.%04d.png`, which is this project's choice and unverified against the original |
| Playback loop off/loop/ping-pong, range, timer | Done, tested |
| Decoding off the UI thread, frame cache | Done and tested (see the measurements in the history below). The cache now has a size budget set in Preferences; that change is written, not built |
| FFmpeg decoding | Tested on a lossless clip and run on an H.264 clip; constant frame rate only |
| Bookmarks (frame, range, panel, slider marks, next/previous, undo) | Done and tested before the latest round. Since then rewritten for timelines (a bookmark belongs to the source under the playhead) and given a filter (all / only frames with drawings / only frames with notes): written |
| Image sequences (import, folder import with sequence detection, numbered-file display) | Written |
| Timelines: edit clips in a panel, play clips back to back, each trimmed to its source's cut in/out | Written |
| Two viewers (side by side or stacked), second viewer offset, active-viewer highlight | Written |
| Per-source cut in/out, TC offset (per source and project), audio delay, hold annotations, remembered zoom and pan | Written; saved in the project file |
| Frame numbering: global, image-file number, inside the range, timecode | Written |
| Drawing: pencil, arrow, ellipse, rectangle, text, **eraser, select/move/delete, eyedropper, laser pointer**; colour, width, opacity, fill, pencil smoothing; copy/cut/paste drawings between frames | The first five tools and undo were tested before the latest round; everything else is written. Pen pressure is not used |
| Overlays: centre, thirds, phi grid, dynamic symmetry, safe area; flip horizontal; ghosting of neighbouring frames and of neighbouring annotated frames; background cycle | Written |
| Export: video through `ffmpeg` (H.264, ProRes, VP9, MPEG-4; with or without the drawings; with the sound when one video is shown), image sequence (PNG/JPG/TIFF/BMP), all frames / playback range / bookmarked frames; save or copy the current frame | Written. Raw frames are piped to `ffmpeg`; its message is shown when it fails |
| Audio: playback, mute, volume, scrub on stepping, audio delay | Written. Limits: one source at normal speed only (no sound on timelines, no time-stretch), whole track decoded to 48 kHz stereo in memory (capped at about 45 minutes) |
| Preferences (ffmpeg path, cache size, audio buffer, sequence frame rate, confirm on close, API on/off and port), recent files | Written |
| Client API dispatcher | 18 of 40 commands, tested. **The remaining 22 are deliberately not being done.** |
| Client API TCP server | Driven end to end over a socket by hand before the latest round; no automated test; **framing assumed** |
| Blender sync | Add-on written. The plain-Python protocol and mapping logic is tested; the Blender part has never been loaded into Blender. Maya sync is dropped in favour of Blender |
| Colour management (BT.709, gamma) | Not started (see the TODO in `FfmpegFrameSource.cpp`) |

Keys: B frame bookmark, Shift+B range bookmark, `[` / `]` previous/next bookmark, I / O range
start/end, Space play, Left/Right step, V P A E R T X S D L pick a drawing tool (move, pencil,
arrow, ellipse, rectangle, text, eraser, select, eyedropper, laser), M mute, K cycle the viewer
background, Shift+F flip, 0 reset zoom, Ctrl+G go to frame, Ctrl+E export, Ctrl+I import,
Ctrl+Alt+C/X/V copy, cut, paste drawings.

Measurements from the earlier decoding work (one 1080p H.264 clip, Apple M1): the old blocking
jump took about 66 ms of frozen UI; with the worker thread, playback at 30 fps had the exact frame
on time for 150 of 150 ticks, calls cost under 50 µs, and a jump shows the exact frame after about
53 ms on average (122 ms worst). It was checked clean under ThreadSanitizer. Playback follows the
clock and drops frames when decoding is late; real footage (high bitrate, ProRes, 4K) will be
slower. The newer threaded code (clip sequences, audio decoding, export) is also clean under ThreadSanitizer in the tests.

Known gaps and risks of the unbuilt code:

- Timelines: a clip's cut range is read when the timeline is shown; changing a cut while the
  timeline is on screen reconfigures playback, which resets nothing else but is untested.
- Hold annotations show the nearest earlier annotated frame's strokes and cannot be edited there.
- Two viewers share one playhead, so two clips of different length simply run out at different times.
- Export needs even picture sizes for H.264 (the dialog says so); odd sizes work for images.
- Sound on a timeline, time-stretch and sound for image sequences are not implemented.
- The unbuilt code got one read-through by two independent readers (no compiler involved). They
  found no compile error but several logic bugs, which were fixed (eraser skipping fast strokes,
  select/move gesture, cancel while ffmpeg encodes, sound for bookmarked-frame export, cache
  eviction thrash, timeline edits stopping playback, saved range hiding a later cut). Still open and
  known: the cut-in and cut-out commands treat a crossing differently; ghost drawings ignore "hold";
  the text tool's hit box is a fixed size; a selected single-dot pencil stroke shows no highlight;
  the export dialog cannot warn about an odd source size before the export starts; exported video
  carries no colour-matrix tag. 

## Project file (`.kpro`) — provisional

Plain JSON. Key names were read from the original; **nesting and value types are inferred** and
all of it lives in `src/core/KproFormat.cpp`:

- `_meta { format }`, `_project { source_root_dir, image_seq_frame_rate, time_control_frame_offset, viewer { layout } }`
- `folders[]`, `sources[]`, `timelines[] { clips[] }`, ids in `_id`
- per source: `path`, `relative_path`, cut in/out, audio index/delay, TC offset override,
  `bookmarks[]`, `range_bookmarks[]`, `frame_data[] { frame, note, note_label, strokes[] }`
- strokes: `type`, `color` (`#AARRGGBB`), `width`, `text`, `points` as the string `"x,y;x,y"`

To settle it, save a small project from a licensed copy of the original (one video, one
annotation, one bookmark), compare it with the output of `projectToJson`, and fix the differences
in that one file. Base folder ids (`sources`, `timelines`) are this project's choice and will
likely differ.

## Client API

`kenaepro --api-port <n>` (or Preferences > Client API) listens on 127.0.0.1. Request
`{ "cmd": "...", ... }`, response `{ "success": true, "result": {...} }` or
`{ "success": false, "msg": "..." }`. Send `initialize` with `api_version` first. The app keeps one
initialised state for the whole process, so a second connection is answered "Already initialized";
that is harmless.

Implemented: `echo`, `initialize`, `get_config`, `get_project_path`, `new_project`,
`open_project`, `save_project`, `import_file` (`"sequence": true` for an image sequence),
`get_folders`, `get_sources`, `get_timelines`, `add_folder`, `add_timeline`, `remove`, `get_frame`,
`set_frame`, `set_playing`, `get_range`, `set_range`, `get_default_timeline`,
`set_default_timeline`, `get_timeline_elements`, `insert_element_in_timeline`,
`remove_element_from_timeline`, `get_active_in_viewer`, `set_active_in_viewer`,
`set_viewer_layout`, `get_bookmarks`, `create_bookmarks`, `clear_bookmarks`, `add_notes`,
`get_tc_frame_offset_override`, `set_tc_frame_offset_override`.

Recognised but answering "not implemented yet": the other commands listed in
`plannedCommands()` in `src/api/ApiDispatcher.cpp`. They are not planned for now.

The line-per-message framing and parameter names are guesses. Capture real traffic from the
original before claiming compatibility.

## Python client and Maya

`python/kenaepro_client.py` wraps every implemented command (`KenaeProClient`), and works from a
shell too (`python3 python/kenaepro_client.py get_frame`). Tests: `python3 -I python/test_client.py`.

Maya: copy `maya/kenaepro_maya.py` and `python/kenaepro_client.py` to `~/maya/scripts`, then in
the Script Editor run `import kenaepro_maya; kenaepro_maya.show()`. "Playblast to KenaePRO"
playblasts the playback range to PNG frames and opens them as an image sequence; "Connect sync"
keeps Maya's frame and KenaePRO's playhead together. Not yet run inside Maya.

## Blender sync

`blender/kenaepro_sync.py` is an add-on that keeps Blender's current frame and the app's
playhead together, using only `initialize`, `get_frame`, `set_frame` and `set_playing`.

1. Start the app with the Client API on (`kenaepro --api-port 4545`, or tick it in Preferences).
2. In Blender: Edit > Preferences > Add-ons > Install..., pick the file, enable "KenaePRO sync".
   (An installed "framereview sync" from an earlier version can be removed.)
3. In a 3D viewport press N, open the "KenaePRO" tab, press Connect.

Scrubbing in Blender moves the app; scrubbing or playing in the app moves Blender (polled ten times
a second, so Blender follows playback a little coarsely). A frame one side just received is not sent
back. "Mirror play/pause" starts and stops the app with Blender's playback. The mapping is
`review_frame = round((blender_frame - base) * multiplier) + offset`; `base` is the scene start by
default, `offset` is the 0-based review frame that matches it, and the rate ratio handles a 24 fps
review of a 48 fps scene (0.5). Mapping changes apply on the next Connect. Blender calls the app
on its main thread with a half-second timeout, so a stuck app disconnects the add-on instead of
freezing Blender.

Test the plain-Python part without Blender: `python3 -I blender/test_sync_protocol.py`.

## Next steps, in order

1. Try the Blender add-on in Blender, and the audio and export paths on real footage.
2. Try the Maya script inside Maya, and the new viewer gestures and hotkey editor by hand.
3. Playback sync: an optional mode that holds the playhead until the exact frame is ready, and
   hardware decoding for 4K and high-bitrate material.
4. Lock the `.kpro` schema against a real sample.
5. Sound on timelines and time-stretch (use SoundTouch (LGPL); Rubber Band is GPL), colour
   management, Flatpak packaging.

## License

KenaePRO is free software under the GNU General Public License v3.0; see [LICENSE](LICENSE).
Qt and FFmpeg are used under the LGPL.

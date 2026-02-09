<!-- Copilot / AI agent instructions for gstApp -->

# Quick Orientation
- **Purpose:** This is an embedded-oriented GStreamer-based recording + RTSP application (gstApp). It assembles multiple "bins" (VideoBin, EncoderBin, RecordBin, MuxSinkBin, RtspServerBin, CaptureBin, AudioBin) into a single `GstPipeline` and drives capture, encoding, file-splitting and RTSP streaming.
- **Where to start reading:** `main.cpp` (program flow and wiring), `parser.cpp`/`parser.h` (CLI + JSON config parsing), each `*Bin.cpp`/`*.h` (component implementation), and `Makefile` (build flags + deps).

# Big‑picture architecture
- **Components:**
  - `VideoBin` — captures CSI camera input, exposes src pads for capture/record/rtsp.
  - `EncoderBin` — encodes video when `dual_enc == FALSE` (single encoder path) or used differently when `dual_enc == TRUE`.
  - `RecordBin` + `MuxSinkBin` — handles recording, file split logic and audio mixing into recorded files.
  - `RtspServerBin` — provides RTSP server sinks for streaming.
  - `CaptureBin` — per-channel appsrc capture pipeline.
  - `AudioBin` — single audio source that is linked into per-channel sinks and RTSP.
  - `ParserClass` (`parser.cpp`) — central place for command-line and JSON configuration (`json_parser(DEFAULT_JSON_PATH, ...)`).
  - `tcpServer`, `ipc` — network/IPC control hooks used at runtime.

- **Data flow:** CSI camera -> `VideoBin` -> (EncoderBin or RecordBin) -> `MuxSinkBin` -> disk
  and/or -> `RtspServerBin` -> network. Audio flows from `AudioBin` into `MuxSinkBin` and RTSP paths.

- **Why structured this way:** the code keeps capture/encoding/recording/streaming logic separated by Bin classes that expose `getBin*Pad()` helpers used by `main.cpp` to link pads at startup. This makes it easy to add/remove sinks by matching pad add + gst_pad_link patterns used throughout `main.cpp`.

# Build / Run / Debug workflows
- Build: run `make` at repository root. The `Makefile` uses `pkg-config` for GStreamer (+ gst-rtsp-server, glib, json-c, openssl, turbojpeg, rnnoise). Output binary: `bin/gstApp` and object files under `obj/`.

- Important `Makefile` notes:
  - Cross-compile/sysroot options are present but commented out — the project is often built on-target or with a custom toolchain.
  - Additional link flags: LDFLAGS includes a hard-coded rnnoise library path (`-L.../rnnoise/lib -lrnnoise`).

- Run: `./bin/gstApp [options]` — configuration comes from `parser.cpp` (JSON at `DEFAULT_JSON_PATH` and CLI args via `arg_parser`). Examples of runtime options are parsed by `ParserClass` — inspect `parser.cpp` for available flags.

- Logging & debug:
  - The app calls `GST_DEBUG_BIN_TO_DOT_FILE(...)` in places; set `GST_DEBUG` environment and `cmdArg.dotDir` to capture DOT graphs.
  - Use `GST_DEBUG` to increase verbosity (e.g. `GST_DEBUG=3 ./bin/gstApp ...`).
  - The code writes some runtime errors to `/tmp/gst_err` (see `bus_message_parse` error handling).

# Project-specific patterns, conventions and pitfalls
- Pattern: pad creation & linking in `main.cpp` — add sink/source by:
  1. Calling the relevant `*Bin.init()`
  2. Calling `addBin*SrcPad()` / `addBin*SinkPad()` on the appropriate bin
  3. Linking via `gst_pad_link(srcPad, sinkPad)` and checking return for `GST_PAD_LINK_OK`
  4. On failure the code typically `goto main_end` — follow this pattern to keep error handling consistent.

- Dual vs single encoder: `cmdArg.dual_enc` affects whether `EncoderBin` is used for both RTSP and recording (single encoder) or `RecordBin` + encoder are used separately. Inspect `main.cpp` where `dual_enc` branches for correct wiring.

- Threading/event loop: uses GLib threads + `g_main_loop` and `g_main_context_iteration` rather than a single `g_main_loop_run()`; there's also a custom non-blocking stdin checker (`check_terminal_input`) that calls `ParserClass::cmd_parser`.

- Hardware commands and side-effects: `config_camera()` issues `i2cwrite` / `i2cread` system calls. Running on a development machine may require avoiding these or running with hardware present. Tests/CI must mock or avoid `config_camera` side effects.

- Global state: `pipeline`, `cmdArg`, and `loop` are globals used across bins. New code should prefer using existing APIs rather than adding new globals.

# Integration points & external deps
- GStreamer (+ gst-rtsp-server), GLib. See `Makefile` `LIBS` and `pkg-config` usage.
- rnnoise: local link path in `Makefile` — validate presence when building.
- Hardware: uses `i2cwrite`/`i2cread` and expects CSI camera devices; be cautious running on non-target hosts.
- AES password storage: `AESClass` reads/writes default path `DEFAULT_PASSWD_PATH` (see `main.cpp:getPasswdWithAES`).

# How to add a new sink or feature (concise recipe)
- Example: to add a new network sink using existing pattern:
  - Add a new `MyNetBin.{cpp,h}` exposing `init()`, `addBinSinkPad()`, `getBinSinkPad()`.
  - In `main.cpp` after `init()` of source bins, call `myNetBin.init()` then `addBin*SrcPad()` on source and link with `gst_pad_link(sourcePad, myNetBin.getBinSinkPad())` and follow the `if(...) goto main_end;` style on failures.

# Files to inspect for concrete examples
- Startup & wiring: `main.cpp` (bus setup, pad linking, threads, split logic)
- Argument/JSON parsing: `parser.cpp / parser.h`
- Bins (pattern examples): `videoBin.*`, `encoderBin.*`, `recordBin.*`, `muxSinkBin.*`, `rtspServerBin.*`, `captureBin.*`, `audioBin.*`
- Utilities and logging: `util.cpp / util.h`

# Quick checklist for PRs by an AI agent
- Follow pad-linking pattern in `main.cpp` and reuse existing `addBin*` and `getBin*Pad` helpers.
- Avoid changing global `pipeline` usage without cause; prefer integrating with existing bins.
- If modifying hardware config code (`config_camera`), keep side effects gated and document any system calls.

If anything here is unclear or you'd like more detail on wiring for a particular Bin (example diffs for adding a new RTSP substream, or a small runnable test harness), tell me which area and I will expand with concrete code snippets.

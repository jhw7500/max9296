# Handoff — 482caedc-5b1b-4976-8f78-b920be4873cf
_Last updated: 2026-08-12T09:05:12+09:00 · Tool: claude-code · Session: 482caedc-5b1b-4976-8f78-b920be4873cf_

## Active Task
Determine why FHD (1920×1080) @60fps is unreachable on the i.MX8MP + MAX9296 + MAX9295 + AP1302 + AR0234 camera stack, and land the findings as a written analysis plus a reusable per-layer fps diagnostic toolkit. The investigation itself is **finished and conclusive**; what remains is committing the deliverables and deciding whether to pursue the vendor-firmware path.

## Current State

**Done:**
- **Root cause established — two independent gates, both must be cleared** (`docs/fps-limit-analysis.md:9-70`):
  - **(a) Sensor readout time.** 1080p mode line time 26.27 µs × 1080 lines = `SENSOR_FRAME_TIME` 28,368 µs > the 16,667 µs 60fps trigger period, so the sensor falls to a trigger multiple → 19.8–21.8 fps.
  - **(b) MIPI output bandwidth.** AP1302 output is 300 Mbps/lane × 4 = 1.2 Gbps. Theoretical FHD ceiling 36.2 fps, measured 28.3 fps. FHD60 needs 1,990 Mbps = 166% of available.
  - At stock settings both gates bite at ~28–30 fps, i.e. the vendor tuned 1080p as a 30fps mode end to end.
- **Gate (a) is runtime-fixable from the driver** and every register was verified on the board (`docs/fps-limit-analysis.md:419-443`): `R0x201C PREVIEW_LINE_TIME` (0 → 13.0 µs drops `SENSOR_FRAME_TIME` to 14,063 µs), `R0x2020 PREVIEW_MAX_FPS` (30.0 → 60.0), `R0x6112 TRIGGER_MAX_MISMATCH` (20 → 0 lifts FHD@60 from 19.8 to **26.7 fps**). A ready-to-paste ATOMIC write sequence is in the doc at `docs/fps-limit-analysis.md:433-441`.
- **Gate (b) source located to a single byte** (`docs/fps-limit-analysis.md:205-267`, `601-627`): firmware blob `v4l-ap1302-ar0234.fw` offset `0x11` holds `0x2c`, i.e. `0x012c0000` = 300.0 in big-endian u16.16. Patching it to `0xf4` really does move `HINF_MIPI_FREQ` to 499.2 Mbps and the firmware even recomputes sensor line time 26.27 → 15.96 µs on its own — proving the byte feeds the clock tree.
- **But the patch is not a solution.** With the patched blob the AP1302 emits **zero** frames (`R0x0002 HINF_FRAME_CNT` = 0, read upstream of the serializer, so it is the source dying, not the downstream). Companion clock values (PLL / dividers) inside the blob must move together and their locations are unknown. The patched blob exists on the board as `v4l-ap1302-ar0234.fw.mipi500` (md5 `fe68e401f86cbb1c0721d68408b82a0f`); original is `…fw.orig` (md5 `c0830fa0fae6c3c6fecdfd1c84ea9c70`).
- **4-layer measurement methodology built and validated** — sensor (`R0x00FC SENSOR_TOTAL_FRAME_TIME`) → ISP output (`R0x0002` high byte `HINF_FRAME_CNT`) → CSI2 (`/proc/interrupts` ÷ 2, ratio verified 2.000 and resolution-independent) → ISI. The i2c control channel keeps working even when the video link is dead, which is what let us split "source not emitting" from "downstream not receiving".
- **Five tools written** (`tools/`, deployed to the board at `/root/camtest/`): `cam_hard_reset.sh` (module + CSI2/ISI unbind/bind, with an rmmod-failure guard), `cam_fps_probe.sh` (starts a stream to test a condition), `cam_fps_matrix.sh` (4-case channel matrix), `cam_fps_stack.sh` (observe-only 4-layer diagnosis), `cam_fps_watch.sh` (lightweight, touches no i2c). The distinction that matters: probe/matrix *create* a stream, stack/watch only *observe*.
- **Dual-mode AP1302 addressing fixed in `cam_fps_stack.sh`** (`tools/cam_fps_stack.sh:140-151`). Addresses are `0x11`/`0x12` in dual modes (2560×720, 3840×1080) and `0x3c` in single modes — set by the driver from the mode, not discovered (`max9296.c:58-60`, `max9296.c:1966-1969`). Detection is width-first with the `0x11` probe only as fallback, because a non-streaming AP1302 does not answer and would be misread as single. Verified on board: `0x11` and `0x12` return independent counters (0x39 vs 0x3a).
- **Two side findings documented**: the pipeline wedge is caused by `mipi_csis_phy_reset()` being absent from `mipi_csis_start_stream()` in `imx8-mipi-csi2-sam.c` (only probe and runtime-PM resume call it), which is why unbind/bind recovers but `rmmod`/`modprobe` does not (`docs/fps-limit-analysis.md:291-316`); and ISI0/ISI1 are structurally asymmetric with an `ISI_2K` workaround currently in place (`docs/fps-limit-analysis.md:317-395`).
- **Operational recommendation recorded** (`docs/fps-limit-analysis.md:676-684`): FHD 30fps (28.3–29.6 measured) is the correct production setting; FHD 40/50/60 are *worse* (19.9–21.8) and should be blocked; HD 1280×720@60 measures 54.0–55.5 fps and is the only immediate 60fps path, needing no firmware, driver, or DTS change.
- **Board left healthy**: original firmware restored, dual 3840×1080@30, `edgeconf_pim.json` back to `fps=30` with ch0+ch1 enabled (backup `edgeconf_pim.json.bak-052122`), `cam-operate.service` enabled and active, both `-ch0.mp4` and `-ch1.mp4` recording.
- **Knowledge persisted to Notion** (session review completed 2026-08-12): KB [FHD 60fps root cause](https://app.notion.com/p/FHD-60fps-AP1302-blob-MIPI-3b98a230a04e81b88147fdaed9adc595), References [analysis doc + 5 tools](https://app.notion.com/p/max9296-fps-4-5-docs-tools-3b98a230a04e81c391ddc639d925a6bf), plus augments to [fps measurement procedure](https://app.notion.com/p/3b98a230a04e818c93d5fb20e1772a15) and [AP1302 i2c reference](https://app.notion.com/p/3678a230a04e81c08ba0c40631b56aa0).

**In flight:**
- Nothing is running on the board and no background job is pending.
- All deliverables are **uncommitted** on branch `fix/power-refcount-fsync-gates`. `tools/cam_fps_stack.sh` — the most important of the five — is still untracked and would be missed by `git commit -a`.
- `max9296.c` is dirty with a **single commented-out line** (`//{0x00, 0x0320, 2, 0x26, 1, 10},`), an experiment leftover. It is functionally identical to HEAD; decide whether to keep it as a documented dead end or drop it.

**Tried and rejected** (every entry backed by board measurement, `docs/fps-limit-analysis.md:268-289`):
- **ISI bottleneck** — CSI2 301 frames = ISI 301 frames, zero loss. (Not the fps cause; the ISI0/1 asymmetry in §6.2 is a separate real issue.)
- **GMSL/SERDES bandwidth** — AP1302 output 20.0 fps equals SoC receive 20.0 fps, measured simultaneously upstream and downstream.
- **Sensor capability** — forcing line time to 13.02 µs yields `SENSOR_FRAME_TIME` 14,063 µs (≈71 fps) in real operation. The AR0234 is not the limit.
- **Exposure ceiling** — exposure 10 ms against a 28 ms frame; cutting to 5 ms left `SENSOR_FRAME_TIME` unchanged.
- **Flicker correction** — `FLICK_CTRL` (R0x5440) reads `0x0000`, already disabled in this firmware; enabling it changed nothing.
- **AP1302 chip throughput** — spec is 450 Mpixel/s and 1080p@120fps; FHD60 needs 124.4 Mpixel/s = 28% of spec.
- **AP1302 error state** — `WARNING_0~3` and `ERROR` identical to the known-good 720p@60 case.
- **SoC receiver** — 4 lanes at CSIS 500 MHz, within i.MX8MP spec (and explains why an earlier 266→500 MHz clock bump had no effect).
- **MAX9296 `0x0320` runtime writes** (`0x25`/`0x26`/`0x2f`) — all produced zero frames. The driver writes this in `load_regs` *before* CSI output is enabled, so a later poke has no effect.
- **`csis-hs-settle` runtime sweep** via `0x32e50024` DPHYCTRL[31:24], values 4–30 — all produced zero CSI2 events. `mipi_csis_set_hsync_settle()` runs inside `start_stream()` before system enable, so the D-PHY never re-acquires HS.
- **`0x0471` restore** and **`sensor->restart` path** — both already correct in the failing state.
- **Partially effective:** `TRIGGER_MAX_MISMATCH` = 0 improves FHD@60 to 26.7 fps and HD@50 to 49.8 fps — but FHD is still worse than simply requesting 30fps, so there is no practical gain without also clearing gate (b).

## Next Steps
1. **Commit the deliverables.** `git add tools/cam_fps_stack.sh` first (it is untracked), then commit `docs/fps-limit-analysis.md` and all five `tools/cam_*.sh` together. Decide on the `max9296.c` line: either revert it (`git checkout -- max9296.c`) or keep the commented `//{0x00, 0x0320, 2, 0x26, 1, 10},` with a one-line note that runtime `0x0320` changes were measured to produce zero frames. Do **not** sweep in `docs/imx8mp.dtsi`, `docs/imx8mp-evk-test.dtb`, `docs/session-2026-08-10-worklog.md`, or `dw100-spike/` — see Open Questions.
2. **Decide whether to open the vendor (onsemi) request** for a coherent 60fps AP1302 firmware. The complete evidence package is already written at `docs/fps-limit-analysis.md:456-474`: current `HINF_MIPI_FREQ` 300 Mbps/lane, required ≥498 Mbps/lane, chip spec 4 lane × 1.2 Gbps, the blob-offset-0x11 experiment and its zero-output outcome, and proof the AR0234 runs at 13 µs line time. This is a human/business decision, not a code change.
3. **Only after a coherent firmware exists**, implement gate (a) in the driver — the ATOMIC sequence at `docs/fps-limit-analysis.md:433-441` writes `R0x201C`=13.0 µs, `R0x2020`=60.0, `R0x6112`=0 right after firmware load, near the existing `0x1186` write in `max9296_s_stream()`. Gate it on the requested fps so ≤30fps modes keep current behaviour. On its own this caps at 26.7 fps, so it is not worth landing before gate (b) opens.
4. **Optional, independent of the above:** fix the pipeline wedge by adding `mipi_csis_phy_reset()` to `mipi_csis_start_stream()` in `imx8-mipi-csi2-sam.c`. Note the wedge was only reproducible under an artificial "restart a 2-second stream within 0.5 s" pattern and does **not** occur with the real gstApp kill/restart cycle, so this is hardening rather than a live bug.

## Key Decisions
- **Decision:** Treat FHD60 as blocked on vendor firmware rather than continuing register/blob exploration. · **Why:** The one byte that sets output bandwidth was found and proven live, but changing it alone kills ISP output because companion clock values must stay coherent, and those are unlocated inside an opaque blob. · **Alternative considered:** Brute-force searching the blob for the companion PLL/divider values — rejected as an unbounded search with a bricked-camera failure mode on every trial.
- **Decision:** Measure at four layers via hardware counters instead of trusting gstApp output. · **Why:** The gstApp pipeline contains `videorate` and caps negotiation, so downstream fps is not the source rate — this misled the investigation twice early on. · **Alternative considered:** Instrumenting gstApp — rejected because it still cannot see upstream of the SoC.
- **Decision:** Detect dual mode by subdev format width, with the `0x11` i2c probe only as fallback. · **Why:** The driver derives the addresses from the mode (`max9296.c:1966-1969`), so width is the cause and i2c response is merely the effect; a non-streaming AP1302 does not answer and would be misclassified as single. · **Alternative considered:** `i2cdetect` response pattern (what the older Notion note prescribed) — rejected for that false-negative.
- **Decision:** Recommend keeping FHD at 30fps in production and using HD@60 when 60fps is required. · **Why:** Requesting FHD 40/50/60 measures 19.9–21.8 fps, strictly worse than requesting 30 (28.3–29.6), because the sensor drops to a trigger multiple. · **Alternative considered:** Shipping FHD@60 with `TRIGGER_MAX_MISMATCH`=0 for 26.7 fps — still below the 30fps setting, so pointless.

## Open Questions
- [ ] Should the onsemi/vendor firmware request actually be filed, and by whom? Everything else on the FHD60 path is blocked behind it.
- [ ] Do `docs/imx8mp.dtsi`, `docs/imx8mp-evk-test.dtb`, and `docs/session-2026-08-10-worklog.md` (from predecessor session `2958c6b2`) belong in this commit, a separate one, or `.gitignore`? They were reference material pulled in during investigation, not products of it.
- [ ] `dw100-spike/` is untracked and dated 2026-06-05 (i.MX8MP Dewarp Engine backport feasibility) — unrelated to this session. Commit separately, or leave it alone?
- [ ] The ISI IRQ-per-frame ratio drifts between 1.37 and 2.95 in the 4-channel dual 2560×720@60 case while the frame count stays stable at 59 fps. Cause unidentified (overflow / retry / chain events suspected). Not frame loss, but unexplained — worth a look if ISI behaviour ever matters.
- [ ] `max9296.c:3208-3210` comments out the `R0x2012` output-format write, and enabling it plus reloading still leaves the register at `0x50` — the write path is never entered even on the `stream_on == 0` branch. Condition unidentified. No impact on gstApp (g2d converts to NV12).

## Working Environment
- Branch: `fix/power-refcount-fsync-gates` · Base: `master`
- Target board: pim-camera-v016 at `192.168.214.4`; tools deploy to `/root/camtest/`
- Commands to run: no in-repo build or test suite. The kernel module is rebuilt outside this repo and copied to the board by the user. Verification is empirical:
  - `/root/camtest/cam_fps_stack.sh` — 4-layer diagnosis of a running stream (observe only)
  - `/root/camtest/cam_fps_probe.sh -r` — start a stream to test a specific resolution/fps
  - `/root/camtest/cam_hard_reset.sh -s` / `-s -S` — module + CSI2/ISI rebind; `-s` stops `cam-operate.service`, `-S` restarts it
  - `bash -n tools/cam_*.sh` — syntax check before deploying
- Known broken / skipped: no automated tests exist for this driver. `rmmod`/`modprobe` alone cannot clear a wedged pipeline (D-PHY stays latched) — use `cam_hard_reset.sh`. Never proceed to SoC-driver unbind after a failed `rmmod`: the module refcount sticks at −1, `/dev/video3,4` disappear, and even `reboot -f` fails (sysrq required). The hard-reset script guards this.
- Changed files (`git diff --stat HEAD`):
  ```
   docs/fps-limit-analysis.md | 684 +++++++++++++++++++++++++++++++++++++++++++++
   max9296.c                  |   2 +-
   tools/cam_fps_matrix.sh    | 128 +++++++++
   tools/cam_fps_probe.sh     | 243 ++++++++++++++++
   tools/cam_fps_watch.sh     | 106 +++++++
   tools/cam_hard_reset.sh    | 207 ++++++++++++++
   6 files changed, 1369 insertions(+), 1 deletion(-)
  ```
  Untracked and **not** in the stat above: `tools/cam_fps_stack.sh` (17 KB, the primary diagnostic tool), `docs/imx8mp.dtsi`, `docs/imx8mp-evk-test.dtb`, `docs/session-2026-08-10-worklog.md`, `dw100-spike/`.

## Context for the next tool (3-5 sentences)
This repo holds an out-of-tree i.MX8MP camera driver (`max9296.c`) for a GMSL chain of MAX9296 deserializer → MAX9295 serializer → AP1302 ISP → AR0234 sensor, and the work here was a pure investigation into an fps ceiling, not a feature change. The controlling constraint is that the AP1302's behaviour is set by an opaque vendor firmware blob loaded at stream start: one byte in it fixes MIPI output bandwidth at 1.2 Gbps, which caps FHD at roughly 28 fps no matter what the driver or DTS does, and the sensor-side gate is separately tuned for 30fps — both were proven on hardware, so no further diagnosis is needed or wanted. Everything measurable has been measured and written to `docs/fps-limit-analysis.md` (684 lines, the single source of truth; read §1, §5, and §7 before touching anything), with five board-side diagnostic scripts under `tools/`. The immediate job is clerical — stage `tools/cam_fps_stack.sh`, which is untracked and would otherwise be lost, and commit the doc plus tools — after which the FHD60 effort is blocked on a business decision to request coherent firmware from the vendor. This handoff was written by Claude Code and is tool-agnostic; whoever picks it up should resist re-running experiments listed under "Tried and rejected", since each one already has a board measurement behind it.

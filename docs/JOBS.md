# Jobs

Open jobs only. Finished specs and reports live in git history; H10–H31 are
in `git show 9bf40bb:docs/JOBS.md`. Format: `## H<n>: title (owner)`, then
`- **Intern (H<n>): ready for review.**` and `- **Lead (H<n> review):**`.
The product is called **LLCast**; code names (`lls_`, `llstream_*`) stay for now.
`src/drm_display.c` belongs to the user until their display-timing fix lands.

Per the user: write the code first and test lightly, by extending existing
tests (no new test programs or matrices). Device runs happen when hardware is
available. Don't commit: report under the job. The Lead reviews and commits at
each milestone. Re-read this file after any compaction.

## Windows HEVC receive (Intern)

The PC activated Microsoft's HEVC extension, but the decoder failed to start.
**Done when** the PC plays an HEVC stream from a sender.

### Windows HEVC input type follow-up — Intern (2026-10-29)

- The PC's `llcast-20261029-185921-26124-1284739625.log` identifies
  `SetOutputType(NV12)` failing with `MF_E_ATTRIBUTENOTFOUND` (`0xc00d36e6`),
  after successful activation, GPU support and input-type setup. LLCast had
  omitted input frame dimensions. HEVC now supplies an initial 1280×720 size
  before `SetInputType`; H.264 is unchanged. Actual dimensions still come from
  the SPS and the existing output stream-change handler, with no forced scale.
- Microsoft's [HEVC decoder documentation](https://learn.microsoft.com/en-us/windows/win32/medfound/h-265---hevc-video-decoder)
  describes in-stream dimension changes. The missing input-size requirement
  is also recorded in its [documentation feedback](https://github.com/MicrosoftDocs/feedback/issues/3081).
  This is the targeted fix for the observed negotiation error; hardware HEVC
  startup/playback still needs confirmation on the PC.
- MinGW build and forbidden-import checks pass. Published and hash-verified
  all four EXEs to `/mnt/hgfs/deb13/LLCast` at 2026-10-29T11:07:00Z,
  `9bf40bb-dirty`; LLCast SHA-256 starts `1a43b8e36f79`. Kept the loaded old
  EXE as `LLCast-running-3ed36fd558ff.exe` so reopening loads the update.
  Evidence: `build/results/windows-followup/hevc-input-size-*`. No commit.
- PC confirmation: `llcast-20261029-204254-53936-1290953046.log` now reports
  `HEVCVideoExtension, HEVC, D3D11 NV12` and receiver readiness with codecs
  `0x6` (H.264 + HEVC). The missing-attribute startup failure is resolved.
  This log contains no incoming stream yet, so HEVC playback is still unverified.

## H16: Where the PC capture tail comes from (Intern, research)

The 2026-10-27 stream measured capture to encoded at a median of 5.8 ms and a
p99 of 234 ms (PC clock).
- **Found so far:** in the saved box rows, every age above 100 ms reuses an
  earlier DDA present time. That is the age of repeated content, not a
  stall. Fresh content: p99 20.1 ms, max 52.2 ms at startup.
  `build/results/h16-capture-tail/README.md` has the analysis. Live latency
  now excludes repeats.
- **Still to do,** from a new PC profile (`--profile-to`, `docs/PROFILING.md`):
  split the fresh-frame tail by stage (present → acquire → conversion submit
  → bitstream lock). Correlate what stalls remain with `accumulated`,
  keyframes, duplication losses and desktop mode changes. The original PC
  logs were lost with the old share.

## Hardware checks still open

These are implemented and host-tested, but no device has confirmed them yet.
Ask the Lead before using the box, or before asking the user to run something.
- **PC:**
  - Media Foundation encode, picture and recovery. NVENC versus MF encode
    latency on the same GPU (H27).
  - H.264 and AV1 picture, recovery and latency (H28, H29).
  - A desktop update arriving after a repeat is shown at once.
    RECOVER → encode while a DXGI wait is pending (H20).
  - The 2:1 scale exactness pattern (H12).
  - A receiver restart, a DHCP blip and a locked desktop keep the session (H13).
  - Stop hang: cause unconfirmed. The shutdown stage logs are in place, and
    later runs exited cleanly.
- **Box:**
  - Extreme keeps Android's HDMI mode with no projector re-sync; the 50 Hz
    warning (H31; ADB was offline).
  - Switching between 720p and 1080p senders without a receiver restart (H12).
  - Normal versus Extreme: frame rate, losses and display time for each,
    with start and end events and clock domains named (H24).
  - Force-stopping the app mid-session leaves the Extreme session running (H10).
  - Known: restoring Android after Back drops the network for about 30 s (H30).
- **Android sender:** 60 fps on a static screen, an IDR on RECOVER, and
  rotation, with live MediaProjection capture (H25). The tablet's H.264
  path has passed only a 42-frame generated-image check.
- **Tablet:** sustained PC-to-tablet throughput, latency and energy in
  Extreme, and a long soak. The watchdog heartbeat was tested for six minutes.

## Lead: to decide after measuring

- Per-frame FRAME_REPORT is on all the time: 60 uplink packets a second on the
  half-duplex Wi-Fi link. It only feeds the sender's summary and logs.
  Measure its effect on downlink jitter, then consider sending reports only
  while profiling.

## Mac AV1 hardware receive (Intern)

- User requested VideoToolbox hardware only, for M3 and newer. The existing
  AV1 OBU → av1C/sample → NV12 → Metal path is retained. Register Apple's
  supplemental AV1 decoder once before capability checks/session creation;
  keep requiring hardware acceleration, with no dav1d dependency or fallback.
- Receiving settings now expose, load and save the AV1 checkbox, disabled
  when hardware support is absent. Previously saving settings could remove
  AV1 from the receive mask because only HEVC/H.264 were represented. Existing
  saved choices are preserved; enable AV1 explicitly if it was excluded.
  Auto still chooses HEVC, then H.264. One decoder startup log identifies
  the actual codec and dimensions.
- Extended the existing Mac self-test: AV1 format/sample conversion, then
  two embedded key/inter frames and NV12/IOSurface pixel checks when AV1
  hardware exists. No AV1 encoder or external fixture is needed on the Mac.
  Unsupported hardware is skipped without weakening the HEVC check.
- Light checks: changed Mac files and the sample helper compile for arm64
  and x86_64, macOS 14 target, with clang 19/macOS 15.5 SDK and warnings as
  errors. Host AU/Mac-options tests pass; the generated AV1 fixture decodes
  to the expected Y/U/V values on Linux. Mac linking, UI and hardware playback
  remain untested here. Evidence: `build/results/mac-av1/`. No commit.
- Reference: Apple's [supplemental decoder registration](https://developer.apple.com/documentation/videotoolbox/vtregistersupplementalvideodecoderifavailable(_:)).

# AV1 implementation (H28)

Status (2026-10-29): the box/Extreme MPP plugin now advertises HEVC only; AV1
there and in Android Normal without hardware AV1 comes from dav1d (CPU), see
`tablet-av1.md`. AV1 latency comparisons are still unmeasured.

- Wire v2 codec 3 is active. Each frame is a temporal unit beginning with a
  temporal delimiter, followed by OBUs carrying their own size fields. IVF and
  AV1 Annex-B container lengths are not transmitted. Only one spatial/temporal
  layer is accepted. A recovery frame has its own valid sequence header and
  begins with a shown KEY_FRAME; an intra-only or show-existing frame is not KEY.
- `src/common/av1_obu.c` reads bounded OBU lengths, splits temporal units, and
  parses sequence-header dimensions and av1C fields. It does not validate tile
  entropy data; decoder errors still use the existing recovery path. The
  existing AU test contains two small libaom-generated frames and malformed
  variants; no external test fixture or codec library is needed at runtime.
- NVENC queries the device's codec GUIDs. AV1 uses Main 8-bit 4:2:0, P1/ULL,
  CBR, one-frame VBV, no B-frames, repeated sequence headers, BT.709 limited and
  left chroma. MF uses its AV01 subtype and the H27 hardware path. Both normalize
  missing delimiters and validate the resulting temporal unit before sending.
- MPP advertises AV1 only when the installed vendor library reports support.
  VideoToolbox advertises it only when `VTIsHardwareDecodeSupported` succeeds;
  its format description contains av1C plus the sequence OBU. Samples omit
  temporal delimiters, as required by the AV1 ISO media binding.
- Lead integrated the H28 Android Normal-receiver update: hardware
  `video/av01`, a third JNI decoder slot and capability bit.
- H29 user decision: Auto selects HEVC then H.264, never AV1. AV1 requires an
  explicit selection and hardware support at both ends. Use the Windows CLI with
  `--codec hevc` and `--codec av1`, identical mode/bitrate/backend and opt-in
  profiles, to compare on the same sender and receiver. Hardware checks must
  cover picture/colour correctness, recovery after loss and capture-to-flip
  timing, plus optical capture-to-projector timing for glass-to-glass claims.
  No AV1 hardware latency or compatibility results have been measured here.

Sources: [AV1 syntax](https://aomediacodec.github.io/av1-spec/),
[av1C and sample binding](https://aomediacodec.github.io/av1-isobmff/),
[CoreMedia format creation](https://developer.apple.com/documentation/coremedia/cmvideoformatdescriptioncreate(allocator:codectype:width:height:extensions:formatdescriptionout:)),
[NVENC guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html),
and the pinned `third_party/nv-codec-headers/include/ffnvcodec/nvEncodeAPI.h`.

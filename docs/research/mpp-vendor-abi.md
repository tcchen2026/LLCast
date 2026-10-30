# R2 — Vendor MPP ABI and HEVC fast modes

Status: acted on. `src/mpp_decoder.c` passes 32-bit `MppPollType` timeouts
(static-asserted) and sets H6's `DISABLE_ERROR`. The fast-parse comparison
has not been run.

2026-10-26. **All eight requested command IDs match the actual vendor binary.**
NV12 and existing colour enums match too. Do not infer complete API compatibility:
format sentinels changed, and the vendor timeout parameter is 32-bit.

## Evidence and command values

Read-only analysis of the previously pulled `build/vendor/libmpp.so`:

```text
file: ELF64 little-endian AArch64, Android 27, NDK r21e, debug_info, not stripped
version previously logged on box: 8a4fa843 (2023-07-31)
Build ID: adad12a99593b26da212669308a69494e4f6cf9f
SHA256: d19975db086a59c3073ea3fe085c0f092eb660f640eb9a456c517842d162a296
```

I extracted enum constants from the library's DWARF using NDK
`llvm-dwarfdump --name=CONSTANT build/vendor/libmpp.so` and compared them with a
host-compiled program including our headers at `20959ec3dabb` (2026-10-16).
For example, `MPP_SET_OUTPUT_TIMEOUT` yields `DW_AT_const_value (2097159)`.
Repeated compilation-unit definitions agreed.

| Command | Vendor = current headers | Parameter for this firmware |
| --- | --- | --- |
| MPP_SET_INPUT_TIMEOUT | 0x200006 | 32-bit MppPollType pointer; see below |
| MPP_SET_OUTPUT_TIMEOUT | 0x200007 | 32-bit MppPollType pointer; see below |
| MPP_DEC_SET_EXT_BUF_GROUP | 0x310002 | MppBufferGroup handle, not pointer-to-handle |
| MPP_DEC_SET_INFO_CHANGE_READY | 0x310003 | NULL |
| MPP_DEC_SET_PARSER_SPLIT_MODE | 0x310005 | RK_U32 pointer; set before init |
| MPP_DEC_SET_PARSER_FAST_MODE | 0x310006 | RK_U32 pointer; set before init |
| MPP_DEC_SET_IMMEDIATE_OUT | 0x31000c | RK_U32 pointer |
| MPP_DEC_SET_ENABLE_FAST_PLAY | 0x31000e | RK_U32 pointer in this version |

Exact vendor commit `8a4fa843` was not found in the public Rockchip repository.
For readable implementation, use nearby official
[`4fe51a6c2961`, 2023-07-28, RK3528A support](https://github.com/rockchip-linux/mpp/commit/4fe51a6c2961656624df1f8b560e6a79e15a543f),
and distinguish it from binary-confirmed findings.
[Old command enum](https://github.com/rockchip-linux/mpp/blob/4fe51a6c2961656624df1f8b560e6a79e15a543f/inc/rk_mpi_cmd.h),
[current command enum](https://github.com/rockchip-linux/mpp/blob/20959ec3dabb313b8c33ca0b7b09c06ffd2a9caa/inc/rk_mpi_cmd.h).

**Timeout trap:** current comments say `RK_S64`, but vendor
`Mpp::control_mpp` reads a word: instruction at `0x40698` is `ldr w4,[x2]`.
The adjacent validation accepts -1 (block), 0 (nonblock), or 1–8000 ms.
The original `RK_S64 timeout=20` happened to work because this little-endian
binary reads its low 32 bits. Use the known vendor parameter layout, or document
that compatibility explicitly; do not assume arbitrary 64-bit timeouts work.
[Contemporaneous implementation](https://github.com/rockchip-linux/mpp/blob/4fe51a6c2961656624df1f8b560e6a79e15a543f/mpp/mpp.cpp#L1074).

## Format and colour ABI

- Existing `MppFrameFormat` IDs agree: YUV formats 0–16 and RGB formats
  0x10000–0x1000d. In particular `MPP_FMT_YUV420SP=0` is unchanged.
- New headers add YUV444SP_10BIT/AYUV and ARGB4444/ARGB1555 entries absent from
  vendor DWARF. Sentinels differ: YUV_BUTT 17 -> 20, RGB_BUTT 65550 -> 65552,
  FMT_BUTT 65551 -> 65553. Do not use new sentinel ranges as proof of firmware
  format support; accept only tested linear NV12 for this renderer.
- All 16 `MppFrameColorSpace` entries, including NB, agree. Crucially
  `MPP_FRAME_SPC_BT709=1`, while DRM plane BT.709 is **3**. Map explicitly.
  UNSPECIFIED=2; RGB=0; BT470BG=5; SMPTE170M=6; BT2020_NCL=9.
- `MppFrameColorRange` also agrees: unspecified=0, MPEG/limited=1,
  JPEG/full=2. Matrix and range are separate MPP fields.

These are binary-DWARF comparisons with
[our frame header](https://github.com/rockchip-linux/mpp/blob/20959ec3dabb313b8c33ca0b7b09c06ffd2a9caa/inc/mpp_frame.h).
Continue using opaque frame/buffer handles and exported accessors; enum agreement
does not validate private structure layouts or newer controls.

## What the fast modes do

- **Parser fast mode** allows parsing/register preparation to overlap prior
  hardware work. The 2023 decoder normally selects three HAL tasks when enabled;
  HEVC HAL advertises support and allocates multiple register/syntax buffers.
  It does not remove P-frame reference dependencies, reduce hardware decode
  duration by definition, or turn incomplete AUs into safe submissions.
  With one input in flight, expect little overlap benefit; benchmark bounded
  pipelining only after display is decoupled.
  [Decoder init](https://github.com/rockchip-linux/mpp/blob/4fe51a6c2961656624df1f8b560e6a79e15a543f/mpp/codec/mpp_dec.cpp#L650),
  [previous-task wait](https://github.com/rockchip-linux/mpp/blob/4fe51a6c2961656624df1f8b560e6a79e15a543f/mpp/codec/mpp_dec_normal.cpp#L453).
- The box's recorded hardware ID `0x70393f05` selects **VDPU382**, despite the
  kernel node being named rkvdec0. Actual `hal_h265d_init` disassembly selects
  `hal_h265d_vdpu382` at `0xef0b8–0xef0d0` (confirmed via its GOT relocation).
  Examine that backend, not only `hal_h265d_rkv.c`. Its fast mode uses three
  buffer sets and different error propagation; retain errinfo/discard checks.
  [HAL selection](https://github.com/rockchip-linux/mpp/blob/4fe51a6c2961656624df1f8b560e6a79e15a543f/mpp/hal/rkdec/h265d/hal_h265d_api.c#L55),
  [VDPU382 buffers/error handling](https://github.com/rockchip-linux/mpp/blob/4fe51a6c2961656624df1f8b560e6a79e15a543f/mpp/hal/rkdec/h265d/hal_h265d_vdpu382.c#L130).
- **Immediate output** makes HEVC output the current decoded picture in decode
  order. **Fast play** instead bypasses normal reorder waiting for IDR, or the
  first eligible BLA picture; it is not a general per-frame speed control.
  The immediate-output branch returns before fast-play logic, so fast-play adds
  no benefit there. With zero `num_reorder_pics`, ordinary output already needs
  no reorder queue once a picture is ready. Binary `mpp_hevc_output_frame` at
  `0x94e34` confirms the early fast-out branch and IDR/BLA checks at `0x94f24`.
  [Matching source routine](https://github.com/rockchip-linux/mpp/blob/4fe51a6c2961656624df1f8b560e6a79e15a543f/mpp/codec/dec/h265/h265d_parser.c#L1152).

**Lead: optional later test:** compare fast-parse 0/1 with identical no-B HEVC,
bounded submission depth and display settings; measure AU-ready-to-flip as well
as decode delivery and verify pixels/loss recovery. Do not toggle decoder flags
mid-session. The H6 user decision below supersedes the original recommendation
to leave error checking enabled. No box settings or binaries were changed
for this research.

## H6: damaged output control (offline binary evidence)

`MPP_DEC_SET_DISABLE_ERROR = 3211275 = 0x31000b`, unchanged in our headers.
The pulled binary above supplies both the enum DWARF and the implementation:

```text
llvm-dwarfdump --name=MPP_DEC_SET_DISABLE_ERROR build/vendor/libmpp.so
  DW_AT_const_value (3211275)
llvm-dwarfdump --name=disable_error build/vendor/libmpp.so
  DW_AT_type ("RK_U32"), mpp_dec_cfg.h:74, member offset 0x30
mpp_dec_set_cfg_by_cmd: table at 0x17d804, entry 7 -> 0x45664
  0x45668: ldr w4, [x2]        # reads exactly 32 bits from the argument
  0x4576c: str w4, [x0, #0x30] # disable_error in decoder configuration
h265d_callback:
  0x95874: ldr w8, [x8, #0x30]
  0x9587c: cbnz w8, 0x95a34    # bypass error/reference marking
  0x958d8: bl mpp_frame_set_errinfo # bypassed when enabled
```

H6 sets `RK_U32 disable_error = 1` after initialization, alongside immediate
output. This enables the vendor's error-suppression path to keep pictures
flowing. It can also suppress `errinfo`, so absence of that flag does **not**
prove intact references: packet loss independently marks frames damaged until
a recovery key. Returned errinfo/discard pictures with buffers are displayed;
an indication without a buffer is counted, requests recovery, and does not
reset the decoder. Put/get failure or a 100 ms stall still triggers reset.

Raw DWARF, jump-table calculation and disassembly are in
`build/results/h6-mpp-abi/`. This confirms the control ABI and branch behavior,
not successful concealment on the device. T2 must verify actual damaged output
and continued flips with 1% injected packet loss when ADB returns.

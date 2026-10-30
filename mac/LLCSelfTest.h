// LLCast --selftest: the whole path except capture and display, needing no
// permissions: synthetic NV12 -> hardware HEVC -> UDP loopback -> receiver
// session -> hardware decode -> luma compared with the source. Also checks
// AV1 format/samples and, when available, hardware key/inter decode and pixels.
// Returns 0 on success, 77 when this Mac (or VM) has no hardware HEVC,
// 1 on failure.
#pragma once
int LLCRunSelfTest(void);

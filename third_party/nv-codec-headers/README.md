# NVENC declarations

Encoding-only subset of [FFmpeg nv-codec-headers n13.0.19.0](https://github.com/FFmpeg/nv-codec-headers/tree/n13.0.19.0),
commit `e844e5b26f46bb77479f063029595293aa8f812d` (SDK 13.0.19 / API 13.0).
`include/ffnvcodec/nvEncodeAPI.h` is unmodified; its NVIDIA MIT notice is retained.
`UPSTREAM_README` is the upstream driver requirement. CUDA/NVDEC headers are not
used or vendored. No NVIDIA libraries or binaries are distributed here.

The PC driver is not known yet. The user authorized selecting a reasonably recent
tag: API 13.0 provides the required HEVC P1/ULL controls without depending on newer
13.1 features. [Upstream requires Windows driver 570.0 or later](https://github.com/FFmpeg/nv-codec-headers/blob/n13.0.19.0/README).
That is a minimum API requirement, not proof the GPU supports HEVC or our texture
path; capability queries, preset initialization and resource registration also
have to succeed.

`NvencEncoder::open()` loads the driver's `nvEncodeAPI64.dll` from System32, calls
`NvEncodeAPIGetMaxSupportedVersion`, and rejects support below `(13 << 4) | 0`
before creating the versioned function table or session. This query's packing is
different from `NVENCAPI_VERSION`. It prints the reported API and the required
driver baseline. There is no ABI-version downgrade with newer struct layouts.

To change the baseline, replace the header and this provenance together, then
rebuild and validate against the PC driver; do not just lower a version constant.

# R1 — Rockchip DRM/VOP2

2026-10-26. **Use atomic NONBLOCK + PAGE_FLIP_EVENT, plane COLOR_SPACE=3,
and leave ASYNC_COMMIT=0.** Non-blocking commits with flip events now run on the
box at 1080p60; fences remain untested.

Sources are Rockchip's BSP, not mainline: historical
[`8ea19e33432c` (2023-07-31, 5.10.160)](https://github.com/rockchip-linux/kernel/tree/8ea19e33432c15ced01500a93a04598a1a156cfc),
cross-checked against develop-5.10
[`95a3ad83f7f9` (2026-08-28)](https://github.com/rockchip-linux/kernel/tree/95a3ad83f7f90e0e672b9c99b0858b4964318667).
Neither is proven identical to the installed 5.10.157 firmware.

## Plane input colour

`COLOR_SPACE` uses V4L2 numbering, **not** MPP's matrix enum or connector
`Colorspace`. Accepted property range is 0–12. The vendor CSC mapping is:

| Value | V4L2 meaning | YUV-to-RGB CSC selected |
| --- | --- | --- |
| 0 | DEFAULT | BT.709 limited |
| 1 | SMPTE170M | BT.601 limited |
| 2 | SMPTE240M | BT.709 limited (driver mapping) |
| 3 | REC709 | **BT.709 limited** |
| 4 | BT878 | default: BT.709 limited |
| 5, 6 | 470_SYSTEM_M, 470_SYSTEM_BG | BT.601 limited |
| 7 | JPEG | BT.601 full |
| 8, 9 | SRGB, OPRGB | default: BT.709 limited |
| 10 | BT2020 | BT.2020 limited |
| 11, 12 | RAW, DCI_P3 | default: BT.709 limited |

For NV12 into RGB composition, `vop2_setup_csc_mode()` enables Y2R and uses this
mapping. Value 3 is therefore appropriate for our limited-range HEVC output;
it is not a separate matrix plus range setting. Full-range BT.709 constants
exist privately at 0xfe, but cannot pass the exposed 0–12 property range.
[VOP2 mapping](https://github.com/rockchip-linux/kernel/blob/8ea19e33432c15ced01500a93a04598a1a156cfc/drivers/gpu/drm/rockchip/rockchip_drm_vop2.c#L2736),
[property creation](https://github.com/rockchip-linux/kernel/blob/8ea19e33432c15ced01500a93a04598a1a156cfc/drivers/gpu/drm/rockchip/rockchip_drm_drv.c#L1300),
[V4L2 values](https://github.com/rockchip-linux/kernel/blob/95a3ad83f7f90e0e672b9c99b0858b4964318667/include/uapi/linux/videodev2.h#L202).

## Commit and retirement semantics

- **ASYNC_COMMIT:** the property is stored in plane state, then consulted by
  Rockchip's legacy `DRM_IOCTL_MODE_SETPLANE` helpers to set
  `legacy_cursor_update`. Ordinary atomic requests do not use it to request
  immediate scanout. Do not interpret its presence as tearing support.
  [Legacy path](https://github.com/rockchip-linux/kernel/blob/8ea19e33432c15ced01500a93a04598a1a156cfc/drivers/gpu/drm/rockchip/rockchip_drm_vop2.c#L5490).
- Legacy cursor-style updates skip the helper's vblank wait; that changes CPU
  waiting/cleanup, not a guarantee of safe immediate framebuffer reuse.
  This BSP's atomic ioctl explicitly rejects `DRM_MODE_PAGE_FLIP_ASYNC` with
  `EINVAL`, even though `async_page_flip` is advertised for legacy operation.
  [Helper](https://github.com/rockchip-linux/kernel/blob/8ea19e33432c15ced01500a93a04598a1a156cfc/drivers/gpu/drm/drm_atomic_helper.c#L1510),
  [ioctl rejection](https://github.com/rockchip-linux/kernel/blob/8ea19e33432c15ced01500a93a04598a1a156cfc/drivers/gpu/drm/drm_atomic_uapi.c#L1315).
- **NONBLOCK + EVENT:** Rockchip uses `drm_atomic_helper_commit`, with a commit
  worker and vblank retirement. Nonblocking removes the caller's refresh wait,
  not the hardware refresh boundary. The helper returns `EBUSY` while a prior
  affected CRTC/plane/connector flip is unfinished. Keep one pending commit;
  retry after draining events, replacing only the uncommitted ready frame.
  [Rockchip commit tail](https://github.com/rockchip-linux/kernel/blob/8ea19e33432c15ced01500a93a04598a1a156cfc/drivers/gpu/drm/rockchip/rockchip_drm_fb.c#L206),
  [stall checks](https://github.com/rockchip-linux/kernel/blob/8ea19e33432c15ced01500a93a04598a1a156cfc/drivers/gpu/drm/drm_atomic_helper.c#L1952).
- VOP2 delivers pending flip events from its frame-start/vblank interrupt;
  topology changes can skip a vblank. Keep routing/geometry fixed during
  steady-state timing. [IRQ path](https://github.com/rockchip-linux/kernel/blob/8ea19e33432c15ced01500a93a04598a1a156cfc/drivers/gpu/drm/rockchip/rockchip_drm_vop2.c#L10665).
- **Timestamp:** event `sec/usec` is monotonic vblank accounting time, not when
  `drmHandleEvent()` runs. This VOP2 source lacks a precise scanout timestamp
  callback, so accounting falls back to `ktime_get()` near interrupt handling;
  include IRQ delay/uncertainty in phase alignment. It is not projector light time.
  [Timestamp fallback](https://github.com/rockchip-linux/kernel/blob/8ea19e33432c15ced01500a93a04598a1a156cfc/drivers/gpu/drm/drm_vblank.c#L852),
  [event timestamp](https://github.com/rockchip-linux/kernel/blob/8ea19e33432c15ced01500a93a04598a1a156cfc/drivers/gpu/drm/drm_vblank.c#L977).
- **OUT_FENCE_PTR:** exposed on CRTC 85; core creates a CRTC fence and attaches
  it to the same pending event. Pass a valid userspace pointer to an `int32_t`
  initialized to -1; after successful commit, poll/close the returned sync-file
  FD. It signals application of the new state, retiring the previous framebuffer;
  the newly displayed MPP frame must survive until a later replacement.
  Source supports it, but property existence alone is not a runtime test.
  [Fence setup](https://github.com/rockchip-linux/kernel/blob/8ea19e33432c15ced01500a93a04598a1a156cfc/drivers/gpu/drm/drm_atomic_uapi.c#L1114).

## HDMI output is separate

`color_format`: 0 RGB, 1 YCbCr444, 2 YCbCr422, 3 YCbCr420; 4/5 are
high/low-subsampling preference policies, 6 invalid. Prefer the already-working
RGB/8-bit output for initial tests. `hdmi_quant_range`: 0 default (mode/sink
dependent), 1 limited, 2 full. Connector `Colorspace=2` means BT709_YCC;
**plane COLOR_SPACE=3 and connector Colorspace=3 mean different things**.
[Connector properties](https://github.com/rockchip-linux/kernel/blob/8ea19e33432c15ced01500a93a04598a1a156cfc/drivers/gpu/drm/rockchip/dw_hdmi-rockchip.c#L2564),
[observed enum dump](../../build/probe/drm-props.txt).

The bridge applies RGB range conversion and HDMI AVI signalling separately from
plane Y2R. Match projector range; limited input NV12 does not require limited
HDMI RGB. The vendor quant-range setter can call HDMI setup/AVMUTE immediately:
do not assume connector-property writes, including TEST_ONLY, are side-effect-free.
[Property setter](https://github.com/rockchip-linux/kernel/blob/8ea19e33432c15ced01500a93a04598a1a156cfc/drivers/gpu/drm/rockchip/dw_hdmi-rockchip.c#L2871),
[bridge CSC](https://github.com/rockchip-linux/kernel/blob/95a3ad83f7f90e0e672b9c99b0858b4964318667/drivers/gpu/drm/bridge/synopsys/dw-hdmi.c#L1434),
[range update](https://github.com/rockchip-linux/kernel/blob/95a3ad83f7f90e0e672b9c99b0858b4964318667/drivers/gpu/drm/bridge/synopsys/dw-hdmi.c#L3583).

## Box evidence and next check

Saved `build/probe/drm-props.txt` (abridged):

```text
Connector 333: color_format=0; color_depth=8; hdmi_quant_range=0; Colorspace=0
CRTC 85: OUT_FENCE_PTR=0 [0..18446744073709551615]
Plane 57: COLOR_SPACE=0 [0..12]; ASYNC_COMMIT=0 [0..1]
```

My read-only `adb shell 'uname -a; cat /sys/kernel/debug/dri/0/summary; ...'`
returned `error: closed`; subsequent `adb devices -l` reported
`127.0.0.1:5555 offline`. No reconnection, property write or modeset attempted.

**Lead: during your owned display test, please check** NONBLOCK return duration,
event sequence/cadence, intentional second-commit EBUSY, and OUT_FENCE_PTR signal
versus the replacing flip. Use COLOR_SPACE=3 with known limited-range bars and
verify Android restoration. Any connector-range or legacy ASYNC_COMMIT experiment
needs a coordinated display slot; none was run for this research.

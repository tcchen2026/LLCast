# LLCast

Low-latency screen streaming from a Windows NVIDIA gaming PC (wired Ethernet)
to a Rockchip RK3528A Android TV box on Wi-Fi, within 30 Mbps. The goal is the
lowest glass-to-glass latency; fluency and frame rate come before picture
perfection.

```
PC:  Desktop Duplication -> GPU scale/copy + NV12 -> NVENC HEVC (P1, ultra-low latency,
     P-frames only) -> UDP datagrams (+ optional Reed-Solomon parity)
Box: UDP receive + reassembly (+ FEC repair) -> Rockchip MPP decode -> DRM plane flip
```

The sender mirrors its main screen with a profile: Low (1280×720 at 30 fps),
Medium (1920×1080 at 60 fps) or a custom size and rate, scaled to fit, or the
top-left part unscaled with "no scale". The bandwidth budget is Auto (from the
profile and codec) or typed; 10% of it is FEC parity. A lost
frame is never resent or waited for: FEC repairs what it can, otherwise the box
keeps decoding and showing frames (marked damaged) and asks for a recovery
keyframe. Rules for contributors are in [AGENTS.md](AGENTS.md).

## Status

- **Working:** the PC streams its desktop to the box over Wi-Fi at 1080p60,
  indefinitely, with keep-moving loss handling and optional FEC. Measured
  about 80 ms glass to glass (camera, PC monitor to projector); the box-side
  breakdown is in [docs/PLAN.md](docs/PLAN.md).
- **Apps:** LLCast for Windows, Android and macOS; each sends and receives.
  Requirements are in [docs/release-notes.md](docs/release-notes.md).
- **Open:** a timed display commit to cut the 22 ms flip wait, the PC capture
  tail and the reference baseline; see [docs/PLAN.md](docs/PLAN.md).
- The code and repository still use the working name LLStream (`lls_`,
  `llstream_*`).

The plan, phases and decisions are in [docs/PLAN.md](docs/PLAN.md); the
architecture in [docs/DESIGN.md](docs/DESIGN.md).

## Layout

| Path | Contents |
| --- | --- |
| `src/common/` | Portable C: wire protocol, frame assembler, FEC, receiver session, statistics |
| `src/` | Box programs: `llstream_rx` (receiver), MPP decoder, DRM display, `hevc_decode_test` |
| `sender/` | Portable sender session and `llstream_send` (clip replay, synthetic probe) |
| `sender/win/` | `llstream_win` (capture, convert, encode, send) and `llstream_clock` |
| `sender/win/llcast/` | `LLCast.exe`, the Windows window |
| `receiver/win/` | Windows hardware H.264/HEVC decode and Direct3D presentation |
| `mac/` | `LLCast.app` for macOS: sender and receiver ([docs/MACOS.md](docs/MACOS.md)) |
| `android/` | `LLCast.apk`: sender, Normal receiver and rooted Extreme receiver ([android/README.md](android/README.md)) |
| `tools/` | Box and VM runners, analysis, Windows deploy; `tools/windows/` holds the PC launchers |
| `tests/` | Host tests (CTest) |
| `docs/` | Plan, design, jobs, research notes |

## Build and test

```sh
cmake -S . -B build/host && cmake --build build/host -j8
ctest --test-dir build/host --output-on-failure
python3 tools/deploy_windows.py        # mingw build + test kit to the shared folder
python3 tools/build_apk.py --ci        # build/apk/LLCast.apk (development signing by default)
tools/macos/build.sh --test            # on a Mac: universal LLCast.app and HEVC self-test
```

Versions come from the commit's UTC date and first six hash digits:
`tools/current-tree-version.sh [commit]` prints `vYYYY.MM.DD-abcdef`.
Windows and Android use that version without `v`; macOS uses `YYYY.MM.DD`.
Windows' numeric file version and the Mac build number include the commit's
UTC minute; Android's version code is its Unix timestamp divided by 60.
Commits within the same minute share these numeric versions. Source exports
without Git build as `0.0.0-dev`. Generated stamps stay in build directories.

## Releases

[Download requirements](docs/release-notes.md). Run **Release**
([release.yml](.github/workflows/release.yml)) by hand to create a draft,
build Windows → Android → macOS, then publish only when all three downloads
are present. `prerelease` marks new releases; `smoke` adds Windows/Wine and
Mac chooser screenshots. A missing file leaves the draft unpublished.

The **Windows**, **Android** and **macOS** workflows also run individually
and replace their own asset on the same commit's release. All keep build
artifacts. Without signing secrets, Android keeps a development APK and Mac
an ad-hoc app zip, with no release asset. **Checks** runs on push/PR, only on
Linux: host tests, MinGW build/import checks, APK build and prechecks. It
uses no signing secrets and uploads no artifacts.

Create a GitHub environment named `release` with these secrets:

| Platform | Secrets |
| --- | --- |
| Android | `LLCAST_KEYSTORE_BASE64`, `LLCAST_STORE_PASSWORD`, `LLCAST_KEY_ALIAS`; optional `LLCAST_KEY_PASSWORD` (defaults to the store password) |
| Mac | `MACOS_DEVELOPER_ID_P12` (base64), `MACOS_DEVELOPER_ID_PASSWORD`, `ASC_KEY_ID`, `ASC_KEY_P8` (the `.p8` text), `ASC_ISSUER_ID` |
| Windows | None; the EXE is unsigned |

Create the Android release key **once**, keep a backup, and use it for every
update. These commands prompt for the PKCS12 password and write an unwrapped
base64 value for `LLCAST_KEYSTORE_BASE64`; set the alias secret to `llcast`.
Keep the key and its encoded copy out of Git.

```sh
umask 077
mkdir -p build/signing
keytool -genkeypair -keystore build/signing/llcast-release.p12 -storetype PKCS12 \
  -alias llcast -keyalg RSA -keysize 3072 -validity 10000 -dname "CN=LLCast"
base64 < build/signing/llcast-release.p12 | tr -d '\n' > build/signing/llcast-release.p12.base64
```

## Run

- **On the PC:** the launchers in the shared folder (`clock.bat`, `stream.bat`,
  `vmtest.bat`, ...), described in `tools/windows/README.txt`. Each writes
  `logs/<stamp>-<name>/`.
- **On the box:** `python3 tools/run_receiver.py --display` (or `--no-decode`
  for a network-only probe). It prints the box address for the PC launchers.
- **On this VM, without the box:** `python3 tools/run_receiver.py --local --dump --profile --seconds 120`,
  then `vmtest.bat` on the PC.

Runner and analysis details: [tools/README.md](tools/README.md). Windows
send/receive details: [sender/win/README.md](sender/win/README.md).

## License

Apache License 2.0; see [LICENSE](LICENSE). Third-party headers in
`third_party/` keep their own licenses, listed in [NOTICE](NOTICE).

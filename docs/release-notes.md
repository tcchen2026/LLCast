# LLCast

Screen streaming over UDP, with optional FEC and a focus on fluency and low
latency. Download the sender and the receiver for your devices.

| Download | Contents and requirements |
| --- | --- |
| **LLCast-Windows-x64.zip** | `LLCast.exe`, `LICENSE`, `NOTICE`. Windows 10/11 x64; sends or receives. **Send** needs a hardware HEVC, H.264 or AV1 encoder, through NVENC (NVIDIA driver 570 or later) or Media Foundation, and runs from the unlocked physical desktop. **Receive** needs hardware H.264 or HEVC decoding; HEVC also needs Microsoft's HEVC Video Extensions. The EXE is unsigned, so Windows SmartScreen may ask before opening it. |
| **LLCast-Android.apk** | Sender and receiver for **Android 9 or later** on 64- or 32-bit ARM (phones, tablets, TVs; remote or touch). **Send** needs a hardware HEVC, H.264 or AV1 encoder and asks to capture the screen. **Receive**: *Normal* needs no root and uses a hardware HEVC, H.264 or AV1 decoder, or software AV1. *Extreme low latency* needs root and 64-bit ARM: HEVC on an **RK3528** box (tested on the HK1 RBOX K8), or AV1 on a device with a compatible display (tested on an MT8781 tablet). It takes over the display until Back is pressed. |
| **LLCast-macOS.dmg** | Universal app for **macOS 14 or later**, Apple silicon or Intel; sends or receives. Sending needs a hardware HEVC or H.264 encoder and Screen Recording permission; receiving, a hardware HEVC or H.264 decoder. Allow local network access when asked. Signed with Developer ID, notarized and stapled. |

Use wired Ethernet for the sender where possible. A receiver waits between
senders; normal sessions have no duration limit. Video only: audio and input
forwarding are not included.

Release APK updates use the same signing key. A development-signed APK or
ad-hoc Mac zip from an Actions artifact is a test build, not a release download.

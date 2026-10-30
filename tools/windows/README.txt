LLCast test kit for the Windows PC
===================================

Everything runs from this folder. Each script writes a new folder under logs\
containing run.log and VERSION.txt (the build that ran). Per-frame timings go
to the profile collector on the Linux VM when the script names one.
Leave the logs where they are: they are read from the Linux VM through this
shared folder.

Never run tests through Remote Desktop: capture and timing need the normal
logged-in desktop.

LLCast.exe
    First choose Send or Receive, as in the Mac app.
    Send: type the receiver's address and press Start. The app checks the
    PC and the receiver first and explains anything that stops it.
    Receive: the full-screen waiting view shows one address (the default
    route's address when available) and port.
    Send to that address from your other device. Hardware H.264 and HEVC
    are supported; missing HEVC support opens an installation guide, with
    a Microsoft Store link or the option to continue with H.264.
    Esc goes back. Normal waits for refresh; Lowest latency may tear.
    If Windows asks, allow LLCast on your private network. No idle timeout.
    Logs: logs beside the EXE, with %LOCALAPPDATA%\LLCast\logs as fallback.

clock.bat
    Shows a millisecond clock at the top-left of the main screen (Esc quits).
    While the stream runs, photograph the monitor and the projector in one
    shot. The difference between the two readings is the latency of the
    streamed path relative to the monitor. Use the strips if the digits
    are blurred: the upper block moves 10 ms per cell, the lower block 100 ms
    per cell.

replay.bat BOX_IP
    Replays the 1080p60 test clip (10 times) to the box receiver.

probe.bat BOX_IP MBPS
    Sends synthetic 60 fps frames at MBPS for 30 s, to measure the Wi-Fi path.

info.bat
    Lists GPUs, displays, Desktop Duplication and NVENC support. Run first.

capture.bat
    No network. Shows the clock for moving content, then captures 10 s of the
    whole main screen fitted to 1920x1080 and encodes it to capture.hevc.
    Leave the PC alone while it runs (about 15 s).

capture-console.bat
    Only if you reach the PC through Remote Desktop: run as administrator.
    It moves your session onto the PC's monitor (Windows tscon), runs
    capture.bat there, then locks the PC. RDP disconnects; reconnect after
    about 30 s.

pattern.bat
    No capture, no network. Encodes a known test pattern so the colour
    conversion can be checked pixel by pixel.

stream.bat BOX_IP
    Streams the whole main screen fitted to 1920x1080 until Ctrl-C, with no time limit.

timestamp.html
    Open locally in a browser on the PC. Press F or F11 for full screen.
    The whole desktop is streamed by default, keeping its aspect ratio.
    Photograph the PC and projector together; subtract their elapsed counters
    for the streamed path's extra display delay relative to the PC monitor.
    Updates follow browser frames; millisecond digits do not imply 1000 fps.

vmtest.bat [VM_IP]
    No box needed. Streams a moving test pattern to the receiver on the
    Linux VM (default 192.168.185.100) in three 20 s runs: clean, with 1%
    test loss, and with 1% test loss plus 20% FEC. Works over Remote
    Desktop. Start it only when the VM receiver is running.

The box receiver is started from the Linux side; each test's instructions
say which mode it needs.

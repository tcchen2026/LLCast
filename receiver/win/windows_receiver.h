#pragma once
#include "precheck.h"
extern "C" {
#include "lls_rx_session.h"
}
#include <memory>

namespace lls {
struct ReceiveStatus {
    bool ready = false, failed = false, hevc_installed = false, finished = false;
    bool codecs_checked = false, hevc_gpu = false;
    HRESULT hevc_error = S_OK;
    uint32_t codecs = 0;
    uint16_t port = 0;
    uint8_t codec = 0;
    uint64_t presented = 0, last_present_ns = 0;
    LlsRxStatus network{};
    Problem problem;
};

// Owns RX, decode and one replaceable decoded-picture mailbox. Window lifetime
// must encompass stop(); no worker calls SendMessage or touches UI controls.
class WindowsReceiver {
    struct State;
    std::unique_ptr<State> state_;
public:
    WindowsReceiver();
    ~WindowsReceiver();
    WindowsReceiver(const WindowsReceiver &) = delete;
    WindowsReceiver &operator=(const WindowsReceiver &) = delete;
    void start(HWND video, bool tearing, const LlsProfileConfig &profile);
    ReceiveStatus status();
    void request_stop(); // UI keeps pumping DXGI window messages until finished
    void stop();
};
} // namespace lls

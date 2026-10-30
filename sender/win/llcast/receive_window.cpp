#include "receive_window.h"
#include "windows_receiver.h"
#include "resource.h"
#include "options.h"
#include <commctrl.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <string>
extern "C" {
#include "lls_net.h"
}

namespace llcast {
namespace {
constexpr wchar_t HEVC_STORE[] = L"https://apps.microsoft.com/detail/9nmzlz57r3t7";

void hevc_guidance(HWND parent, const lls::ReceiveStatus &status)
{
    const bool h264 = (status.codecs & LLS_CODEC_BIT(LLS_CODEC_H264)) != 0;
    const bool store = !status.hevc_installed || status.hevc_gpu;
    const TASKDIALOG_BUTTON buttons[] = {
        {100, L"Open HEVC Video Extensions\nCheck installation and updates in Microsoft Store"},
        {101, h264 ? L"Continue with H.264" : L"Back to receiver status"},
    };
    const wchar_t *headline;
    std::wstring text;
    if (!status.hevc_installed) {
        headline = L"Add HEVC support to receive HEVC video";
        text = L"Windows HEVC decoding is missing or could not be loaded.\n\n"
            L"1. Open HEVC Video Extensions and install it.\n"
            L"2. Close and reopen LLCast, then choose Receive.\n";
        if (!status.hevc_gpu) text += L"\nThis PC also needs a GPU/driver with HEVC hardware decoding support.";
    } else if (!status.hevc_gpu) {
        headline = L"HEVC hardware decoding is unavailable";
        text = L"HEVC is installed, but this GPU or its driver does not offer HEVC hardware decoding.\n\n"
            L"Check for a graphics driver update. Installing the HEVC extension again will not add GPU support.";
    } else {
        headline = L"HEVC could not start";
        text = L"Windows has an HEVC decoder and the GPU supports HEVC, but LLCast could not start decoding.\n\n"
            L"Check for updates to HEVC Video Extensions and the graphics driver. "
            L"If both are up to date, the decoder may have a compatibility problem with LLCast.";
    }
    if (h264) text += L"\nYou can receive now by choosing Auto or H.264 on the sender.";
    wchar_t detail[80];
    std::swprintf(detail, 80, L"HEVC decoder check: HRESULT 0x%08lx", (unsigned long)status.hevc_error);
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config); config.hwndParent = parent;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = L"LLCast";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = headline;
    config.pszContent = text.c_str();
    config.pszExpandedInformation = FAILED(status.hevc_error) ? detail : nullptr;
    config.cButtons = store ? 2 : 1; config.pButtons = buttons + (store ? 0 : 1); config.nDefaultButton = 101;
    int selected = 0;
    if (FAILED(TaskDialogIndirect(&config, &selected, nullptr, nullptr))) {
        if (store) text += L"\n\nOpen the Microsoft Store page now?";
        if (MessageBoxW(parent, text.c_str(), L"LLCast — HEVC support", MB_ICONINFORMATION |
                        (store ? MB_YESNO | MB_DEFBUTTON2 : MB_OK)) == IDYES && store) selected = 100;
    }
    if (selected == 100) ShellExecuteW(parent, L"open", HEVC_STORE, nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK video_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    // Flip-model HWNDs must not also paint through GDI (including STATIC's fill).
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) { PAINTSTRUCT paint{}; BeginPaint(window, &paint); EndPaint(window, &paint); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

std::wstring local_address()
{
    ULONG size = 16 * 1024;
    std::vector<unsigned char> memory(size);
    ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_DNS_SERVER, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(memory.data()), &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        memory.resize(size);
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(memory.data()), &size);
    }
    PMIB_IPFORWARD_TABLE2 routes = nullptr;
    GetIpForwardTable2(AF_INET, &routes); // read routing state; sends no packets
    std::wstring address;
    ReceiveAddressRank best;
    if (result == NO_ERROR) for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(memory.data()); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        ReceiveAddressRank rank;
        rank.index = adapter->IfIndex;
        rank.lan = adapter->IfType == IF_TYPE_ETHERNET_CSMACD || adapter->IfType == IF_TYPE_IEEE80211;
        if (routes) for (ULONG i = 0; i < routes->NumEntries; ++i) {
            const auto &route = routes->Table[i];
            if (route.InterfaceIndex != adapter->IfIndex || route.DestinationPrefix.PrefixLength ||
                route.Loopback || !route.ValidLifetime) continue;
            rank.default_route = true;
            rank.metric = std::min(rank.metric, uint64_t(route.Metric) + adapter->Ipv4Metric);
        }
        for (auto *entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
            if (!entry->Address.lpSockaddr || entry->Address.lpSockaddr->sa_family != AF_INET ||
                entry->DadState != IpDadStatePreferred) continue;
            const auto *ip = reinterpret_cast<const sockaddr_in *>(entry->Address.lpSockaddr);
            const auto *bytes = reinterpret_cast<const unsigned char *>(&ip->sin_addr);
            if (!bytes[0] || bytes[0] == 127 || (bytes[0] == 169 && bytes[1] == 254)) continue;
            wchar_t text[32]; std::swprintf(text, 32, L"%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
            if (address.empty() || rank < best) { address = text; best = rank; }
        }
    }
    if (routes) FreeMibTable(routes);
    return address.empty() ? L"No LAN address — connect to Wi-Fi or Ethernet" : address;
}

struct View {
    lls::WindowsReceiver receiver;
    LlsProfileConfig profile{};
    bool tearing = false, stopping = false;
    bool hevc_notice_shown = false, hevc_notice_open = false;
    HFONT font = nullptr;
    std::wstring text, addresses;
    unsigned ticks = 0;
    void layout(HWND dialog) {
        RECT r{}; GetClientRect(dialog, &r);
        MoveWindow(GetDlgItem(dialog, IDC_RX_VIDEO), 0, 0, r.right, r.bottom, TRUE);
        MoveWindow(GetDlgItem(dialog, IDC_RX_BACKGROUND), 0, 0, r.right, r.bottom, TRUE);
        const int margin = MulDiv(36, int(GetDpiForWindow(dialog)), 96);
        const int top = std::max(margin, int(r.bottom / 2 - 180));
        MoveWindow(GetDlgItem(dialog, IDC_RX_TEXT), margin, top, std::max(1, int(r.right - margin * 2)),
                   std::max(1, int(r.bottom - top - margin * 3)), TRUE);
        MoveWindow(GetDlgItem(dialog, IDC_RX_HEVC), r.right / 2 - 140, r.bottom - margin * 2, 280, 32, TRUE);
        MoveWindow(GetDlgItem(dialog, IDCANCEL), r.right - margin - 100, margin, 100, 32, TRUE);
    }
    void update(HWND dialog) {
        if (hevc_notice_open) return; // the task dialog pumps this window's timer too
        const auto status = receiver.status();
        if (stopping && status.finished) { receiver.stop(); EndDialog(dialog, 0); return; }
        if (++ticks % 20 == 1) addresses = local_address();
        bool waiting = stopping || status.failed || !status.ready || !status.last_present_ns || status.network.unsupported_codec ||
            lls_net_now_ns() - status.last_present_ns > 1000000000ULL;
        std::wstring message;
        const bool hevc_unavailable = status.codecs_checked && !(status.codecs & LLS_CODEC_BIT(LLS_CODEC_HEVC));
        if (stopping) message = L"Stopping…";
        else if (status.failed) message = status.problem.headline + L"\n\n" + status.problem.advice + L"\n\nPress Esc to go back.";
        else if (!status.ready) message = L"Checking hardware video decoding…\n\nPress Esc to go back.";
        else {
            message = L"Waiting for a sender\n\n" + addresses + L"\nPort " + std::to_wstring(status.port) +
                L"\n\nOn your other device, choose Send and enter this address.\nPress Esc to go back.";
            if (status.network.unsupported_codec) {
                const char *codec = lls_codec_name(status.network.unsupported_codec);
                std::wstring name; while (*codec) name += wchar_t(*codec++);
                message = L"This sender uses " + name + L", which this PC cannot receive.\nChoose Auto or H.264 on the sender.\n\n" + message;
            }
            if (!(status.codecs & LLS_CODEC_BIT(LLS_CODEC_HEVC))) {
                message += !status.hevc_installed ? L"\n\nHEVC support is not installed or available in Windows.\nH.264 is ready. Add HEVC Video Extensions to receive HEVC."
                    : !status.hevc_gpu ? L"\n\nHEVC hardware decoding is unavailable on this GPU. H.264 is ready."
                    : L"\n\nThe HEVC decoder could not start. Update the graphics driver and HEVC Video Extensions. H.264 is ready.";
            }
            if (!(status.codecs & LLS_CODEC_BIT(LLS_CODEC_H264)))
                message += L"\n\nH.264 hardware decoding is unavailable. Choose HEVC on the sender.";
        }
        if (text != message) { text = message; SetDlgItemTextW(dialog, IDC_RX_TEXT, text.c_str()); }
        for (int id : {IDC_RX_BACKGROUND, IDC_RX_TEXT, IDCANCEL}) ShowWindow(GetDlgItem(dialog, id), waiting ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(dialog, IDC_RX_HEVC), waiting && hevc_unavailable && !stopping ? SW_SHOW : SW_HIDE);
        EnableWindow(GetDlgItem(dialog, IDCANCEL), !stopping);
        if (hevc_unavailable && !stopping && !hevc_notice_shown) {
            hevc_notice_shown = hevc_notice_open = true;
            std::fprintf(stderr, "Receiver showing HEVC guidance: component=%u GPU=%u HRESULT 0x%08lx\n",
                unsigned(status.hevc_installed), unsigned(status.hevc_gpu), (unsigned long)status.hevc_error);
            hevc_guidance(dialog, status);
            hevc_notice_open = false;
        }
    }
};

INT_PTR CALLBACK receive_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto *view = reinterpret_cast<View *>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG: {
        view = reinterpret_cast<View *>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        MONITORINFO monitor{sizeof(monitor), {}, {}, 0};
        GetMonitorInfoW(MonitorFromWindow(GetParent(dialog), MONITOR_DEFAULTTOPRIMARY), &monitor);
        SetWindowPos(dialog, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
            monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_SHOWWINDOW);
        view->font = CreateFontW(-MulDiv(18, int(GetDpiForWindow(dialog)), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SendDlgItemMessageW(dialog, IDC_RX_TEXT, WM_SETFONT, reinterpret_cast<WPARAM>(view->font), TRUE);
        view->layout(dialog);
        view->receiver.start(GetDlgItem(dialog, IDC_RX_VIDEO), view->tearing, view->profile);
        view->update(dialog);
        SetTimer(dialog, 1, 250, nullptr);
        SetFocus(dialog);
        return FALSE;
    }
    case WM_SIZE:
        if (view) view->layout(dialog);
        return TRUE;
    case WM_ERASEBKGND: {
        RECT r{}; GetClientRect(dialog, &r); FillRect(reinterpret_cast<HDC>(wparam), &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))); return TRUE;
    }
    case WM_DRAWITEM:
        if (wparam == IDC_RX_BACKGROUND) {
            const auto *draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
            FillRect(draw->hDC, &draw->rcItem, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            return TRUE;
        }
        return FALSE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORDLG:
        SetTextColor(reinterpret_cast<HDC>(wparam), RGB(240, 240, 240));
        SetBkColor(reinterpret_cast<HDC>(wparam), RGB(0, 0, 0));
        return reinterpret_cast<INT_PTR>(GetStockObject(BLACK_BRUSH));
    case WM_TIMER:
        if (view) view->update(dialog);
        return TRUE;
    case WM_CLOSE:
    case WM_COMMAND:
        if (!view) return FALSE;
        if (message == WM_COMMAND && LOWORD(wparam) == IDC_RX_HEVC) {
            const auto status = view->receiver.status();
            view->hevc_notice_open = true;
            hevc_guidance(dialog, status);
            view->hevc_notice_open = false;
            return TRUE;
        }
        if (message == WM_CLOSE || LOWORD(wparam) == IDCANCEL) {
            view->stopping = true; view->receiver.request_stop(); view->update(dialog);
            return TRUE;
        }
        return FALSE;
    case WM_DESTROY:
        KillTimer(dialog, 1);
        if (view && view->font) { DeleteObject(view->font); view->font = nullptr; }
        return TRUE;
    }
    return FALSE;
}
}

void receive_window(HWND parent, bool tearing, const LlsProfileConfig &profile)
{
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = video_proc;
    cls.lpszClassName = L"LLCastVideo"; cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&cls); // the class is reused after returning to the chooser
    View view;
    view.tearing = tearing; view.profile = profile;
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_RECEIVE), parent, receive_proc, reinterpret_cast<LPARAM>(&view));
}
} // namespace llcast

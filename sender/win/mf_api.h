#pragma once
#include "win_util.h"
#include <mfapi.h>
#include <mfidl.h>

namespace lls {
// Lazy, process-lifetime imports: Windows N must still start without MFPlat.
// Encoder shutdown callbacks can outlive an individual encoder/receiver.
struct MfApi {
    using Enum2 = HRESULT (WINAPI *)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *,
        const MFT_REGISTER_TYPE_INFO *, IMFAttributes *, IMFActivate ***, UINT32 *);
    HMODULE library = nullptr;
    bool available = false;
    Enum2 enum2 = nullptr;
#define LLCAST_MF_FUNCTIONS(X) \
    X(MFStartup) X(MFShutdown) X(MFTEnumEx) X(MFCreateAttributes) \
    X(MFCreateDXGIDeviceManager) X(MFCreateMediaType) X(MFCreateSample) \
    X(MFCreateDXGISurfaceBuffer) X(MFCreateAlignedMemoryBuffer)
#define DECLARE_MF(name) decltype(&::name) name = nullptr;
    LLCAST_MF_FUNCTIONS(DECLARE_MF)
#undef DECLARE_MF
    MfApi() {
        library = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) return;
        available = true;
#define LOAD_MF(name) name = dll_function<decltype(name)>(library, #name); available &= name != nullptr;
        LLCAST_MF_FUNCTIONS(LOAD_MF)
#undef LOAD_MF
#undef LLCAST_MF_FUNCTIONS
        FARPROC address = GetProcAddress(library, "MFTEnum2");
        static_assert(sizeof(address) == sizeof(enum2), "Windows function pointer size");
        std::memcpy(&enum2, &address, sizeof(enum2));
    }
};
inline const MfApi &mf_api() { static const MfApi api; return api; }
} // namespace lls

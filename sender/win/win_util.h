#pragma once
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace lls {
inline bool hr_ok(HRESULT result, const char *operation)
{
    if (SUCCEEDED(result)) return true;
    std::fprintf(stderr, "%s failed: HRESULT 0x%08lx\n", operation,
                 static_cast<unsigned long>(result));
    return false;
}

template<typename Function> Function dll_function(HMODULE library, const char *name)
{
    const FARPROC address = GetProcAddress(library, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address), "Windows function pointer size");
    std::memcpy(&function, &address, sizeof(function));
    if (!function) std::fprintf(stderr, "Missing DLL entry point: %s\n", name);
    return function;
}
} // namespace lls

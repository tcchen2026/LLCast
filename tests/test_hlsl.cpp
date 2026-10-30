#include "nv12_shaders.h"
#include "scale_shaders.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

int main()
{
    const char *entries[] = {"vertex", "luma", "chroma", "luma", "chroma", "scale", "scale"};
    for (unsigned i = 0; i < 7; ++i) {
        const D3D_SHADER_MACRO macros[] = {{"OUTPUT_WIDTH", "1920"},
            {"OUTPUT_HEIGHT", "1080"}, {"INPUT_LINEAR", i >= 3 ? "1" : "0"},
            {"REENCODE_SRGB", i == 6 ? "1" : "0"}, {nullptr, nullptr}};
        const char *source = i < 5 ? lls::nv12_hlsl : lls::scale_hlsl;
        Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
        const HRESULT hr = D3DCompile(source, std::strlen(source),
            i < 5 ? "nv12_shaders.h" : "scale_shaders.h", macros, nullptr, entries[i], i == 0 ? "vs_5_0" : "ps_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS |
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors) std::fprintf(stderr, "%s\n", static_cast<const char *>(errors->GetBufferPointer()));
        if (FAILED(hr) || !code || !code->GetBufferSize()) return 1;
        if (i < 5) std::printf("%s linear=%u: %zu bytes\n", entries[i], unsigned(i >= 3), code->GetBufferSize());
        else std::printf("scale reencode_srgb=%u: %zu bytes\n", unsigned(i == 6), code->GetBufferSize());
    }
}

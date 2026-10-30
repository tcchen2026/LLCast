#pragma once
#include <d3d11.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

namespace lls {
struct DecodedPicture {
    // Retain the sample as well as its texture: the decoder pool owns the surface.
    Microsoft::WRL::ComPtr<IMFSample> sample;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    UINT subresource = 0;
    RECT source{};
    int64_t token = 0; // opaque sample identity, not a playback clock
};

class MfDecoder {
    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    RECT source_{};
    bool started_ = false;
    HRESULT output_type();
    HRESULT output(std::vector<DecodedPicture> &pictures);
public:
    ~MfDecoder() { close(); }
    // Enumerates Windows' synchronous DXVA controllers, not just hardware MFTs:
    // Microsoft's H.264/HEVC transforms are synchronous even with GPU decoding.
    static bool installed(uint8_t codec);
    static bool gpu_supports(ID3D11Device *device, uint8_t codec);
    HRESULT open(IMFDXGIDeviceManager *manager, uint8_t codec);
    HRESULT push(const uint8_t *data, size_t size, bool key, int64_t token,
                 std::vector<DecodedPicture> &pictures);
    void close();
};
} // namespace lls

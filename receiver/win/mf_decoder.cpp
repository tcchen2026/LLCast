#include "mf_decoder.h"
#include "mf_api.h"
#include <mferror.h>
#include <strmif.h>
#include <codecapi.h>
#include <algorithm>
#include <cstring>
#include <limits>
extern "C" {
#include "llstream_proto.h"
}

using Microsoft::WRL::ComPtr;
namespace lls {
namespace {
const GUID &subtype(uint8_t codec) { return codec == LLS_CODEC_H264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC; }
struct Candidates {
    IMFActivate **items = nullptr;
    UINT32 count = 0;
    explicit Candidates(uint8_t codec) {
        const auto &api = mf_api();
        MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, subtype(codec)};
        if (api.available) api.MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &input, nullptr, &items, &count);
    }
    ~Candidates() {
        for (UINT32 i = 0; i < count; ++i) items[i]->Release();
        CoTaskMemFree(items);
    }
};
void codec_setting(ICodecAPI *api, const GUID &key, bool boolean) {
    VARIANT value{};
    value.vt = boolean ? VT_BOOL : VT_UI4;
    if (boolean) value.boolVal = VARIANT_TRUE; else value.ulVal = 1;
    api->SetValue(&key, &value); // optional optimization, never a software fallback
}
}

bool MfDecoder::installed(uint8_t codec)
{
    Candidates candidates(codec);
    for (UINT32 i = 0; i < candidates.count; ++i) {
        ComPtr<IMFTransform> transform;
        const HRESULT hr = candidates.items[i]->ActivateObject(IID_PPV_ARGS(&transform));
        if (SUCCEEDED(hr)) {
            candidates.items[i]->ShutdownObject();
            return true;
        }
        std::fprintf(stderr, "Receiver %s component activation failed: HRESULT 0x%08lx\n",
            lls_codec_name(codec), (unsigned long)hr);
    }
    return false;
}

bool MfDecoder::gpu_supports(ID3D11Device *device, uint8_t codec)
{
    ComPtr<ID3D11VideoDevice> video;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&video)))) return false;
    const GUID wanted = codec == LLS_CODEC_H264 ? D3D11_DECODER_PROFILE_H264_VLD_NOFGT : D3D11_DECODER_PROFILE_HEVC_VLD_MAIN;
    for (UINT i = 0; i < video->GetVideoDecoderProfileCount(); ++i) {
        GUID profile{}; BOOL nv12 = FALSE;
        if (SUCCEEDED(video->GetVideoDecoderProfile(i, &profile)) && profile == wanted &&
            SUCCEEDED(video->CheckVideoDecoderFormat(&profile, DXGI_FORMAT_NV12, &nv12)) && nv12) return true;
    }
    return false;
}

HRESULT MfDecoder::open(IMFDXGIDeviceManager *manager, uint8_t codec)
{
    close();
    Candidates candidates(codec);
    HRESULT result = MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 i = 0; i < candidates.count; ++i) {
        ComPtr<IMFTransform> transform;
        ComPtr<IMFAttributes> attributes;
        UINT32 aware = 0, async = 0;
        result = candidates.items[i]->ActivateObject(IID_PPV_ARGS(&transform));
        if (FAILED(result)) continue;
        // Detach the activation's cached instance: the transform owns its lifetime.
        candidates.items[i]->DetachObject();
        if (FAILED(transform->GetAttributes(&attributes))) continue;
        attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async);
        if (async || FAILED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware)) || !aware) continue;
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        attributes->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
        // Windows H.264 uses UI4 for this property; HEVC uses BOOL.
        ComPtr<ICodecAPI> controls;
        if (SUCCEEDED(transform->QueryInterface(IID_ICodecAPI, reinterpret_cast<void **>(controls.GetAddressOf())))) {
            codec_setting(controls.Get(), CODECAPI_AVLowLatencyMode, codec != LLS_CODEC_H264);
            if (codec == LLS_CODEC_H264) codec_setting(controls.Get(), CODECAPI_AVDecVideoAcceleration_H264, true);
        }
        // Never detach the device manager and retry in software.
        result = transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager));
        if (FAILED(result)) continue;
        ComPtr<IMFMediaType> input;
        result = mf_api().MFCreateMediaType(&input);
        if (SUCCEEDED(result)) result = input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(result)) result = input->SetGUID(MF_MT_SUBTYPE, subtype(codec));
        if (SUCCEEDED(result) && codec == LLS_CODEC_HEVC) {
            result = input->SetUINT32(MF_MT_MPEG2_PROFILE, 1); // Main, 8 bit
            // The HEVC MFT accepts a sizeless input but then rejects its own
            // NV12 output with MF_E_ATTRIBUTENOTFOUND. Supply its bootstrap
            // size; the first SPS supplies the real size via STREAM_CHANGE.
            if (SUCCEEDED(result)) result = input->SetUINT64(MF_MT_FRAME_SIZE, (UINT64(1280) << 32) | 720);
        }
        if (SUCCEEDED(result)) result = transform->SetInputType(0, input.Get(), 0);
        if (FAILED(result)) continue;
        transform_ = transform;
        result = output_type();
        if (SUCCEEDED(result)) result = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (SUCCEEDED(result)) result = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (SUCCEEDED(result)) {
            started_ = true;
            wchar_t name[256]{};
            candidates.items[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, nullptr);
            std::fprintf(stderr, "Receiver decoder: %ls, %s, D3D11 NV12, low latency requested\n", name, lls_codec_name(codec));
            return S_OK;
        }
        close();
    }
    return FAILED(result) ? result : MF_E_UNSUPPORTED_D3D_TYPE;
}

HRESULT MfDecoder::output_type()
{
    HRESULT rejected = MF_E_INVALIDMEDIATYPE;
    for (DWORD i = 0; i < 64; ++i) {
        ComPtr<IMFMediaType> type;
        HRESULT hr = transform_->GetOutputAvailableType(0, i, &type);
        if (FAILED(hr)) {
            std::fprintf(stderr, "Receiver output enumeration: index=%lu HRESULT 0x%08lx\n", i, (unsigned long)hr);
            // End-of-list must not hide why the advertised NV12 type failed.
            return hr == MF_E_NO_MORE_TYPES && i ? rejected : hr;
        }
        GUID format{};
        if (FAILED(hr = type->GetGUID(MF_MT_SUBTYPE, &format))) { rejected = hr; continue; }
        UINT64 size = 0;
        type->GetUINT64(MF_MT_FRAME_SIZE, &size);
        std::fprintf(stderr, "Receiver output candidate %lu: subtype=0x%08lx %ux%u%s\n",
            i, (unsigned long)format.Data1, UINT32(size >> 32), UINT32(size),
            format == MFVideoFormat_NV12 ? " NV12" : " (not NV12)");
        if (format != MFVideoFormat_NV12) continue;
        if (FAILED(hr = transform_->SetOutputType(0, type.Get(), 0))) {
            rejected = hr;
            std::fprintf(stderr, "Receiver SetOutputType(NV12) failed: HRESULT 0x%08lx\n", (unsigned long)hr);
            continue;
        }
        // Minimal input types may expose a placeholder before the first SPS.
        // Keep it negotiable; dimensions must be known once a picture is output.
        type->GetUINT64(MF_MT_FRAME_SIZE, &size);
        const auto width = UINT32(size >> 32), height = UINT32(size);
        if (width > 16384 || height > 16384) return MF_E_INVALIDMEDIATYPE;
        source_ = {0, 0, LONG(width), LONG(height)};
        MFVideoArea area{}; UINT32 bytes = 0;
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8 *>(&area), sizeof(area), &bytes)) &&
            bytes == sizeof(area) && area.OffsetX.value >= 0 && area.OffsetY.value >= 0 &&
            area.Area.cx > 0 && area.Area.cy > 0 && int64_t(area.OffsetX.value) + area.Area.cx <= width &&
            int64_t(area.OffsetY.value) + area.Area.cy <= height)
            source_ = {area.OffsetX.value, area.OffsetY.value, area.OffsetX.value + area.Area.cx, area.OffsetY.value + area.Area.cy};
        MFT_OUTPUT_STREAM_INFO info{};
        hr = transform_->GetOutputStreamInfo(0, &info);
        if (FAILED(hr)) return hr;
        // DXVA output surfaces must be allocated by the decoder.
        return info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)
            ? S_OK : MF_E_UNSUPPORTED_D3D_TYPE;
    }
    return rejected;
}

HRESULT MfDecoder::output(std::vector<DecodedPicture> &pictures)
{
    for (unsigned tries = 0; tries < 64; ++tries) {
        MFT_OUTPUT_DATA_BUFFER out{}; DWORD status = 0;
        HRESULT hr = transform_->ProcessOutput(0, 1, &out, &status);
        ComPtr<IMFSample> sample; sample.Attach(out.pSample);
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (FAILED(hr = output_type())) return hr;
            continue;
        }
        if (FAILED(hr)) return hr;
        if (!sample) return E_UNEXPECTED;
        if (source_.right <= source_.left || source_.bottom <= source_.top) return MF_E_INVALIDMEDIATYPE;
        DecodedPicture picture;
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFDXGIBuffer> dxgi;
        if (FAILED(hr = sample->GetBufferByIndex(0, &buffer)) ||
            FAILED(hr = buffer.As(&dxgi)) ||
            FAILED(hr = dxgi->GetResource(IID_PPV_ARGS(&picture.texture))) ||
            FAILED(hr = dxgi->GetSubresourceIndex(&picture.subresource)) ||
            FAILED(hr = sample->GetSampleTime(&picture.token))) return hr;
        D3D11_TEXTURE2D_DESC desc{}; picture.texture->GetDesc(&desc);
        // A software frame or a copied RGB texture is not hardware decode.
        if (desc.Format != DXGI_FORMAT_NV12 || !(desc.BindFlags & D3D11_BIND_DECODER) ||
            !desc.MipLevels || picture.subresource >= desc.ArraySize * desc.MipLevels ||
            source_.right > LONG(desc.Width) || source_.bottom > LONG(desc.Height)) return MF_E_UNSUPPORTED_D3D_TYPE;
        picture.source = source_;
        picture.sample = std::move(sample);
        pictures.push_back(std::move(picture));
        if (pictures.size() > 16) return E_UNEXPECTED;
    }
    return E_UNEXPECTED;
}

HRESULT MfDecoder::push(const uint8_t *data, size_t size, bool key, int64_t token,
                       std::vector<DecodedPicture> &pictures)
{
    if (!transform_ || !data || !size || size > std::numeric_limits<DWORD>::max()) return E_INVALIDARG;
    const auto &api = mf_api();
    ComPtr<IMFMediaBuffer> buffer; ComPtr<IMFSample> sample;
    HRESULT hr = api.MFCreateAlignedMemoryBuffer(DWORD(size), 15, &buffer);
    BYTE *bytes = nullptr;
    if (SUCCEEDED(hr)) hr = buffer->Lock(&bytes, nullptr, nullptr);
    if (FAILED(hr)) return hr;
    std::memcpy(bytes, data, size);
    buffer->Unlock();
    if (FAILED(hr = buffer->SetCurrentLength(DWORD(size))) || FAILED(hr = api.MFCreateSample(&sample)) ||
        FAILED(hr = sample->AddBuffer(buffer.Get())) || FAILED(hr = sample->SetSampleTime(token)) ||
        FAILED(hr = sample->SetSampleDuration(1)) || FAILED(hr = sample->SetUINT32(MFSampleExtension_CleanPoint, key))) return hr;
    hr = transform_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (FAILED(hr = output(pictures))) return hr;
        hr = transform_->ProcessInput(0, sample.Get(), 0);
    }
    return FAILED(hr) ? hr : output(pictures);
}

void MfDecoder::close()
{
    if (transform_ && started_) {
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    transform_.Reset();
    started_ = false;
    source_ = {};
}
} // namespace lls

#include "mf_encoder.h"
#include "win_util.h"
#include "mf_api.h"
#include <d3d10.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>
extern "C" {
#include "hevc_au.h"
#include "av1_obu.h"
}

using Microsoft::WRL::ComPtr;
namespace lls {
namespace {
// MFTEnum2 is optional (Windows 10 1809+). Keep 1803 and mingw-w64 11 usable.
// GUID from Microsoft's mfapi.h; SetBlob, not UINT64, is the documented type.
constexpr GUID adapter_luid = {0x1d39518c,0xe220,0x4da8,{0xa0,0x7f,0xba,0x17,0x25,0x52,0xd6,0xb1}};
// MFVideoFormat_AV1 is absent in older mingw/Windows SDK headers: AV01 FourCC.
constexpr GUID av1_subtype = {0x31305641,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
const GUID &subtype(uint8_t codec) {
    return codec == LLS_CODEC_AV1 ? av1_subtype : codec == LLS_CODEC_H264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC;
}

// One outstanding request. The callback owns only this object, never State;
// shutdown may deliver a late callback after the pipeline has released State.
class Events final : public IMFAsyncCallback {
    std::atomic<ULONG> refs_{1};
    ComPtr<IMFMediaEventGenerator> source_;
    ComPtr<IMFMediaEvent> event_;
    HANDLE ready_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT result_ = S_OK;
public:
    explicit Events(IMFMediaEventGenerator *source) : source_(source) {}
    ~Events() { if (ready_) CloseHandle(ready_); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IMFAsyncCallback)) return E_NOINTERFACE;
        *out = static_cast<IMFAsyncCallback *>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD *, DWORD *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult *result) override {
        result_ = source_->EndGetEvent(result, &event_);
        SetEvent(ready_); // publishes result_ and event_ to the waiting thread
        return S_OK;
    }
    bool next(MediaEventType &type, const volatile long &stop) {
        if (!ready_ || stop || !hr_ok(source_->BeginGetEvent(this, nullptr), "MF BeginGetEvent")) return false;
        for (;;) {
            DWORD result = WaitForSingleObject(ready_, 100);
            if (result == WAIT_OBJECT_0) break;
            if (result != WAIT_TIMEOUT || stop) return false;
        }
        if (!hr_ok(result_, "MF EndGetEvent") || !event_) return false;
        HRESULT status = S_OK;
        bool ok = hr_ok(event_->GetStatus(&status), "MF event status") && hr_ok(status, "MF encoder event") &&
            hr_ok(event_->GetType(&type), "MF event type");
        event_.Reset();
        return ok;
    }
};

bool setting(ICodecAPI *api, const GUID &key, uint32_t value, const char *name,
             bool boolean = false, bool optional = false)
{
    VARIANT v{};
    v.vt = boolean ? VT_BOOL : VT_UI4;
    if (boolean) v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    else v.ulVal = value;
    const HRESULT hr = api->SetValue(&key, &v);
    if (FAILED(hr) && optional) {
        std::fprintf(stderr, "%s unavailable (HRESULT 0x%08lx); continuing\n", name, (unsigned long)hr);
        return false;
    }
    return hr_ok(hr, name);
}

bool media_type(IMFMediaType *t, const EncoderConfig &c, bool input)
{
    HRESULT hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_SUBTYPE, input ? MFVideoFormat_NV12 : subtype(c.codec));
    if (SUCCEEDED(hr)) hr = t->SetUINT64(MF_MT_FRAME_SIZE, (uint64_t(c.width) << 32) | c.height);
    if (SUCCEEDED(hr)) hr = t->SetUINT64(MF_MT_FRAME_RATE, (uint64_t(c.fps) << 32) | 1);
    if (SUCCEEDED(hr)) hr = t->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, (uint64_t(1) << 32) | 1);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_VIDEO_CHROMA_SITING, MFVideoChromaSubsampling_MPEG2);
    if (!input && SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AVG_BITRATE, c.bitrate_bps);
    if (!input && SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_MPEG2_PROFILE,
        c.codec == LLS_CODEC_AV1 ? 0 : c.codec == LLS_CODEC_H264 ? 77 : 1);
    return hr_ok(hr, "MF media type");
}

bool enumerate(const MfApi &api, ID3D11Device *device, uint8_t codec, IMFActivate ***items, UINT32 *count)
{
    MFT_REGISTER_TYPE_INFO input = {MFMediaType_Video, MFVideoFormat_NV12}, output = {MFMediaType_Video, subtype(codec)};
    constexpr UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
    if (!api.enum2) return hr_ok(api.MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &input, &output, items, count), "MFTEnumEx hardware encoders");
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC desc{};
    ComPtr<IMFAttributes> attributes;
    if (!hr_ok(device->QueryInterface(IID_PPV_ARGS(&dxgi)), "MF DXGI device") ||
        !hr_ok(dxgi->GetAdapter(&adapter), "MF adapter") || !hr_ok(adapter->GetDesc(&desc), "MF adapter LUID") ||
        !hr_ok(api.MFCreateAttributes(&attributes, 1), "MF enumeration attributes") ||
        !hr_ok(attributes->SetBlob(adapter_luid, reinterpret_cast<const BYTE *>(&desc.AdapterLuid), sizeof(LUID)), "MF adapter filter")) return false;
    return hr_ok(api.enum2(MFT_CATEGORY_VIDEO_ENCODER, flags, &input, &output, attributes.Get(), items, count), "MFTEnum2 hardware encoders on capture GPU");
}
} // namespace

struct MfEncoder::State {
    const MfApi &api = mf_api();
    bool com = false, mf = false, started = false;
    EncoderConfig config;
    std::string label = "Media Foundation";
    ComPtr<IMFActivate> activation;
    ComPtr<IMFTransform> transform;
    ComPtr<ICodecAPI> controls;
    ComPtr<IMFDXGIDeviceManager> manager;
    ComPtr<Events> events;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> input;
    std::vector<uint8_t> bytes, headers;
    DWORD in_id = 0, out_id = 0;
    unsigned credits = 0;
    uint64_t index = 0;

    ~State() {
        if (transform && started) {
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        if (activation) activation->ShutdownObject();
        events.Reset(); controls.Reset(); transform.Reset(); activation.Reset();
        manager.Reset(); input.Reset(); context.Reset();
        if (mf) api.MFShutdown();
        if (com) CoUninitialize();
    }
    void read_headers() {
        ComPtr<IMFMediaType> type;
        UINT32 size = 0;
        if (SUCCEEDED(transform->GetOutputCurrentType(out_id, &type)) &&
            SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size && size <= 1024 * 1024) {
            headers.resize(size);
            if (FAILED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, headers.data(), size, &size))) headers.clear();
            // AV1 MFTs may provide av1C instead of raw config OBUs.
            if (config.codec == LLS_CODEC_AV1 && headers.size() >= 4 && headers[0] == 0x81)
                headers.erase(headers.begin(), headers.begin() + 4);
            if (config.codec == LLS_CODEC_AV1) {
                std::vector<uint8_t> sequence;
                size_t cursor = 0;
                while (cursor < headers.size()) {
                    LlsAv1Obu obu; LlsAv1Sequence parsed;
                    if (!lls_av1_next_obu(headers.data(), headers.size(), &cursor, &obu)) break;
                    if (obu.type == 1 && lls_av1_sequence(headers.data() + obu.payload_offset, obu.payload_size, &parsed)) {
                        sequence.assign(headers.begin() + obu.offset, headers.begin() + obu.offset + obu.size);
                        break;
                    }
                }
                headers = std::move(sequence); // no delimiter or container prefix in cached headers
            }
        }
    }
    bool setup(IMFActivate *candidate, ID3D11Device *device, ID3D11Texture2D *texture, const EncoderConfig &c) {
        config = c; activation = candidate; input = texture;
        WCHAR *name = nullptr; UINT32 length = 0;
        if (SUCCEEDED(candidate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length))) {
            int size = WideCharToMultiByte(CP_UTF8, 0, name, int(length), nullptr, 0, nullptr, nullptr);
            if (size > 0) {
                std::string utf8(size_t(size), '\0');
                WideCharToMultiByte(CP_UTF8, 0, name, int(length), utf8.data(), size, nullptr, nullptr);
                label += ": " + utf8;
            }
            CoTaskMemFree(name);
        }
        std::fprintf(stderr, "Trying %s (%s)\n", label.c_str(), lls_codec_name(c.codec));
        if (!hr_ok(activation->ActivateObject(IID_PPV_ARGS(&transform)), "MF activate hardware encoder")) return false;
        ComPtr<IMFAttributes> attrs;
        UINT32 async = 0, d3d = 0;
        if (FAILED(transform->GetAttributes(&attrs)) || FAILED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async)) ||
            FAILED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d)) || !async || !d3d) {
            std::fprintf(stderr, "MF encoder lacks asynchronous D3D11 hardware input\n"); return false;
        }
        if (!hr_ok(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "MF async unlock") ||
            !hr_ok(attrs->SetUINT32(MF_LOW_LATENCY, TRUE), "MF low latency") ||
            !hr_ok(transform->QueryInterface(IID_ICodecAPI, reinterpret_cast<void **>(controls.GetAddressOf())), "MF ICodecAPI")) return false;
        UINT token = 0;
        device->GetImmediateContext(&context);
        ComPtr<ID3D10Multithread> multithread;
        if (!hr_ok(context.As(&multithread), "MF D3D multithread protection")) return false;
        multithread->SetMultithreadProtected(TRUE);
        if (!hr_ok(api.MFCreateDXGIDeviceManager(&token, &manager), "MFCreateDXGIDeviceManager") ||
            !hr_ok(manager->ResetDevice(device, token), "MF ResetDevice") ||
            !hr_ok(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.Get())), "MF D3D manager")) return false;
        DWORD inputs = 0, outputs = 0;
        if (FAILED(transform->GetStreamCount(&inputs, &outputs)) || inputs != 1 || outputs != 1) return false;
        HRESULT ids = transform->GetStreamIDs(1, &in_id, 1, &out_id);
        if (ids != E_NOTIMPL && !hr_ok(ids, "MF stream IDs")) return false;
        if (ids == E_NOTIMPL) in_id = out_id = 0;
        if (!setting(controls.Get(), CODECAPI_AVLowLatencyMode, 1, "MF low-latency codec mode", true) ||
            !setting(controls.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR, "MF CBR") ||
            !setting(controls.Get(), CODECAPI_AVEncCommonMeanBitRate, c.bitrate_bps, "MF bitrate")) return false;
        // BPictureCount is optional for hardware MFTs, including NVIDIA's.
        // AVLowLatencyMode above remains required: its contract is one output
        // per input without reordering. Some codecs implement low-delay B slices.
        setting(controls.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0, "MF no B frames (optional)", false, true);
        // Some hardware caps GOP size; recovery still explicitly requests keys.
        setting(controls.Get(), CODECAPI_AVEncMPVGOPSize, UINT32_MAX, "MF infinite GOP (optional)", false, true);
        ComPtr<IMFMediaType> out, in;
        if (FAILED(api.MFCreateMediaType(&out)) || FAILED(api.MFCreateMediaType(&in)) ||
            !media_type(out.Get(), c, false) || !media_type(in.Get(), c, true) ||
            !hr_ok(transform->SetOutputType(out_id, out.Get(), 0), "MF output type") ||
            !hr_ok(transform->SetInputType(in_id, in.Get(), 0), "MF NV12 input type")) return false;
        ComPtr<IMFMediaEventGenerator> source;
        if (!hr_ok(transform.As(&source), "MF event generator")) return false;
        events.Attach(new Events(source.Get()));
        read_headers();
        if (!hr_ok(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "MF begin streaming")) return false;
        started = true;
        return hr_ok(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "MF start stream");
    }
};

MfEncoder::MfEncoder() = default;
MfEncoder::~MfEncoder() = default;
uint8_t MfEncoder::codec() const { return state_ ? state_->config.codec : 0; }
const char *MfEncoder::name() const { return state_ ? state_->label.c_str() : "Media Foundation"; }

bool MfEncoder::open(ID3D11Device *device, ID3D11Texture2D *input, const EncoderConfig &config)
{
    state_.reset();
    if (!device || !input || config.codec < LLS_CODEC_HEVC || config.codec > LLS_CODEC_AV1 ||
        !config.fps || !config.width || !config.height || !config.bitrate_bps) return false;
    D3D11_TEXTURE2D_DESC desc{}; input->GetDesc(&desc);
    ComPtr<ID3D11Device> owner; input->GetDevice(&owner);
    if (owner.Get() != device || desc.Width != config.width || desc.Height != config.height ||
        desc.Format != DXGI_FORMAT_NV12 || desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1) return false;
    // An outer lifetime keeps COM/MF alive while trying candidates. A selected
    // State retains its own reference, balanced on the same pipeline thread.
    const MfApi &api = mf_api();
    if (!api.available) return false;
    State runtime;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
    runtime.com = SUCCEEDED(hr);
    if (!hr_ok(api.MFStartup(MF_VERSION, MFSTARTUP_FULL), "MFStartup")) return false;
    runtime.mf = true;
    IMFActivate **items = nullptr; UINT32 count = 0;
    if (!enumerate(api, device, config.codec, &items, &count)) return false;
    for (UINT32 i = 0; i < count && !state_; ++i) {
        auto candidate = std::make_unique<State>();
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        candidate->com = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) break;
        candidate->mf = SUCCEEDED(api.MFStartup(MF_VERSION, MFSTARTUP_FULL));
        if (candidate->mf && candidate->setup(items[i], device, input, config)) state_ = std::move(candidate);
    }
    for (UINT32 i = 0; i < count; ++i) items[i]->Release();
    CoTaskMemFree(items);
    if (state_) std::fprintf(stderr, "%s: %s %ux%u %u fps CBR %u, low latency, no reordering, BT.709 limited, GPU NV12\n",
        name(), lls_codec_name(codec()), config.width, config.height, config.fps, config.bitrate_bps);
    return bool(state_);
}

bool MfEncoder::encode(bool key, EncodedFrame &frame, const volatile long &stop)
{
    frame = {};
    if (!state_ || stop) return false;
    State &s = *state_;
    while (!s.credits) {
        MediaEventType type;
        if (!s.events->next(type, stop)) return false;
        if (type == METransformNeedInput) ++s.credits;
        else if (type == METransformHaveOutput) return false; // no submitted frame
    }
    if (key && !setting(s.controls.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1, "MF force keyframe")) return false;
    ComPtr<IMFSample> sample; ComPtr<IMFMediaBuffer> surface;
    if (!hr_ok(s.api.MFCreateSample(&sample), "MF input sample") ||
        !hr_ok(s.api.MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), s.input.Get(), 0, FALSE, &surface), "MF GPU input buffer")) return false;
    DWORD length = 0;
    const LONGLONG input_time = LONGLONG(s.index * 10000000 / s.config.fps);
    if (!hr_ok(surface->GetMaxLength(&length), "MF surface length") ||
        !hr_ok(surface->SetCurrentLength(length), "MF surface current length") ||
        !hr_ok(sample->AddBuffer(surface.Get()), "MF add GPU surface") ||
        !hr_ok(sample->SetSampleTime(input_time), "MF timestamp") ||
        !hr_ok(sample->SetSampleDuration(10000000 / s.config.fps), "MF duration")) return false;
    s.context->Flush();
    LARGE_INTEGER submitted{}; QueryPerformanceCounter(&submitted); frame.submit_qpc = submitted.QuadPart;
    if (!hr_ok(s.transform->ProcessInput(s.in_id, sample.Get(), 0), "MF ProcessInput")) return false;
    --s.credits; ++s.index;
    bool have_output = false;
    unsigned format_changes = 0;
    for (;;) {
        MediaEventType type;
        if (have_output) type = METransformHaveOutput;
        else if (!s.events->next(type, stop)) return false;
        if (type == METransformNeedInput) {
            if (++s.credits > 32) return false;
            continue;
        }
        if (type != METransformHaveOutput) continue;
        have_output = true;
        MFT_OUTPUT_STREAM_INFO info{};
        if (!hr_ok(s.transform->GetOutputStreamInfo(s.out_id, &info), "MF output info")) return false;
        ComPtr<IMFSample> output;
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            ComPtr<IMFMediaBuffer> buffer;
            if (!info.cbSize || !hr_ok(s.api.MFCreateSample(&output), "MF output sample") ||
                !hr_ok(s.api.MFCreateAlignedMemoryBuffer(info.cbSize, info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer), "MF output allocation") ||
                !hr_ok(output->AddBuffer(buffer.Get()), "MF output buffer")) return false;
        }
        MFT_OUTPUT_DATA_BUFFER result{}; result.dwStreamID = s.out_id; result.pSample = output.Get();
        DWORD flags = 0;
        HRESULT hr = s.transform->ProcessOutput(0, 1, &result, &flags);
        if (result.pEvents) result.pEvents->Release();
        if (!output) output.Attach(result.pSample);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (++format_changes > 2) return false;
            // Accept only this configured codec/geometry; retain VUI/rate settings.
            ComPtr<IMFMediaType> changed;
            if (FAILED(s.api.MFCreateMediaType(&changed)) || !media_type(changed.Get(), s.config, false) ||
                !hr_ok(s.transform->SetOutputType(s.out_id, changed.Get(), 0), "MF changed output type")) return false;
            s.read_headers();
            continue;
        }
        if (!hr_ok(hr, "MF ProcessOutput") || !output || (result.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE)) return false;
        LONGLONG output_time = 0;
        if (!hr_ok(output->GetSampleTime(&output_time), "MF output timestamp")) return false;
        if (output_time != input_time) {
            std::fprintf(stderr, "MF encoder reordered output: timestamp=%lld expected=%lld\n", output_time, input_time);
            return false;
        }
        ComPtr<IMFMediaBuffer> buffer;
        if (!hr_ok(output->ConvertToContiguousBuffer(&buffer), "MF bitstream buffer")) return false;
        BYTE *data = nullptr; DWORD size = 0;
        if (!hr_ok(buffer->Lock(&data, nullptr, &size), "MF bitstream lock")) return false;
        if (!data || !size) { buffer->Unlock(); return false; }
        LARGE_INTEGER done{}; QueryPerformanceCounter(&done); frame.done_qpc = done.QuadPart;
        s.bytes.assign(data, data + size);
        buffer->Unlock();
        if (s.config.codec == LLS_CODEC_AV1 && ((s.bytes[0] >> 3) & 15) != 2)
            s.bytes.insert(s.bytes.begin(), {0x12, 0x00});
        UINT32 clean = 0; output->GetUINT32(MFSampleExtension_CleanPoint, &clean);
        LlsAccessUnit *units = nullptr; size_t count = 0;
        bool valid = lls_split_access_units(s.config.codec, s.bytes.data(), s.bytes.size(), &units, &count);
        bool is_key = valid && count == 1 && units[0].key;
        free(units);
        if (!is_key && (clean || key)) {
            s.read_headers();
            size_t header_offset = 0;
            if (s.config.codec == LLS_CODEC_AV1) {
                LlsAv1Obu delimiter;
                if (!lls_av1_next_obu(s.bytes.data(), s.bytes.size(), &header_offset, &delimiter) || delimiter.type != 2) return false;
            }
            s.bytes.insert(s.bytes.begin() + header_offset, s.headers.begin(), s.headers.end());
            units = nullptr; count = 0;
            valid = lls_split_access_units(s.config.codec, s.bytes.data(), s.bytes.size(), &units, &count);
            is_key = valid && count == 1 && units[0].key;
            free(units);
        }
        if (!valid || count != 1 || (key && !is_key)) {
            std::fprintf(stderr, "MF encoder did not return one valid %s frame%s\n", lls_codec_name(s.config.codec), key ? " with recovery headers" : "");
            return false;
        }
        frame.data = s.bytes.data(); frame.size = s.bytes.size(); frame.key = is_key;
        return true;
    }
}
} // namespace lls

#include "nvenc_encoder.h"
#include "win_util.h"
#include <ffnvcodec/nvEncodeAPI.h>
#include <wrl/client.h>

extern "C" {
#include "hevc_au.h"
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

namespace lls {
struct NvencEncoder::State {
    HMODULE library = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api{};
    void *encoder = nullptr;
    NV_ENC_REGISTERED_PTR registered = nullptr;
    NV_ENC_INPUT_PTR mapped = nullptr;
    NV_ENC_OUTPUT_PTR bitstream = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> input;
    EncoderConfig config;
    GUID codec_guid{};
    std::vector<uint8_t> bytes;
    uint32_t index = 0, last_hw_status = 0;
    bool locked = false, initialized = false, failed = false;

    bool check(NVENCSTATUS result, const char *operation) {
        if (result == NV_ENC_SUCCESS) return true;
        const char *detail = encoder && api.nvEncGetLastErrorString ?
            api.nvEncGetLastErrorString(encoder) : nullptr;
        std::fprintf(stderr, "%s failed: NVENC %d%s%s\n", operation, int(result),
                     detail ? ": " : "", detail ? detail : "");
        failed = true;
        return false;
    }
    bool cap(NV_ENC_CAPS id, int &value) {
        NV_ENC_CAPS_PARAM caps{};
        caps.version = NV_ENC_CAPS_PARAM_VER;
        caps.capsToQuery = id;
        return check(api.nvEncGetEncodeCaps(encoder, codec_guid,
                                          &caps, &value), "NvEncGetEncodeCaps");
    }
};

bool NvencEncoder::open(ID3D11Device *device, ID3D11Texture2D *input,
                        const EncoderConfig &config)
{
    close();
    if (!device || !input || config.codec < LLS_CODEC_HEVC || config.codec > LLS_CODEC_AV1 || !config.width || !config.height ||
        (config.width & 1) || (config.height & 1) || !config.fps || !config.bitrate_bps)
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    input->GetDesc(&desc);
    Microsoft::WRL::ComPtr<ID3D11Device> input_device;
    input->GetDevice(&input_device);
    if (input_device.Get() != device || desc.Format != DXGI_FORMAT_NV12 ||
        desc.Width != config.width || desc.Height != config.height ||
        desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1) {
        std::fprintf(stderr, "NVENC input must be one NV12 texture on the capture device\n");
        return false;
    }
    state_ = new (std::nothrow) State;
    if (!state_) return false;
    State &s = *state_;
    s.config = config;
    s.codec_guid = config.codec == LLS_CODEC_AV1 ? NV_ENC_CODEC_AV1_GUID :
        config.codec == LLS_CODEC_H264 ? NV_ENC_CODEC_H264_GUID : NV_ENC_CODEC_HEVC_GUID;
    s.device = device;
    s.input = input;
    s.library = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s.library) {
        std::fprintf(stderr, "Cannot load nvEncodeAPI64.dll (error %lu); install NVIDIA driver >=570\n",
                     GetLastError());
        return false;
    }
    using GetVersion = NVENCSTATUS (NVENCAPI *)(uint32_t *);
    using Create = NVENCSTATUS (NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST *);
    const auto get_version = dll_function<GetVersion>(s.library, "NvEncodeAPIGetMaxSupportedVersion");
    const auto create = dll_function<Create>(s.library, "NvEncodeAPICreateInstance");
    uint32_t supported = 0;
    if (!get_version || !create || !s.check(get_version(&supported), "NVENC API version"))
        return false;
    // GetMaxSupportedVersion uses major<<4 | minor, unlike NVENCAPI_VERSION.
    constexpr uint32_t required = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    std::fprintf(stderr, "NVENC driver API %u.%u; built for %u.%u (n13.0.19.0)\n",
                 supported >> 4, supported & 15, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
    if (supported < required) {
        std::fprintf(stderr, "Driver is too old: API 13.0 / driver >=570 is required\n");
        return false;
    }
    s.api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (!s.check(create(&s.api), "NvEncodeAPICreateInstance")) return false;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session{};
    session.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    session.device = device;
    session.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    session.apiVersion = NVENCAPI_VERSION;
    if (!s.check(s.api.nvEncOpenEncodeSessionEx(&session, &s.encoder),
                 "NvEncOpenEncodeSessionEx(D3D11)")) return false;

    uint32_t count = 0;
    if (!s.check(s.api.nvEncGetEncodeGUIDCount(s.encoder, &count), "NVENC codec count"))
        return false;
    std::vector<GUID> codecs(count);
    if (!count || !s.check(s.api.nvEncGetEncodeGUIDs(s.encoder, codecs.data(), count, &count),
                          "NVENC codec list")) return false;
    if (std::none_of(codecs.begin(), codecs.end(), [&s](const GUID &id) {
            return IsEqualGUID(id, s.codec_guid);
        })) {
        std::fprintf(stderr, "This GPU has no %s encoder\n", lls_codec_name(config.codec));
        return false;
    }
    int max_width = 0, max_height = 0, custom_vbv = 0;
    if (!s.cap(NV_ENC_CAPS_WIDTH_MAX, max_width) ||
        !s.cap(NV_ENC_CAPS_HEIGHT_MAX, max_height) ||
        !s.cap(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE, custom_vbv)) return false;
    if (max_width < int(config.width) || max_height < int(config.height) || !custom_vbv) {
        std::fprintf(stderr, "%s encoder lacks required dimensions or custom VBV support\n", lls_codec_name(config.codec));
        return false;
    }
    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    if (!s.check(s.api.nvEncGetEncodePresetConfigEx(s.encoder, s.codec_guid,
            NV_ENC_PRESET_P1_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset),
            "NVENC P1/ULL preset")) return false;
    NV_ENC_CONFIG settings = preset.presetCfg;
    settings.profileGUID = config.codec == LLS_CODEC_AV1 ? NV_ENC_AV1_PROFILE_MAIN_GUID :
        config.codec == LLS_CODEC_H264 ? NV_ENC_H264_PROFILE_MAIN_GUID : NV_ENC_HEVC_PROFILE_MAIN_GUID;
    settings.gopLength = NVENC_INFINITE_GOPLENGTH;
    settings.frameIntervalP = 1;
    settings.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    auto &rc = settings.rcParams;
    rc = {};
    rc.version = NV_ENC_RC_PARAMS_VER;
    rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    rc.averageBitRate = rc.maxBitRate = config.bitrate_bps;
    rc.vbvBufferSize = rc.vbvInitialDelay =
        uint32_t((uint64_t(config.bitrate_bps) + config.fps - 1) / config.fps);
    rc.zeroReorderDelay = 1;
    rc.lowDelayKeyFrameScale = 1;
    rc.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    if (config.codec == LLS_CODEC_AV1) {
        auto &av1 = settings.encodeCodecConfig.av1Config;
        av1 = {};
        // Unlike HEVC/H.264, AV1 level zero means Level 2.0, not automatic.
        av1.level = NV_ENC_LEVEL_AV1_AUTOSELECT;
        av1.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        av1.chromaFormatIDC = 1;
        av1.inputBitDepth = av1.outputBitDepth = NV_ENC_BIT_DEPTH_8;
        av1.repeatSeqHdr = 1;
        av1.outputAnnexBFormat = 0; // sized OBUs, not AV1 Annex-B length wrappers
        av1.numFwdRefs = NV_ENC_NUM_REF_FRAMES_1;
        av1.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        av1.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        av1.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        av1.colorRange = 0;
        av1.chromaSamplePosition = 1;
    } else if (config.codec == LLS_CODEC_H264) {
        auto &h264 = settings.encodeCodecConfig.h264Config;
        h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        h264.chromaFormatIDC = 1;
        h264.inputBitDepth = h264.outputBitDepth = NV_ENC_BIT_DEPTH_8;
        h264.disableSPSPPS = 0;
        h264.repeatSPSPPS = 1;
        h264.outputAUD = 1;
        h264.enableIntraRefresh = h264.enableLTR = h264.enableFillerDataInsertion = 0;
        h264.enableTemporalSVC = h264.enableStereoMVC = 0;
        h264.hierarchicalPFrames = h264.hierarchicalBFrames = 0;
        h264.maxNumRefFrames = 1;
        h264.numRefL0 = NV_ENC_NUM_REF_FRAMES_1;
        h264.numRefL1 = NV_ENC_NUM_REF_FRAMES_AUTOSELECT;
        h264.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
    } else {
        auto &hevc = settings.encodeCodecConfig.hevcConfig;
        hevc.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        hevc.chromaFormatIDC = 1;
        hevc.inputBitDepth = hevc.outputBitDepth = NV_ENC_BIT_DEPTH_8;
        hevc.disableSPSPPS = 0;
        hevc.repeatSPSPPS = 1;
        hevc.outputAUD = 1;
        hevc.enableIntraRefresh = 0;
        hevc.enableLTR = 0;
        hevc.enableFillerDataInsertion = 0;
        hevc.maxNumRefFramesInDPB = 1;
        hevc.numRefL0 = NV_ENC_NUM_REF_FRAMES_1;
        hevc.numRefL1 = NV_ENC_NUM_REF_FRAMES_AUTOSELECT;
        hevc.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
    }
    if (config.codec != LLS_CODEC_AV1) {
        auto &vui = config.codec == LLS_CODEC_H264 ? settings.encodeCodecConfig.h264Config.h264VUIParameters :
                                                   settings.encodeCodecConfig.hevcConfig.hevcVUIParameters;
        vui = {};
        vui.videoSignalTypePresentFlag = vui.colourDescriptionPresentFlag = 1;
        vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
        vui.videoFullRangeFlag = 0;
        vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        vui.chromaSampleLocationFlag = 1;
        vui.chromaSampleLocationTop = vui.chromaSampleLocationBot = 0;
    }
    NV_ENC_INITIALIZE_PARAMS init{};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = s.codec_guid;
    init.presetGUID = NV_ENC_PRESET_P1_GUID;
    init.encodeWidth = init.darWidth = init.maxEncodeWidth = config.width;
    init.encodeHeight = init.darHeight = init.maxEncodeHeight = config.height;
    init.frameRateNum = config.fps;
    init.frameRateDen = 1;
    init.enableEncodeAsync = 0;
    init.enablePTD = 1;
    init.encodeConfig = &settings;
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    if (!s.check(s.api.nvEncInitializeEncoder(s.encoder, &init), "NvEncInitializeEncoder"))
        return false;
    s.initialized = true;
    NV_ENC_REGISTER_RESOURCE resource{};
    resource.version = NV_ENC_REGISTER_RESOURCE_VER;
    resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    resource.resourceToRegister = input;
    resource.width = config.width;
    resource.height = config.height;
    resource.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    resource.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (!s.check(s.api.nvEncRegisterResource(s.encoder, &resource),
                 "NvEncRegisterResource(NV12)")) return false;
    s.registered = resource.registeredResource;
    NV_ENC_CREATE_BITSTREAM_BUFFER buffer{};
    buffer.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (!s.check(s.api.nvEncCreateBitstreamBuffer(s.encoder, &buffer),
                 "NvEncCreateBitstreamBuffer")) return false;
    s.bitstream = buffer.bitstreamBuffer;
    std::fprintf(stderr, "%s Main %ux%u %u fps, P1 ULL CBR %u bps, VBV %u bits, one reference\n",
                 lls_codec_name(config.codec), config.width, config.height, config.fps, config.bitrate_bps, rc.vbvBufferSize);
    return true;
}

uint8_t NvencEncoder::codec() const { return state_ ? state_->config.codec : 0; }

bool NvencEncoder::encode(bool force_idr, EncodedFrame &frame, const volatile long &stop)
{
    if (stop) return false;
    frame = {};
    if (!state_ || !state_->bitstream || state_->failed) return false;
    State &s = *state_;
    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = s.registered;
    if (!s.check(s.api.nvEncMapInputResource(s.encoder, &map), "NvEncMapInputResource"))
        return false;
    s.mapped = map.mappedResource;
    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = s.config.width;
    picture.inputHeight = s.config.height;
    picture.inputPitch = s.config.width; // API contract when the D3D pitch is unknown.
    picture.frameIdx = s.index;
    picture.inputTimeStamp = s.index++;
    picture.inputBuffer = s.mapped;
    picture.outputBitstream = s.bitstream;
    picture.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    force_idr = force_idr || picture.frameIdx == 0;
    if (force_idr)
        picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    LARGE_INTEGER submitted{};
    QueryPerformanceCounter(&submitted);
    frame.submit_qpc = submitted.QuadPart;
    if (!s.check(s.api.nvEncEncodePicture(s.encoder, &picture), "NvEncEncodePicture(sync)"))
        return false;
    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = s.bitstream;
    lock.doNotWait = 0;
    if (!s.check(s.api.nvEncLockBitstream(s.encoder, &lock), "NvEncLockBitstream"))
        return false;
    s.locked = true;
    LARGE_INTEGER done{};
    QueryPerformanceCounter(&done);
    frame.done_qpc = done.QuadPart;
    // hwEncodeStatus is informational: the API documents no success value,
    // and other OSS softwares do not test it. Log changes, never fail on it.
    if (lock.hwEncodeStatus != s.last_hw_status) {
        std::fprintf(stderr, "NVENC hwEncodeStatus=%u at frame %u\n", lock.hwEncodeStatus, picture.frameIdx);
        s.last_hw_status = lock.hwEncodeStatus;
    }
    if (!lock.bitstreamBufferPtr || !lock.bitstreamSizeInBytes ||
        lock.outputTimeStamp != picture.inputTimeStamp || lock.pictureType == NV_ENC_PIC_TYPE_B) {
        std::fprintf(stderr, "NVENC output unusable at frame %u: ptr=%p bytes=%u timestamp=%llu "
                     "(expected %llu) picture_type=%d\n", picture.frameIdx, lock.bitstreamBufferPtr,
                     lock.bitstreamSizeInBytes, (unsigned long long)lock.outputTimeStamp,
                     (unsigned long long)picture.inputTimeStamp, int(lock.pictureType));
        s.failed = true;
        return false;
    }
    const auto *data = static_cast<const uint8_t *>(lock.bitstreamBufferPtr);
    s.bytes.assign(data, data + lock.bitstreamSizeInBytes);
    frame.key = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
    if (!s.check(s.api.nvEncUnlockBitstream(s.encoder, s.bitstream), "NvEncUnlockBitstream"))
        return false;
    s.locked = false;
    if (!s.check(s.api.nvEncUnmapInputResource(s.encoder, s.mapped), "NvEncUnmapInputResource"))
        return false;
    s.mapped = nullptr;
    if (s.config.codec == LLS_CODEC_AV1 && !s.bytes.empty() && ((s.bytes[0] >> 3) & 15) != 2)
        s.bytes.insert(s.bytes.begin(), {0x12, 0x00}); // temporal delimiter
    if (frame.key || force_idr || s.config.codec == LLS_CODEC_AV1) {
        HevcAccessUnit *units = nullptr;
        size_t count = 0;
        const bool valid = lls_split_access_units(s.config.codec, s.bytes.data(), s.bytes.size(), &units, &count);
        const bool key = valid && count == 1 && units[0].key;
        std::free(units);
        if (!valid || count != 1 || (force_idr && !key) || (s.config.codec != LLS_CODEC_AV1 && !key)) {
            std::fprintf(stderr, "NVENC output lacks one valid %s frame%s\n", lls_codec_name(s.config.codec), force_idr ? " with recovery headers" : "");
            s.failed = true;
            return false;
        }
        frame.key = key;
    }
    frame.data = s.bytes.data();
    frame.size = s.bytes.size();
    return true;
}

void NvencEncoder::close()
{
    if (!state_) return;
    State &s = *state_;
    if (s.encoder) {
        if (s.locked) s.api.nvEncUnlockBitstream(s.encoder, s.bitstream);
        if (s.initialized) {
            std::fprintf(stderr, "NVENC close: end of stream\n");
            NV_ENC_PIC_PARAMS eos{};
            eos.version = NV_ENC_PIC_PARAMS_VER;
            eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
            s.api.nvEncEncodePicture(s.encoder, &eos);
        }
        std::fprintf(stderr, "NVENC close: releasing buffers\n");
        if (s.mapped) s.api.nvEncUnmapInputResource(s.encoder, s.mapped);
        if (s.bitstream) s.api.nvEncDestroyBitstreamBuffer(s.encoder, s.bitstream);
        if (s.registered) s.api.nvEncUnregisterResource(s.encoder, s.registered);
        std::fprintf(stderr, "NVENC close: destroying encoder\n");
        s.api.nvEncDestroyEncoder(s.encoder);
    }
    if (s.library) FreeLibrary(s.library);
    delete state_;
    state_ = nullptr;
    std::fprintf(stderr, "NVENC close: complete\n");
}
} // namespace lls

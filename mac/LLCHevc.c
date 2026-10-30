#include "LLCHevc.h"
#include "av1_obu.h"

#include <stdlib.h>
#include <string.h>

enum { NAL_VPS = 32, NAL_SPS = 33, NAL_PPS = 34, NAL_AUD = 35 };

static bool reserve(LLCBytes *b, size_t extra)
{
    if (b->size + extra <= b->capacity) return true;
    size_t capacity = b->capacity ? b->capacity : 65536;
    while (capacity < b->size + extra) capacity *= 2;
    uint8_t *data = realloc(b->data, capacity);
    if (!data) return false;
    b->data = data;
    b->capacity = capacity;
    return true;
}

static bool append(LLCBytes *b, const void *data, size_t size)
{
    if (!reserve(b, size)) return false;
    memcpy(b->data + b->size, data, size);
    b->size += size;
    return true;
}

void llc_bytes_free(LLCBytes *bytes)
{
    free(bytes->data);
    *bytes = (LLCBytes){0};
}

// The next NAL unit at or after *pos, without its start code or trailing
// zero bytes. Emulation prevention keeps 00 00 01 out of NAL payloads.
static bool next_nal(const uint8_t *d, size_t size, size_t *pos, const uint8_t **nal, size_t *nal_size)
{
    size_t i = *pos;
    while (i + 3 <= size && !(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)) ++i;
    if (i + 3 > size) return false;
    const size_t start = i + 3;
    size_t j = start;
    while (j + 3 <= size && !(d[j] == 0 && d[j + 1] == 0 && d[j + 2] == 1)) ++j;
    size_t end = j + 3 <= size ? j : size;
    *pos = end;
    while (end > start && d[end - 1] == 0) --end;
    *nal = d + start;
    *nal_size = end - start;
    return true;
}

static int nal_type(uint8_t codec, const uint8_t *nal)
{ return codec == LLS_CODEC_H264 ? nal[0] & 31 : (nal[0] >> 1) & 63; }

static CMVideoFormatDescriptionRef av1_format(const uint8_t *au, size_t size)
{
    size_t cursor = 0;
    LlsAv1Obu obu;
    while (cursor < size) {
        if (!lls_av1_next_obu(au, size, &cursor, &obu)) return NULL;
        if (obu.type != 1) continue;
        LlsAv1Sequence sequence;
        if (!lls_av1_sequence(au + obu.payload_offset, obu.payload_size, &sequence)) return NULL;
        LLCBytes config = {0};
        if (!append(&config, sequence.av1c, 4) || !append(&config, au + obu.offset, obu.size)) {
            llc_bytes_free(&config); return NULL;
        }
        CFDataRef data = CFDataCreate(kCFAllocatorDefault, config.data, (CFIndex)config.size);
        llc_bytes_free(&config);
        if (!data) return NULL;
        const void *atom_key = CFSTR("av1C"), *atom_value = data;
        CFDictionaryRef atoms = CFDictionaryCreate(kCFAllocatorDefault, &atom_key, &atom_value, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const void *key = kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, *value = atoms;
        CFDictionaryRef extensions = atoms ? CFDictionaryCreate(kCFAllocatorDefault, &key, &value, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : NULL;
        CMVideoFormatDescriptionRef format = NULL;
        if (extensions && CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_AV1,
            (int32_t)sequence.width, (int32_t)sequence.height, extensions, &format) != noErr) format = NULL;
        if (extensions) CFRelease(extensions);
        if (atoms) CFRelease(atoms);
        CFRelease(data);
        return format;
    }
    return NULL;
}

CMVideoFormatDescriptionRef llc_video_format_from_annexb(uint8_t codec, const uint8_t *au, size_t size)
{
    if (codec == LLS_CODEC_AV1) return av1_format(au, size);
    if (codec != LLS_CODEC_HEVC && codec != LLS_CODEC_H264) return NULL;
    const int first = codec == LLS_CODEC_H264 ? 7 : NAL_VPS;
    const int count = codec == LLS_CODEC_H264 ? 2 : 3;
    const uint8_t *sets[3] = {NULL, NULL, NULL};
    size_t sizes[3] = {0, 0, 0}, pos = 0, nal_size;
    const uint8_t *nal;
    while (next_nal(au, size, &pos, &nal, &nal_size)) {
        if (nal_size < 2 || (nal[0] & 0x80)) continue;
        const int type = nal_type(codec, nal);
        if (type >= first && type < first + count && !sets[type - first]) {
            sets[type - first] = nal;
            sizes[type - first] = nal_size;
        }
    }
    if (!sets[0] || !sets[1] || (count == 3 && !sets[2])) return NULL;
    CMVideoFormatDescriptionRef format = NULL;
    OSStatus status = codec == LLS_CODEC_H264 ?
        CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, sets, sizes, 4, &format) :
        CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, 3, sets, sizes, 4, NULL, &format);
    if (status != noErr)
        return NULL;
    return format;
}

CMSampleBufferRef llc_video_sample_from_annexb(uint8_t codec, const uint8_t *au, size_t size,
                                              CMVideoFormatDescriptionRef format)
{
    if (codec < LLS_CODEC_HEVC || codec > LLS_CODEC_AV1) return NULL;
    LLCBytes avcc = {0};
    size_t pos = 0, nal_size;
    const uint8_t *nal;
    while (codec != LLS_CODEC_AV1 && next_nal(au, size, &pos, &nal, &nal_size)) {
        if (nal_size < 2) continue;
        const int type = nal_type(codec, nal);
        if (codec == LLS_CODEC_H264 ? (type == 7 || type == 8 || type == 9) :
            ((type >= NAL_VPS && type <= NAL_PPS) || type == NAL_AUD)) continue;
        const uint8_t length[4] = {(uint8_t)(nal_size >> 24), (uint8_t)(nal_size >> 16),
                                   (uint8_t)(nal_size >> 8), (uint8_t)nal_size};
        if (!append(&avcc, length, 4) || !append(&avcc, nal, nal_size)) {
            llc_bytes_free(&avcc);
            return NULL;
        }
    }
    while (codec == LLS_CODEC_AV1 && pos < size) {
        LlsAv1Obu obu;
        if (!lls_av1_next_obu(au, size, &pos, &obu) ||
            (obu.type != 2 && !append(&avcc, au + obu.offset, obu.size))) {
            llc_bytes_free(&avcc); return NULL;
        }
    }
    CMBlockBufferRef block = NULL;
    CMSampleBufferRef sample = NULL;
    if (avcc.size &&
        CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL, avcc.size, kCFAllocatorDefault,
                                           NULL, 0, avcc.size, kCMBlockBufferAssureMemoryNowFlag,
                                           &block) == kCMBlockBufferNoErr &&
        CMBlockBufferReplaceDataBytes(avcc.data, block, 0, avcc.size) == kCMBlockBufferNoErr) {
        const size_t sample_size = avcc.size;
        if (CMSampleBufferCreateReady(kCFAllocatorDefault, block, format, 1, 0, NULL, 1, &sample_size,
                                      &sample) != noErr)
            sample = NULL;
    }
    if (block) CFRelease(block);
    llc_bytes_free(&avcc);
    return sample;
}

bool llc_video_annexb_from_sample(uint8_t codec, CMSampleBufferRef sample, bool parameter_sets, LLCBytes *out)
{
    static const uint8_t start[4] = {0, 0, 0, 1};
    out->size = 0;
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
    int length_size = 4;
    size_t count = 0;
    if (codec != LLS_CODEC_HEVC && codec != LLS_CODEC_H264) return false;
    OSStatus (*get_set)(CMFormatDescriptionRef, size_t, const uint8_t **, size_t *, size_t *, int *) =
        codec == LLS_CODEC_H264 ? CMVideoFormatDescriptionGetH264ParameterSetAtIndex :
                                CMVideoFormatDescriptionGetHEVCParameterSetAtIndex;
    if (!format || CMFormatDescriptionGetMediaSubType(format) !=
            (codec == LLS_CODEC_H264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC) ||
        get_set(format, 0, NULL, NULL, &count, &length_size) != noErr ||
        length_size < 1 || length_size > 4)
        return false;
    if (parameter_sets) {
        for (size_t i = 0; i < count; ++i) {
            const uint8_t *set;
            size_t set_size;
            if (get_set(format, i, &set, &set_size, NULL, NULL) != noErr ||
                !append(out, start, 4) || !append(out, set, set_size))
                return false;
        }
    }
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    const size_t total = block ? CMBlockBufferGetDataLength(block) : 0;
    // Copy out (the block buffer may not be contiguous), then write each
    // length-prefixed NAL unit after a start code.
    LLCBytes avcc = {0};
    if (!total || !reserve(&avcc, total) ||
        CMBlockBufferCopyDataBytes(block, 0, total, avcc.data) != kCMBlockBufferNoErr) {
        llc_bytes_free(&avcc);
        return false;
    }
    avcc.size = total;
    bool ok = true;
    for (size_t pos = 0; pos < total && ok;) {
        if (total - pos < (size_t)length_size) { ok = false; break; }
        size_t nal_size = 0;
        for (int i = 0; i < length_size; ++i) nal_size = nal_size << 8 | avcc.data[pos + i];
        pos += (size_t)length_size;
        if (!nal_size || nal_size > total - pos) { ok = false; break; }
        ok = append(out, start, 4) && append(out, avcc.data + pos, nal_size);
        pos += nal_size;
    }
    llc_bytes_free(&avcc);
    return ok && out->size;
}

bool llc_sample_is_key(CMSampleBufferRef sample)
{
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || !CFArrayGetCount(attachments)) return true;
    CFDictionaryRef first = CFArrayGetValueAtIndex(attachments, 0);
    CFBooleanRef not_sync = CFDictionaryGetValue(first, kCMSampleAttachmentKey_NotSync);
    return !not_sync || !CFBooleanGetValue(not_sync);
}

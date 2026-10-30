#pragma once
namespace lls {
inline constexpr char scale_hlsl[] = R"hlsl(
Texture2D<float4> source : register(t0);
SamplerState bilinear_clamp : register(s0);
cbuffer Geometry : register(b0) {
    float2 inverse_source;
    float2 ratio;
    float2 origin;
    float2 fit_size;
    int2 taps;
    float2 padding;
};

float encode_srgb(float v)
{
    v = saturate(v);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

float4 scale(float4 position : SV_Position) : SV_Target
{
    float2 centre = (position.xy - origin) / fit_size;
    float3 rgb = float3(0, 0, 0);
    // Bounded at 16 bilinear samples. At 2:1 the one centred tap averages
    // exactly 2x2 source pixels; at 4:1 and 8:1 the pairs tile the footprint.
    [loop] for (int y = 0; y < taps.y; ++y) {
        [loop] for (int x = 0; x < taps.x; ++x) {
            float2 delta = ((float2(x, y) + 0.5) / float2(taps) - 0.5) * ratio;
            rgb += source.SampleLevel(bilinear_clamp, centre + delta * inverse_source, 0).rgb;
        }
    }
    rgb /= float(taps.x * taps.y);
#if REENCODE_SRGB
    // An _SRGB SRV decodes before filtering. Our UNORM private copy needs
    // encoded values, since the unchanged NV12 shader expects gamma RGB.
    rgb = float3(encode_srgb(rgb.r), encode_srgb(rgb.g), encode_srgb(rgb.b));
#endif
    // FP16 goes to an FP16 private copy unchanged, still linear. Its existing
    // NV12 shader performs the final SDR clipping and transfer function.
    return float4(rgb, 1);
}
)hlsl";
} // namespace lls

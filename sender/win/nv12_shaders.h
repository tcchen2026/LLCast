#pragma once
// OUTPUT_WIDTH/OUTPUT_HEIGHT and INPUT_LINEAR (FP16 SDR scRGB vs BGRA8).
namespace lls {
inline constexpr char nv12_hlsl[] = R"hlsl(
Texture2D<float4> source : register(t0);

float linear_to_srgb(float value)
{
    // SDR Advanced Color uses 1.0 for reference white. Clip out-of-gamut
    // channels for the BT.709 output; this is not HDR tone mapping.
    value = saturate(value);
    return value <= 0.0031308 ? value * 12.92 : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}

float4 vertex(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 rgb_at(int2 xy)
{
    uint w, h;
    source.GetDimensions(w, h);
    // Desktop pixels inside the encoded image, as signed bounds: fxc rejects
    // signed/unsigned comparisons.
    int2 valid = int2(min(w, uint(OUTPUT_WIDTH)), min(h, uint(OUTPUT_HEIGHT)));
    // Extend the encoded image's border, not the smaller desktop's border:
    // padding inside OUTPUT_WIDTH must remain black.
    xy.x = clamp(xy.x, 0, OUTPUT_WIDTH - 1);
    if (xy.x >= valid.x || xy.y >= valid.y)
        return float3(0, 0, 0);
    // Views return logical RGBA. Load avoids interpolation/scaling.
    float3 rgb = source.Load(int3(xy, 0)).rgb;
#if INPUT_LINEAR
    rgb = float3(linear_to_srgb(rgb.r), linear_to_srgb(rgb.g), linear_to_srgb(rgb.b));
#endif
    return rgb;
}

float luma(float4 position : SV_Position) : SV_Target
{
    float y = dot(rgb_at(int2(position.xy)), float3(0.2126, 0.7152, 0.0722));
    return (16.0 + 219.0 * y) / 255.0;
}

float2 chroma(float4 position : SV_Position) : SV_Target
{
    int2 xy = int2(position.xy) * 2;
    // Left-sited horizontally, centred vertically (HEVC chroma location 0).
    float3 rgb = (rgb_at(xy + int2(-1, 0)) + rgb_at(xy + int2(1, 0)) +
                  rgb_at(xy + int2(-1, 1)) + rgb_at(xy + int2(1, 1))) * 0.125 +
                 (rgb_at(xy) + rgb_at(xy + int2(0, 1))) * 0.25;
    float y = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    return (128.0 + 112.0 * float2((rgb.b - y) / (1.0 - 0.0722),
                                  (rgb.r - y) / (1.0 - 0.2126))) / 255.0;
}
)hlsl";
} // namespace lls

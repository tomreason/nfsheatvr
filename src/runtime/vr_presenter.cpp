#include "vr_presenter.hpp"

#include "logger.hpp"
#include "camera_probe.hpp"
#include "depth_observer.hpp"
#include "frostbite_camera.hpp"
#include "input_bridge.hpp"
#include "vr_settings.hpp"

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nfsheatvr {
namespace {
constexpr const char* kVertexShader = R"(
struct Output { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Output main(uint id : SV_VertexID) {
    const float2 positions[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    const float2 uvs[3] = { float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0) };
    Output output; output.position = float4(positions[id], 0.0, 1.0); output.uv = uvs[id]; return output;
})";

constexpr const char* kPixelShader = R"(
Texture2D sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);
cbuffer PresentationLayout : register(b0) {
    float2 contentScale;
    float2 sourceCropScale;
    float2 padding;
    float2 fullscreenFov;
    float2 sourceTexelSize;
    float2 upscalerSettings;
};
float WarpFovCoordinate(float coordinate, float fovScale) {
    const float amount = abs(fovScale - 1.0) * 2.8;
    if (amount < 0.001) return coordinate;
    // Both curves keep -1 and +1 fixed. Therefore FOV adjustment remains
    // edge-to-edge instead of revealing the old square virtual screen.
    return fovScale > 1.0 ? atan(coordinate * amount) / atan(amount)
                          : tan(coordinate * atan(amount)) / amount;
}
float3 ToneMapForHeadset(float3 colour) {
    // The normal game back buffer is already display-encoded. Frostbite's
    // experimental HDR candidate is not, so bring every sample into the same
    // display space before the spatial filter compares local contrast.
    if (fullscreenFov.y > 0.5) {
        colour = max(colour, 0.0);
        colour = (colour * (2.51 * colour + 0.03)) / (colour * (2.43 * colour + 0.59) + 0.14);
        colour = pow(saturate(colour), 1.0 / 2.2);
    }
    return colour;
}
float3 SampleForHeadset(float2 sampleUv) {
    return ToneMapForHeadset(sourceTexture.SampleLevel(sourceSampler, sampleUv, 0).rgb);
}
float3 ApplyHeadsetUpscaler(float2 centreUv, float3 centre) {
    const float strength = saturate(upscalerSettings.x);
    if (strength < 0.001) return centre;

    // This is the sharpening stage of a spatial upscaler. It restores local
    // edge contrast after the game image is sampled into a larger OpenXR eye
    // texture, with no frame history or added VR latency.
    const float3 north = SampleForHeadset(centreUv + float2(0.0, -sourceTexelSize.y));
    const float3 south = SampleForHeadset(centreUv + float2(0.0,  sourceTexelSize.y));
    const float3 west = SampleForHeadset(centreUv + float2(-sourceTexelSize.x, 0.0));
    const float3 east = SampleForHeadset(centreUv + float2( sourceTexelSize.x, 0.0));
    const float3 blur = (north + south + west + east) * 0.25;
    const float3 localMin = min(centre, min(min(north, south), min(west, east)));
    const float3 localMax = max(centre, max(max(north, south), max(west, east)));
    const float edge = dot(abs(centre - blur), float3(0.2126, 0.7152, 0.0722));
    const float adaptiveStrength = strength * (0.12 + 0.48 * saturate(edge * 5.0));
    const float3 enhanced = centre + (centre - blur) * adaptiveStrength;
    // Bound the boost locally so dashboard text and UI do not gain halos.
    const float3 limit = max(localMax - centre, centre - localMin) * 0.72;
    return clamp(enhanced, centre - limit, centre + limit);
}
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    // The game back buffer is normally 16:9 while a headset eye texture is
    // close to square.  Keep the game image proportional and draw black bars
    // around it rather than stretching it to fill the eye.
    const float2 contentUv = (uv - float2(0.5, 0.5)) / contentScale + float2(0.5, 0.5);
    if (contentUv.x < 0.0 || contentUv.x > 1.0 || contentUv.y < 0.0 || contentUv.y > 1.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float2 sourceCoordinate = float2(WarpFovCoordinate((contentUv.x - 0.5) * 2.0, fullscreenFov.x),
                                           WarpFovCoordinate((contentUv.y - 0.5) * 2.0, fullscreenFov.x)) * 0.5;
    const float2 sourceUv = sourceCoordinate * sourceCropScale + float2(0.5, 0.5);
    const float3 sourceColour = SampleForHeadset(sourceUv);
    return float4(ApplyHeadsetUpscaler(sourceUv, sourceColour), 1.0);
})";

constexpr const char* kDepthDebugPixelShader = R"(
Texture2D sourceTexture : register(t0);
Texture2D<float> depthTexture : register(t1);
SamplerState sourceSampler : register(s0);
SamplerState depthSampler : register(s1);
cbuffer PresentationLayout : register(b0) {
    float2 contentScale;
    float2 sourceCropScale;
    float2 padding;
    float2 fullscreenFov;
    float2 sourceTexelSize;
    float2 upscalerSettings;
};
float WarpFovCoordinate(float coordinate, float fovScale) {
    const float amount = abs(fovScale - 1.0) * 2.8;
    if (amount < 0.001) return coordinate;
    return fovScale > 1.0 ? atan(coordinate * amount) / atan(amount)
                          : tan(coordinate * atan(amount)) / amount;
}
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    // Draw a small, unmistakable swatch so F12's candidate switch is visible
    // even when a selected texture has no useful scene depth.
    if (uv.x < 0.055 && uv.y < 0.055) {
        if (padding.x < 0.5) return float4(1.0, 0.12, 0.12, 1.0); // candidate 0
        if (padding.x < 1.5) return float4(0.12, 1.0, 0.12, 1.0); // candidate 1
        return float4(0.12, 0.35, 1.0, 1.0);                       // candidate 2+
    }
    const float2 contentUv = (uv - float2(0.5, 0.5)) / contentScale + float2(0.5, 0.5);
    if (contentUv.x < 0.0 || contentUv.x > 1.0 || contentUv.y < 0.0 || contentUv.y > 1.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float2 sourceCoordinate = float2(WarpFovCoordinate((contentUv.x - 0.5) * 2.0, fullscreenFov.x),
                                           WarpFovCoordinate((contentUv.y - 0.5) * 2.0, fullscreenFov.x)) * 0.5;
    const float2 sourceUv = sourceCoordinate * sourceCropScale + float2(0.5, 0.5);
    // Frostbite's readable depth candidates use non-linear / sometimes
    // reversed-Z encodings. A raw 0..1 preview turns a legitimate buffer
    // nearly black. The two logarithmic band channels make either convention
    // visible: red follows raw Z, green follows inverted Z.
    const float depth = saturate(depthTexture.SampleLevel(depthSampler, sourceUv, 0));
    const float rawBands = frac(-log2(max(depth, 1e-8)) * 0.15);
    const float invertedBands = frac(-log2(max(1.0 - depth, 1e-8)) * 0.15);
    const float detail = max(rawBands, invertedBands);
    return float4(rawBands, invertedBands, 0.16 + detail * 0.76, 1.0);
})";

constexpr const char* kDepthStereoPixelShader = R"(
Texture2D sourceTexture : register(t0);
Texture2D<float> depthTexture : register(t1);
SamplerState sourceSampler : register(s0);
SamplerState depthSampler : register(s1);
cbuffer PresentationLayout : register(b0) {
    float2 contentScale;
    float2 sourceCropScale;
    // padding.x = left (+1) or right (-1) eye; padding.y = max UV shift.
    float2 padding;
    float2 fullscreenFov;
    float2 sourceTexelSize;
    float2 upscalerSettings;
};
float WarpFovCoordinate(float coordinate, float fovScale) {
    const float amount = abs(fovScale - 1.0) * 2.8;
    if (amount < 0.001) return coordinate;
    return fovScale > 1.0 ? atan(coordinate * amount) / atan(amount)
                          : tan(coordinate * atan(amount)) / amount;
}
float3 ToneMapForHeadset(float3 colour) {
    if (fullscreenFov.y > 0.5) {
        colour = max(colour, 0.0);
        colour = (colour * (2.51 * colour + 0.03)) / (colour * (2.43 * colour + 0.59) + 0.14);
        colour = pow(saturate(colour), 1.0 / 2.2);
    }
    return colour;
}
float3 SampleForHeadset(float2 sampleUv) {
    return ToneMapForHeadset(sourceTexture.SampleLevel(sourceSampler, sampleUv, 0).rgb);
}
float3 ApplyHeadsetUpscaler(float2 centreUv, float3 centre) {
    const float strength = saturate(upscalerSettings.x);
    if (strength < 0.001) return centre;
    const float3 north = SampleForHeadset(centreUv + float2(0.0, -sourceTexelSize.y));
    const float3 south = SampleForHeadset(centreUv + float2(0.0,  sourceTexelSize.y));
    const float3 west = SampleForHeadset(centreUv + float2(-sourceTexelSize.x, 0.0));
    const float3 east = SampleForHeadset(centreUv + float2( sourceTexelSize.x, 0.0));
    const float3 blur = (north + south + west + east) * 0.25;
    const float3 localMin = min(centre, min(min(north, south), min(west, east)));
    const float3 localMax = max(centre, max(max(north, south), max(west, east)));
    const float edge = dot(abs(centre - blur), float3(0.2126, 0.7152, 0.0722));
    const float adaptiveStrength = strength * (0.12 + 0.48 * saturate(edge * 5.0));
    const float3 enhanced = centre + (centre - blur) * adaptiveStrength;
    const float3 limit = max(localMax - centre, centre - localMin) * 0.72;
    return clamp(enhanced, centre - limit, centre + limit);
}
float StereoForeground(float2 centreUv) {
    const float centreDepth = saturate(depthTexture.SampleLevel(depthSampler, centreUv, 0));
    // A monoscopic depth warp cannot reconstruct pixels hidden behind a
    // nearby edge. Fade parallax at such discontinuities rather than pulling
    // background colour through the object as a long crack.
    const float leftDepth = saturate(depthTexture.SampleLevel(depthSampler, centreUv - float2(sourceTexelSize.x, 0.0), 0));
    const float rightDepth = saturate(depthTexture.SampleLevel(depthSampler, centreUv + float2(sourceTexelSize.x, 0.0), 0));
    const float aboveDepth = saturate(depthTexture.SampleLevel(depthSampler, centreUv - float2(0.0, sourceTexelSize.y), 0));
    const float belowDepth = saturate(depthTexture.SampleLevel(depthSampler, centreUv + float2(0.0, sourceTexelSize.y), 0));
    const float discontinuity = max(max(abs(centreDepth - leftDepth), abs(centreDepth - rightDepth)),
                                    max(abs(centreDepth - aboveDepth), abs(centreDepth - belowDepth)));
    return centreDepth * (1.0 - smoothstep(0.008, 0.055, discontinuity));
}
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    const float2 contentUv = (uv - float2(0.5, 0.5)) / contentScale + float2(0.5, 0.5);
    if (contentUv.x < 0.0 || contentUv.x > 1.0 || contentUv.y < 0.0 || contentUv.y > 1.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float2 sourceCoordinate = float2(WarpFovCoordinate((contentUv.x - 0.5) * 2.0, fullscreenFov.x),
                                           WarpFovCoordinate((contentUv.y - 0.5) * 2.0, fullscreenFov.x)) * 0.5;
    const float2 centreUv = sourceCoordinate * sourceCropScale + float2(0.5, 0.5);
    // Frostbite normally uses reversed-Z, where depth is proportional to
    // inverse distance. Keep that relationship linear: the former fractional
    // power exaggerated distant geometry and created a false stereo band.
    // This is image reprojection, not a second engine render.
    const float foreground = StereoForeground(centreUv);
    const float2 stereoUv = clamp(centreUv - float2(padding.x * padding.y * foreground, 0.0),
                                  float2(0.0, 0.0), float2(1.0, 1.0));
    const float3 sourceColour = SampleForHeadset(stereoUv);
    return float4(ApplyHeadsetUpscaler(stereoUv, sourceColour), 1.0);
})";

// AMD FidelityFX Super Resolution 1 reference implementation (EASU/RCAS),
// adapted to this D3D11 fullscreen pipeline. Copyright (c) 2021 Advanced
// Micro Devices, Inc.; distributed under the MIT license. The original source
// is https://github.com/GPUOpen-Effects/FidelityFX-FSR.
constexpr const char* kFsrEasuPixelShader = R"(
struct Output { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Texture2D sourceTexture : register(t0);
Texture2D<float> depthTexture : register(t1);
SamplerState sourceSampler : register(s0);
SamplerState depthSampler : register(s1);
cbuffer FsrEasuConstants : register(b0) {
    float4 easuCon0;
    float4 easuCon1;
    float4 easuCon2;
    float4 easuCon3;
    float depthStereoShift;
    float depthEyeSign;
    float2 unused;
};
void EasuTap(inout float3 colour, inout float weight, float2 offset, float2 direction,
             float2 length, float lobe, float clipping, float3 sampleColour) {
    float2 rotated;
    rotated.x = offset.x * direction.x + offset.y * direction.y;
    rotated.y = offset.x * -direction.y + offset.y * direction.x;
    rotated *= length;
    const float distanceSquared = min(dot(rotated, rotated), clipping);
    float base = 0.4 * distanceSquared - 1.0;
    float window = lobe * distanceSquared - 1.0;
    base *= base;
    window *= window;
    base = 1.5625 * base - 0.5625;
    const float tapWeight = base * window;
    colour += sampleColour * tapWeight;
    weight += tapWeight;
}
void EasuSet(inout float2 direction, inout float length, float2 fractionalPosition, float bilinearWeight,
             float lumaA, float lumaB, float lumaC, float lumaD, float lumaE) {
    const float dc = lumaD - lumaC;
    const float cb = lumaC - lumaB;
    const float directionX = lumaD - lumaB;
    const float lengthX = saturate(abs(directionX) / max(max(abs(dc), abs(cb)), 1e-6));
    direction.x += directionX * bilinearWeight;
    length += lengthX * lengthX * bilinearWeight;
    const float ec = lumaE - lumaC;
    const float ca = lumaC - lumaA;
    const float directionY = lumaE - lumaA;
    const float lengthY = saturate(abs(directionY) / max(max(abs(ec), abs(ca)), 1e-6));
    direction.y += directionY * bilinearWeight;
    length += lengthY * lengthY * bilinearWeight;
}
float StereoForeground(float2 depthUv) {
    const float centreDepth = saturate(depthTexture.SampleLevel(depthSampler, depthUv, 0));
    const float2 texel = easuCon1.xy;
    const float leftDepth = saturate(depthTexture.SampleLevel(depthSampler, depthUv - float2(texel.x, 0.0), 0));
    const float rightDepth = saturate(depthTexture.SampleLevel(depthSampler, depthUv + float2(texel.x, 0.0), 0));
    const float aboveDepth = saturate(depthTexture.SampleLevel(depthSampler, depthUv - float2(0.0, texel.y), 0));
    const float belowDepth = saturate(depthTexture.SampleLevel(depthSampler, depthUv + float2(0.0, texel.y), 0));
    const float discontinuity = max(max(abs(centreDepth - leftDepth), abs(centreDepth - rightDepth)),
                                    max(abs(centreDepth - aboveDepth), abs(centreDepth - belowDepth)));
    return centreDepth * (1.0 - smoothstep(0.008, 0.055, discontinuity));
}
float3 FsrEasu(uint2 outputPixel) {
    float2 pixelPosition = float2(outputPixel) * easuCon0.xy + easuCon0.zw;
    const float2 integerPosition = floor(pixelPosition);
    const float2 fractionalPosition = pixelPosition - integerPosition;
    float2 gatherPosition = integerPosition * easuCon1.xy + easuCon1.zw;

    // Keep the established depth-stereo reprojection on the FSR input. The
    // shift is intentionally evaluated once per output pixel, matching the
    // original stereo path and avoiding a false centre band at depth edges.
    if (depthStereoShift > 0.0) {
        const float2 depthUv = (pixelPosition + float2(0.5, 0.5)) * easuCon1.xy;
        const float foreground = StereoForeground(depthUv);
        gatherPosition -= float2(depthEyeSign * depthStereoShift * foreground, 0.0);
    }
    const float2 gatherPosition1 = gatherPosition + easuCon2.xy;
    const float2 gatherPosition2 = gatherPosition + easuCon2.zw;
    const float2 gatherPosition3 = gatherPosition + easuCon3.xy;
    const float4 bczzR = sourceTexture.GatherRed(sourceSampler, gatherPosition);
    const float4 bczzG = sourceTexture.GatherGreen(sourceSampler, gatherPosition);
    const float4 bczzB = sourceTexture.GatherBlue(sourceSampler, gatherPosition);
    const float4 ijfeR = sourceTexture.GatherRed(sourceSampler, gatherPosition1);
    const float4 ijfeG = sourceTexture.GatherGreen(sourceSampler, gatherPosition1);
    const float4 ijfeB = sourceTexture.GatherBlue(sourceSampler, gatherPosition1);
    const float4 klhgR = sourceTexture.GatherRed(sourceSampler, gatherPosition2);
    const float4 klhgG = sourceTexture.GatherGreen(sourceSampler, gatherPosition2);
    const float4 klhgB = sourceTexture.GatherBlue(sourceSampler, gatherPosition2);
    const float4 zzonR = sourceTexture.GatherRed(sourceSampler, gatherPosition3);
    const float4 zzonG = sourceTexture.GatherGreen(sourceSampler, gatherPosition3);
    const float4 zzonB = sourceTexture.GatherBlue(sourceSampler, gatherPosition3);
    const float4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    const float4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    const float4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    const float4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
    const float bL = bczzL.x, cL = bczzL.y;
    const float iL = ijfeL.x, jL = ijfeL.y, fL = ijfeL.z, eL = ijfeL.w;
    const float kL = klhgL.x, lL = klhgL.y, hL = klhgL.z, gL = klhgL.w;
    const float oL = zzonL.z, nL = zzonL.w;
    float2 direction = float2(0.0, 0.0);
    float edgeLength = 0.0;
    EasuSet(direction, edgeLength, fractionalPosition, (1.0-fractionalPosition.x)*(1.0-fractionalPosition.y), bL,eL,fL,gL,jL);
    EasuSet(direction, edgeLength, fractionalPosition,      fractionalPosition.x *(1.0-fractionalPosition.y), cL,fL,gL,hL,kL);
    EasuSet(direction, edgeLength, fractionalPosition, (1.0-fractionalPosition.x)*     fractionalPosition.y,  fL,iL,jL,kL,nL);
    EasuSet(direction, edgeLength, fractionalPosition,      fractionalPosition.x *     fractionalPosition.y,  gL,jL,kL,lL,oL);
    const float directionLengthSquared = dot(direction, direction);
    if (directionLengthSquared < 1.0 / 32768.0) direction = float2(1.0, 0.0);
    else direction *= rsqrt(directionLengthSquared);
    edgeLength = edgeLength * 0.5;
    edgeLength *= edgeLength;
    const float stretch = dot(direction, direction) / max(max(abs(direction.x), abs(direction.y)), 1e-6);
    const float2 anisotropicLength = float2(1.0 + (stretch - 1.0) * edgeLength, 1.0 - 0.5 * edgeLength);
    const float lobe = 0.5 + (0.21 - 0.5) * edgeLength;
    const float clipping = 1.0 / lobe;
    const float3 min4 = min(min(float3(ijfeR.z,ijfeG.z,ijfeB.z), float3(klhgR.w,klhgG.w,klhgB.w)),
                            min(float3(ijfeR.y,ijfeG.y,ijfeB.y), float3(klhgR.x,klhgG.x,klhgB.x)));
    const float3 max4 = max(max(float3(ijfeR.z,ijfeG.z,ijfeB.z), float3(klhgR.w,klhgG.w,klhgB.w)),
                            max(float3(ijfeR.y,ijfeG.y,ijfeB.y), float3(klhgR.x,klhgG.x,klhgB.x)));
    float3 accumulatedColour = float3(0.0, 0.0, 0.0);
    float accumulatedWeight = 0.0;
    EasuTap(accumulatedColour, accumulatedWeight, float2( 0.0,-1.0)-fractionalPosition,direction,anisotropicLength,lobe,clipping,float3(bczzR.x,bczzG.x,bczzB.x));
    EasuTap(accumulatedColour, accumulatedWeight, float2( 1.0,-1.0)-fractionalPosition,direction,anisotropicLength,lobe,clipping,float3(bczzR.y,bczzG.y,bczzB.y));
    EasuTap(accumulatedColour, accumulatedWeight, float2(-1.0, 1.0)-fractionalPosition,direction,anisotropicLength,lobe,clipping,float3(ijfeR.x,ijfeG.x,ijfeB.x));
    EasuTap(accumulatedColour, accumulatedWeight, float2( 0.0, 1.0)-fractionalPosition,direction,anisotropicLength,lobe,clipping,float3(ijfeR.y,ijfeG.y,ijfeB.y));
    EasuTap(accumulatedColour, accumulatedWeight, float2( 0.0, 0.0)-fractionalPosition,direction,anisotropicLength,lobe,clipping,float3(ijfeR.z,ijfeG.z,ijfeB.z));
    EasuTap(accumulatedColour, accumulatedWeight, float2(-1.0, 0.0)-fractionalPosition,direction,anisotropicLength,lobe,clipping,float3(ijfeR.w,ijfeG.w,ijfeB.w));
    EasuTap(accumulatedColour, accumulatedWeight, float2( 1.0, 1.0)-fractionalPosition,direction,anisotropicLength,lobe,clipping,float3(klhgR.x,klhgG.x,klhgB.x));
    EasuTap(accumulatedColour, accumulatedWeight, float2( 2.0, 1.0)-fractionalPosition,direction,anisotropicLength,lobe,clipping,float3(klhgR.y,klhgG.y,klhgB.y));
    EasuTap(accumulatedColour, accumulatedWeight, float2( 2.0, 0.0)-fractionalPosition,direction,anisotropicLength,lobe,clipping,float3(klhgR.z,klhgG.z,klhgB.z));
    EasuTap(accumulatedColour, accumulatedWeight, float2( 1.0, 0.0)-fractionalPosition,direction,anisotropicLength,lobe,clipping,float3(klhgR.w,klhgG.w,klhgB.w));
    EasuTap(accumulatedColour, accumulatedWeight, float2( 1.0, 2.0)-fractionalPosition,direction,anisotropicLength,lobe,clipping,float3(zzonR.z,zzonG.z,zzonB.z));
    EasuTap(accumulatedColour, accumulatedWeight, float2( 0.0, 2.0)-fractionalPosition,direction,anisotropicLength,lobe,clipping,float3(zzonR.w,zzonG.w,zzonB.w));
    return clamp(accumulatedColour / max(accumulatedWeight, 1e-6), min4, max4);
}
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return float4(FsrEasu(uint2(position.xy)), 1.0);
})";

constexpr const char* kFsrRcasPixelShader = R"(
Texture2D sourceTexture : register(t0);
cbuffer FsrRcasConstants : register(b0) {
    float rcasAttenuation;
    float rcasWidth;
    float rcasHeight;
    float unused;
};
float3 LoadSource(int2 position) {
    const int2 size = int2(rcasWidth, rcasHeight);
    return sourceTexture.Load(int3(clamp(position, int2(0, 0), size - int2(1, 1)), 0)).rgb;
}
float3 FsrRcas(uint2 outputPixel) {
    const int2 pixel = int2(outputPixel);
    const float3 b = LoadSource(pixel + int2( 0,-1));
    const float3 d = LoadSource(pixel + int2(-1, 0));
    const float3 e = LoadSource(pixel);
    const float3 f = LoadSource(pixel + int2( 1, 0));
    const float3 h = LoadSource(pixel + int2( 0, 1));
    const float lumaB = b.b*0.5 + (b.r*0.5 + b.g);
    const float lumaD = d.b*0.5 + (d.r*0.5 + d.g);
    const float lumaE = e.b*0.5 + (e.r*0.5 + e.g);
    const float lumaF = f.b*0.5 + (f.r*0.5 + f.g);
    const float lumaH = h.b*0.5 + (h.r*0.5 + h.g);
    const float lumaRange = max(max(max(lumaB,lumaD),max(lumaE,lumaF)),lumaH) -
                            min(min(min(lumaB,lumaD),min(lumaE,lumaF)),lumaH);
    const float noise = 1.0 - 0.5 * saturate(abs(0.25*(lumaB+lumaD+lumaF+lumaH)-lumaE) /
                                               (lumaRange + 1e-6));
    const float3 min4 = min(min(b,d), min(f,h));
    const float3 max4 = max(max(b,d), max(f,h));
    const float3 hitMin = min(min4,e) / max(4.0 * max4, 1e-6);
    const float3 hitMax = (1.0 - max(max4,e)) / min(4.0 * min4 - 4.0, -1e-6);
    const float lobe = max(-0.1875, min(max(max(-hitMin, hitMax).r,
                                            max(-hitMin, hitMax).g),
                                        max(-hitMin, hitMax).b)) * exp2(-rcasAttenuation) * noise;
    return (lobe*(b+d+f+h)+e) / (4.0*lobe+1.0);
}
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return float4(saturate(FsrRcas(uint2(position.xy))), 1.0);
})";

bool Succeeded(const XrResult result, const wchar_t* operation) {
    if (XR_SUCCEEDED(result)) return true;
    Log(std::wstring(operation) + L" failed (OpenXR " + std::to_wstring(result) + L").");
    return false;
}

float WrapRadians(float value) {
    constexpr float pi = 3.14159265358979323846f;
    constexpr float twoPi = pi * 2.0f;
    while (value > pi) value -= twoPi;
    while (value < -pi) value += twoPi;
    return value;
}

float YawFromOrientation(const XrQuaternionf& orientation) {
    const float sine = 2.0f * (orientation.w * orientation.y + orientation.z * orientation.x);
    const float cosine = 1.0f - 2.0f * (orientation.y * orientation.y + orientation.x * orientation.x);
    return std::atan2(sine, cosine);
}

float PitchFromOrientation(const XrQuaternionf& orientation) {
    const float sine = 2.0f * (orientation.w * orientation.x - orientation.y * orientation.z);
    return std::asin(sine < -1.0f ? -1.0f : (sine > 1.0f ? 1.0f : sine));
}

DXGI_FORMAT SelectColourFormat(const std::vector<int64_t>& formats) {
    constexpr std::array<DXGI_FORMAT, 4> preferred{
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    };
    for (const auto wanted : preferred) {
        for (const auto supported : formats) {
            if (static_cast<int64_t>(wanted) == supported) return wanted;
        }
    }
    return formats.empty() ? DXGI_FORMAT_UNKNOWN : static_cast<DXGI_FORMAT>(formats.front());
}

DXGI_FORMAT ConcreteRtvFormat(const DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    default: return format;
    }
}

DXGI_FORMAT ConcreteSrvFormat(const DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
    }
}
} // namespace

struct VrPresenter::State {
    struct Eye {
        XrSwapchain swapchain{XR_NULL_HANDLE};
        uint32_t width{};
        uint32_t height{};
        std::vector<XrSwapchainImageD3D11KHR> images;
        std::vector<ComPtr<ID3D11RenderTargetView>> renderTargets;
        // EASU writes the high-quality spatially upscaled image here. RCAS
        // then reads this texture and writes the sharpened result into the
        // OpenXR image, exactly as the two-pass FSR 1 reference pipeline.
        ComPtr<ID3D11Texture2D> fsrIntermediate;
        ComPtr<ID3D11RenderTargetView> fsrIntermediateTarget;
        ComPtr<ID3D11ShaderResourceView> fsrIntermediateView;
    };

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    // Frostbite/EA overlays can create additional, small DXGI swap chains.
    // Only the largest chain is the actual game canvas.  Presenting an
    // auxiliary chain is mostly harmless in mono, but is catastrophic for
    // alternate-frame stereo because it advances the left/right scheduler.
    ComPtr<IDXGISwapChain> gameSwapChain;
    uint64_t gameSwapChainLargestPixelCount{};
    std::vector<std::uintptr_t> ignoredSwapChains;
    std::array<ComPtr<ID3D11Texture2D>, 2> sourceCopies;
    std::array<ComPtr<ID3D11ShaderResourceView>, 2> sourceViews;
    std::array<bool, 2> sourceEyeValid{};
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11PixelShader> depthDebugPixelShader;
    ComPtr<ID3D11PixelShader> depthStereoPixelShader;
    ComPtr<ID3D11PixelShader> fsrEasuPixelShader;
    ComPtr<ID3D11PixelShader> fsrRcasPixelShader;
    ComPtr<ID3D11ShaderResourceView> depthDebugView;
    ID3D11Texture2D* depthDebugTexture{};
    // Candidate 1 is the full-size 1920x1080 depth surface verified in Heat.
    std::size_t depthDebugCandidate{1};
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11SamplerState> depthSampler;
    ComPtr<ID3D11Buffer> presentationLayoutBuffer;
    ComPtr<ID3D11Buffer> fsrEasuConstantsBuffer;
    ComPtr<ID3D11Buffer> fsrRcasConstantsBuffer;
    DXGI_FORMAT sourceFormat{DXGI_FORMAT_UNKNOWN};
    UINT sourceWidth{};
    UINT sourceHeight{};
    bool sourceIsHdr{};

    XrInstance instance{XR_NULL_HANDLE};
    XrSession session{XR_NULL_HANDLE};
    XrSpace space{XR_NULL_HANDLE};
    XrSessionState sessionState{XR_SESSION_STATE_UNKNOWN};
    bool sessionRunning{};
    bool firstProjectionFrameLogged{};
    bool headLookEnabled{};
    bool headLookHotkeyWasDown{};
    bool cullTransformSplitEnabled{true};
    bool cullTransformSplitHotkeyWasDown{};
    bool recenterHotkeyWasDown{};
    bool projectionRecenterHotkeyWasDown{};
    bool settingsHotkeyWasDown{};
    bool pipelineProbeHotkeyWasDown{};
    bool haveHeadLookBaseline{};
    bool haveProjectionBaseline{};
    bool monoPresentationLogged{};
    bool headsetFullscreenPresentationLogged{};
    bool fsr1PresentationLogged{};
    bool presentDepthProbeLogged{};
    bool presentResourceProbeLogged{};
    bool headLookInputLogged{};
    bool headPosePredictionLogged{};
    bool alternateFrameStereoActive{};
    bool alternateFrameStereoLogged{};
    bool alternateFrameStereoHotkeyWasDown{};
    bool depthDebugEnabled{};
    bool depthDebugHotkeyWasDown{};
    bool depthCandidateHotkeyWasDown{};
    // High-resolution Frostbite targets are retained only for diagnostics.
    // They include intermediate lighting, reflection and post-process passes,
    // not a single complete scene image, so normal presentation must keep the
    // final composited game frame.
    bool highQualityCaptureEnabled{};
    bool highQualityCaptureHotkeyWasDown{};
    bool highQualityCaptureDumpRequested{};
    bool highQualityCaptureDumpHotkeyWasDown{};
    bool highQualityCaptureInspectionWaitingLogged{};
    std::size_t highQualityCaptureDumpNextCandidate{};
    bool highQualityCaptureLogged{};
    bool highQualityCaptureWaitingLogged{};
    std::size_t highQualityCaptureCandidate{};
    ComPtr<ID3D11Texture2D> highQualityCaptureTexture;
    ColourRenderTargetCandidateInfo highQualityCaptureInfo{};
    UINT highQualityCaptureBackBufferWidth{};
    UINT highQualityCaptureBackBufferHeight{};
    bool latestDrawnHighPassEnabled{};
    bool latestDrawnHighPassInspectionRequested{};
    bool latestDrawnHighPassWaitingLogged{};
    bool latestDrawnHighPassLogged{};
    float lastHeadYaw{};
    float lastHeadPitch{};
    float matchedHeadsetEngineFovDegrees{};
    uint32_t gameRenderEye{};
    bool auxiliaryBackBufferLogged{};
    XrPosef projectionPose{};
    XrFovf projectionFov{};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    VrSettings settings{};
    ULONGLONG nextSettingsPollTime{};
    std::vector<XrView> views;
    std::vector<XrView> headTrackingViews;
    std::vector<Eye> eyes;
};

namespace {
uint64_t SwapChainPixelCount(IDXGISwapChain* swapChain) {
    if (swapChain == nullptr) return 0;
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swapChain->GetDesc(&description))) return 0;
    return static_cast<uint64_t>(description.BufferDesc.Width) * description.BufferDesc.Height;
}

bool SelectGameSwapChain(VrPresenter::State& state, IDXGISwapChain* swapChain) {
    if (swapChain == nullptr) return false;
    const uint64_t pixelCount = SwapChainPixelCount(swapChain);
    if (state.gameSwapChain.Get() == swapChain) {
        state.gameSwapChainLargestPixelCount = (std::max)(state.gameSwapChainLargestPixelCount, pixelCount);
        return true;
    }

    // A larger canvas is the legitimate game swap chain after a fullscreen
    // transition. Promote it and discard the old left/right history rather
    // than combining views from two unrelated swap chains.
    if (!state.gameSwapChain || pixelCount > state.gameSwapChainLargestPixelCount) {
        state.gameSwapChain = swapChain;
        state.gameSwapChainLargestPixelCount = pixelCount;
        state.sourceEyeValid.fill(false);
        state.gameRenderEye = 0;
        Log(L"Game swap chain promoted: " + std::to_wstring(reinterpret_cast<std::uintptr_t>(swapChain)) +
            L", pixels=" + std::to_wstring(pixelCount) + L". Auxiliary chains will be ignored.");
        return true;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(swapChain);
    if (std::find(state.ignoredSwapChains.begin(), state.ignoredSwapChains.end(), address) == state.ignoredSwapChains.end()) {
        state.ignoredSwapChains.push_back(address);
        Log(L"Ignoring auxiliary swap chain " + std::to_wstring(address) + L", pixels=" +
            std::to_wstring(pixelCount) + L" (game canvas has " +
            std::to_wstring(state.gameSwapChainLargestPixelCount) + L").");
    }
    return false;
}

XrFovf ComfortableMonoFov(const VrPresenter::State::Eye& eye, const int horizontalFovDegrees) {
    // The slider names the horizontal FOV of the actual game screen. The
    // surrounding black bars get an eye-texture FOV with matching aspect.
    const float eyeAspect = static_cast<float>(eye.width) / static_cast<float>(eye.height);
    constexpr float kRadiansPerDegree = 0.01745329251994329577f;
    const float halfHorizontal = static_cast<float>(horizontalFovDegrees) * kRadiansPerDegree * 0.5f;
    const float halfVertical = std::atan(std::tan(halfHorizontal) / eyeAspect);
    return {-halfHorizontal, halfHorizontal, halfVertical, -halfVertical};
}

float ScaleFovAngle(const float angle, const int percentage) {
    // Scale tangent-space extent rather than the raw angle.  This preserves
    // the projection's shape at wide Pimax angles and avoids a linear-angle
    // exaggeration near the lens edge.
    const float scale = static_cast<float>(percentage) / 100.0f;
    return std::atan(std::tan(angle) * scale);
}

float DepthStereoShiftForStrength(const int percentage) {
    // Screen-space stereo has no information behind foreground objects. A
    // linear high settings used to reach a 3%+ UV shift and exposed those
    // missing pixels as long "cracked glass" lines. Preserve the useful
    // 0..200 range, then approach a safe ceiling smoothly through 600.
    const float units = std::clamp(static_cast<float>(percentage) / 100.0f, 0.0f, 6.0f);
    const float linearRange = 0.0075f * (std::min)(units, 2.0f);
    const float compressedTail = 0.0030f * (1.0f - std::exp(-(std::max)(units - 2.0f, 0.0f)));
    return linearRange + compressedTail;
}

XrFovf SharedHeadsetFov(const std::vector<XrView>& views, const int horizontalPercent, const int verticalPercent) {
    // A single game image has no eye separation. Give both eyes one common
    // union FOV so the wide outer region is covered while every source pixel
    // maps to the same head-relative ray in each eye.
    XrFovf shared = views.front().fov;
    for (std::size_t index = 1; index < views.size(); ++index) {
        const XrFovf& fov = views[index].fov;
        shared.angleLeft = (std::min)(shared.angleLeft, fov.angleLeft);
        shared.angleRight = (std::max)(shared.angleRight, fov.angleRight);
        shared.angleDown = (std::min)(shared.angleDown, fov.angleDown);
        shared.angleUp = (std::max)(shared.angleUp, fov.angleUp);
    }
    shared.angleLeft = ScaleFovAngle(shared.angleLeft, horizontalPercent);
    shared.angleRight = ScaleFovAngle(shared.angleRight, horizontalPercent);
    shared.angleDown = ScaleFovAngle(shared.angleDown, verticalPercent);
    shared.angleUp = ScaleFovAngle(shared.angleUp, verticalPercent);
    return shared;
}

void ApplyHmdMatchedEngineFov(VrPresenter::State& state, const XrFovf& headsetFov) {
    if (state.settings.engineFovOverrideEnabled == 0 || state.settings.matchEngineFovToHeadset == 0) return;
    constexpr float kDegreesPerRadian = 57.295779513082320876f;
    const float verticalDegrees = std::clamp((headsetFov.angleUp - headsetFov.angleDown) * kDegreesPerRadian,
                                             60.0f, 150.0f);
    if (std::fabs(verticalDegrees - state.matchedHeadsetEngineFovDegrees) < 0.25f) return;
    SetFrostbiteEngineFovOverride(true, verticalDegrees);
    state.matchedHeadsetEngineFovDegrees = verticalDegrees;
    Log(L"Engine FOV matched to the active OpenXR headset view: " +
        std::to_wstring(static_cast<int>(std::lround(verticalDegrees))) + L" degrees.");
}

void ApplyCameraSettings(const VrSettings& settings) {
    SetFrostbiteHeadTrackingTuning(static_cast<float>(settings.headRotationGainPercent) / 100.0f,
                                   static_cast<float>(settings.headTranslationPercent) / 100.0f);
    SetFrostbiteCameraOffset(static_cast<float>(settings.cameraOffsetRightCentimetres) / 100.0f,
                             static_cast<float>(settings.cameraOffsetUpCentimetres) / 100.0f,
                             static_cast<float>(settings.cameraOffsetForwardCentimetres) / 100.0f);
    SetFrostbiteCullTransformSplitEnabled(settings.cullTransformSplitEnabled != 0);
    SetFrostbiteEngineFovOverride(settings.engineFovOverrideEnabled != 0 && settings.matchEngineFovToHeadset == 0,
                                  static_cast<float>(settings.engineVerticalFovDegrees));
}

void RefreshRuntimeSettings(VrPresenter::State& state) {
    const ULONGLONG now = GetTickCount64();
    if (now < state.nextSettingsPollTime) return;
    state.nextSettingsPollTime = now + 250;
    const VrSettings updated = LoadVrSettings();
    if (updated == state.settings) return;

    const bool fovChanged = updated.screenFovDegrees != state.settings.screenFovDegrees;
    const bool verticalFovChanged = updated.verticalFovPercent != state.settings.verticalFovPercent;
    const bool headsetFullscreenFovChanged =
        updated.headsetFullscreenHorizontalFovPercent != state.settings.headsetFullscreenHorizontalFovPercent ||
        updated.headsetFullscreenVerticalFovPercent != state.settings.headsetFullscreenVerticalFovPercent;
    const bool cullSplitChanged = updated.cullTransformSplitEnabled != state.settings.cullTransformSplitEnabled;
    const bool headsetFullscreenChanged = updated.headsetFullscreenEnabled != state.settings.headsetFullscreenEnabled;
    const bool engineFovChanged = updated.engineFovOverrideEnabled != state.settings.engineFovOverrideEnabled ||
                                  updated.matchEngineFovToHeadset != state.settings.matchEngineFovToHeadset ||
                                  updated.engineVerticalFovDegrees != state.settings.engineVerticalFovDegrees;
    const bool depthStereoChanged = updated.depthStereoEnabled != state.settings.depthStereoEnabled ||
                                    updated.depthStereoStrengthPercent != state.settings.depthStereoStrengthPercent;
    const bool headsetUpscalerChanged = updated.headsetUpscalerSharpnessPercent !=
                                        state.settings.headsetUpscalerSharpnessPercent;
    const bool latestHighPassChanged = updated.highResolutionLatestPassEnabled !=
                                       state.settings.highResolutionLatestPassEnabled;
    const bool latestHighPassInspectionChanged = updated.highResolutionLatestPassInspectionEnabled !=
                                                 state.settings.highResolutionLatestPassInspectionEnabled;
    const bool alternateFrameStereoChanged = updated.alternateFrameStereoEnabled != state.settings.alternateFrameStereoEnabled;
    const bool alternateFrameIpdChanged = updated.alternateFrameStereoIpdPercent != state.settings.alternateFrameStereoIpdPercent;
    const bool panelRecenterRequested = updated.recenterRequestId != state.settings.recenterRequestId;
    const bool cameraTuningChanged = updated.headRotationGainPercent != state.settings.headRotationGainPercent ||
                                     updated.headTranslationPercent != state.settings.headTranslationPercent ||
                                     updated.cameraOffsetRightCentimetres != state.settings.cameraOffsetRightCentimetres ||
                                     updated.cameraOffsetUpCentimetres != state.settings.cameraOffsetUpCentimetres ||
                                     updated.cameraOffsetForwardCentimetres != state.settings.cameraOffsetForwardCentimetres ||
                                     cullSplitChanged;
    state.settings = updated;
    if (latestHighPassChanged) {
        state.latestDrawnHighPassEnabled = updated.highResolutionLatestPassEnabled != 0;
        state.latestDrawnHighPassLogged = false;
        Log(state.latestDrawnHighPassEnabled
                ? L"Live high-resolution Frostbite pass enabled."
                : L"Live high-resolution Frostbite pass disabled; using the stable final game frame.");
    }
    if (latestHighPassInspectionChanged && updated.highResolutionLatestPassInspectionEnabled != 0) {
        state.latestDrawnHighPassInspectionRequested = true;
        Log(L"Live high-resolution Frostbite pass inspection requested from settings.");
    }
    if (cullSplitChanged) state.cullTransformSplitEnabled = updated.cullTransformSplitEnabled != 0;
    ApplyCameraSettings(updated);
    if (headsetFullscreenChanged) {
        state.monoPresentationLogged = false;
        state.headsetFullscreenPresentationLogged = false;
        Log(updated.headsetFullscreenEnabled != 0
                ? L"Headset-fullscreen presentation enabled: native headset FOV, head-locked image."
                : L"Virtual-screen presentation enabled: F10 recentres the screen.");
    }
    if (headsetFullscreenFovChanged && updated.headsetFullscreenEnabled != 0) {
        Log(L"Headset-fullscreen FOV updated: horizontal " +
            std::to_wstring(updated.headsetFullscreenHorizontalFovPercent) + L"%, vertical " +
            std::to_wstring(updated.headsetFullscreenVerticalFovPercent) + L"%.");
    }
    if (engineFovChanged) {
        state.matchedHeadsetEngineFovDegrees = 0.0f;
        Log(updated.engineFovOverrideEnabled == 0
                ? L"Real Frostbite FOV override disabled; using the game's normal camera projection."
                : (updated.matchEngineFovToHeadset != 0
                    ? L"Real Frostbite FOV will now match the active OpenXR headset view."
                    : L"Real Frostbite vertical FOV override updated to " +
                      std::to_wstring(updated.engineVerticalFovDegrees) + L" degrees."));
    }
    if (fovChanged && updated.headsetFullscreenEnabled == 0 && state.haveProjectionBaseline && !state.eyes.empty()) {
        state.projectionFov = ComfortableMonoFov(state.eyes.front(), updated.screenFovDegrees);
        Log(L"Mono screen FOV updated live to " + std::to_wstring(updated.screenFovDegrees) + L" degrees.");
    }
    if (verticalFovChanged && updated.headsetFullscreenEnabled == 0) {
        Log(L"Vertical mono framing updated live to " + std::to_wstring(updated.verticalFovPercent) +
            L"% (side crop preserves proportions).");
    }
    if ((alternateFrameStereoChanged || alternateFrameIpdChanged) && state.alternateFrameStereoActive) {
        // Retire the old alternating-frame experiment completely.  It can
        // leave a stale eye selected at startup even when the current panel
        // is configured for depth stereo, which produces an intermittent
        // black headset image.  Depth reconstruction remains the only
        // stereo path and the normal mono presenter stays its safe fallback.
        state.alternateFrameStereoActive = false;
        state.settings.alternateFrameStereoEnabled = 0;
        state.gameRenderEye = 0;
        state.sourceEyeValid.fill(false);
        state.alternateFrameStereoLogged = false;
        SetFrostbiteStereoEyeOffset(0.0f);
        Log(L"Legacy alternate-frame stereo ignored; presenting the stable mono/depth image.");
    }
    if (depthStereoChanged) {
        Log(updated.depthStereoEnabled != 0
                ? L"Stable depth-reconstructed stereo enabled. Both eyes are derived from the same Frostbite frame."
                : L"Depth-reconstructed stereo disabled; presenting the safe mono image.");
    }
    if (headsetUpscalerChanged) {
        state.fsr1PresentationLogged = false;
        Log(updated.headsetUpscalerSharpnessPercent == 0
                ? L"Headset upscaler disabled; using the unfiltered game image."
                : L"Headset upscaler updated: adaptive sharpness " +
                  std::to_wstring(updated.headsetUpscalerSharpnessPercent) + L"%.");
    }
    if (panelRecenterRequested) {
        state.haveHeadLookBaseline = false;
        ClearHeadLookRawDeltas();
        ClearHeadLookGamepadStick();
        Log(L"Direct head tracking recentered from the settings panel.");
    }
    if (cameraTuningChanged) {
        Log(L"Camera tuning updated: rotation " + std::to_wstring(updated.headRotationGainPercent) +
            L"%, translation " + std::to_wstring(updated.headTranslationPercent) + L"%, offset R/U/F " +
            std::to_wstring(updated.cameraOffsetRightCentimetres) + L"/" +
            std::to_wstring(updated.cameraOffsetUpCentimetres) + L"/" +
            std::to_wstring(updated.cameraOffsetForwardCentimetres) + L" cm, vehicle visibility " +
            (updated.cullTransformSplitEnabled != 0 ? L"on." : L"off."));
    }
}

void UpdateSettingsPanelHotkey(VrPresenter::State& state) {
    // Pimax's runtime service crashed when the bare Insert scan code was
    // delivered during an active OpenXR session. Use Home instead: it is not
    // claimed by the runtime and still permits a live external settings panel.
    const bool settingsDown = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
    if (settingsDown && !state.settingsHotkeyWasDown) {
        HWND existingPanel = FindWindowW(L"NFSHeatVRControlWindow", nullptr);
        if (existingPanel != nullptr) {
            ShowWindow(existingPanel, SW_RESTORE);
            SetForegroundWindow(existingPanel);
        } else {
            const auto controlPath = VrSettingsPath().parent_path() / L"NFSHeatVRControl.exe";
            if (std::filesystem::is_regular_file(controlPath)) {
                STARTUPINFOW startup{};
                startup.cb = sizeof(startup);
                startup.dwFlags = STARTF_USESHOWWINDOW;
                startup.wShowWindow = SW_SHOWNOACTIVATE;
                PROCESS_INFORMATION process{};
                std::wstring command = L"\"" + controlPath.wstring() + L"\"";
                if (CreateProcessW(controlPath.c_str(), command.data(), nullptr, nullptr, FALSE,
                                   CREATE_NEW_PROCESS_GROUP, nullptr, controlPath.parent_path().c_str(),
                                   &startup, &process) == FALSE) {
                    Log(L"Could not open NFSHeatVRControl.exe from the Home shortcut (Win32 error " +
                        std::to_wstring(GetLastError()) + L").");
                } else {
                    CloseHandle(process.hThread);
                    CloseHandle(process.hProcess);
                    Log(L"Settings panel started from the Home shortcut without changing VR focus.");
                }
            } else {
                Log(L"Home shortcut could not find NFSHeatVRControl.exe beside the runtime.");
            }
        }
    }
    state.settingsHotkeyWasDown = settingsDown;
}

void DestroyOpenXr(VrPresenter::State& state) {
    SetFrostbiteStereoEyeOffset(0.0f);
    if (state.sessionRunning) xrEndSession(state.session);
    if (state.space != XR_NULL_HANDLE) xrDestroySpace(state.space);
    for (auto& eye : state.eyes) {
        if (eye.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(eye.swapchain);
    }
    if (state.session != XR_NULL_HANDLE) xrDestroySession(state.session);
    if (state.instance != XR_NULL_HANDLE) xrDestroyInstance(state.instance);
}

bool CreateShaderPipeline(VrPresenter::State& state) {
    ComPtr<ID3DBlob> vertex;
    ComPtr<ID3DBlob> pixel;
    ComPtr<ID3DBlob> depthDebugPixel;
    ComPtr<ID3DBlob> depthStereoPixel;
    ComPtr<ID3DBlob> fsrEasuPixel;
    ComPtr<ID3DBlob> fsrRcasPixel;
    ComPtr<ID3DBlob> errors;
    HRESULT result = D3DCompile(kVertexShader, std::strlen(kVertexShader), "NFSHeatVR-vertex", nullptr, nullptr,
                               "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertex, &errors);
    if (FAILED(result)) {
        Log(L"Could not compile the VR vertex shader.");
        return false;
    }
    result = D3DCompile(kPixelShader, std::strlen(kPixelShader), "NFSHeatVR-pixel", nullptr, nullptr,
                        "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixel, &errors);
    if (FAILED(result)) {
        Log(L"Could not compile the VR pixel shader.");
        return false;
    }
    result = D3DCompile(kDepthDebugPixelShader, std::strlen(kDepthDebugPixelShader), "NFSHeatVR-depth-debug", nullptr, nullptr,
                        "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &depthDebugPixel, &errors);
    if (FAILED(result)) {
        Log(L"Could not compile the stereo depth debug shader.");
        return false;
    }
    result = D3DCompile(kDepthStereoPixelShader, std::strlen(kDepthStereoPixelShader), "NFSHeatVR-depth-stereo", nullptr, nullptr,
                        "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &depthStereoPixel, &errors);
    if (FAILED(result)) {
        Log(L"Could not compile the experimental depth stereo shader.");
        return false;
    }
    result = D3DCompile(kFsrEasuPixelShader, std::strlen(kFsrEasuPixelShader), "NFSHeatVR-fsr-easu", nullptr, nullptr,
                        "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &fsrEasuPixel, &errors);
    if (FAILED(result)) {
        Log(L"Could not compile the FSR 1 EASU shader.");
        return false;
    }
    result = D3DCompile(kFsrRcasPixelShader, std::strlen(kFsrRcasPixelShader), "NFSHeatVR-fsr-rcas", nullptr, nullptr,
                        "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &fsrRcasPixel, &errors);
    if (FAILED(result)) {
        Log(L"Could not compile the FSR 1 RCAS shader.");
        return false;
    }
    if (FAILED(state.device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &state.vertexShader)) ||
        FAILED(state.device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &state.pixelShader)) ||
        FAILED(state.device->CreatePixelShader(depthDebugPixel->GetBufferPointer(), depthDebugPixel->GetBufferSize(), nullptr,
                                               &state.depthDebugPixelShader)) ||
        FAILED(state.device->CreatePixelShader(depthStereoPixel->GetBufferPointer(), depthStereoPixel->GetBufferSize(), nullptr,
                                               &state.depthStereoPixelShader)) ||
        FAILED(state.device->CreatePixelShader(fsrEasuPixel->GetBufferPointer(), fsrEasuPixel->GetBufferSize(), nullptr,
                                               &state.fsrEasuPixelShader)) ||
        FAILED(state.device->CreatePixelShader(fsrRcasPixel->GetBufferPointer(), fsrRcasPixel->GetBufferSize(), nullptr,
                                               &state.fsrRcasPixelShader))) {
        Log(L"Could not create the VR shader pipeline.");
        return false;
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(state.device->CreateSamplerState(&sampler, &state.sampler))) {
        Log(L"Could not create the VR sampler.");
        return false;
    }
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(state.device->CreateSamplerState(&sampler, &state.depthSampler))) {
        Log(L"Could not create the stereo depth sampler.");
        return false;
    }
    D3D11_BUFFER_DESC layoutBuffer{};
    layoutBuffer.ByteWidth = sizeof(float) * 16;
    layoutBuffer.Usage = D3D11_USAGE_DEFAULT;
    layoutBuffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(state.device->CreateBuffer(&layoutBuffer, nullptr, &state.presentationLayoutBuffer))) {
        Log(L"Could not create the VR presentation-layout buffer.");
        return false;
    }
    layoutBuffer.ByteWidth = sizeof(float) * 20;
    if (FAILED(state.device->CreateBuffer(&layoutBuffer, nullptr, &state.fsrEasuConstantsBuffer))) {
        Log(L"Could not create the FSR 1 EASU constants buffer.");
        return false;
    }
    layoutBuffer.ByteWidth = sizeof(float) * 4;
    if (FAILED(state.device->CreateBuffer(&layoutBuffer, nullptr, &state.fsrRcasConstantsBuffer))) {
        Log(L"Could not create the FSR 1 RCAS constants buffer.");
        return false;
    }
    return true;
}

bool CreateEye(VrPresenter::State& state, const XrViewConfigurationView& view, const DXGI_FORMAT format) {
    VrPresenter::State::Eye eye{};
    eye.width = view.recommendedImageRectWidth;
    eye.height = view.recommendedImageRectHeight;
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    createInfo.format = format;
    createInfo.sampleCount = 1;
    createInfo.width = eye.width;
    createInfo.height = eye.height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    if (!Succeeded(xrCreateSwapchain(state.session, &createInfo, &eye.swapchain), L"xrCreateSwapchain")) return false;

    uint32_t imageCount = 0;
    if (!Succeeded(xrEnumerateSwapchainImages(eye.swapchain, 0, &imageCount, nullptr), L"xrEnumerateSwapchainImages(count)")) return false;
    eye.images.resize(imageCount);
    eye.renderTargets.resize(imageCount);
    for (auto& image : eye.images) image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    if (!Succeeded(xrEnumerateSwapchainImages(eye.swapchain, imageCount, &imageCount,
                                               reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data())),
                   L"xrEnumerateSwapchainImages(images)")) return false;
    for (uint32_t index = 0; index < imageCount; ++index) {
        if (eye.images[index].texture == nullptr) {
            Log(L"OpenXR returned a null D3D11 eye texture.");
            return false;
        }
        D3D11_TEXTURE2D_DESC textureDescription{};
        eye.images[index].texture->GetDesc(&textureDescription);
        D3D11_RENDER_TARGET_VIEW_DESC viewDescription{};
        viewDescription.Format = ConcreteRtvFormat(textureDescription.Format);
        viewDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        viewDescription.Texture2D.MipSlice = 0;
        const HRESULT result = state.device->CreateRenderTargetView(eye.images[index].texture, &viewDescription, &eye.renderTargets[index]);
        if (FAILED(result)) {
            Log(L"OpenXR eye RTV failed: HRESULT=" + std::to_wstring(static_cast<unsigned long>(result)) +
                L", resource-format=" + std::to_wstring(static_cast<int>(textureDescription.Format)) +
                L", bind-flags=" + std::to_wstring(textureDescription.BindFlags));
            return false;
        }
    }
    D3D11_TEXTURE2D_DESC intermediateDescription{};
    intermediateDescription.Width = eye.width;
    intermediateDescription.Height = eye.height;
    intermediateDescription.MipLevels = 1;
    intermediateDescription.ArraySize = 1;
    intermediateDescription.Format = ConcreteRtvFormat(format);
    intermediateDescription.SampleDesc.Count = 1;
    intermediateDescription.Usage = D3D11_USAGE_DEFAULT;
    intermediateDescription.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(state.device->CreateTexture2D(&intermediateDescription, nullptr, &eye.fsrIntermediate))) {
        Log(L"Could not allocate the FSR 1 intermediate eye texture.");
        return false;
    }
    D3D11_RENDER_TARGET_VIEW_DESC intermediateTargetDescription{};
    intermediateTargetDescription.Format = ConcreteRtvFormat(intermediateDescription.Format);
    intermediateTargetDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(state.device->CreateRenderTargetView(eye.fsrIntermediate.Get(), &intermediateTargetDescription,
                                                     &eye.fsrIntermediateTarget))) {
        Log(L"Could not create the FSR 1 intermediate eye target.");
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC intermediateViewDescription{};
    intermediateViewDescription.Format = ConcreteSrvFormat(intermediateDescription.Format);
    intermediateViewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    intermediateViewDescription.Texture2D.MostDetailedMip = 0;
    intermediateViewDescription.Texture2D.MipLevels = 1;
    if (FAILED(state.device->CreateShaderResourceView(eye.fsrIntermediate.Get(), &intermediateViewDescription,
                                                       &eye.fsrIntermediateView))) {
        Log(L"Could not create the FSR 1 intermediate eye view.");
        return false;
    }
    state.eyes.emplace_back(std::move(eye));
    return true;
}

bool CreateCopyResources(VrPresenter::State& state, ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    if (description.SampleDesc.Count != 1 || description.Format == DXGI_FORMAT_UNKNOWN) {
        Log(L"Unsupported game back-buffer format or multisampling.");
        return false;
    }
    if (state.sourceCopies[0] && state.sourceFormat == description.Format &&
        state.sourceWidth == description.Width && state.sourceHeight == description.Height) return true;

    const uint32_t previousWidth = state.sourceWidth;
    const uint32_t previousHeight = state.sourceHeight;
    const DXGI_FORMAT previousFormat = state.sourceFormat;

    for (auto& copy : state.sourceCopies) copy.Reset();
    for (auto& view : state.sourceViews) view.Reset();
    state.sourceEyeValid.fill(false);
    state.fsr1PresentationLogged = false;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    description.CPUAccessFlags = 0;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.MiscFlags = 0;
    for (std::size_t eyeIndex = 0; eyeIndex < state.sourceCopies.size(); ++eyeIndex) {
        const HRESULT createTextureResult = state.device->CreateTexture2D(&description, nullptr, &state.sourceCopies[eyeIndex]);
        if (FAILED(createTextureResult)) {
            Log(L"Could not allocate an eye copy of the game source (HRESULT " +
                std::to_wstring(static_cast<uint32_t>(createTextureResult)) + L").");
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        viewDescription.Format = ConcreteSrvFormat(description.Format);
        viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDescription.Texture2D.MostDetailedMip = 0;
        viewDescription.Texture2D.MipLevels = description.MipLevels == 0 ? UINT(-1) : description.MipLevels;
        const HRESULT createViewResult = state.device->CreateShaderResourceView(state.sourceCopies[eyeIndex].Get(), &viewDescription,
                                                                                  &state.sourceViews[eyeIndex]);
        if (FAILED(createViewResult)) {
            Log(L"Could not create a shader view for an eye copy of the game source (format " +
                std::to_wstring(static_cast<int>(description.Format)) + L", HRESULT " +
                std::to_wstring(static_cast<uint32_t>(createViewResult)) + L").");
            return false;
        }
    }
    state.sourceFormat = description.Format;
    state.sourceWidth = description.Width;
    state.sourceHeight = description.Height;
    const DXGI_FORMAT readableSourceFormat = ConcreteSrvFormat(description.Format);
    state.sourceIsHdr = readableSourceFormat == DXGI_FORMAT_R11G11B10_FLOAT ||
                        readableSourceFormat == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (previousWidth == 0 || previousHeight == 0) {
        Log(L"Game capture source detected: " + std::to_wstring(description.Width) + L"x" +
            std::to_wstring(description.Height) + L", format=" + std::to_wstring(static_cast<int>(description.Format)) + L".");
    } else {
        Log(L"Game capture source changed: " + std::to_wstring(previousWidth) + L"x" +
            std::to_wstring(previousHeight) + L" (format " + std::to_wstring(static_cast<int>(previousFormat)) +
            L") -> " + std::to_wstring(description.Width) + L"x" + std::to_wstring(description.Height) +
            L" (format " + std::to_wstring(static_cast<int>(description.Format)) + L").");
    }
    return true;
}

bool DumpHighQualitySourceForInspection(VrPresenter::State& state, ID3D11Texture2D* sourceTexture,
                                        const std::size_t candidateIndex) {
    if (sourceTexture == nullptr) return false;

    D3D11_TEXTURE2D_DESC sourceDescription{};
    sourceTexture->GetDesc(&sourceDescription);
    if (sourceDescription.SampleDesc.Count != 1 || sourceDescription.Width == 0 || sourceDescription.Height == 0) {
        Log(L"High-quality source screenshot skipped: multisampled or empty source.");
        return false;
    }

    const DXGI_FORMAT format = sourceDescription.Format;
    const bool r10 = format == DXGI_FORMAT_R10G10B10A2_TYPELESS || format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                     format == DXGI_FORMAT_R10G10B10A2_UINT;
    const bool rgba8 = format == DXGI_FORMAT_R8G8B8A8_TYPELESS || format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                       format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool bgra8 = format == DXGI_FORMAT_B8G8R8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                       format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool r11g11b10 = format == DXGI_FORMAT_R11G11B10_FLOAT;
    if (!r10 && !rgba8 && !bgra8 && !r11g11b10) {
        Log(L"High-quality source screenshot skipped: candidate " + std::to_wstring(candidateIndex) +
            L" has unsupported format " + std::to_wstring(static_cast<int>(format)) + L".");
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDescription = sourceDescription;
    stagingDescription.MipLevels = 1;
    stagingDescription.ArraySize = 1;
    stagingDescription.BindFlags = 0;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> stagingTexture;
    const HRESULT createResult = state.device->CreateTexture2D(&stagingDescription, nullptr, &stagingTexture);
    if (FAILED(createResult)) {
        Log(L"Could not allocate a staging texture for the high-quality-source screenshot (HRESULT " +
            std::to_wstring(static_cast<uint32_t>(createResult)) + L").");
        return false;
    }
    state.context->CopySubresourceRegion(stagingTexture.Get(), 0, 0, 0, 0, sourceTexture, 0, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapResult = state.context->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mapResult)) {
        Log(L"Could not read the high-quality-source screenshot (HRESULT " +
            std::to_wstring(static_cast<uint32_t>(mapResult)) + L").");
        return false;
    }

    const uint32_t rowBytes = sourceDescription.Width * 3u;
    const uint32_t paddedRowBytes = (rowBytes + 3u) & ~3u;
    const uint64_t pixelBytes = static_cast<uint64_t>(paddedRowBytes) * sourceDescription.Height;
    const uint64_t fileBytes = 54u + pixelBytes;
    if (fileBytes > UINT32_MAX) {
        state.context->Unmap(stagingTexture.Get(), 0);
        Log(L"High-quality source screenshot skipped: image is too large for BMP.");
        return false;
    }

    const std::filesystem::path outputPath = VrSettingsPath().parent_path() /
                                             (L"NFSHeatVR-high-source-" + std::to_wstring(candidateIndex) + L".bmp");
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        state.context->Unmap(stagingTexture.Get(), 0);
        Log(L"Could not create the high-quality-source screenshot file.");
        return false;
    }

    std::array<uint8_t, 54> header{};
    const auto put16 = [&header](const std::size_t offset, const uint16_t value) {
        header[offset] = static_cast<uint8_t>(value & 0xffu);
        header[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
    };
    const auto put32 = [&header](const std::size_t offset, const uint32_t value) {
        header[offset] = static_cast<uint8_t>(value & 0xffu);
        header[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
        header[offset + 2] = static_cast<uint8_t>((value >> 16u) & 0xffu);
        header[offset + 3] = static_cast<uint8_t>((value >> 24u) & 0xffu);
    };
    header[0] = 'B';
    header[1] = 'M';
    put32(2, static_cast<uint32_t>(fileBytes));
    put32(10, 54);
    put32(14, 40);
    put32(18, sourceDescription.Width);
    put32(22, sourceDescription.Height);
    put16(26, 1);
    put16(28, 24);
    put32(34, static_cast<uint32_t>(pixelBytes));
    output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));

    const auto decodeUnsignedFloat = [](const uint32_t packed, const uint32_t mantissaBits) {
        const uint32_t mantissaMask = (1u << mantissaBits) - 1u;
        const uint32_t exponent = packed >> mantissaBits;
        const uint32_t mantissa = packed & mantissaMask;
        if (exponent == 0) {
            return std::ldexp(static_cast<float>(mantissa), -14 - static_cast<int>(mantissaBits));
        }
        if (exponent == 31) return 65504.0f;
        return std::ldexp(1.0f + static_cast<float>(mantissa) / static_cast<float>(1u << mantissaBits),
                          static_cast<int>(exponent) - 15);
    };
    const auto displayEncodeHdr = [](const float colour) {
        const float nonNegative = std::max(colour, 0.0f);
        const float mapped = (nonNegative * (2.51f * nonNegative + 0.03f)) /
                             (nonNegative * (2.43f * nonNegative + 0.59f) + 0.14f);
        return static_cast<uint8_t>(std::lround(std::pow(std::clamp(mapped, 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f));
    };
    std::vector<uint8_t> outputRow(paddedRowBytes, 0);
    for (UINT y = 0; y < sourceDescription.Height; ++y) {
        const auto* sourceRow = static_cast<const uint8_t*>(mapped.pData) +
                                static_cast<std::size_t>(sourceDescription.Height - 1u - y) * mapped.RowPitch;
        for (UINT x = 0; x < sourceDescription.Width; ++x) {
            const auto* sourcePixel = sourceRow + static_cast<std::size_t>(x) * 4u;
            auto* outputPixel = outputRow.data() + static_cast<std::size_t>(x) * 3u;
            if (r10) {
                uint32_t packed{};
                std::memcpy(&packed, sourcePixel, sizeof(packed));
                outputPixel[0] = static_cast<uint8_t>(((packed >> 20u) & 0x3ffu) >> 2u);
                outputPixel[1] = static_cast<uint8_t>(((packed >> 10u) & 0x3ffu) >> 2u);
                outputPixel[2] = static_cast<uint8_t>((packed & 0x3ffu) >> 2u);
            } else if (rgba8) {
                outputPixel[0] = sourcePixel[2];
                outputPixel[1] = sourcePixel[1];
                outputPixel[2] = sourcePixel[0];
            } else if (bgra8) {
                outputPixel[0] = sourcePixel[0];
                outputPixel[1] = sourcePixel[1];
                outputPixel[2] = sourcePixel[2];
            } else {
                uint32_t packed{};
                std::memcpy(&packed, sourcePixel, sizeof(packed));
                const float red = decodeUnsignedFloat(packed & 0x7ffu, 6);
                const float green = decodeUnsignedFloat((packed >> 11u) & 0x7ffu, 6);
                const float blue = decodeUnsignedFloat((packed >> 22u) & 0x3ffu, 5);
                outputPixel[0] = displayEncodeHdr(blue);
                outputPixel[1] = displayEncodeHdr(green);
                outputPixel[2] = displayEncodeHdr(red);
            }
        }
        output.write(reinterpret_cast<const char*>(outputRow.data()), static_cast<std::streamsize>(outputRow.size()));
    }
    state.context->Unmap(stagingTexture.Get(), 0);
    output.close();
    if (!output) {
        Log(L"Could not finish writing the high-quality-source screenshot.");
        return false;
    }
    Log(L"High-quality source screenshot saved: " + outputPath.wstring() + L" (candidate " +
        std::to_wstring(candidateIndex) + L", " + std::to_wstring(sourceDescription.Width) + L"x" +
        std::to_wstring(sourceDescription.Height) + L").");
    return true;
}

bool CaptureSourceFrame(VrPresenter::State& state, ID3D11Texture2D* sourceTexture) {
    if (sourceTexture == nullptr || !CreateCopyResources(state, sourceTexture)) return false;
    const uint32_t eyeIndex = state.alternateFrameStereoActive ? state.gameRenderEye : 0;
    state.context->CopyResource(state.sourceCopies[eyeIndex].Get(), sourceTexture);
    state.sourceEyeValid[eyeIndex] = true;
    // Until both real eye frames have arrived, present the available eye in
    // both slots rather than flashing a black image during the transition.
    const uint32_t otherEye = 1u - eyeIndex;
    if (state.alternateFrameStereoActive && !state.sourceEyeValid[otherEye]) {
        state.context->CopyResource(state.sourceCopies[otherEye].Get(), sourceTexture);
        state.sourceEyeValid[otherEye] = true;
    }
    return true;
}

bool IsAuxiliaryBackBuffer(const VrPresenter::State& state, ID3D11Texture2D* gameTexture) {
    if (gameTexture == nullptr || state.sourceWidth == 0 || state.sourceHeight == 0) return false;

    D3D11_TEXTURE2D_DESC description{};
    gameTexture->GetDesc(&description);
    if (description.Width == 0 || description.Height == 0) return true;

    const uint64_t previousPixels = static_cast<uint64_t>(state.sourceWidth) * state.sourceHeight;
    const uint64_t incomingPixels = static_cast<uint64_t>(description.Width) * description.Height;
    const float previousAspect = static_cast<float>(state.sourceWidth) / static_cast<float>(state.sourceHeight);
    const float incomingAspect = static_cast<float>(description.Width) / static_cast<float>(description.Height);

    // Heat's actual canvas is consistently ultrawide (2560x1080 or the
    // user's 5120x2160 custom mode).  The EA/Frostbite helper can present a
    // much smaller 1280x720 surface through the same hooked DXGI path.  Do
    // not let that helper replace an eye image or consume a stereo eye turn.
    const bool substantiallySmaller = incomingPixels * 2u < previousPixels;
    const bool aspectMismatch = std::fabs(incomingAspect - previousAspect) > 0.10f;
    return substantiallySmaller && aspectMismatch;
}

bool CaptureGameFrame(VrPresenter::State& state, ID3D11Texture2D* gameTexture) {
    if (gameTexture == nullptr) return false;
    if (IsAuxiliaryBackBuffer(state, gameTexture)) {
        if (!state.auxiliaryBackBufferLogged) {
            D3D11_TEXTURE2D_DESC description{};
            gameTexture->GetDesc(&description);
            Log(L"Ignoring auxiliary back buffer " + std::to_wstring(description.Width) + L"x" +
                std::to_wstring(description.Height) + L"; retaining the game canvas " +
                std::to_wstring(state.sourceWidth) + L"x" + std::to_wstring(state.sourceHeight) +
                L" and its current stereo eye.");
            state.auxiliaryBackBufferLogged = true;
        }
        return false;
    }
    state.auxiliaryBackBufferLogged = false;
    if (state.latestDrawnHighPassEnabled || state.latestDrawnHighPassInspectionRequested) {
        ComPtr<ID3D11Texture2D> latestHighPassTexture;
        ColourRenderTargetCandidateInfo latestHighPassInfo{};
        if (AcquireLatestDrawnHighColourRenderTarget(&latestHighPassTexture, latestHighPassInfo)) {
            state.latestDrawnHighPassWaitingLogged = false;
            if (state.latestDrawnHighPassInspectionRequested) {
                DumpHighQualitySourceForInspection(state, latestHighPassTexture.Get(), 100);
                state.latestDrawnHighPassInspectionRequested = false;
            }
            if (state.latestDrawnHighPassEnabled && CaptureSourceFrame(state, latestHighPassTexture.Get())) {
                if (!state.latestDrawnHighPassLogged) {
                    state.latestDrawnHighPassLogged = true;
                    Log(L"Latest Frostbite wide colour pass active: " + std::to_wstring(latestHighPassInfo.width) + L"x" +
                        std::to_wstring(latestHighPassInfo.height) + L", format=" +
                        std::to_wstring(static_cast<int>(latestHighPassInfo.format)) + L".");
                }
                return true;
            }
        } else if (!state.latestDrawnHighPassWaitingLogged) {
            state.latestDrawnHighPassWaitingLogged = true;
            Log(L"Waiting for Frostbite to draw the first high-resolution wide colour pass.");
        }
    }
    if (state.highQualityCaptureEnabled || state.highQualityCaptureDumpRequested) {
        D3D11_TEXTURE2D_DESC backBufferDescription{};
        gameTexture->GetDesc(&backBufferDescription);
        const std::size_t candidateCount = HighQualityColourRenderTargetCount(backBufferDescription);
        if (candidateCount == 0) {
            // This is expected until Frostbite has finished bringing up the
            // main renderer.  Do not turn the native path off merely because
            // an introductory frame arrived first.
            if (!state.highQualityCaptureWaitingLogged) {
                state.highQualityCaptureWaitingLogged = true;
                Log(L"Waiting for Frostbite's native high-resolution colour source; using the final frame temporarily.");
            }
        } else {
            state.highQualityCaptureWaitingLogged = false;
            if (state.highQualityCaptureDumpRequested) {
                // Frostbite creates the first high-resolution target while
                // clearing the loading screen.  It is legitimately black.
                // Wait until the later render passes have populated a stable
                // candidate set, then inspect it without replacing the HMD
                // source frame.
                constexpr std::size_t kCaptureSetSize = 6;
                if (candidateCount < kCaptureSetSize) {
                    if (!state.highQualityCaptureInspectionWaitingLogged) {
                        state.highQualityCaptureInspectionWaitingLogged = true;
                        Log(L"Waiting for a stable set of high-resolution Frostbite targets before inspection.");
                    }
                } else {
                    state.highQualityCaptureInspectionWaitingLogged = false;
                    if (state.highQualityCaptureDumpNextCandidate < kCaptureSetSize) {
                        ComPtr<ID3D11Texture2D> captureTexture;
                        ColourRenderTargetCandidateInfo captureInfo{};
                        const std::size_t captureIndex = state.highQualityCaptureDumpNextCandidate++;
                        if (AcquireHighQualityColourRenderTarget(backBufferDescription, captureIndex,
                                                                  &captureTexture, captureInfo)) {
                            DumpHighQualitySourceForInspection(state, captureTexture.Get(), captureIndex);
                        } else {
                            Log(L"Could not acquire high-quality source candidate " + std::to_wstring(captureIndex) +
                                L" for the diagnostic capture set.");
                        }
                    }
                    if (state.highQualityCaptureDumpNextCandidate >= kCaptureSetSize) {
                        state.highQualityCaptureDumpRequested = false;
                        Log(L"High-quality source capture set complete.");
                    }
                }
            }
            if (state.highQualityCaptureEnabled) {
                if (state.highQualityCaptureCandidate >= candidateCount) {
                    state.highQualityCaptureCandidate = 0;
                    state.highQualityCaptureLogged = false;
                    Log(L"High-quality Frostbite colour-source selection wrapped to candidate 0.");
                }
                ComPtr<ID3D11Texture2D> highQualityTexture;
                ColourRenderTargetCandidateInfo highQualityInfo{};
                if (AcquireHighQualityColourRenderTarget(backBufferDescription, state.highQualityCaptureCandidate,
                                                          &highQualityTexture, highQualityInfo) &&
                    CaptureSourceFrame(state, highQualityTexture.Get())) {
                    if (!state.highQualityCaptureLogged) {
                        state.highQualityCaptureLogged = true;
                        Log(L"High-quality Frostbite colour source active: candidate " +
                            std::to_wstring(state.highQualityCaptureCandidate) + L", " +
                            std::to_wstring(highQualityInfo.width) + L"x" + std::to_wstring(highQualityInfo.height) +
                            L", format=" + std::to_wstring(static_cast<int>(highQualityInfo.format)) + L".");
                    }
                    return true;
                }
                state.highQualityCaptureEnabled = false;
                state.highQualityCaptureLogged = false;
                Log(L"High-quality Frostbite colour source was incompatible; safely falling back to the final game frame.");
            }
        }
    }
    return CaptureSourceFrame(state, gameTexture);
}

void ScheduleAlternateFrameEye(VrPresenter::State& state) {
    if (!state.alternateFrameStereoActive || state.views.size() < 2) {
        state.gameRenderEye = 0;
        SetFrostbiteStereoEyeOffset(0.0f);
        return;
    }

    // The frame just captured was rendered for gameRenderEye. Configure the
    // opposite physical eye before Frostbite begins its next render frame.
    state.gameRenderEye = 1u - state.gameRenderEye;
    const XrVector3f& left = state.views[0].pose.position;
    const XrVector3f& right = state.views[1].pose.position;
    const float dx = right.x - left.x;
    const float dy = right.y - left.y;
    const float dz = right.z - left.z;
    const float runtimeIpd = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float ipd = runtimeIpd > 0.04f && runtimeIpd < 0.09f ? runtimeIpd : 0.064f;
    const float halfSeparation = ipd * 0.5f *
                                 (static_cast<float>(state.settings.alternateFrameStereoIpdPercent) / 100.0f);
    const float eyeOffset = state.gameRenderEye == 0 ? -halfSeparation : halfSeparation;
    SetFrostbiteStereoEyeOffset(eyeOffset);
    if (!state.alternateFrameStereoLogged) {
        Log(L"Alternate-frame stereo scheduling real Frostbite eyes at " +
            std::to_wstring(state.settings.alternateFrameStereoIpdPercent) + L"% of runtime IPD.");
        state.alternateFrameStereoLogged = true;
    }
}

void PollEvents(VrPresenter::State& state) {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(state.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            state.sessionState = changed->state;
            if (changed->state == XR_SESSION_STATE_READY && !state.sessionRunning) {
                const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                if (Succeeded(xrBeginSession(state.session, &begin), L"xrBeginSession")) state.sessionRunning = true;
            } else if (changed->state == XR_SESSION_STATE_STOPPING && state.sessionRunning) {
                xrEndSession(state.session);
                state.sessionRunning = false;
            }
        }
        event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void UpdateHeadLook(VrPresenter::State& state, const std::vector<XrView>& headTrackingViews) {
    // A modifier makes this safe from normal NFS Heat controls.  Unlike
    // Insert/Home it never opens a separate Win32 window, so exclusive
    // fullscreen does not lose focus or renegotiate its display mode.
    const bool alternateStereoDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
                                     (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
    if (alternateStereoDown && !state.alternateFrameStereoHotkeyWasDown) {
        state.settings.depthStereoEnabled = state.settings.depthStereoEnabled == 0 ? 1 : 0;
        // The former alternate-frame experiment cannot form a temporally
        // stable eye pair.  Ctrl+F3 deliberately selects the stable
        // depth-reconstruction path instead and retires that mode.
        state.alternateFrameStereoActive = false;
        state.settings.alternateFrameStereoEnabled = 0;
        state.gameRenderEye = 0;
        state.sourceEyeValid.fill(false);
        state.alternateFrameStereoLogged = false;
        SetFrostbiteStereoEyeOffset(0.0f);
        SaveVrSettings(state.settings);
        Log(state.settings.depthStereoEnabled != 0
                ? L"Stable depth-reconstructed stereo enabled directly (Ctrl+F3)."
                : L"Depth-reconstructed stereo disabled directly (Ctrl+F3); presenting the safe mono image.");
    }
    state.alternateFrameStereoHotkeyWasDown = alternateStereoDown;

    const bool cullSplitDown = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (cullSplitDown && !state.cullTransformSplitHotkeyWasDown) {
        state.cullTransformSplitEnabled = !state.cullTransformSplitEnabled;
        state.settings.cullTransformSplitEnabled = state.cullTransformSplitEnabled ? 1 : 0;
        SaveVrSettings(state.settings);
        SetFrostbiteCullTransformSplitEnabled(state.cullTransformSplitEnabled);
        Log(state.cullTransformSplitEnabled
                ? L"Quality-preserving vehicle visibility enabled (F7)."
                : L"Quality-preserving vehicle visibility disabled (F7)."
        );
    }
    state.cullTransformSplitHotkeyWasDown = cullSplitDown;

    const bool highQualityCaptureDown = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (highQualityCaptureDown && !state.highQualityCaptureHotkeyWasDown) {
        if (!state.highQualityCaptureEnabled) {
            state.highQualityCaptureEnabled = true;
            state.highQualityCaptureCandidate = 0;
            state.highQualityCaptureLogged = false;
            state.highQualityCaptureDumpRequested = false;
            state.highQualityCaptureTexture.Reset();
            Log(L"High-quality Frostbite colour-source trial enabled (F5): candidate 0 will be used when available.");
        } else {
            ++state.highQualityCaptureCandidate;
            state.highQualityCaptureLogged = false;
            state.highQualityCaptureDumpRequested = false;
            state.highQualityCaptureTexture.Reset();
            Log(L"High-quality Frostbite colour-source trial advanced to the next candidate (F5).");
        }
    }
    state.highQualityCaptureHotkeyWasDown = highQualityCaptureDown;

    const bool highQualityCaptureDumpDown = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
    if (highQualityCaptureDumpDown && !state.highQualityCaptureDumpHotkeyWasDown) {
        if (state.highQualityCaptureEnabled) {
            state.highQualityCaptureDumpRequested = true;
            state.highQualityCaptureDumpNextCandidate = 0;
            Log(L"High-quality source capture set requested (F4): the first six candidates will be saved one per frame.");
        } else {
            Log(L"High-quality source screenshot requires F5 to be enabled first.");
        }
    }
    state.highQualityCaptureDumpHotkeyWasDown = highQualityCaptureDumpDown;

    const bool depthDebugDown = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (depthDebugDown && !state.depthDebugHotkeyWasDown) {
        state.depthDebugEnabled = !state.depthDebugEnabled;
        Log(state.depthDebugEnabled
                ? L"Stereo depth preview enabled (F11). F12 selects another depth candidate."
                : L"Stereo depth preview disabled (F11).");
    }
    state.depthDebugHotkeyWasDown = depthDebugDown;

    const bool depthCandidateDown = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (depthCandidateDown && !state.depthCandidateHotkeyWasDown) {
        const std::size_t candidateCount = StereoDepthCandidateCount();
        if (candidateCount == 0) {
            Log(L"Stereo depth preview: no shader-readable depth candidate is available yet.");
        } else {
            state.depthDebugCandidate = (state.depthDebugCandidate + 1) % candidateCount;
            state.depthDebugView.Reset();
            state.depthDebugTexture = nullptr;
            Log(L"Stereo depth preview selected candidate " + std::to_wstring(state.depthDebugCandidate) + L" (F12).");
        }
    }
    state.depthCandidateHotkeyWasDown = depthCandidateDown;

    const bool toggleDown = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (toggleDown && !state.headLookHotkeyWasDown) {
        state.headLookEnabled = !state.headLookEnabled;
        state.haveHeadLookBaseline = false;
        ClearHeadLookRawDeltas();
        ClearHeadLookGamepadStick();
        SetFrostbiteHeadTrackingEnabled(state.headLookEnabled);
        Log(state.headLookEnabled ? L"Direct Frostbite head tracking enabled (F8). Press F9 to recenter."
                                  : L"Head-look disabled (F8).");
    }
    state.headLookHotkeyWasDown = toggleDown;

    const bool recenterDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    const bool recenterRequested = recenterDown && !state.recenterHotkeyWasDown;
    if (recenterRequested) {
        state.haveHeadLookBaseline = false;
        ClearHeadLookGamepadStick();
        Log(L"Direct head tracking recentered (F9).");
    }
    state.recenterHotkeyWasDown = recenterDown;
    if (!state.headLookEnabled || headTrackingViews.empty()) {
        state.haveHeadLookBaseline = false;
        ClearHeadLookGamepadStick();
        SetFrostbiteHeadTrackingEnabled(false);
        return;
    }

    const XrPosef& pose = headTrackingViews.front().pose;
    const bool captureCentre = !state.haveHeadLookBaseline || recenterRequested;
    UpdateFrostbiteHeadPose(pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w,
                            pose.position.x, pose.position.y, pose.position.z, captureCentre);
    state.haveHeadLookBaseline = true;
    SetFrostbiteHeadTrackingEnabled(true);
}

void LogPipelineProbe(VrPresenter::State& state) {
    constexpr UINT slotCount = 14;
    std::array<ID3D11Buffer*, slotCount> buffers{};
    state.context->VSGetConstantBuffers(0, slotCount, buffers.data());
    Log(L"Camera probe: reading currently bound vertex-shader constant buffers.");
    for (UINT slot = 0; slot < slotCount; ++slot) {
        ComPtr<ID3D11Buffer> buffer;
        buffer.Attach(buffers[slot]);
        if (!buffer) continue;
        D3D11_BUFFER_DESC description{};
        buffer->GetDesc(&description);
        if (description.ByteWidth < sizeof(float) * 16) continue;

        D3D11_BUFFER_DESC staging{};
        staging.ByteWidth = description.ByteWidth;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> readable;
        if (FAILED(state.device->CreateBuffer(&staging, nullptr, &readable))) {
            Log(L"Camera probe: could not create staging copy for VS slot " + std::to_wstring(slot) + L".");
            continue;
        }
        state.context->CopyResource(readable.Get(), buffer.Get());
        state.context->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(state.context->Map(readable.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            Log(L"Camera probe: could not map VS slot " + std::to_wstring(slot) + L".");
            continue;
        }
        const auto* values = static_cast<const float*>(mapped.pData);
        const UINT matrixCount = (std::min)(4u, description.ByteWidth / static_cast<UINT>(sizeof(float) * 16));
        for (UINT matrix = 0; matrix < matrixCount; ++matrix) {
            std::wostringstream line;
            line << std::fixed << std::setprecision(3);
            line << L"Camera probe VS" << slot << L" m" << matrix << L":";
            for (UINT value = 0; value < 16; ++value) line << L" " << values[matrix * 16 + value];
            Log(line.str());
        }
        state.context->Unmap(readable.Get(), 0);
    }
}

void LogPresentDepthBinding(VrPresenter::State& state) {
    if (state.presentDepthProbeLogged) return;
    state.presentDepthProbeLogged = true;

    constexpr UINT kRenderTargetSlots = 8;
    std::array<ID3D11RenderTargetView*, kRenderTargetSlots> renderTargets{};
    ID3D11DepthStencilView* depthStencil = nullptr;
    state.context->OMGetRenderTargets(kRenderTargetSlots, renderTargets.data(), &depthStencil);
    for (ID3D11RenderTargetView* const target : renderTargets) {
        if (target != nullptr) target->Release();
    }
    if (depthStencil == nullptr) {
        Log(L"Stereo depth probe: no depth-stencil view is bound at Present.");
        return;
    }
    ComPtr<ID3D11DepthStencilView> depthView;
    depthView.Attach(depthStencil);
    D3D11_DEPTH_STENCIL_VIEW_DESC viewDescription{};
    depthView->GetDesc(&viewDescription);
    ComPtr<ID3D11Resource> resource;
    depthView->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (resource == nullptr || FAILED(resource.As(&texture))) {
        Log(L"Stereo depth probe: the bound depth view is not a Texture2D.");
        return;
    }
    D3D11_TEXTURE2D_DESC textureDescription{};
    texture->GetDesc(&textureDescription);
    std::wostringstream line;
    line << L"Stereo depth probe: Present DSV=" << static_cast<int>(viewDescription.Format)
         << L", texture=" << textureDescription.Width << L"x" << textureDescription.Height
         << L", format=" << static_cast<int>(textureDescription.Format)
         << L", bind=0x" << std::hex << textureDescription.BindFlags
         << L", samples=" << std::dec << textureDescription.SampleDesc.Count << L".";
    Log(line.str());
}

void LogPresentShaderResources(VrPresenter::State& state) {
    if (state.presentResourceProbeLogged) return;
    state.presentResourceProbeLogged = true;

    constexpr UINT kShaderResourceSlots = 32;
    const auto inspectStage = [&state](const wchar_t* const stage, ID3D11ShaderResourceView* const* views) {
        bool foundDepth{};
        for (UINT slot = 0; slot < kShaderResourceSlots; ++slot) {
            ComPtr<ID3D11ShaderResourceView> view;
            view.Attach(views[slot]);
            if (view == nullptr) continue;
            ComPtr<ID3D11Resource> resource;
            view->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;
            if (resource == nullptr || FAILED(resource.As(&texture))) continue;
            D3D11_TEXTURE2D_DESC textureDescription{};
            texture->GetDesc(&textureDescription);
            if ((textureDescription.BindFlags & D3D11_BIND_DEPTH_STENCIL) == 0) continue;
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
            view->GetDesc(&viewDescription);
            std::wostringstream line;
            line << L"Stereo depth probe: " << stage << L" SRV slot=" << slot
                 << L", texture=" << textureDescription.Width << L"x" << textureDescription.Height
                 << L", texture-format=" << static_cast<int>(textureDescription.Format)
                 << L", view-format=" << static_cast<int>(viewDescription.Format)
                 << L", bind=0x" << std::hex << textureDescription.BindFlags << L".";
            Log(line.str());
            foundDepth = true;
        }
        if (!foundDepth) Log(std::wstring(L"Stereo depth probe: no depth texture is bound to ") + stage + L" shader slots at Present.");
    };

    std::array<ID3D11ShaderResourceView*, kShaderResourceSlots> pixelViews{};
    state.context->PSGetShaderResources(0, kShaderResourceSlots, pixelViews.data());
    inspectStage(L"pixel", pixelViews.data());
    std::array<ID3D11ShaderResourceView*, kShaderResourceSlots> computeViews{};
    state.context->CSGetShaderResources(0, kShaderResourceSlots, computeViews.data());
    inspectStage(L"compute", computeViews.data());
}

DXGI_FORMAT DepthShaderResourceFormat(const DXGI_FORMAT textureFormat) {
    switch (textureFormat) {
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

void PreferDepthCandidateMatchingSource(VrPresenter::State& state) {
    const std::size_t candidateCount = StereoDepthCandidateCount();
    if (candidateCount == 0 || state.sourceWidth == 0 || state.sourceHeight == 0) return;

    std::size_t bestCandidate = state.depthDebugCandidate % candidateCount;
    uint64_t bestDifference = UINT64_MAX;
    for (std::size_t candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
        ComPtr<ID3D11Texture2D> texture;
        StereoDepthCandidateInfo info{};
        if (!AcquireStereoDepthCandidate(candidateIndex, &texture, info) || info.width == 0 || info.height == 0) continue;
        const uint64_t widthDifference = info.width > state.sourceWidth ? info.width - state.sourceWidth
                                                                         : state.sourceWidth - info.width;
        const uint64_t heightDifference = info.height > state.sourceHeight ? info.height - state.sourceHeight
                                                                             : state.sourceHeight - info.height;
        const uint64_t difference = widthDifference * 100000u + heightDifference;
        if (difference < bestDifference) {
            bestDifference = difference;
            bestCandidate = candidateIndex;
        }
        if (difference == 0) break;
    }
    if (bestCandidate != state.depthDebugCandidate) {
        state.depthDebugCandidate = bestCandidate;
        state.depthDebugView.Reset();
        state.depthDebugTexture = nullptr;
        Log(L"Depth stereo selected the source-matched candidate " + std::to_wstring(bestCandidate) + L".");
    }
}

ID3D11ShaderResourceView* GetStereoDepthPreviewView(VrPresenter::State& state) {
    const std::size_t candidateCount = StereoDepthCandidateCount();
    if (candidateCount == 0) return nullptr;
    state.depthDebugCandidate %= candidateCount;

    ComPtr<ID3D11Texture2D> texture;
    StereoDepthCandidateInfo info{};
    if (!AcquireStereoDepthCandidate(state.depthDebugCandidate, &texture, info)) return nullptr;
    if (state.depthDebugTexture == texture.Get() && state.depthDebugView != nullptr) return state.depthDebugView.Get();

    state.depthDebugView.Reset();
    state.depthDebugTexture = nullptr;
    const DXGI_FORMAT viewFormat = DepthShaderResourceFormat(info.format);
    if (viewFormat == DXGI_FORMAT_UNKNOWN) {
        Log(L"Stereo depth preview: selected texture has no supported shader view format.");
        return nullptr;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    viewDescription.Format = viewFormat;
    viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDescription.Texture2D.MostDetailedMip = 0;
    viewDescription.Texture2D.MipLevels = 1;
    if (FAILED(state.device->CreateShaderResourceView(texture.Get(), &viewDescription, &state.depthDebugView))) {
        Log(L"Stereo depth preview: could not create a shader view for the selected candidate.");
        return nullptr;
    }
    state.depthDebugTexture = texture.Get();
    Log(L"Stereo depth preview bound candidate " + std::to_wstring(state.depthDebugCandidate) + L": " +
        std::to_wstring(info.width) + L"x" + std::to_wstring(info.height) + L".");
    return state.depthDebugView.Get();
}

bool BlitFsr1ToEye(VrPresenter::State& state, VrPresenter::State::Eye& eye, const uint32_t imageIndex,
                   const uint32_t eyeIndex, const uint32_t sourceEyeIndex, ID3D11ShaderResourceView* const depthView,
                   const bool depthStereo, const float sourceCropScaleX, const float sourceCropScaleY) {
    if (sourceEyeIndex >= state.sourceViews.size() || !state.sourceEyeValid[sourceEyeIndex] ||
        eye.fsrIntermediateTarget == nullptr || eye.fsrIntermediateView == nullptr ||
        state.sourceWidth == 0 || state.sourceHeight == 0) return false;

    // EASU is an upscaler, not a downsampler. Keep the regular, stable copy
    // path if Heat already gives us more source pixels than the eye texture.
    const float inputWidth = static_cast<float>(state.sourceWidth) * sourceCropScaleX;
    const float inputHeight = static_cast<float>(state.sourceHeight) * sourceCropScaleY;
    if (inputWidth > static_cast<float>(eye.width) || inputHeight > static_cast<float>(eye.height)) return false;

    const float offsetX = (static_cast<float>(state.sourceWidth) - inputWidth) * 0.5f;
    const float offsetY = (static_cast<float>(state.sourceHeight) - inputHeight) * 0.5f;
    // FsrEasuConOffset from AMD's FSR 1 reference, expressed as floats rather
    // than its portable bit-packed constants. The source viewport is the
    // centred crop that fills a headset eye without changing its aspect.
    const std::array<float, 20> easuConstants{
        inputWidth / static_cast<float>(eye.width), inputHeight / static_cast<float>(eye.height),
        0.5f * inputWidth / static_cast<float>(eye.width) - 0.5f + offsetX,
        0.5f * inputHeight / static_cast<float>(eye.height) - 0.5f + offsetY,
        1.0f / static_cast<float>(state.sourceWidth), 1.0f / static_cast<float>(state.sourceHeight),
        1.0f / static_cast<float>(state.sourceWidth), -1.0f / static_cast<float>(state.sourceHeight),
        -1.0f / static_cast<float>(state.sourceWidth), 2.0f / static_cast<float>(state.sourceHeight),
        1.0f / static_cast<float>(state.sourceWidth), 2.0f / static_cast<float>(state.sourceHeight),
        0.0f, 4.0f / static_cast<float>(state.sourceHeight), 0.0f, 0.0f,
        depthStereo ? DepthStereoShiftForStrength(state.settings.depthStereoStrengthPercent) : 0.0f,
        depthStereo ? (eyeIndex == 0 ? 1.0f : -1.0f) : 0.0f,
        0.0f, 0.0f,
    };
    state.context->UpdateSubresource(state.fsrEasuConstantsBuffer.Get(), 0, nullptr, easuConstants.data(), 0, 0);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(eye.width);
    viewport.Height = static_cast<float>(eye.height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    ID3D11RenderTargetView* intermediateTarget = eye.fsrIntermediateTarget.Get();
    ID3D11ShaderResourceView* sourceView = state.sourceViews[sourceEyeIndex].Get();
    ID3D11SamplerState* sourceSampler = state.sampler.Get();
    ID3D11SamplerState* pointDepthSampler = state.depthSampler.Get();
    ID3D11Buffer* easuBuffer = state.fsrEasuConstantsBuffer.Get();
    const std::array<ID3D11ShaderResourceView*, 2> easuResources{sourceView, depthView};
    const std::array<ID3D11SamplerState*, 2> easuSamplers{sourceSampler, pointDepthSampler};
    state.context->OMSetRenderTargets(1, &intermediateTarget, nullptr);
    state.context->RSSetViewports(1, &viewport);
    state.context->IASetInputLayout(nullptr);
    state.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    state.context->VSSetShader(state.vertexShader.Get(), nullptr, 0);
    state.context->PSSetShader(state.fsrEasuPixelShader.Get(), nullptr, 0);
    state.context->PSSetShaderResources(0, 2, easuResources.data());
    state.context->PSSetSamplers(0, 2, easuSamplers.data());
    state.context->PSSetConstantBuffers(0, 1, &easuBuffer);
    state.context->Draw(3, 0);
    const std::array<ID3D11ShaderResourceView*, 2> nullEasuViews{};
    ID3D11Buffer* nullBuffer = nullptr;
    state.context->PSSetShaderResources(0, 2, nullEasuViews.data());
    state.context->PSSetConstantBuffers(0, 1, &nullBuffer);

    // RCAS uses a stop-based attenuation: 0 is strongest, two stops is a
    // gentle result. Map the panel's intuitive 0..100 value onto that range.
    const float attenuation = 2.0f * (1.0f - static_cast<float>(state.settings.headsetUpscalerSharpnessPercent) / 100.0f);
    const std::array<float, 4> rcasConstants{attenuation, static_cast<float>(eye.width),
                                               static_cast<float>(eye.height), 0.0f};
    state.context->UpdateSubresource(state.fsrRcasConstantsBuffer.Get(), 0, nullptr, rcasConstants.data(), 0, 0);
    ID3D11RenderTargetView* finalTarget = eye.renderTargets[imageIndex].Get();
    ID3D11ShaderResourceView* intermediateView = eye.fsrIntermediateView.Get();
    ID3D11Buffer* rcasBuffer = state.fsrRcasConstantsBuffer.Get();
    state.context->OMSetRenderTargets(1, &finalTarget, nullptr);
    state.context->PSSetShader(state.fsrRcasPixelShader.Get(), nullptr, 0);
    state.context->PSSetShaderResources(0, 1, &intermediateView);
    state.context->PSSetConstantBuffers(0, 1, &rcasBuffer);
    state.context->Draw(3, 0);
    ID3D11ShaderResourceView* nullView = nullptr;
    state.context->PSSetShaderResources(0, 1, &nullView);
    state.context->PSSetConstantBuffers(0, 1, &nullBuffer);
    return true;
}

bool BlitToEye(VrPresenter::State& state, ID3D11Texture2D* gameTexture, VrPresenter::State::Eye& eye,
               const uint32_t imageIndex, const uint32_t eyeIndex, uint32_t sourceEyeIndex) {
    (void)gameTexture;
    if (sourceEyeIndex >= state.sourceViews.size() || !state.sourceEyeValid[sourceEyeIndex]) sourceEyeIndex = 0;
    if (!state.sourceEyeValid[sourceEyeIndex]) return false;

    const bool depthPreviewRequested = state.depthDebugEnabled;
    const bool depthStereoRequested = !depthPreviewRequested && state.settings.depthStereoEnabled != 0 &&
                                      !state.alternateFrameStereoActive;
    if (depthStereoRequested) PreferDepthCandidateMatchingSource(state);
    ID3D11ShaderResourceView* depthView = (depthPreviewRequested || depthStereoRequested)
        ? GetStereoDepthPreviewView(state) : nullptr;
    const bool depthPreview = depthPreviewRequested && depthView != nullptr;
    const bool depthStereo = depthStereoRequested && depthView != nullptr;

    const float sourceAspect = static_cast<float>(state.sourceWidth) / static_cast<float>(state.sourceHeight);
    const float eyeAspect = static_cast<float>(eye.width) / static_cast<float>(eye.height);
    float contentScaleX = 1.0f;
    float contentScaleY = 1.0f;
    float sourceCropScaleX = 1.0f;
    float sourceCropScaleY = 1.0f;
    if (state.settings.headsetFullscreenEnabled != 0) {
        // A full headset image must fill the native eye projection. Keep the
        // source proportional by cropping the surplus source axis, never by
        // stretching its pixels or leaving virtual-screen bars.
        if (sourceAspect > eyeAspect) {
            sourceCropScaleX = eyeAspect / sourceAspect;
        } else {
            sourceCropScaleY = sourceAspect / eyeAspect;
        }
        if (!state.headsetFullscreenPresentationLogged) {
            state.headsetFullscreenPresentationLogged = true;
            std::wostringstream layout;
            layout << L"Headset-fullscreen layout: game=" << state.sourceWidth << L"x" << state.sourceHeight
                   << L", eye=" << eye.width << L"x" << eye.height
                   << L". Side/top crop fills the native headset FOV without stretching.";
            Log(layout.str());
        }
    } else {
        const float verticalFovScale = static_cast<float>(state.settings.verticalFovPercent) / 100.0f;
        const float displayedSourceAspect = sourceAspect / verticalFovScale;
        if (displayedSourceAspect > eyeAspect) {
            contentScaleY = eyeAspect / displayedSourceAspect;
        } else {
            contentScaleX = displayedSourceAspect / eyeAspect;
        }
        sourceCropScaleX = 1.0f / verticalFovScale;
        if (!state.monoPresentationLogged) {
            state.monoPresentationLogged = true;
            std::wostringstream layout;
            layout << L"Virtual-screen layout: game=" << state.sourceWidth << L"x" << state.sourceHeight
                   << L" (aspect " << std::fixed << std::setprecision(3) << sourceAspect << L"), eye="
                   << eye.width << L"x" << eye.height
                   << L". Aspect preserved; vertical framing=" << state.settings.verticalFovPercent << L"%.";
            Log(layout.str());
        }
    }
    const bool fsr1Requested = !depthPreview && state.settings.headsetFullscreenEnabled != 0 &&
                               state.settings.headsetUpscalerSharpnessPercent != 0 && !state.sourceIsHdr;
    if (fsr1Requested && BlitFsr1ToEye(state, eye, imageIndex, eyeIndex, sourceEyeIndex, depthView,
                                        depthStereo, sourceCropScaleX, sourceCropScaleY)) {
        if (!state.fsr1PresentationLogged) {
            state.fsr1PresentationLogged = true;
            Log(L"AMD FSR 1 EASU + RCAS active for the headset copy; no external injector is used.");
        }
        return true;
    }
    // No post-process FOV warp here.  Altering a completed mono texture made
    // the cabin look binocular and cannot be geometrically correct.  The
    // optional FOV control now changes Frostbite's real camera projection.
    constexpr float fullscreenFovScale = 1.0f;
    // A modest UV shift corresponds to the eye baseline at the very near
    // end of Frostbite's reversed-Z depth range.  Depth attenuates it for
    // the rest of the scene, keeping the effect steady instead of alternating
    // completed game frames between the eyes.
    const float depthStereoShift = depthStereo ? DepthStereoShiftForStrength(state.settings.depthStereoStrengthPercent) : 0.0f;
    const float depthEyeSign = eyeIndex == 0 ? 1.0f : -1.0f;
    const std::array<float, 16> presentationLayout{contentScaleX, contentScaleY,
                                                     sourceCropScaleX, sourceCropScaleY,
                                                     depthPreview ? static_cast<float>(state.depthDebugCandidate)
                                                                  : (depthStereo ? depthEyeSign : 0.0f),
                                                     depthStereoShift,
                                                     fullscreenFovScale, state.sourceIsHdr ? 1.0f : 0.0f,
                                                     1.0f / static_cast<float>((std::max)(state.sourceWidth, 1u)),
                                                     1.0f / static_cast<float>((std::max)(state.sourceHeight, 1u)),
                                                     static_cast<float>(state.settings.headsetUpscalerSharpnessPercent) / 100.0f,
                                                     0.0f,
                                                     0.0f, 0.0f, 0.0f, 0.0f};
    state.context->UpdateSubresource(state.presentationLayoutBuffer.Get(), 0, nullptr,
                                     presentationLayout.data(), 0, 0);

    ID3D11RenderTargetView* target = eye.renderTargets[imageIndex].Get();
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(eye.width);
    viewport.Height = static_cast<float>(eye.height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    state.context->OMSetRenderTargets(1, &target, nullptr);
    state.context->RSSetViewports(1, &viewport);
    constexpr std::array<float, 4> kBlack{0.0f, 0.0f, 0.0f, 1.0f};
    state.context->ClearRenderTargetView(target, kBlack.data());
    state.context->IASetInputLayout(nullptr);
    state.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    state.context->VSSetShader(state.vertexShader.Get(), nullptr, 0);
    state.context->PSSetShader(depthPreview ? state.depthDebugPixelShader.Get()
                                            : (depthStereo ? state.depthStereoPixelShader.Get() : state.pixelShader.Get()),
                               nullptr, 0);
    ID3D11ShaderResourceView* sourceView = state.sourceViews[sourceEyeIndex].Get();
    ID3D11SamplerState* sampler = state.sampler.Get();
    ID3D11SamplerState* depthSampler = state.depthSampler.Get();
    ID3D11Buffer* presentationLayoutBuffer = state.presentationLayoutBuffer.Get();
    const std::array<ID3D11ShaderResourceView*, 2> shaderResources{sourceView, depthView};
    const UINT shaderResourceCount = (depthPreview || depthStereo) ? 2u : 1u;
    state.context->PSSetShaderResources(0, shaderResourceCount, shaderResources.data());
    const std::array<ID3D11SamplerState*, 2> samplers{sampler, depthSampler};
    state.context->PSSetSamplers(0, shaderResourceCount, samplers.data());
    state.context->PSSetConstantBuffers(0, 1, &presentationLayoutBuffer);
    state.context->Draw(3, 0);
    const std::array<ID3D11ShaderResourceView*, 2> nullViews{};
    ID3D11Buffer* nullBuffer = nullptr;
    state.context->PSSetShaderResources(0, shaderResourceCount, nullViews.data());
    state.context->PSSetConstantBuffers(0, 1, &nullBuffer);
    return true;
}
} // namespace

VrPresenter::VrPresenter() = default;

VrPresenter::~VrPresenter() {
    if (state_) DestroyOpenXr(*state_);
}

bool VrPresenter::IsReady() const {
    return state_ != nullptr && state_->session != XR_NULL_HANDLE;
}

bool VrPresenter::Initialise(IDXGISwapChain* swapChain) {
    if (state_) {
        DestroyOpenXr(*state_);
        state_.reset();
    }
    auto candidate = std::make_unique<State>();
    candidate->settings = LoadVrSettings();
    candidate->cullTransformSplitEnabled = candidate->settings.cullTransformSplitEnabled != 0;
    if (candidate->settings.highResolutionSourceInspectionEnabled != 0) {
        candidate->highQualityCaptureDumpRequested = true;
        Log(L"Safe native high-resolution source inspection armed; the headset keeps the final game frame.");
    }
    candidate->latestDrawnHighPassEnabled = candidate->settings.highResolutionLatestPassEnabled != 0;
    candidate->latestDrawnHighPassInspectionRequested =
        candidate->settings.highResolutionLatestPassInspectionEnabled != 0;
    if (candidate->latestDrawnHighPassInspectionRequested) {
        Log(L"Live high-resolution Frostbite pass inspection armed; the headset keeps the final game frame.");
    }
    if (candidate->settings.highResolutionSourceEnabled != 0) {
        candidate->highQualityCaptureEnabled = true;
        candidate->highQualityCaptureCandidate = static_cast<std::size_t>(candidate->settings.highResolutionSourceCandidate);
        Log(L"Native high-resolution source trial armed at candidate " +
            std::to_wstring(candidate->highQualityCaptureCandidate) +
            L".");
    }
    // Alternating completed Frostbite frames was a retired experiment. Never
    // restore it from an older INI file; it must not influence startup.
    candidate->alternateFrameStereoActive = false;
    candidate->settings.alternateFrameStereoEnabled = 0;
    // Head tracking is on from the first submitted OpenXR frame.  The first
    // valid HMD pose becomes the centre automatically, so no F8 step is
    // needed after launching the game.
    candidate->headLookEnabled = true;
    ApplyCameraSettings(candidate->settings);
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&candidate->device)))) {
        Log(L"The active renderer is not D3D11. DX12 support is the next backend; this build leaves it untouched.");
        return false;
    }
    candidate->device->GetImmediateContext(&candidate->context);
    candidate->gameSwapChain = swapChain;
    candidate->gameSwapChainLargestPixelCount = SwapChainPixelCount(swapChain);
    InstallCameraDrawProbe(candidate->device.Get(), candidate->context.Get());
    InstallFrostbiteCameraObserver();

    uint32_t extensionCount = 0;
    if (!Succeeded(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr), L"xrEnumerateInstanceExtensionProperties(count)")) return false;
    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    if (!Succeeded(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()), L"xrEnumerateInstanceExtensionProperties(list)")) return false;
    bool hasD3D11 = false;
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) hasD3D11 = true;
    }
    if (!hasD3D11) {
        Log(L"The selected OpenXR runtime does not expose XR_KHR_D3D11_enable.");
        return false;
    }

    const char* enabledExtensions[]{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(instanceInfo.applicationInfo.applicationName, "NFS Heat VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
    instanceInfo.applicationInfo.applicationVersion = 1;
    std::strncpy(instanceInfo.applicationInfo.engineName, "Frostbite bridge", XR_MAX_ENGINE_NAME_SIZE - 1);
    instanceInfo.applicationInfo.engineVersion = 1;
    instanceInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.enabledExtensionNames = enabledExtensions;
    if (!Succeeded(xrCreateInstance(&instanceInfo, &candidate->instance), L"xrCreateInstance")) return false;

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId{};
    if (!Succeeded(xrGetSystem(candidate->instance, &systemInfo, &systemId), L"xrGetSystem")) {
        DestroyOpenXr(*candidate);
        return false;
    }

    PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
    if (!Succeeded(xrGetInstanceProcAddr(candidate->instance, "xrGetD3D11GraphicsRequirementsKHR",
                                         reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)),
                   L"xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)")) {
        DestroyOpenXr(*candidate);
        return false;
    }
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!Succeeded(getRequirements(candidate->instance, systemId, &requirements), L"xrGetD3D11GraphicsRequirementsKHR")) {
        DestroyOpenXr(*candidate);
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDescription{};
    if (SUCCEEDED(candidate->device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&adapterDescription)) &&
        (adapterDescription.AdapterLuid.HighPart != requirements.adapterLuid.HighPart ||
         adapterDescription.AdapterLuid.LowPart != requirements.adapterLuid.LowPart)) {
        Log(L"The game GPU does not match the OpenXR runtime GPU. Refusing to submit cross-adapter textures.");
        DestroyOpenXr(*candidate);
        return false;
    }

    XrGraphicsBindingD3D11KHR graphicsBinding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    graphicsBinding.device = candidate->device.Get();
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &graphicsBinding;
    sessionInfo.systemId = systemId;
    if (!Succeeded(xrCreateSession(candidate->instance, &sessionInfo, &candidate->session), L"xrCreateSession")) {
        DestroyOpenXr(*candidate);
        return false;
    }
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Succeeded(xrCreateReferenceSpace(candidate->session, &spaceInfo, &candidate->space), L"xrCreateReferenceSpace")) {
        DestroyOpenXr(*candidate);
        return false;
    }

    uint32_t viewCount = 0;
    if (!Succeeded(xrEnumerateViewConfigurationViews(candidate->instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                      0, &viewCount, nullptr), L"xrEnumerateViewConfigurationViews(count)")) {
        DestroyOpenXr(*candidate);
        return false;
    }
    if (viewCount != 2) {
        Log(L"OpenXR did not report a stereo view configuration.");
        DestroyOpenXr(*candidate);
        return false;
    }
    std::vector<XrViewConfigurationView> configurationViews(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (!Succeeded(xrEnumerateViewConfigurationViews(candidate->instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                      viewCount, &viewCount, configurationViews.data()), L"xrEnumerateViewConfigurationViews(list)")) {
        DestroyOpenXr(*candidate);
        return false;
    }
    candidate->views.resize(viewCount, {XR_TYPE_VIEW});
    candidate->headTrackingViews.resize(viewCount, {XR_TYPE_VIEW});
    uint32_t formatCount = 0;
    if (!Succeeded(xrEnumerateSwapchainFormats(candidate->session, 0, &formatCount, nullptr), L"xrEnumerateSwapchainFormats(count)")) {
        DestroyOpenXr(*candidate);
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    if (!Succeeded(xrEnumerateSwapchainFormats(candidate->session, formatCount, &formatCount, formats.data()), L"xrEnumerateSwapchainFormats(list)")) {
        DestroyOpenXr(*candidate);
        return false;
    }
    std::wstring formatList;
    for (const auto supported : formats) {
        if (!formatList.empty()) formatList += L",";
        formatList += std::to_wstring(supported);
    }
    const DXGI_FORMAT format = SelectColourFormat(formats);
    Log(L"OpenXR colour formats: [" + formatList + L"], selected=" + std::to_wstring(static_cast<int>(format)) + L".");
    if (format == DXGI_FORMAT_UNKNOWN || !CreateEye(*candidate, configurationViews[0], format) ||
        !CreateEye(*candidate, configurationViews[1], format) || !CreateShaderPipeline(*candidate)) {
        DestroyOpenXr(*candidate);
        return false;
    }
    Log(L"OpenXR D3D11 presenter ready. Fullscreen mode uses the native headset FOV and follows the headset; F10 recentres only virtual-screen mode.");
    state_ = std::move(candidate);
    return true;
}

void VrPresenter::PresentFrame(IDXGISwapChain* swapChain) {
    if (!state_) return;
    auto& state = *state_;
    if (!SelectGameSwapChain(state, swapChain)) return;
    PollEvents(state);
    if (!state.sessionRunning) return;
    RefreshRuntimeSettings(state);
    UpdateSettingsPanelHotkey(state);

    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    if (!Succeeded(xrWaitFrame(state.session, &waitInfo, &state.frameState), L"xrWaitFrame")) return;
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (!Succeeded(xrBeginFrame(state.session, &beginInfo), L"xrBeginFrame")) return;

    XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    // The views used by the composition layer must match this frame's display
    // time so a fullscreen layer follows the headset exactly.
    locateInfo.displayTime = state.frameState.predictedDisplayTime;
    locateInfo.space = state.space;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    uint32_t locatedViewCount = 0;
    bool rendered = state.frameState.shouldRender &&
                    Succeeded(xrLocateViews(state.session, &locateInfo, &viewState, static_cast<uint32_t>(state.views.size()),
                                             &locatedViewCount, state.views.data()), L"xrLocateViews") &&
                    locatedViewCount == state.views.size();
    const std::vector<XrView>* headTrackingViews = &state.views;
    if (rendered && state.headLookEnabled && state.frameState.predictedDisplayPeriod > 0) {
        // Frostbite has already rendered its camera by the time this Present
        // hook runs. Its next frame consumes this pose, so obtain a second
        // view one display interval ahead without moving the presentation.
        XrViewLocateInfo predictedLocateInfo = locateInfo;
        predictedLocateInfo.displayTime += state.frameState.predictedDisplayPeriod;
        XrViewState predictedViewState{XR_TYPE_VIEW_STATE};
        uint32_t predictedViewCount = 0;
        if (Succeeded(xrLocateViews(state.session, &predictedLocateInfo, &predictedViewState,
                                    static_cast<uint32_t>(state.headTrackingViews.size()), &predictedViewCount,
                                    state.headTrackingViews.data()), L"xrLocateViews(predicted)") &&
            predictedViewCount == state.headTrackingViews.size()) {
            headTrackingViews = &state.headTrackingViews;
            if (!state.headPosePredictionLogged) {
                Log(L"Head tracking uses one predicted display interval to compensate the Frostbite render-frame delay.");
                state.headPosePredictionLogged = true;
            }
        }
    }
    if (rendered) UpdateHeadLook(state, *headTrackingViews);
    const bool probeDown = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (probeDown && !state.pipelineProbeHotkeyWasDown && rendered) RequestCameraDrawProbe();
    state.pipelineProbeHotkeyWasDown = probeDown;

    if (rendered) {
        LogPresentDepthBinding(state);
        LogPresentShaderResources(state);
    }
    ComPtr<ID3D11Texture2D> gameTexture;
    if (rendered && FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&gameTexture)))) {
        Log(L"Could not access the D3D11 game back buffer.");
        rendered = false;
    }
    bool capturedGameFrame = false;
    if (rendered) {
        capturedGameFrame = CaptureGameFrame(state, gameTexture.Get());
        // An EA/Frostbite helper present is not a game camera frame. Keep
        // showing the last complete pair rather than blanking the headset,
        // and most importantly do not advance the left/right scheduler.
        if (!capturedGameFrame && !state.sourceEyeValid[0]) rendered = false;
    }
    // The game produces one non-VR camera.  Describing both texture copies as
    // the same centre-eye projection avoids a false left/right camera offset.
    XrPosef locatedCentrePose = state.views.front().pose;
    if (state.views.size() == 2) {
        locatedCentrePose.position.x = (state.views[0].pose.position.x + state.views[1].pose.position.x) * 0.5f;
        locatedCentrePose.position.y = (state.views[0].pose.position.y + state.views[1].pose.position.y) * 0.5f;
        locatedCentrePose.position.z = (state.views[0].pose.position.z + state.views[1].pose.position.z) * 0.5f;
    }
    const bool headsetFullscreen = state.settings.headsetFullscreenEnabled != 0;
    const bool alternateFrameStereo = state.alternateFrameStereoActive;
    if (!headsetFullscreen) {
        const bool resetProjectionDown = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (resetProjectionDown && !state.projectionRecenterHotkeyWasDown) {
            state.haveProjectionBaseline = false;
            Log(L"Projection orientation recentered (F10).");
        }
        state.projectionRecenterHotkeyWasDown = resetProjectionDown;
        if (!state.haveProjectionBaseline) {
            state.projectionPose = locatedCentrePose;
            state.projectionFov = ComfortableMonoFov(state.eyes.front(), state.settings.screenFovDegrees);
            state.haveProjectionBaseline = true;
            Log(L"Projection orientation baseline captured (F10 recentres the virtual screen).");
        }
    }
    const XrPosef& centrePose = headsetFullscreen ? locatedCentrePose : state.projectionPose;
    const XrFovf headsetFov = headsetFullscreen
        ? SharedHeadsetFov(state.views, state.settings.headsetFullscreenHorizontalFovPercent,
                           state.settings.headsetFullscreenVerticalFovPercent)
        : XrFovf{};
    if (headsetFullscreen) ApplyHmdMatchedEngineFov(state, headsetFov);
    const XrFovf& centreFov = headsetFullscreen ? headsetFov : state.projectionFov;
    std::vector<XrCompositionLayerProjectionView> projectionViews(
        state.eyes.size(), {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
    for (uint32_t eyeIndex = 0; rendered && eyeIndex < state.eyes.size(); ++eyeIndex) {
        auto& eye = state.eyes[eyeIndex];
        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        imageWait.timeout = XR_INFINITE_DURATION;
        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        rendered = Succeeded(xrAcquireSwapchainImage(eye.swapchain, &acquireInfo, &imageIndex), L"xrAcquireSwapchainImage") &&
                   Succeeded(xrWaitSwapchainImage(eye.swapchain, &imageWait), L"xrWaitSwapchainImage");
        const uint32_t sourceEyeIndex = alternateFrameStereo ? eyeIndex : 0;
        if (rendered) rendered = BlitToEye(state, gameTexture.Get(), eye, imageIndex, eyeIndex, sourceEyeIndex);
        state.context->Flush();
        if (!Succeeded(xrReleaseSwapchainImage(eye.swapchain, &releaseInfo), L"xrReleaseSwapchainImage")) rendered = false;

        // The game framebuffer is still projected by Frostbite's ordinary
        // mono projection.  Until that engine projection is replaced with
        // the matching per-eye asymmetric frustum, giving OpenXR a different
        // pose/FOV here crops and shifts the picture (especially on canted
        // Pimax panels).  Keep the known-good common canvas mapping while
        // AFR supplies distinct left/right *game* camera frames.
        projectionViews[eyeIndex].pose = centrePose;
        projectionViews[eyeIndex].fov = centreFov;
        projectionViews[eyeIndex].subImage.swapchain = eye.swapchain;
        projectionViews[eyeIndex].subImage.imageRect.offset = {0, 0};
        projectionViews[eyeIndex].subImage.imageRect.extent = {static_cast<int32_t>(eye.width), static_cast<int32_t>(eye.height)};
        projectionViews[eyeIndex].subImage.imageArrayIndex = 0;
    }

    if (rendered && capturedGameFrame) ScheduleAlternateFrameEye(state);

    XrCompositionLayerProjection projectionLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projectionLayer.space = state.space;
    projectionLayer.viewCount = static_cast<uint32_t>(projectionViews.size());
    projectionLayer.views = projectionViews.data();
    const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer)};
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = state.frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = rendered ? 1u : 0u;
    endInfo.layers = rendered ? layers : nullptr;
    if (Succeeded(xrEndFrame(state.session, &endInfo), L"xrEndFrame") && rendered && !state.firstProjectionFrameLogged) {
        state.firstProjectionFrameLogged = true;
        Log(L"First centralised projection frame submitted to the OpenXR compositor.");
    }
}

} // namespace nfsheatvr

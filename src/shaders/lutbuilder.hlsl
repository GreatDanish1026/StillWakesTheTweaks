// Replacement for Still Wakes the Deep's colour grading LUT builder: UE 5.4
// PostProcessCombineLUTs.usf MainCS, permutation SKIP_TEMPERATURE=1, BLENDCOUNT=1,
// OUTPUT_DEVICE_SRGB=0, USE_VOLUME_LUT=1 (game CS 0x129A2FB0).
//
// CombineLUTsCommon below is Epic's code (UE 5.4) with our HDR output added. TonemapCommon.ush,
// ACES.ush and GammaCorrectionCommon.ush are included unmodified from the local UE install at
// build time and aren't kept in this repository.
//
// With Settings.Enable = 0 this is the stock shader. With it on, HDR output (devices 3/4) is
// built from the game's SDR image instead of ACES (docs/HDR_PLAN.md):
//   - shadows and midtones are the SDR result as a gamma 2.2 display shows it (black floor)
//   - scaled so SDR white sits at PaperWhite nits (mid grey)
//   - highlights extend by luminance only, keeping SDR hue and chroma, up to Peak nits

// ---------------------------------------------------------------- what Common.ush provides

#define half float
#define half2 float2
#define half3 float3
#define half4 float4
#define half3x3 float3x3
#define BRANCH [branch]
#define FLATTEN [flatten]
#define UNROLL [unroll]
#define LOOP [loop]
#define PI 3.1415926535897932f
float Square(float x) { return x * x; }
float2 Square(float2 x) { return x * x; }
float3 Square(float3 x) { return x * x; }
float4 Square(float4 x) { return x * x; }

#define USE_VOLUME_LUT 1
#define COMPUTESHADER 1
#define THREADGROUP_SIZE 8
#ifndef SKIP_TEMPERATURE
#define SKIP_TEMPERATURE 1
#endif
#define BLENDCOUNT 1
#define USE_COLOR_MATRIX 1
#define USE_SHADOW_TINT 1
#define USE_CONTRAST 1

// The game's parameter block, at the offsets its shader reflects (FCombineLUTParameters).
cbuffer _RootShaderParameters : register(b0) {
    float4 ACESMinMaxData : packoffset(c8);
    float4 ACESMidData : packoffset(c9);
    float4 ACESCoefsLow_0 : packoffset(c10);
    float4 ACESCoefsHigh_0 : packoffset(c11);
    float ACESCoefsLow_4 : packoffset(c12.x);
    float ACESCoefsHigh_4 : packoffset(c12.y);
    float ACESSceneColorMultiplier : packoffset(c12.z);
    float ACESGamutCompression : packoffset(c12.w);
    float4 OverlayColor : packoffset(c13);
    float3 ColorScale : packoffset(c14);
    float4 ColorSaturation : packoffset(c15);
    float4 ColorContrast : packoffset(c16);
    float4 ColorGamma : packoffset(c17);
    float4 ColorGain : packoffset(c18);
    float4 ColorOffset : packoffset(c19);
    float4 ColorSaturationShadows : packoffset(c20);
    float4 ColorContrastShadows : packoffset(c21);
    float4 ColorGammaShadows : packoffset(c22);
    float4 ColorGainShadows : packoffset(c23);
    float4 ColorOffsetShadows : packoffset(c24);
    float4 ColorSaturationMidtones : packoffset(c25);
    float4 ColorContrastMidtones : packoffset(c26);
    float4 ColorGammaMidtones : packoffset(c27);
    float4 ColorGainMidtones : packoffset(c28);
    float4 ColorOffsetMidtones : packoffset(c29);
    float4 ColorSaturationHighlights : packoffset(c30);
    float4 ColorContrastHighlights : packoffset(c31);
    float4 ColorGammaHighlights : packoffset(c32);
    float4 ColorGainHighlights : packoffset(c33);
    float4 ColorOffsetHighlights : packoffset(c34);
    float LUTSize : packoffset(c35.x);
    float WhiteTemp : packoffset(c35.y);
    float WhiteTint : packoffset(c35.z);
    float ColorCorrectionShadowsMax : packoffset(c35.w);
    float ColorCorrectionHighlightsMin : packoffset(c36.x);
    float ColorCorrectionHighlightsMax : packoffset(c36.y);
    float BlueCorrection : packoffset(c36.z);
    float ExpandGamut : packoffset(c36.w);
    float ToneCurveAmount : packoffset(c37.x);
    float FilmSlope : packoffset(c37.y);
    float FilmToe : packoffset(c37.z);
    float FilmShoulder : packoffset(c37.w);
    float FilmBlackClip : packoffset(c38.x);
    float FilmWhiteClip : packoffset(c38.y);
    uint bUseMobileTonemapper : packoffset(c38.z);
    uint bIsTemperatureWhiteBalance : packoffset(c38.w);
    float3 MappingPolynomial : packoffset(c39);
    float3 InverseGamma : packoffset(c40);
    uint OutputDevice : packoffset(c40.w);
    uint OutputGamut : packoffset(c41.x);
    float OutputMaxLuminance : packoffset(c41.y);
    float2 OutputExtentInverse : packoffset(c42);
};

struct FWorkingColorSpaceConstants {
    row_major float4x4 ToXYZ;
    row_major float4x4 FromXYZ;
    row_major float4x4 ToAP1;
    row_major float4x4 FromAP1;
    row_major float4x4 ToAP0;
    uint bIsSRGB;
};
cbuffer WorkingColorSpace : register(b1) {
    FWorkingColorSpaceConstants WorkingColorSpace;
};

RWTexture3D<float4> RWOutputTexture : register(u0);

// Ours: root constants the add-on appends to the game's root signature.
cbuffer SwtdSettings : register(b13, space50) {
    float SwtdEnable;        // 0: stock shader
    float SwtdPaperWhite;    // nits for SDR white
    float SwtdPeak;          // nits; 0: the game's menu value (ACESMinMaxData.w)
    float SwtdBlackFloor;    // 0: sRGB curve (stock), 1: gamma 2.2
    float SwtdHighlightSat;  // 0: SDR chroma in highlights, 1: untonemapped chroma
    float SwtdKnee;          // SDR luminance where the extension starts blending in
    float SwtdPad0;
    float SwtdMagic;         // 20261019: the add-on pushed these; anything else runs stock
};

// PostProcessCommon.ush
half3 ColorCorrection(half3 InLDRColor) {
    return MappingPolynomial.x * (InLDRColor * InLDRColor) + MappingPolynomial.y * InLDRColor + MappingPolynomial.z;
}

#include "TonemapCommon.ush"

// ---------------------------------------------------------------- our HDR output

static const float3 kRec709Y = float3(0.2126, 0.7152, 0.0722);
static const float3x3 kRec709To2020 = {
    0.6274040, 0.3292820, 0.0433136,
    0.0690970, 0.9195400, 0.0113612,
    0.0163916, 0.0880132, 0.8955950,
};

// x below the knee unchanged; above it, an exponential approach to Ceiling (C1 at the knee).
float RollOff(float x, float Knee, float Ceiling) {
    if (x <= Knee || Ceiling <= Knee) return min(x, max(Ceiling, Knee));
    float Range = Ceiling - Knee;
    return Knee + Range * (1.0 - exp(-(x - Knee) / Range));
}

// Sdr: the game's SDR result before encoding, BT.709 linear, white = 1.
// Graded: the untonemapped graded colour, BT.709 linear (scene, 1.0 = 100 nits).
// Returns BT.709 nits.
float3 SwtdHdr(float3 Sdr, float3 Graded) {
    Sdr = saturate(Sdr);

    // Black floor: what a gamma 2.2 display emits for the sRGB-encoded SDR value.
    float3 Sdr22 = pow(LinearToSrgb(Sdr), 2.2);
    float3 Display = lerp(Sdr, Sdr22, SwtdBlackFloor);

    float PaperWhite = max(SwtdPaperWhite, 1.0);
    float Peak = SwtdPeak > 0 ? SwtdPeak : ACESMinMaxData.w;
    float PeakRatio = max(Peak / PaperWhite, 1.0);

    // Untonemapped luminance, scaled so it meets the SDR curve at mid grey.
    float MidOut = dot(FilmToneMap(float3(0.18, 0.18, 0.18)), kRec709Y);
    float Gain = MidOut / 0.18;
    float YUntonemapped = max(dot(Graded, kRec709Y), 0) * Gain;

    float YSdr = dot(Display, kRec709Y);
    float Knee = saturate(SwtdKnee);
    float YExtended = RollOff(YUntonemapped, Knee, PeakRatio);
    float W = smoothstep(Knee, 1.0, YSdr);
    float Y = max(YSdr, lerp(YSdr, YExtended, W));

    // Luminance only: SDR hue and chroma.
    float3 Color = YSdr > 1e-6 ? Display * (Y / YSdr) : Display;
    // Optional: some of the untonemapped chroma back in the highlights.
    float YG = dot(max(Graded, 0), kRec709Y);
    if (SwtdHighlightSat > 0 && YG > 1e-6) {
        float3 Alt = max(Graded, 0) * (Y / YG);
        Color = lerp(Color, Alt, SwtdHighlightSat * W);
    }
    // Keep every channel under the peak without changing hue.
    float MaxC = max(Color.r, max(Color.g, Color.b));
    if (MaxC > PeakRatio) Color *= PeakRatio / MaxC;

    return Color * PaperWhite;
}

// ---------------------------------------------------------------- Epic: CombineLUTsCommon

float3 ColorCorrect(float3 WorkingColor, float4 ColorSaturation, float4 ColorContrast, float4 ColorGamma,
                    float4 ColorGain, float4 ColorOffset) {
    float Luma = dot(WorkingColor, AP1_RGB2Y);
    WorkingColor = max(0, lerp(Luma.xxx, WorkingColor, ColorSaturation.xyz * ColorSaturation.w));
    WorkingColor = pow(WorkingColor * (1.0 / 0.18), ColorContrast.xyz * ColorContrast.w) * 0.18;
    WorkingColor = pow(WorkingColor, 1.0 / (ColorGamma.xyz * ColorGamma.w));
    WorkingColor = WorkingColor * (ColorGain.xyz * ColorGain.w) + (ColorOffset.xyz + ColorOffset.w);
    return WorkingColor;
}

float3 ColorCorrectAll(float3 WorkingColor) {
    float Luma = dot(WorkingColor, AP1_RGB2Y);
    float3 CCColorShadows = ColorCorrect(WorkingColor, ColorSaturationShadows * ColorSaturation,
                                         ColorContrastShadows * ColorContrast, ColorGammaShadows * ColorGamma,
                                         ColorGainShadows * ColorGain, ColorOffsetShadows + ColorOffset);
    float CCWeightShadows = 1 - smoothstep(0, ColorCorrectionShadowsMax, Luma);
    float3 CCColorHighlights = ColorCorrect(WorkingColor, ColorSaturationHighlights * ColorSaturation,
                                            ColorContrastHighlights * ColorContrast, ColorGammaHighlights * ColorGamma,
                                            ColorGainHighlights * ColorGain, ColorOffsetHighlights + ColorOffset);
    float CCWeightHighlights = smoothstep(ColorCorrectionHighlightsMin, ColorCorrectionHighlightsMax, Luma);
    float3 CCColorMidtones = ColorCorrect(WorkingColor, ColorSaturationMidtones * ColorSaturation,
                                          ColorContrastMidtones * ColorContrast, ColorGammaMidtones * ColorGamma,
                                          ColorGainMidtones * ColorGain, ColorOffsetMidtones + ColorOffset);
    float CCWeightMidtones = 1 - CCWeightShadows - CCWeightHighlights;
    return CCColorShadows * CCWeightShadows + CCColorMidtones * CCWeightMidtones + CCColorHighlights * CCWeightHighlights;
}

uint GetOutputDevice() { return OutputDevice; }

float4 CombineLUTsCommon(float2 InUV, uint InLayerIndex) {
    float4 Neutral;
    {
        float2 UV = InUV - float2(0.5f / LUTSize, 0.5f / LUTSize);
        Neutral = float4(UV * LUTSize / (LUTSize - 1), InLayerIndex / (LUTSize - 1), 0);
    }

    float4 OutColor = 0;

    const float3x3 AP1_2_sRGB = mul(XYZ_2_sRGB_MAT, mul(D60_2_D65_CAT, AP1_2_XYZ_MAT));
    const float3x3 AP0_2_AP1 = mul(XYZ_2_AP1_MAT, AP0_2_XYZ_MAT);
    const float3x3 AP1_2_AP0 = mul(XYZ_2_AP0_MAT, AP1_2_XYZ_MAT);
    const float3x3 AP1_2_Output = OuputGamutMappingMatrix(OutputGamut);

    float3 LUTEncodedColor = Neutral.rgb;
    float3 LinearColor;
    if (GetOutputDevice() >= TONEMAPPER_OUTPUT_ACES1000nitST2084) {
        LinearColor = ST2084ToLinear(LUTEncodedColor) * LinearToNitsScaleInverse;
    } else {
        LinearColor = LogToLin(LUTEncodedColor) - LogToLin(0);
    }

#if !SKIP_TEMPERATURE
    float3 BalancedColor = WhiteBalance(LinearColor, WhiteTemp, WhiteTint, bIsTemperatureWhiteBalance,
                                        (float3x3)WorkingColorSpace.ToXYZ, (float3x3)WorkingColorSpace.FromXYZ);
#else
    float3 BalancedColor = LinearColor;
#endif

    float3 ColorAP1 = mul((float3x3)WorkingColorSpace.ToAP1, BalancedColor);

    float LumaAP1 = dot(ColorAP1, AP1_RGB2Y);
    float3 ChromaAP1 = ColorAP1 / LumaAP1;
    float ChromaDistSqr = dot(ChromaAP1 - 1, ChromaAP1 - 1);
    float ExpandAmount = (1 - exp2(-4 * ChromaDistSqr)) * (1 - exp2(-4 * ExpandGamut * LumaAP1 * LumaAP1));

    const float3x3 Wide_2_XYZ_MAT = {
        0.5441691, 0.2395926, 0.1666943,
        0.2394656, 0.7021530, 0.0583814,
        -0.0023439, 0.0361834, 1.0552183,
    };
    const float3x3 Wide_2_AP1 = mul(XYZ_2_AP1_MAT, Wide_2_XYZ_MAT);
    const float3x3 ExpandMat = mul(Wide_2_AP1, AP1_2_sRGB);

    float3 ColorExpand = mul(ExpandMat, ColorAP1);
    ColorAP1 = lerp(ColorAP1, ColorExpand, ExpandAmount);

    ColorAP1 = ColorCorrectAll(ColorAP1);

    float3 GradedColor = mul((float3x3)WorkingColorSpace.FromAP1, ColorAP1);

    const float3x3 BlueCorrect = {
        0.9404372683, -0.0183068787, 0.0778696104,
        0.0083786969, 0.8286599939, 0.1629613092,
        0.0005471261, -0.0008833746, 1.0003362486,
    };
    const float3x3 BlueCorrectInv = {
        1.06318, 0.0233956, -0.0865726,
        -0.0106337, 1.20632, -0.19569,
        -0.000590887, 0.00105248, 0.999538,
    };
    const float3x3 BlueCorrectAP1 = mul(AP0_2_AP1, mul(BlueCorrect, AP1_2_AP0));
    const float3x3 BlueCorrectInvAP1 = mul(AP0_2_AP1, mul(BlueCorrectInv, AP1_2_AP0));

    ColorAP1 = lerp(ColorAP1, mul(BlueCorrectAP1, ColorAP1), BlueCorrection);
    float3 ToneMappedColorAP1 = FilmToneMap(ColorAP1);
    ColorAP1 = lerp(ColorAP1, ToneMappedColorAP1, ToneCurveAmount);
    ColorAP1 = lerp(ColorAP1, mul(BlueCorrectInvAP1, ColorAP1), BlueCorrection);

    float3 FilmColor = max(0, mul((float3x3)WorkingColorSpace.FromAP1, ColorAP1));

    FilmColor = ColorCorrection(FilmColor);

    float3 FilmColorNoGamma = lerp(FilmColor * ColorScale, OverlayColor.rgb, OverlayColor.a);
    GradedColor = lerp(GradedColor * ColorScale, OverlayColor.rgb, OverlayColor.a);

    FilmColor = pow(max(0, FilmColorNoGamma), InverseGamma.y);

    half3 OutDeviceColor = 0;

    const bool bOurs = SwtdMagic == 20261019.0 && SwtdEnable > 0.5 && (GetOutputDevice() == TONEMAPPER_OUTPUT_ACES1000nitST2084 ||
                                             GetOutputDevice() == TONEMAPPER_OUTPUT_ACES2000nitST2084);
    BRANCH
    if (bOurs) {
        float3 Sdr709 = WorkingColorSpace.bIsSRGB ? FilmColor
                                                  : mul(AP1_2_sRGB, mul((float3x3)WorkingColorSpace.ToAP1, FilmColor));
        float3 Graded709 = WorkingColorSpace.bIsSRGB ? GradedColor
                                                     : mul(AP1_2_sRGB, mul((float3x3)WorkingColorSpace.ToAP1, GradedColor));
        float3 Nits709 = SwtdHdr(Sdr709, Graded709);
        OutDeviceColor = LinearToST2084(max(0, mul(kRec709To2020, Nits709)));
    } else if (GetOutputDevice() == TONEMAPPER_OUTPUT_sRGB) {
        float3 OutputGamutColor = WorkingColorSpace.bIsSRGB
                                      ? FilmColor
                                      : mul(AP1_2_Output, mul((float3x3)WorkingColorSpace.ToAP1, FilmColor));
        OutDeviceColor = LinearToSrgb(OutputGamutColor);
    } else if (GetOutputDevice() == TONEMAPPER_OUTPUT_Rec709) {
        float3 OutputGamutColor = mul(AP1_2_Output, mul((float3x3)WorkingColorSpace.ToAP1, FilmColor));
        OutDeviceColor = LinearTo709Branchless(OutputGamutColor);
    } else if (GetOutputDevice() == TONEMAPPER_OUTPUT_ACES1000nitST2084 ||
               GetOutputDevice() == TONEMAPPER_OUTPUT_ACES1000nitScRGB) {
        FACESTonemapParams AcesParams =
            ComputeACESTonemapParams(ACESMinMaxData, ACESMidData, ACESCoefsLow_0, ACESCoefsHigh_0, ACESCoefsLow_4,
                                     ACESCoefsHigh_4, ACESSceneColorMultiplier, ACESGamutCompression);
        float3 ODTColor = ACESOutputTransforms1000(GradedColor, (float3x3)WorkingColorSpace.ToAP0, AcesParams);
        if (GetOutputDevice() != TONEMAPPER_OUTPUT_ACES1000nitScRGB) ODTColor = mul(AP1_2_Output, ODTColor);
        OutDeviceColor = LinearToST2084(ODTColor);
    } else if (GetOutputDevice() == TONEMAPPER_OUTPUT_ACES2000nitST2084 ||
               GetOutputDevice() == TONEMAPPER_OUTPUT_ACES2000nitScRGB) {
        FACESTonemapParams AcesParams =
            ComputeACESTonemapParams(ACESMinMaxData, ACESMidData, ACESCoefsLow_0, ACESCoefsHigh_0, ACESCoefsLow_4,
                                     ACESCoefsHigh_4, ACESSceneColorMultiplier, ACESGamutCompression);
        float3 ODTColor = ACESOutputTransforms2000(GradedColor, (float3x3)WorkingColorSpace.ToAP0, AcesParams);
        if (GetOutputDevice() != TONEMAPPER_OUTPUT_ACES2000nitScRGB) ODTColor = mul(AP1_2_Output, ODTColor);
        OutDeviceColor = LinearToST2084(ODTColor);
    } else if (GetOutputDevice() == TONEMAPPER_OUTPUT_LinearEXR) {
        float3 OutputGamutColor = mul(AP1_2_Output, mul((float3x3)WorkingColorSpace.ToAP1, GradedColor));
        OutDeviceColor = LinearToST2084(OutputGamutColor);
    } else if (GetOutputDevice() == TONEMAPPER_OUTPUT_NoToneCurve) {
        OutDeviceColor = GradedColor;
    } else if (GetOutputDevice() == TONEMAPPER_OUTPUT_WithToneCurve) {
        OutDeviceColor = mul(AP1_2_Output, mul((float3x3)WorkingColorSpace.ToAP1, FilmColorNoGamma));
    } else {
        float3 OutputGamutColor = mul(AP1_2_Output, mul((float3x3)WorkingColorSpace.ToAP1, FilmColor));
        OutDeviceColor = pow(OutputGamutColor, InverseGamma.z);
    }

    OutColor.rgb = OutDeviceColor / 1.05;
    OutColor.a = 0;
    return OutColor;
}

[numthreads(THREADGROUP_SIZE, THREADGROUP_SIZE, THREADGROUP_SIZE)]
void MainCS(uint3 DispatchThreadId : SV_DispatchThreadID) {
    float2 UV = ((float2)DispatchThreadId.xy + 0.5f) * OutputExtentInverse;
    uint LayerIndex = DispatchThreadId.z;
    RWOutputTexture[DispatchThreadId] = CombineLUTsCommon(UV, LayerIndex);
}

// Copied unchanged from crazyriddler/rexglue-native-kit @136bc6c4,
// reference/conan/port/src/native/shaders/blit.hlsl. Kit-authored helper shader (not game content).
// Fullscreen blit used by the native renderer to present a guest front buffer
// (any float-readable format) into the presenter's R10G10B10A2 guest output,
// applying the guest display gamma ramp (256-entry DC_LUT_30_COLOR table, as
// Xenos scans out 8_8_8_8 front buffers).
Texture2D<float4> g_Source : register(t0);
SamplerState g_Sampler : register(s0);
cbuffer GammaRamp : register(b0) {
  uint4 g_GammaTable[64];  // entry i: blue bits 0-9, green 10-19, red 20-29
  uint g_GammaEnabled;
  // DISPLAY TUNING (graphics_menu.cpp): 1, 1 = unchanged.
  float g_Brightness;  // display gamma: c^(1/b)
  float g_Contrast;    // a curve through 0, kContrastPivot and 1 (exponent)
};

// The game's frames average about 0.28 in display values (title screen,
// 2026-11-13): turning around this keeps the average brightness (+-0.003 at
// both ends) where a turn around 0.5 lightened the frame by 38% at 0.5.
static const float kContrastPivot = 0.3;

void VSMain(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
  uv = float2((id << 1) & 2, id & 2);
  pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

uint GammaEntry(float v) {
  uint i = uint(round(saturate(v) * 255.0));
  return g_GammaTable[i >> 2][i & 3];
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float3 c = g_Source.SampleLevel(g_Sampler, uv, 0.0).rgb;
  if (g_GammaEnabled != 0) {
    c = float3((GammaEntry(c.r) >> 20) & 0x3FF, (GammaEntry(c.g) >> 10) & 0x3FF,
               GammaEntry(c.b) & 0x3FF) / 1023.0;
  }
  if (g_Brightness != 1.0 || g_Contrast != 1.0) {
    c = pow(max(c, 1e-6), 1.0 / max(g_Brightness, 0.01));
    // black stays black and white white: below the pivot p * (c / p)^k, above
    // the same mirrored
    const float p = kContrastPivot;
    c = saturate(c);
    float3 lo = p * pow(max(c / p, 1e-6), g_Contrast);
    float3 hi = 1.0 - (1.0 - p) * pow(max((1.0 - c) / (1.0 - p), 1e-6), g_Contrast);
    c = saturate(select(c < p, lo, hi));
  }
  return float4(c, 1.0);
}

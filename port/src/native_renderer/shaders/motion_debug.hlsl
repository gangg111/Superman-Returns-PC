// DLSS debug (sr_dlss_debug=1): the motion vectors as a picture in place of
// the scene. Red/green: x/y motion, 0.5 = none, 8 render pixels = full; blue:
// the pixel has its draw's own (objects') motion, black there = the camera's.
//
// Compile: dxc -T cs_6_0 -E main -Fh motion_debug_cs.h -Vn kMotionDebugCS motion_debug.hlsl

Texture2D<float2> g_Motion : register(t0);
RWTexture2D<float4> g_Out : register(u0);
Texture2D<float2> g_Objects : register(t1);

cbuffer Constants : register(b0) {
  float4 g_Unused[8];
  float2 g_Size;
  float2 g_Pad;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= uint(g_Size.x) || id.y >= uint(g_Size.y)) return;
  float2 m = g_Motion.Load(int3(id.xy, 0));
  float mark = g_Objects.Load(int3(id.xy, 0)).x;
  bool own = mark < 60000.0;
  // Pure green: not a number (a bad motion written by a draw).
  if (isnan(mark) || isinf(mark)) {
    g_Out[id.xy] = float4(0.0, 1.0, 0.0, 1.0);
    return;
  }
  // Pure red: a draw with a previous frame whose previous position fell behind the camera.
  if (mark > 63000.0 && mark < 64500.0) {
    g_Out[id.xy] = float4(1.0, 0.0, 0.0, 1.0);
    return;
  }
  g_Out[id.xy] = float4(saturate(0.5 + m / 16.0), own ? 1.0 : 0.0, 1.0);
}

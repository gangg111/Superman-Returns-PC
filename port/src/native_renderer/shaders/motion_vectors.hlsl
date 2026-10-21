// Camera motion vectors for DLSS, from the scene depth: each pixel's surface
// point is placed in the world with the current frame's camera and projected
// with the previous frame's. The result is in render pixels, previous minus
// current position, without the projection jitter (DLSS's convention).
//
// Compile: dxc -T cs_6_0 -E main -Fh motion_vectors_cs.h -Vn kMotionVectorsCS motion_vectors.hlsl

Texture2D<float> g_Depth : register(t0);
RWTexture2D<float2> g_Motion : register(u0);
// Objects' motion from the scene's own draws (NDC, previous minus current),
// 65000 in x where a draw had no previous frame (and where nothing was drawn).
Texture2D<float2> g_Objects : register(t1);

cbuffer Constants : register(b0) {
  row_major float4x4 g_ClipToWorld;  // inverse of the current (unjittered) view-projection
  row_major float4x4 g_PrevWorldToClip;  // previous view-projection
  float2 g_Size;  // render pixels
  float2 g_Jitter;  // this frame's jitter in render pixels (x right, y down)
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= uint(g_Size.x) || id.y >= uint(g_Size.y)) return;
  float2 objects = g_Objects.Load(int3(id.xy, 0));
  if (objects.x < 60000.0) {
    // NDC (y up) -> pixels (y down).
    g_Motion[id.xy] = float2(objects.x * 0.5 * g_Size.x, -objects.y * 0.5 * g_Size.y);
    return;
  }
  float depth = g_Depth.Load(int3(id.xy, 0));
  // The surface seen at this pixel's center was drawn with the projection
  // moved by the jitter: without it the same point sits at center - jitter.
  float2 pixel = float2(id.xy) + 0.5 - g_Jitter;
  float2 ndc = float2(pixel.x / g_Size.x * 2.0 - 1.0, 1.0 - pixel.y / g_Size.y * 2.0);
  float4 world = mul(g_ClipToWorld, float4(ndc, depth, 1.0));
  world /= world.w;
  float4 prev = mul(g_PrevWorldToClip, world);
  float2 prev_ndc = prev.xy / prev.w;
  float2 prev_pixel = float2((prev_ndc.x * 0.5 + 0.5) * g_Size.x, (0.5 - prev_ndc.y * 0.5) * g_Size.y);
  // Behind the previous camera: no history to point at.
  float2 motion = prev.w > 0.0 ? prev_pixel - pixel : float2(0.0, 0.0);
  g_Motion[id.xy] = motion;
}

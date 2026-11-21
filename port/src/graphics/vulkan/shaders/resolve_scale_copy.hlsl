// A colour resolve with RB_COPY_DEST_INFO copy_dest_exp_bias: Xenos multiplies the copied colour by
// 2^bias. The only one in the game is the sun's shadow map (k_16_16 target, bias -5: -32..32 becomes
// -1..1 for its Poisson filter and the receivers); a plain copy left it 32 times too large and the
// filtered map saturated, so its shadows slid with the camera. The PC renderer's ScaleResolveLayer
// (native_renderer.cpp) does the same after its copy; here the copy and the scale are one dispatch.
[[vk::binding(0,0)]] Texture2D<float4> Source;
[[vk::binding(1,0)]] [[vk::image_format("rg32f")]] RWTexture2D<float2> Destination;
struct Region {int2 source;uint2 size;int2 destination;float scale;uint pad;};
[[vk::push_constant]] ConstantBuffer<Region> region;
[numthreads(8,8,1)]
void CSMain(uint3 id:SV_DispatchThreadID) {
  if(any(id.xy>=region.size)) return;
  Destination[region.destination+int2(id.xy)]=Source.Load(int3(region.source+int2(id.xy),0)).xy*region.scale;
}

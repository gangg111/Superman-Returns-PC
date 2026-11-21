// depth_resolve.hlsl writing straight into the two resolve textures instead of a scratch buffer that
// two vkCmdCopyBufferToImage then copied (mobile renderer plan, E6: the two depth conversions of a
// city frame were the most expensive resolves). Same values: OutDepth (R32_SFLOAT) gets the depth,
// OutRaw (R8G8B8A8_UNORM) the bytes of stencil | d24 << 8 (little-endian: R = stencil, G..A = d24
// from the low byte up); a byte k stored as k / 255 reads back as k.
[[vk::binding(0,0)]] Texture2D<float> SourceDepth;
[[vk::binding(1,0)]] Texture2D<uint> SourceStencil;
[[vk::binding(2,0)]] [[vk::image_format("r32f")]] RWTexture2D<float> OutDepth;
[[vk::binding(3,0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> OutRaw;
struct Region {int2 source;uint2 size;int2 destination;uint raw;uint pad;};
[[vk::push_constant]] ConstantBuffer<Region> region;
[numthreads(8,8,1)]
void CSMain(uint3 id:SV_DispatchThreadID) {
  if(any(id.xy>=region.size)) return;
  int3 p=int3(region.source+int2(id.xy),0);
  float depth=SourceDepth.Load(p);
  uint stencil=SourceStencil.Load(p)&255;
  int2 d=region.destination+int2(id.xy);
  OutDepth[d]=depth;
  if(region.raw!=0) {
    uint d24=uint(round(saturate(depth)*16777215.0));
    OutRaw[d]=float4(stencil,d24&255,(d24>>8)&255,d24>>16)/255.0;
  }
}

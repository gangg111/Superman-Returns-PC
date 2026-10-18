#include "game_pipeline.h"
#include "triangle.h"
#include "device_requirements.h"
#include <chrono>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <optional>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace superman_returns::graphics::vulkan {
namespace {
bool Fail(Error& e,const char* text) {e={"Game pipeline",VK_ERROR_INITIALIZATION_FAILED,text};return false;}
VkBlendFactor Factor(uint32_t f,bool alpha) {
  switch(f) {
  case 0:return VK_BLEND_FACTOR_ZERO;case 1:return VK_BLEND_FACTOR_ONE;
  case 4:return alpha?VK_BLEND_FACTOR_SRC_ALPHA:VK_BLEND_FACTOR_SRC_COLOR;
  case 5:return alpha?VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
  case 6:return VK_BLEND_FACTOR_SRC_ALPHA;case 7:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  case 8:return alpha?VK_BLEND_FACTOR_DST_ALPHA:VK_BLEND_FACTOR_DST_COLOR;
  case 9:return alpha?VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
  case 10:return VK_BLEND_FACTOR_DST_ALPHA;case 11:return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  case 12:case 14:return VK_BLEND_FACTOR_CONSTANT_COLOR;
  case 13:case 15:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
  case 16:return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;default:return VK_BLEND_FACTOR_ONE;
  }
}
VkBlendOp BlendOp(uint32_t n) {constexpr VkBlendOp ops[]{VK_BLEND_OP_ADD,VK_BLEND_OP_SUBTRACT,VK_BLEND_OP_MIN,VK_BLEND_OP_MAX,VK_BLEND_OP_REVERSE_SUBTRACT};return ops[n<5?n:0];}
VkStencilOp StencilOp(uint32_t n) {constexpr VkStencilOp ops[]{VK_STENCIL_OP_KEEP,VK_STENCIL_OP_ZERO,VK_STENCIL_OP_REPLACE,VK_STENCIL_OP_INCREMENT_AND_CLAMP,VK_STENCIL_OP_DECREMENT_AND_CLAMP,VK_STENCIL_OP_INVERT,VK_STENCIL_OP_INCREMENT_AND_WRAP,VK_STENCIL_OP_DECREMENT_AND_WRAP};return ops[n&7];}
std::optional<uint32_t> Location(const guest::VertexAttribute& a) {
  switch(a.usage) {
  case 0:if(a.index<5) return a.index;break;
  case 3:if(a.index<2) return 5+a.index;break;
  case 6:if(!a.index) return 7;break;case 7:if(!a.index) return 8;break;
  case 2:if(!a.index) return 9;break;case 1:if(!a.index) return 10;break;
  case 10:if(a.index<2) return 11+a.index;break;case 5:if(a.index<16) return 13+a.index;break;
  }
  return {};
}
VkFormat VertexFormat(uint32_t t) {
  switch(t) {
  case 0x2c83a4:return VK_FORMAT_R32_SFLOAT;case 0x2c23a5:return VK_FORMAT_R32G32_SFLOAT;
  case 0x2a23b9:return VK_FORMAT_R32G32B32_SFLOAT;case 0x1a23a6:return VK_FORMAT_R32G32B32A32_SFLOAT;
  case 0x182886:return VK_FORMAT_B8G8R8A8_UNORM;
  case 0x1a2286:case 0x1a2386:return VK_FORMAT_R8G8B8A8_UINT;
  case 0x1a2086:case 0x1a2186:return VK_FORMAT_R8G8B8A8_UNORM;
  case 0x2c2359:return VK_FORMAT_R16G16_SINT;
  case 0x2c2259:case 0x2c2059:return VK_FORMAT_R16G16_UNORM;
  case 0x1a235a:case 0x1a215a:return VK_FORMAT_R16G16B16A16_SNORM;
  case 0x2c2159:return VK_FORMAT_R16G16_SNORM;case 0x1a205a:return VK_FORMAT_R16G16B16A16_UNORM;
  case 0x2c82a1:case 0x2a2187:case 0x2a2190:case 0x2a2390:return VK_FORMAT_R32_UINT;
  case 0x2c235f:return VK_FORMAT_R16G16_SFLOAT;case 0x1a2360:return VK_FORMAT_R16G16B16A16_SFLOAT;
  default:return VK_FORMAT_UNDEFINED;
  }
}
uint64_t ShaderDigest(const shaders::CompiledShader& s) {uint64_t h=14695981039346656037ull;for(auto word:s.words) {h^=word;h*=1099511628211ull;}return h;}
std::vector<uint32_t> CacheHeader(const Context& c) {
  std::vector<uint32_t> header{0x33435053,1,c.properties.vendorID,c.properties.deviceID,c.properties.driverVersion};
  for(uint32_t i=0;i<4;++i) {uint32_t word;std::memcpy(&word,c.properties.pipelineCacheUUID+i*4,4);header.push_back(word);}return header;
}
}
std::string ShaderEntryPoint(const shaders::CompiledShader& shader,Error& e) {
  auto fail=[&](const char* message){Fail(e,message);return std::string{};};
  if(shader.words.size()<5 || shader.words[0]!=0x07230203) return fail("Shader has no SPIR-V header");
  if(shader.stage!=shaders::ShaderStage::kVertex && shader.stage!=shaders::ShaderStage::kPixel) return fail("Unsupported shader stage");
  std::string entry;uint32_t expected=shader.stage==shaders::ShaderStage::kVertex?0:4;
  for(size_t i=5;i<shader.words.size();) {
    auto instruction=shader.words[i];uint32_t count=instruction>>16,opcode=instruction&65535;
    if(!count || count>shader.words.size()-i) return fail("Truncated SPIR-V instruction");
    if(opcode==15) {
      if(count<4 || shader.words[i+1]!=expected || !entry.empty()) return fail("SPIR-V entry point stage/count mismatch");
      const auto* bytes=reinterpret_cast<const char*>(shader.words.data()+i+3);size_t capacity=(count-3)*4,length=0;
      while(length<capacity && bytes[length]) ++length;
      if(!length || length==capacity || length>256) return fail("Invalid SPIR-V entry point name");
      entry.assign(bytes,length);
    }
    i+=count;
  }
  if(entry.empty()) return fail("SPIR-V shader has no entry point");e={};return entry;
}
bool PlanGamePipeline(const guest::DrawPacket& draw,const TargetPass& pass,const shaders::CompiledShader& vs,GamePipelinePlan& result,Error& e) {
  if(pass.color_count>4) return Fail(e,"Too many color attachments");
  GamePipelinePlan out;
  switch(draw.primitive) {
  case guest::Primitive::kPoints:out.topology=VK_PRIMITIVE_TOPOLOGY_POINT_LIST;break;
  case guest::Primitive::kLines:out.topology=VK_PRIMITIVE_TOPOLOGY_LINE_LIST;break;
  case guest::Primitive::kLineStrip:out.topology=VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;break;
  case guest::Primitive::kTriangles:out.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;break;
  case guest::Primitive::kTriangleStrip:out.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;break;
  default:return Fail(e,"Primitive requires guest expansion before pipeline creation");
  }
  uint32_t blend=draw.registers[0x201],mode=draw.registers[0x205],depth=draw.registers[0x200],mask=draw.registers[0x104],stencil=draw.registers[0x10d];
  out.restart=draw.indexed && draw.primitive_restart && (out.topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || out.topology==VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
  out.raster.polygonMode=VK_POLYGON_MODE_FILL;out.raster.lineWidth=1;
  out.raster.cullMode=(mode&3)==1?VK_CULL_MODE_FRONT_BIT:(mode&3)==2?VK_CULL_MODE_BACK_BIT:VK_CULL_MODE_NONE;
  out.raster.frontFace=(mode&4)?VK_FRONT_FACE_CLOCKWISE:VK_FRONT_FACE_COUNTER_CLOCKWISE;
  out.raster.depthBiasConstantFactor=float(draw.depth_bias);out.raster.depthBiasSlopeFactor=draw.slope_bias;out.raster.depthBiasEnable=draw.depth_bias!=0 || draw.slope_bias!=0;
  auto& ds=out.depth;bool has_depth=pass.depth_format!=VK_FORMAT_UNDEFINED;
  ds.depthTestEnable=has_depth && (depth&2);ds.depthWriteEnable=has_depth && (depth&4);ds.depthCompareOp=VkCompareOp((depth>>4)&7);ds.stencilTestEnable=has_depth && (depth&1);
  auto face=[&](uint32_t shift) {VkStencilOpState s{};s.compareOp=VkCompareOp((depth>>shift)&7);s.failOp=StencilOp(depth>>(shift+3));s.passOp=StencilOp(depth>>(shift+6));s.depthFailOp=StencilOp(depth>>(shift+9));s.compareMask=(stencil>>8)&255;s.writeMask=(stencil>>16)&255;return s;};
  ds.front=face(8);ds.back=(depth&128)?face(20):ds.front;
  for(uint32_t i=0;i<pass.color_count;++i) {
    auto& c=out.colors[i];c.srcColorBlendFactor=Factor(blend&31,false);c.dstColorBlendFactor=Factor((blend>>8)&31,false);c.colorBlendOp=BlendOp((blend>>5)&7);
    c.srcAlphaBlendFactor=Factor((blend>>16)&31,true);c.dstAlphaBlendFactor=Factor((blend>>24)&31,true);c.alphaBlendOp=BlendOp((blend>>21)&7);c.colorWriteMask=(mask>>(i*4))&15;
    c.blendEnable=!(c.srcColorBlendFactor==VK_BLEND_FACTOR_ONE && c.dstColorBlendFactor==VK_BLEND_FACTOR_ZERO && c.colorBlendOp==VK_BLEND_OP_ADD && c.srcAlphaBlendFactor==VK_BLEND_FACTOR_ONE && c.dstAlphaBlendFactor==VK_BLEND_FACTOR_ZERO && c.alphaBlendOp==VK_BLEND_OP_ADD);
    if(pass.formats[i]==VK_FORMAT_UNDEFINED) c.colorWriteMask=0;
    if(pass.formats[i]==VK_FORMAT_R32_SFLOAT || pass.formats[i]==VK_FORMAT_R32G32_SFLOAT) c.blendEnable=false;
  }
  for(auto& input:vs.inputs) {
    auto found=std::find_if(draw.attributes.begin(),draw.attributes.end(),[&](auto& a){return Location(a)==input.location;});
    VkVertexInputAttributeDescription attribute{input.location,31,VK_FORMAT_R32G32B32A32_SFLOAT,0};uint32_t stride=0;
    if(found!=draw.attributes.end()) {
      attribute.binding=draw.inline_vertices?0:found->stream;attribute.format=VertexFormat(found->type);attribute.offset=found->offset;
      if(draw.inline_vertices) stride=draw.inline_stride;
      else {auto stream=std::find_if(draw.streams.begin(),draw.streams.end(),[&](auto& s){return s.stream==found->stream;});if(stream==draw.streams.end()) return Fail(e,"Vertex input stream absent");stride=stream->stride;}
      if(attribute.binding>=31 || attribute.format==VK_FORMAT_UNDEFINED || !stride) return Fail(e,"Vertex input type/binding/stride unsupported");
    } else if(input.type.starts_with("uint")) attribute.format=VK_FORMAT_R32G32B32A32_UINT;
    else if(input.type.starts_with("int")) attribute.format=VK_FORMAT_R32G32B32A32_SINT;
    if(std::none_of(out.bindings.begin(),out.bindings.end(),[&](auto& b){return b.binding==attribute.binding;})) out.bindings.push_back({attribute.binding,stride,VK_VERTEX_INPUT_RATE_VERTEX});
    out.attributes.push_back(attribute);
  }
  out.key={1,uint64_t(out.topology),out.restart,blend,mode,depth,stencil&0xffff00,mask,pass.color_count,uint64_t(pass.depth_format),uint32_t(draw.depth_bias),std::bit_cast<uint32_t>(draw.slope_bias)};
  for(auto f:pass.formats) out.key.push_back(uint64_t(f));
  for(auto& b:out.bindings) {out.key.push_back(b.binding);out.key.push_back(b.stride);}
  out.key.push_back(UINT64_MAX);
  for(auto& a:out.attributes) {out.key.push_back(a.location);out.key.push_back(a.binding);out.key.push_back(a.format);out.key.push_back(a.offset);}
  result=std::move(out);e={};return true;
}
GamePipeline::~GamePipeline() {if(handle) context->f.vkDestroyPipeline(context->device,handle,nullptr);}
bool GamePipelineMemo::Matches(const guest::DrawPacket& d,const TargetPass& pass,const Shader& vs,const Shader& ps) const {
  if(!pipeline_) return false;
  const auto& l=in_;
  if(l.vs!=vs || l.ps!=ps || l.primitive!=d.primitive || l.indexed!=d.indexed || l.restart!=d.primitive_restart || l.inline_vertices!=d.inline_vertices || l.inline_stride!=d.inline_stride ||
     l.r200!=d.registers[0x200] || l.r201!=d.registers[0x201] || l.r205!=d.registers[0x205] || l.r104!=d.registers[0x104] || l.r10d!=d.registers[0x10d] ||
     l.depth_bias!=d.depth_bias || l.slope_bits!=std::bit_cast<uint32_t>(d.slope_bias) ||
     l.color_count!=pass.color_count || l.formats!=pass.formats || l.depth_format!=pass.depth_format ||
     l.attributes.size()!=d.attributes.size() || l.streams.size()!=d.streams.size()) return false;
  static_assert(sizeof(guest::VertexAttribute)==5*sizeof(uint32_t),"VertexAttribute is compared byte-wise");
  if(!d.attributes.empty() && std::memcmp(l.attributes.data(),d.attributes.data(),d.attributes.size()*sizeof(guest::VertexAttribute))) return false;
  for(size_t i=0;i<d.streams.size();++i) if(l.streams[i].first!=d.streams[i].stream || l.streams[i].second!=d.streams[i].stride) return false;
  return true;
}
void GamePipelineMemo::Store(const guest::DrawPacket& d,const TargetPass& pass,const Shader& vs,const Shader& ps,std::shared_ptr<GamePipeline> pipeline) {
  auto& l=in_;l.vs=vs;l.ps=ps;l.primitive=d.primitive;l.indexed=d.indexed;l.restart=d.primitive_restart;l.inline_vertices=d.inline_vertices;l.inline_stride=d.inline_stride;
  l.r200=d.registers[0x200];l.r201=d.registers[0x201];l.r205=d.registers[0x205];l.r104=d.registers[0x104];l.r10d=d.registers[0x10d];
  l.depth_bias=d.depth_bias;l.slope_bits=std::bit_cast<uint32_t>(d.slope_bias);
  l.color_count=pass.color_count;l.formats=pass.formats;l.depth_format=pass.depth_format;
  l.attributes.assign(d.attributes.begin(),d.attributes.end());
  l.streams.clear();for(auto& s:d.streams) l.streams.emplace_back(s.stream,s.stride);
  pipeline_=std::move(pipeline);
}
bool ValidateGamePipelineFeatures(const GamePipelinePlan& plan,uint32_t colors,const VkPhysicalDeviceLimits& limits,const VkPhysicalDeviceFeatures& features,Error& e) {
  if(colors>4 || colors>limits.maxColorAttachments) {e={"Game pipeline limits",VK_ERROR_FEATURE_NOT_PRESENT,"Color attachment count exceeds device limits"};return false;}
  for(auto& binding:plan.bindings) if(binding.binding>=limits.maxVertexInputBindings || binding.stride>limits.maxVertexInputBindingStride) {e={"Game pipeline limits",VK_ERROR_FEATURE_NOT_PRESENT,"Vertex binding or stride exceeds device limits"};return false;}
  for(auto& attribute:plan.attributes) if(attribute.location>=limits.maxVertexInputAttributes || attribute.offset>limits.maxVertexInputAttributeOffset) {e={"Game pipeline limits",VK_ERROR_FEATURE_NOT_PRESENT,"Vertex location or offset exceeds device limits"};return false;}
  if(!features.independentBlend && colors>1) for(uint32_t i=1;i<colors;++i) {
    auto& a=plan.colors[0];auto& b=plan.colors[i];
    if(a.blendEnable!=b.blendEnable || a.srcColorBlendFactor!=b.srcColorBlendFactor || a.dstColorBlendFactor!=b.dstColorBlendFactor || a.colorBlendOp!=b.colorBlendOp || a.srcAlphaBlendFactor!=b.srcAlphaBlendFactor || a.dstAlphaBlendFactor!=b.dstAlphaBlendFactor || a.alphaBlendOp!=b.alphaBlendOp || a.colorWriteMask!=b.colorWriteMask) {e={"Game pipeline feature",VK_ERROR_FEATURE_NOT_PRESENT,"MRT state requires enabled independentBlend"};return false;}
  }
  e={};return true;
}
GamePipelineStore::~GamePipelineStore() {
  if(!c_.device) return;auto waited=c_.f.vkDeviceWaitIdle(c_.device);pending_.Retire(UINT64_MAX);memo_.Clear();pipelines_.clear();
  if(driver_cache_) {
    if(waited==VK_SUCCESS) {Error e;if(!CheckpointCache(e)) c_.Log(e.operation+": "+e.message);}
    c_.f.vkDestroyPipelineCache(c_.device,driver_cache_,nullptr);
  }
  if(layout_) c_.f.vkDestroyPipelineLayout(c_.device,layout_,nullptr);
}
bool GamePipelineStore::CheckpointCache(Error& e) {
  e={};if(cache_path_.empty() || !cache_dirty_) return true;
  if(!driver_cache_) return Fail(e,"Driver cache is not initialized");
  size_t size=0;
  if(!Check(c_.f.vkGetPipelineCacheData(c_.device,driver_cache_,&size,nullptr),"Read driver cache size",e)) return false;
  if(size<32 || size>64*1024*1024-36) return Fail(e,"Driver cache size is outside the bounded file format");
  std::vector<uint8_t> bytes(size);
  if(!Check(c_.f.vkGetPipelineCacheData(c_.device,driver_cache_,&size,bytes.data()),"Read driver cache data",e)) return false;
  if(size>bytes.size() || size<32) return Fail(e,"Driver returned an invalid cache length");
  auto temporary=cache_path_;temporary+=".tmp";
  std::error_code ec;
  if(!cache_path_.parent_path().empty()) {std::filesystem::create_directories(cache_path_.parent_path(),ec);if(ec) return Fail(e,"Cannot create driver cache directory");}
  const auto header=CacheHeader(c_);
  std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
  file.write(reinterpret_cast<const char*>(header.data()),header.size()*4);
  file.write(reinterpret_cast<const char*>(bytes.data()),size);file.flush();
  const bool written=bool(file);file.close();
  if(!written || file.fail()) {std::filesystem::remove(temporary,ec);return Fail(e,"Cannot write driver cache checkpoint");}
#if defined(_WIN32)
  const bool replaced=MoveFileExW(temporary.c_str(),cache_path_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
  std::filesystem::rename(temporary,cache_path_,ec);const bool replaced=!ec;
#endif
  if(!replaced) {std::filesystem::remove(temporary,ec);return Fail(e,"Cannot replace driver cache checkpoint");}
  cache_dirty_=false;return true;
}
bool GamePipelineStore::Initialize(std::span<const VkDescriptorSetLayout> layouts,const std::filesystem::path& path,Error& e) {
  if(layout_ || layouts.size()!=4) return Fail(e,"Invalid/already initialized pipeline layout");
  VkPipelineLayoutCreateInfo l{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};l.setLayoutCount=uint32_t(layouts.size());l.pSetLayouts=layouts.data();
  if(!Check(c_.f.vkCreatePipelineLayout(c_.device,&l,nullptr,&layout_),"Game pipeline layout",e)) return false;
  std::vector<uint8_t> initial;auto expected=CacheHeader(c_);
  if(!path.empty()) {std::ifstream file(path,std::ios::binary|std::ios::ate);auto size=file.tellg();if(file && size>=std::streamoff(expected.size()*4+32) && size<64*1024*1024) {file.seekg(0);std::vector<uint32_t> header(expected.size());file.read(reinterpret_cast<char*>(header.data()),header.size()*4);if(header==expected) {initial.resize(size_t(size)-header.size()*4);if(!file.read(reinterpret_cast<char*>(initial.data()),initial.size())) initial.clear();}}}
  // Vulkan's own header must agree too; never pass arbitrary/truncated bytes to the driver.
  if(!initial.empty()) {uint32_t fields[4];std::memcpy(fields,initial.data(),16);if(fields[0]<32 || fields[0]>initial.size() || fields[1]!=VK_PIPELINE_CACHE_HEADER_VERSION_ONE || fields[2]!=c_.properties.vendorID || fields[3]!=c_.properties.deviceID || std::memcmp(initial.data()+16,c_.properties.pipelineCacheUUID,16)) initial.clear();}
  VkPipelineCacheCreateInfo cache{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};cache.initialDataSize=initial.size();cache.pInitialData=initial.data();
  if(!Check(c_.f.vkCreatePipelineCache(c_.device,&cache,nullptr,&driver_cache_),"Vulkan driver pipeline cache",e)) return false;
  cache_path_=path;e={};return true;
}
bool GamePipelineStore::Validate(const shaders::CompiledShader& vs,const shaders::CompiledShader* ps,Error& e) const {
  if(!layout_ || !ValidSpirv(vs.words) || (ps && !ValidSpirv(ps->words))) return Fail(e,"Pipeline layout/serial/shaders invalid");
  shaders::ShaderRequirements empty;auto errors=CheckDeviceRequirements(vs.requirements,ps?ps->requirements:empty,GameBindingLayout(),GetDeviceCaps(c_.properties.limits,c_.enabled_features));
  if(ps) {auto pair=shaders::ValidateShaderPair(vs,*ps);errors.insert(errors.end(),pair.begin(),pair.end());}
  if(!errors.empty()) {e={"Game pipeline requirements",VK_ERROR_FEATURE_NOT_PRESENT,errors.front()};return false;}
  return true;
}
std::shared_ptr<GamePipeline> GamePipelineStore::Acquire(const guest::DrawPacket& d,const TargetPass& pass,const shaders::CompiledShader& vs,const shaders::CompiledShader* ps,uint64_t serial,Error& e) {
  if(!serial) {Fail(e,"Pipeline layout/serial/shaders invalid");return {};}
  if(!Validate(vs,ps,e)) return {};
  return AcquireValidated(d,pass,vs,ps,ShaderDigest(vs),ps?ShaderDigest(*ps):0,serial,e);
}
std::shared_ptr<GamePipeline> GamePipelineStore::Acquire(const guest::DrawPacket& d,const TargetPass& pass,const std::shared_ptr<const shaders::CompiledShader>& vs,const std::shared_ptr<const shaders::CompiledShader>& ps,uint64_t serial,Error& e) {
  if(!vs || !serial) {Fail(e,"Pipeline layout/serial/shaders invalid");return {};}
  if(memo_.Matches(d,pass,vs,ps)) {++stats_.memo_hits;pending_.Hold(serial,memo_.pipeline());e={};return memo_.pipeline();}
  // Retained artifacts keep their addresses unique for the cache lifetime.
  auto key=std::pair{vs.get(),ps.get()};auto found=validated_.find(key);
  if(found==validated_.end()) {
    if(!Validate(*vs,ps.get(),e)) return {};
    found=validated_.emplace(key,ShaderPair{vs,ps,ShaderDigest(*vs),ps?ShaderDigest(*ps):0}).first;
  }
  auto result=AcquireValidated(d,pass,*vs,ps.get(),found->second.vs_digest,found->second.ps_digest,serial,e);
  if(result) memo_.Store(d,pass,vs,ps,result);
  return result;
}
std::shared_ptr<GamePipeline> GamePipelineStore::AcquireValidated(const guest::DrawPacket& d,const TargetPass& pass,const shaders::CompiledShader& vs,const shaders::CompiledShader* ps,uint64_t vs_digest,uint64_t ps_digest,uint64_t serial,Error& e) {
  ++stats_.plans;GamePipelinePlan plan;if(!PlanGamePipeline(d,pass,vs,plan,e)) return {};
  if(!ValidateGamePipelineFeatures(plan,pass.color_count,c_.properties.limits,c_.enabled_features,e)) return {};
  plan.key.push_back(vs_digest);plan.key.push_back(ps_digest);
  if(auto found=pipelines_.find(plan.key);found!=pipelines_.end()) {pending_.Hold(serial,found->second);return found->second;}
  for(auto& a:plan.attributes) {VkFormatProperties props{};c_.f.vkGetPhysicalDeviceFormatProperties(c_.physical,a.format,&props);if(!(props.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {Fail(e,"Vertex format unsupported by device");return {};}}
  VkShaderModule modules[2]{};auto release=[&]{for(auto m:modules) if(m) c_.f.vkDestroyShaderModule(c_.device,m,nullptr);};
  VkPipelineShaderStageCreateInfo stages[2]{};std::array<std::string,2> entries;uint32_t stage_count=ps?2:1;
  for(uint32_t i=0;i<stage_count;++i) {auto& s=i?*ps:vs;entries[i]=ShaderEntryPoint(s,e);if(entries[i].empty()) {release();return {};};VkShaderModuleCreateInfo m{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};m.codeSize=s.words.size()*4;m.pCode=s.words.data();if(!Check(c_.f.vkCreateShaderModule(c_.device,&m,nullptr,&modules[i]),"Game shader module",e)) {release();return {};}stages[i]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=modules[i];stages[i].pName=entries[i].c_str();}
  VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};vertex.vertexBindingDescriptionCount=uint32_t(plan.bindings.size());vertex.pVertexBindingDescriptions=plan.bindings.data();vertex.vertexAttributeDescriptionCount=uint32_t(plan.attributes.size());vertex.pVertexAttributeDescriptions=plan.attributes.data();
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=plan.topology;assembly.primitiveRestartEnable=plan.restart;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=viewport.scissorCount=1;
  VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=pass.color_count;blend.pAttachments=plan.colors.data();
  VkDynamicState dynamic[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_BLEND_CONSTANTS,VK_DYNAMIC_STATE_STENCIL_REFERENCE};VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dyn.dynamicStateCount=4;dyn.pDynamicStates=dynamic;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.stageCount=stage_count;info.pStages=stages;info.pVertexInputState=&vertex;info.pInputAssemblyState=&assembly;info.pViewportState=&viewport;info.pRasterizationState=&plan.raster;info.pMultisampleState=&samples;info.pDepthStencilState=&plan.depth;info.pColorBlendState=&blend;info.pDynamicState=&dyn;info.layout=layout_;info.renderPass=pass.render_pass;
  auto p=std::make_shared<GamePipeline>();p->context=&c_;p->bindings=plan.bindings;
  const auto create_started=std::chrono::steady_clock::now();
  auto status=c_.f.vkCreateGraphicsPipelines(c_.device,driver_cache_,1,&info,nullptr,&p->handle);release();if(!Check(status,"Create game graphics pipeline",e)) return {};
  // A slow creation is a driver compile the cache did not cover: a visible hitch.
  if(const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-create_started).count();ms>10)
    c_.Log("Slow game pipeline creation: "+std::to_string(int(ms))+" ms (vs "+std::to_string(vs_digest)+", pipelines "+std::to_string(stats_.created+1)+")");
  pipelines_[plan.key]=p;cache_dirty_=true;++stats_.created;pending_.Hold(serial,p);e={};return p;
}
}

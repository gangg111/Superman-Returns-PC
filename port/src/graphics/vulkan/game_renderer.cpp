#include "game_renderer.h"
#include "resolve.h"
#include "../guest/primitive_expansion.h"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>
namespace superman_returns::graphics::vulkan {
namespace {
uint32_t DumpTexel(VkFormat f) {
  switch(f) {
  case VK_FORMAT_R8_UNORM:return 1;case VK_FORMAT_R8G8_UNORM:case VK_FORMAT_R5G6B5_UNORM_PACK16:case VK_FORMAT_A1R5G5B5_UNORM_PACK16:case VK_FORMAT_R16_UNORM:case VK_FORMAT_R16_SFLOAT:return 2;
  case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_B8G8R8A8_UNORM:case VK_FORMAT_A2B10G10R10_UNORM_PACK32:case VK_FORMAT_R16G16_SNORM:case VK_FORMAT_R16G16_UNORM:case VK_FORMAT_R16G16_SFLOAT:case VK_FORMAT_R32_SFLOAT:case VK_FORMAT_D24_UNORM_S8_UINT:return 4;
  case VK_FORMAT_R16G16B16A16_SFLOAT:case VK_FORMAT_R16G16B16A16_UNORM:case VK_FORMAT_R32G32_SFLOAT:return 8;
  case VK_FORMAT_R32G32B32A32_SFLOAT:return 16;
  default:return 0;
  }
}
std::string Hex(uint64_t v) {std::ostringstream s;s<<std::hex<<v;return s.str();}
std::string Surface(const guest::SurfaceDesc& d) {
  if(!d.id) return "-";
  std::ostringstream s;s<<"e"<<d.geometry.edram_base<<"/f"<<d.geometry.format<<"/"<<d.geometry.width<<"x"<<d.geometry.height<<(d.depth?"D":"");return s.str();
}
constexpr guest::ResourceId MissingVertex=UINT64_MAX-4;
bool Fail(Error& e,const char* message) {e={"Game renderer",VK_ERROR_INITIALIZATION_FAILED,message};return false;}
}
bool GameRenderer::Initialize(const std::filesystem::path& path,Error& e) {return descriptors_.Initialize(e) && pipelines_.Initialize(descriptors_.Layouts(),path,e);}
// SR_VULKAN_NO_STATE_FILTER=1 turns the redundant-command filter off (A/B diagnostics).
bool GameRenderer::StateFilterEnabled() {
  const char* value=std::getenv("SR_VULKAN_NO_STATE_FILTER");
  return !(value && value[0] && value[0]!='0');
}
bool GameRenderer::AliasCheckEnabled() {
  const char* value=std::getenv("SR_VULKAN_CHECK_ALIASES");
  return value && value[0] && value[0]!='0';
}
bool GameRenderer::ConstantBankCacheEnabled() {
  const char* value=std::getenv("SR_VULKAN_CONSTANT_BANK_CACHE");
  return value && value[0] && value[0]!='0';
}
std::shared_ptr<TextureResource> GameRenderer::SelectFrontbuffer(const guest::SwapPacket& swap,Error& e) const {
  if(!swap.frontbuffer || swap.frontbuffer>UINT32_MAX-0x23u) {Fail(e,"Invalid captured frontbuffer object");return {};}
  try {
    auto bytes=swap.memory.Read(uint32_t(swap.frontbuffer)+0x20,4);uint32_t fetch1=0;
    for(auto b:bytes) fetch1=(fetch1<<8)|b;
    std::array<uint32_t,6> fetch{};fetch[1]=fetch1;auto base=ResolvedPhysicalBase(fetch);
    auto found=resolved_latest_.find(base);
    if(found==resolved_latest_.end() || !found->second.image) {Fail(e,"Frontbuffer has no completed native resolve");return {};}
    e={};return found->second.image;
  } catch(const std::exception& ex) {e={"Captured frontbuffer",VK_ERROR_INITIALIZATION_FAILED,ex.what()};return {};}
}
bool GameRenderer::BeginSubmission(VkCommandBuffer command,uint64_t serial,Error& e,VkCommandBuffer upload) {
  constant_bank_cache_.Reset();
  open_pass_.reset();bound_pipeline_=VK_NULL_HANDLE;shadow_.Invalidate();
  if(!resources_.BeginSubmission(command,serial,e,upload) || !targets_.BeginSubmission(command,serial,e)) return false;
  command_=command;serial_=serial;merge_passes_=upload!=VK_NULL_HANDLE;recording_.store(command,std::memory_order_relaxed);
  if(!dummies_) {float missing[]{0,0,0,1};if(!resources_.CreateDummies(e) || !resources_.UploadBuffer(MissingVertex,std::as_bytes(std::span(missing)),1,e)) return false;dummies_=true;}
  e={};return true;
}
void GameRenderer::ClosePass() {
  if(!open_pass_) return;
  c_.f.vkCmdEndRenderPass(command_);open_pass_.reset();bound_pipeline_=VK_NULL_HANDLE;shadow_.Invalidate();
}
void GameRenderer::FinishSubmission() {
  ClosePass();resources_.FinishUploads();recording_.store(VK_NULL_HANDLE,std::memory_order_relaxed);
}
void GameRenderer::Retire(uint64_t serial) {
  for(auto it=transient_buffers_.begin();it!=transient_buffers_.end() && it->first<=serial;) {for(auto id:it->second) resources_.ForgetBuffer(id);it=transient_buffers_.erase(it);}
  depth_resolver_.Retire(serial);descriptors_.Retire(serial);pipelines_.Retire(serial);targets_.Retire(serial);resources_.Retire(serial);if(serial>=serial_) command_=VK_NULL_HANDLE;
}
bool GameRenderer::Upload(const guest::BufferUpdate& update,bool indices,uint64_t version,Error& e) {
  auto& p=update.plan;if(!p.key || !p.size || p.action>2 || p.begin>p.end || p.end>p.size) return Fail(e,"Invalid guest buffer update");
  // Unchanged: it only has to exist, and be held. Looked up in place: Buffer() returned a shared_ptr copy
  // (two atomic count updates) per stream of every draw only to test it.
  if(!p.action) {
    const auto* found=resources_.FindBuffer(p.key);
    if(!found) return Fail(e,"Buffer has no captured upload");
    resources_.Hold(*found);e={};return true;
  }
  uint32_t width=indices?((p.format&3)==2?4:2):1;
  if(indices && (p.size%width || p.begin%width || p.end%width)) return Fail(e,"Invalid index update alignment");
  uint64_t size=indices?uint64_t(p.size)/width*4:p.size;
  if(size>512*1024*1024) return Fail(e,"Buffer update exceeds capture limit");
  auto found=cpu_buffers_.find(p.key);
  if(p.action==1 && (found==cpu_buffers_.end() || found->second.size()!=size)) return Fail(e,"Partial buffer update has no captured base version");
  auto& bytes=cpu_buffers_[p.key];if(p.action==2) bytes.assign(size,0);
  auto source=indices?std::as_bytes(std::span(update.normalized)):std::as_bytes(std::span(update.bytes));
  size_t begin=indices?size_t(p.begin)/width*4:p.begin,end=indices?size_t(p.end)/width*4:p.end;
  if(source.size()!=end-begin) return Fail(e,"Buffer update payload length differs from range");
  std::memcpy(bytes.data()+begin,source.data(),source.size());
  if(profiling_) {++profile_.buffer_uploads;profile_.buffer_bytes+=bytes.size();}
  return p.action==1?resources_.UploadDynamicBuffer(p.key,std::as_bytes(std::span(bytes)),version,e)
                    :resources_.UploadBuffer(p.key,std::as_bytes(std::span(bytes)),version,e);
}
bool GameRenderer::Record(const guest::RenderPacket& packet,VkCommandBuffer command,Error& e) {
  if(command!=command_ || !serial_) return Fail(e,"Packet command buffer differs from active submission");
  if(dumping_) {
    std::ostringstream s;s<<dump_packet_++<<" ";
    if(auto* d=guest::GetDraw(packet)) {
      s<<"draw vs="<<Hex(d->vertex_shader?d->vertex_shader->hash:0)<<" ps="<<Hex(d->pixel_shader?d->pixel_shader->hash:0)<<" prim="<<int(d->primitive)<<" n="<<d->count<<(d->indexed?" idx":"")<<(d->inline_vertices?" inline":"")
       <<" c0="<<Surface(d->colors[0])<<" c1="<<Surface(d->colors[1])<<" ds="<<Surface(d->depth)<<" mask="<<Hex(d->registers[0x104])<<" depthctl="<<Hex(d->registers[0x200])<<" blend0="<<Hex(d->registers[0x201])
       <<" vp="<<d->viewport.x<<","<<d->viewport.y<<","<<d->viewport.width<<"x"<<d->viewport.height<<" z="<<d->viewport.min_depth<<".."<<d->viewport.max_depth<<" sc="<<d->scissor.left<<","<<d->scissor.top<<","<<d->scissor.right<<","<<d->scissor.bottom<<" tex=";
      for(uint32_t slot=0;slot<32;++slot) if((d->texture_fetch[slot][0]&3)==2) s<<slot<<":"<<Hex(d->texture_fetch[slot][1])<<(d->textures[slot]?"u":"")<<",";
    } else if(auto* c=std::get_if<guest::ClearPacket>(&packet)) {
      s<<"clear flags="<<Hex(c->flags)<<" c0="<<Surface(c->colors[0])<<" ds="<<Surface(c->depth_surface)<<" color="<<c->color[0]<<","<<c->color[1]<<","<<c->color[2]<<","<<c->color[3]<<" depth="<<c->depth<<" rects="<<c->rects.size();
    } else if(auto* r=std::get_if<guest::ResolvePacket>(&packet)) {
      s<<"resolve src="<<Surface(r->source_surface)<<" dst="<<Hex(r->destination)<<" fetch1="<<Hex(r->destination_fetch[1])<<" fetch2="<<Hex(r->destination_fetch[2])<<" rect="<<r->source.left<<","<<r->source.top<<","<<r->source.right<<","<<r->source.bottom<<" level="<<r->level<<" slice="<<r->slice<<" clear="<<Hex(r->flags);
    } else if(auto* p=std::get_if<guest::PassPacket>(&packet)) {
      s<<"pass op="<<int(p->operation)<<" pass="<<p->pass<<" color="<<Surface(p->color_surface)<<" ds="<<Surface(p->depth_surface)<<" clear_color="<<p->clear_color;
    } else s<<"other";
    dump_log_.push_back(s.str());
  }
  Lap lap(profiling_,profile_);
  try {
    if(auto* draw=guest::GetDraw(packet)) {bool ok=Draw(*draw,command,e);if(!ok && e.result!=VK_NOT_READY) ++stats_.failed;return ok;}
    if(std::holds_alternative<guest::DrawPacketOwner>(packet)) return Fail(e,"Packet has empty draw payload");
    struct PacketLap {Lap& lap;RecordProfile::Phase phase;~PacketLap(){lap(phase);}} packet_lap{lap,std::holds_alternative<guest::ResolvePacket>(packet)?RecordProfile::kResolves:RecordProfile::kClears};
    ClosePass();
    if(auto* clear=std::get_if<guest::ClearPacket>(&packet)) {if(!targets_.Clear(*clear,e)) return false;++stats_.clears;return true;}
    if(auto* resolve=std::get_if<guest::ResolvePacket>(&packet)) {
      auto plan=PlanResolve(*resolve,targets_,e);if(!e.message.empty()) return false;
      if(resolve->destination) plan.destination=TextureResourceId(resolve->destination_fetch);
      guest::ResourceId raw_id=0;
      if(!plan.empty && plan.operation==ResolveOperation::kDepthConversion) {
        if(!depth_ready_) {if(!depth_resolver_.Initialize(e)) return false;depth_ready_=true;}
        auto depth_fetch=resolve->destination_fetch;depth_fetch[1]=(depth_fetch[1]&~63u)|23u;plan.destination=TextureResourceId(depth_fetch);
        auto raw_fetch=resolve->destination_fetch;raw_fetch[1]=(raw_fetch[1]&~63u)|6u;
        raw_id=TextureResourceId(raw_fetch);
        if(!depth_resolver_.Record(command,plan,raw_id,targets_,resources_,state_,e)) return false;
      } else if(!RecordColorResolve(c_,command,plan,targets_,resources_,state_,e)) return false;
      if(!plan.empty) {
        auto& latest=resolved_latest_[plan.physical_base];latest.image=resources_.Texture(plan.destination,e);if(!latest.image) return false;
        if(dumping_ && !DumpImageNow(command,"p"+std::to_string(dump_packet_-1)+"_resolve_"+Hex(plan.physical_base),latest.image->handle,latest.image->format,{latest.image->extent.width,latest.image->extent.height,1},VK_IMAGE_ASPECT_COLOR_BIT,false,e)) return false;
        latest.raw=raw_id?resources_.Texture(raw_id,e):nullptr;if(raw_id && !latest.raw) return false;
        if(resolve->has_copy_draw) latest.swap=resolve->copy_dest_swap;
        latest.generation=++resolve_generation_;
      }
      if(plan.clear_color || plan.clear_depth) {
        guest::ClearPacket clear{};clear.colors[0]=resolve->clear_color_surface;clear.depth_surface=resolve->clear_depth_surface;
        clear.flags=(plan.clear_color?1u:0u)|(plan.clear_depth?0x30u:0u);clear.color=resolve->clear_color;clear.depth=resolve->clear_depth;clear.stencil=resolve->clear_stencil;
        if(resolve->has_source_rect) clear.rects={resolve->source};
        if(!targets_.Clear(clear,e)) return false;++stats_.clears;
      }
      e={};return true;
    }
    if(auto* pass=std::get_if<guest::PassPacket>(&packet)) {
      if(pass->operation==guest::Op::kEndTiling) tiling_={};
      if(pass->operation==guest::Op::kBeginTiling) {
        tiling_={};for(auto& rect:pass->rects) {tiling_.width=std::max(tiling_.width,uint32_t(std::max(0,rect.right)));tiling_.height=std::max(tiling_.height,uint32_t(std::max(0,rect.bottom)));}
        guest::ClearPacket clear{};clear.colors[0]=pass->color_surface;clear.depth_surface=pass->depth_surface;
        clear.flags=(pass->clear_color?1u:0u)|(pass->depth_surface.id?0x30u:0u);
        clear.color=pass->color;clear.depth=std::clamp(pass->depth,0.0f,1.0f);clear.stencil=pass->stencil;
        if(tiling_.width && tiling_.height) {
          for(auto* surface:{&clear.colors[0],&clear.depth_surface}) if(surface->id) {surface->geometry.width=std::max(surface->geometry.width,tiling_.width);surface->geometry.height=std::max(surface->geometry.height,tiling_.height);}
        }
        if(clear.flags && !targets_.Clear(clear,e)) return false;
      }
      e={};return true;
    }
    return Fail(e,"Resolve/presentation consumer is not integrated yet");
  } catch(const std::exception& ex) {e={"Game render packet",VK_ERROR_INITIALIZATION_FAILED,ex.what()};++stats_.failed;return false;}
}
bool GameRenderer::Draw(const guest::DrawPacket& draw,VkCommandBuffer command,Error& e) {
  if(draw.primitive==guest::Primitive::kRectangles && draw.inline_vertices) {
    guest::DrawPacket expanded=draw;std::string error;
    if(!guest::ExpandRectangles(std::as_bytes(std::span(draw.inline_data)),draw.count,draw.inline_stride,draw.attributes,expanded.inline_data,error)) {e={"Rectangle expansion",VK_ERROR_INITIALIZATION_FAILED,error};return false;}
    expanded.primitive=guest::Primitive::kTriangles;expanded.count=uint32_t(expanded.inline_data.size()/draw.inline_stride);expanded.first=0;return Draw(expanded,command,e);
  }
  if(draw.primitive==guest::Primitive::kQuads && !draw.indexed) {
    guest::DrawPacket expanded=draw;uint64_t end=uint64_t(draw.first)+draw.count;
    if(end>UINT32_MAX || draw.count>16*1024*1024) return Fail(e,"Quad expansion range invalid");
    for(uint32_t q=0;q<draw.count/4;++q) {uint32_t b=draw.first+q*4;expanded.expanded_indices.insert(expanded.expanded_indices.end(),{b,b+1,b+2,b,b+2,b+3});}
    expanded.primitive=guest::Primitive::kTriangles;expanded.indexed=true;expanded.first=0;expanded.count=uint32_t(expanded.expanded_indices.size());return Draw(expanded,command,e);
  }
  Lap lap(profiling_,profile_);if(profiling_) ++profile_.draws;
  // Consume cache updates even while shaders compile. Later action0/partial
  // packets depend on the first owned version and cannot read guest memory.
  // Single-use geometry is written into mapped transient memory: no copy.
  if(draw.inline_vertices) {auto id=InlineBufferBase|draw.command_serial;if(!resources_.UploadTransient(id,std::as_bytes(std::span(draw.inline_data)),0,e)) return false;transient_buffers_[serial_].push_back(id);}
  else for(auto& stream:draw.streams) if(!Upload(stream.update,false,draw.command_serial,e)) return false;
  if(draw.indexed && draw.indices.plan.key && !Upload(draw.indices,true,draw.command_serial,e)) return false;
  if(draw.indexed && !draw.indices.plan.key && draw.expanded_indices.empty() && draw.count) return Fail(e,"Indexed draw has no captured indices");
  auto index_id=draw.indices.plan.key;
  if(!draw.expanded_indices.empty()) {index_id=ExpandedIndexBufferBase|draw.command_serial;if(!resources_.UploadTransient(index_id,std::as_bytes(std::span(draw.expanded_indices)),0,e)) return false;transient_buffers_[serial_].push_back(index_id);}
  if(!draw.count) {e={};return true;}
  if(!draw.vertex_shader || !shaders_) return Fail(e,"Captured vertex shader/lookup absent");
  auto vs=shaders_(*draw.vertex_shader);shaders::ShaderResult ps{shaders::ShaderPoll::ready,{},{}};
  if(draw.pixel_shader) ps=shaders_(*draw.pixel_shader);
  if(vs.status==shaders::ShaderPoll::failed || ps.status==shaders::ShaderPoll::failed) {e={"Game shader",VK_ERROR_INITIALIZATION_FAILED,vs.status==shaders::ShaderPoll::failed?vs.diagnostic:ps.diagnostic};return false;}
  if(vs.status==shaders::ShaderPoll::pending || ps.status==shaders::ShaderPoll::pending) {++stats_.pending;e={"Game shader",VK_NOT_READY,"Captured shader compilation pending"};return false;}
  if(!vs.artifact || (draw.pixel_shader && !ps.artifact)) return Fail(e,"Ready shader has no owned artifact");
  lap(RecordProfile::kUploads);
  auto plan=PlanAttachments(draw,e,draw.tiling_active?tiling_:VkExtent2D{});if(!e.message.empty()) return false;
  auto pass=targets_.PreparePass(plan,e,open_pass_.get());if(!pass) return false;
  // While `pass` is still the open pass, every one of its targets holds the newest write serial, so no alias can
  // apply: whatever else writes a target (clears, resolves, depth aliases, another pass) closes the pass first.
  // SR_VULKAN_CHECK_ALIASES=1 plans anyway and fails the draw if that assumption is ever wrong.
  const bool open=open_pass_==pass;
  std::vector<std::pair<TargetId,AliasPlan>> aliases;
  if(!open || check_aliases_) aliases=targets_.Aliases(*pass);
  if(open && !aliases.empty()) {
    ++stats_.alias_violations;
    c_.Log("GameRenderer alias violation: "+std::to_string(aliases.size())+" alias plan(s) on the open pass (draw serial "+std::to_string(draw.command_serial)+"); the alias skip assumption is wrong");
    return Fail(e,"Alias plan on an open render pass (SR_VULKAN_CHECK_ALIASES)");
  }
  if(!aliases.empty()) ClosePass();
  for(auto& [destination,alias]:aliases) {
    if(!depth_ready_) {if(!depth_resolver_.Initialize(e)) return false;depth_ready_=true;}
    if(!depth_resolver_.RecordAlias(command,destination,alias,targets_,resources_,state_,e,alias_options_)) return false;
  }
  lap(RecordProfile::kTargets);
  // Each slot's resource id once per draw (a 24-byte FNV hash), also for the bindless loop below.
  std::array<guest::ResourceId,32> texture_ids{};
  for(uint32_t slot=0;slot<32;++slot) if((draw.texture_slots>>slot&1) && (draw.texture_fetch[slot][0]&3)==2) {
    auto id=texture_ids[slot]=TextureResourceId(draw.texture_fetch[slot]);
    // Only the value is read: look it up in place and Hold it, with no shared_ptr copy per slot.
    auto find_held=[&]() -> TextureResource* {
      const auto* found=resources_.FindTexture(id);if(!found) return nullptr;
      resources_.Hold(*found);return found->get();
    };
    TextureResource* current=find_held();
    if(auto resolved=resolved_latest_.find(ResolvedPhysicalBase(draw.texture_fetch[slot]));resolved!=resolved_latest_.end()) {
      auto& latest=resolved->second;const auto& selected=((draw.texture_fetch[slot][1]&63)==6 && latest.raw)?latest.raw:latest.image;
      auto mapping=PlanResolvedSwizzle(draw.texture_fetch[slot],selected->format,selected==latest.raw?false:latest.swap);
      const TextureResource* owner=selected.get();while(owner->image_owner) owner=owner->image_owner.get();
      auto view=resolve_views_.find(id);
      const bool same=current && view!=resolve_views_.end() && view->second.owner==owner && current->image_owner.get()==owner &&
        view->second.mapping.r==mapping.r && view->second.mapping.g==mapping.g && view->second.mapping.b==mapping.b && view->second.mapping.a==mapping.a;
      if(!same) {
        if(!resources_.BindTextureView(id,selected,mapping,e)) return false;resolve_views_[id]={owner,mapping};current=find_held();
      }
    }
    if(current && current->state==&state_) {
      if(!state_.Transition(command,current->handle,{VK_IMAGE_ASPECT_COLOR_BIT,0,current->mips,0,current->layers},ImageUsage::Sampled(),e)) return false;
      continue;
    }
    const auto& capture=draw.textures[slot];if(!capture) return Fail(e,"Bound texture needs a captured upload or resolved image");
    if(current && current->version==capture->version) continue;
    guest::LinearTexture texture;std::string reason;if(!decoder_ || !decoder_(*capture,texture,reason)) {e={"Game texture",VK_ERROR_FORMAT_NOT_SUPPORTED,reason.empty()?"Captured texture decoder unavailable":reason};return false;}
    if(!resources_.UploadTexture(id,texture,capture->version,e)) return false;
    if(profiling_) {++profile_.texture_uploads;profile_.texture_bytes+=texture.data.size();}
  }
  lap(RecordProfile::kTextures);
  TransientSlice constants;if(!resources_.MapTransient(sizeof(guest::ConstantSnapshot),constants,e)) return false;
  auto reused=constant_bank_cache_enabled_ && constants.offset<=UINT32_MAX
      ?constant_bank_cache_.Find(serial_,constants.chunk.get(),draw.constants,draw.vs_version,draw.ps_version):ConstantBankCache::Reuse{};
  DrawBindings bindings;if(!BuildBindings(draw,constants.data,bindings,e,(reused.vs?0u:1u)|(reused.ps?0u:2u))) return false;
  lap(RecordProfile::kBindings);
  DescriptorDraw descriptors;if(!descriptors_.Prepare(bindings,draw.texture_fetch,resources_,serial_,constants,descriptors,e)) return false;
  if(reused.vs) descriptors.dynamic_offsets[0]=reused.vs_offset;
  if(reused.ps) descriptors.dynamic_offsets[1]=reused.ps_offset;
  // Prepare flushed this slice; the chunk is held until completion and slices
  // are never overwritten inside a submission. Offset bases remain unchanged.
  if(constant_bank_cache_enabled_) constant_bank_cache_.Commit(serial_,constants.chunk.get(),uint32_t(constants.offset),draw.constants,reused,draw.vs_version,draw.ps_version);
  lap(RecordProfile::kDescriptors);
  auto pipeline=pipelines_.Acquire(draw,*pass,vs.artifact,ps.artifact,serial_,e);if(!pipeline) {e.message+="; VS="+std::to_string(draw.vertex_shader->hash)+", PS="+std::to_string(draw.pixel_shader?draw.pixel_shader->hash:0)+", RT0="+std::to_string(pass->formats[0])+", DS="+std::to_string(pass->depth_format);return false;}
  struct Binding {uint32_t slot;VkBuffer handle;VkDeviceSize offset;};
  std::array<Binding,33> vertices;uint32_t vertex_count=0;  // at most 32 streams (BuildBindings) plus the missing-vertex stub
  // Same as ResourceStore::Buffer without the shared_ptr copy: the draw only reads handle, offset and size.
  auto held_buffer=[&](guest::ResourceId id) -> const BufferResource* {
    const auto* found=resources_.FindBuffer(id);
    if(!found) {e={"Buffer binding",VK_ERROR_INITIALIZATION_FAILED,"Buffer has no captured upload"};return nullptr;}
    resources_.Hold(*found);return found->get();
  };
  if(draw.inline_vertices) {auto* b=held_buffer(InlineBufferBase|draw.command_serial);if(!b) return false;vertices[vertex_count++]={0,b->handle,b->offset};}
  else for(auto& stream:draw.streams) {auto* b=held_buffer(stream.update.plan.key);if(!b || stream.offset>b->size) return Fail(e,"Vertex stream offset exceeds buffer");vertices[vertex_count++]={stream.stream,b->handle,b->offset+stream.offset};}
  auto* missing=held_buffer(MissingVertex);if(!missing) return false;vertices[vertex_count++]={31,missing->handle,missing->offset};
  const BufferResource* indices=nullptr;if(draw.indexed) {indices=held_buffer(index_id);if(!indices || uint64_t(draw.first+uint64_t(draw.count))*4>indices->size) {e={"Index draw",VK_ERROR_INITIALIZATION_FAILED,"Draw exceeds captured buffer: first="+std::to_string(draw.first)+", count="+std::to_string(draw.count)+", bytes="+std::to_string(indices?indices->size:0)+", plan="+std::to_string(draw.indices.plan.size)+", format="+std::to_string(draw.indices.plan.format)+", action="+std::to_string(draw.indices.plan.action)+", serial="+std::to_string(draw.command_serial)};return false;}}
  VkViewport viewport{draw.viewport.x,draw.viewport.y,draw.viewport.width,draw.viewport.height,draw.viewport.min_depth,draw.viewport.max_depth};
  if(viewport.width<=0 || viewport.height<=0) viewport={0,0,float(pass->extent.width),float(pass->extent.height),0,1};
  auto x0=std::clamp(draw.scissor.left,0,int32_t(pass->extent.width)),y0=std::clamp(draw.scissor.top,0,int32_t(pass->extent.height));
  auto x1=std::clamp(draw.scissor.right,x0,int32_t(pass->extent.width)),y1=std::clamp(draw.scissor.bottom,y0,int32_t(pass->extent.height));
  if(x0==x1 || y0==y1) {e={};return true;}
  VkRect2D scissor{{x0,y0},{uint32_t(x1-x0),uint32_t(y1-y0)}};float blend[4];for(uint32_t i=0;i<4;++i) blend[i]=std::bit_cast<float>(draw.registers[0x105+i]);
  lap(RecordProfile::kPipeline);
  if(open_pass_!=pass) {
    ClosePass();
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass->render_pass;begin.framebuffer=pass->framebuffer;begin.renderArea={{0,0},pass->extent};
    c_.f.vkCmdBeginRenderPass(command,&begin,VK_SUBPASS_CONTENTS_INLINE);open_pass_=pass;
  }
  if(bound_pipeline_!=pipeline->handle) {c_.f.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline->handle);bound_pipeline_=pipeline->handle;}
  // Set 0 (the constants) moves with every draw. Sets 1-3 only when their resources change: every game pipeline
  // shares one layout, so binding set 0 alone leaves sets 1-3 in place. The four dynamic states persist on the
  // command buffer across pipeline binds because every game pipeline declares the same four.
  if(shadow_.enabled()) {
    // One call covers set 0 and, when they changed, sets 1-3 (the dynamic offsets belong to set 0 either way).
    const bool shared_changed=shadow_.SetSharedSets(descriptors.sets.data()+1);
    c_.f.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelines_.Layout(),0,shared_changed?4:1,descriptors.sets.data(),uint32_t(descriptors.dynamic_offsets.size()),descriptors.dynamic_offsets.data());
  } else {
    c_.f.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelines_.Layout(),0,4,descriptors.sets.data(),uint32_t(descriptors.dynamic_offsets.size()),descriptors.dynamic_offsets.data());
  }
  for(uint32_t i=0;i<vertex_count;++i) {
    const auto& b=vertices[i];
    if(std::any_of(pipeline->bindings.begin(),pipeline->bindings.end(),[&](const auto& binding){return binding.binding==b.slot;}) && shadow_.SetVertexBuffer(b.slot,b.handle,b.offset))
      c_.f.vkCmdBindVertexBuffers(command,b.slot,1,&b.handle,&b.offset);
  }
  if(shadow_.SetViewport(viewport)) c_.f.vkCmdSetViewport(command,0,1,&viewport);
  if(shadow_.SetScissor(scissor)) c_.f.vkCmdSetScissor(command,0,1,&scissor);
  if(shadow_.SetBlend(blend)) c_.f.vkCmdSetBlendConstants(command,blend);
  const uint32_t stencil=draw.registers[0x10d]&255;
  if(shadow_.SetStencil(stencil)) c_.f.vkCmdSetStencilReference(command,VK_STENCIL_FACE_FRONT_AND_BACK,stencil);
  if(draw.indexed) {
    if(shadow_.SetIndexBuffer(indices->handle,indices->offset,VK_INDEX_TYPE_UINT32)) c_.f.vkCmdBindIndexBuffer(command,indices->handle,indices->offset,VK_INDEX_TYPE_UINT32);
    c_.f.vkCmdDrawIndexed(command,draw.count,1,draw.first,draw.base_vertex,0);
  } else c_.f.vkCmdDraw(command,draw.count,1,draw.first,0);
  if(!merge_passes_) ClosePass();
  targets_.MarkWritten(*pass);++stats_.draws;lap(RecordProfile::kCommands);e={};return true;
}
}

namespace superman_returns::graphics::vulkan {
bool GameRenderer::DumpImageNow(VkCommandBuffer command,const std::string& name,VkImage image,VkFormat format,VkExtent3D extent,VkImageAspectFlags aspects,bool target,Error& e) {
  uint32_t texel=DumpTexel(format);
  if(!texel) {dump_log_.push_back("# skipped "+name+" format="+std::to_string(format));e={};return true;}
  auto buffer=resources_.ReadbackBuffer(VkDeviceSize(extent.width)*extent.height*texel,e);if(!buffer) return false;
  VkImageSubresourceRange range{aspects,0,1,0,1};
  auto previous=state_.Usage(image,VkImageAspectFlagBits(aspects&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,0);
  if(!state_.Transition(command,image,range,ImageUsage::TransferSource(),e)) return false;
  VkBufferImageCopy copy{};copy.imageSubresource={VkImageAspectFlags(aspects&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,0,1};copy.imageExtent=extent;
  c_.f.vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer->handle,1,&copy);
  // Restore the tracked usage so recording continues exactly as without a dump.
  if(previous.layout!=VK_IMAGE_LAYOUT_UNDEFINED && !state_.Transition(command,image,range,previous,e)) return false;
  (void)target;dump_images_.push_back({name,buffer,format,extent,texel});e={};return true;
}
bool GameRenderer::DumpTargets(VkCommandBuffer command,Error& e) {
  ClosePass();
  for(auto& t:targets_.All()) {
    if(!t->initialized) continue;
    auto& g=t->description.geometry;
    if(!DumpImageNow(command,"end_target_e"+std::to_string(g.edram_base)+"_f"+std::to_string(g.format)+"_"+std::to_string(g.width)+"x"+std::to_string(g.height)+(t->description.depth?"_depth":""),t->image,t->format,{g.width,g.height,1},t->aspects,true,e)) return false;
  }
  e={};return true;
}
bool GameRenderer::WriteDump(const std::filesystem::path& dir,Error& e) {
  std::error_code ec;std::filesystem::create_directories(dir,ec);
  {std::ofstream log(dir/"packets.txt");for(auto& line:dump_log_) log<<line<<'\n';}
  for(auto& image:dump_images_) {
    std::vector<uint8_t> bytes;if(!resources_.Readback(image.buffer,bytes,e)) return false;
    std::ofstream out(dir/(image.name+".raw"),std::ios::binary);
    uint32_t header[4]{image.extent.width,image.extent.height,uint32_t(image.format),image.texel};
    out.write(reinterpret_cast<const char*>(header),sizeof(header));out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(std::min<size_t>(bytes.size(),size_t(image.extent.width)*image.extent.height*image.texel)));
  }
  dumping_=false;dump_images_.clear();e={};return true;
}
}

#include "native_provider.h"
#include "shader_process.h"
#include "../game_frame.h"
#include "../composition.h"
#include "../immediate.h"
#include "../frame_loop.h"
#include "../frame_generation_policy.h"
#include "../../../native_renderer/native_renderer.h"
#include "../../../native_renderer/hang_watchdog.h"
#include <rex/ui/surface_win.h>
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <chrono>
#include <thread>
namespace superman_returns::graphics::vulkan {
namespace {
void Report(const Error& e) {REXLOG_ERROR("native Vulkan: {}: {} (VkResult {})",e.operation,e.message,int(e.result));}
struct HostTexture {
  guest::ResourceId id=0;guest::LinearTexture pixels;
  bool repeated=false,linear=true;
};
struct Host {
  Context context;std::mutex gpu_mutex;
  Error initialization_error;
  // Published once initialization succeeds. Packets read it without gpu_mutex,
  // which presentation holds across paint/present (77+ packets per frame each
  // waited for it, about 95% of the frame on Intel UHD).
  std::atomic<GameFrame*> ready_game{nullptr};
  NativeProviderConfig config;
  std::unique_ptr<shaders::VulkanShaderService> shaders;
  std::unique_ptr<GameFrame> game;
  std::unique_ptr<ResourceStore> resources;
  std::unique_ptr<FrontbufferCompositor> compositor;
  std::unique_ptr<ImmediateRenderer> immediate;
  std::mutex texture_mutex;std::map<uint64_t,std::weak_ptr<HostTexture>> textures;
  uint64_t texture_serial=1000000;bool dummies=false;
  Host(NativeProviderConfig cfg):config(std::move(cfg)) {context.logger=[](const std::string& line){REXLOG_INFO("native Vulkan: {}",line);};}
  ~Host() {ready_game.store(nullptr);immediate.reset();compositor.reset();resources.reset();game.reset();shaders.reset();}
  bool Initialize(rex::ui::Win32HwndSurface& window,Error& e) {
    std::array<const char*,2> extensions{VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    if(!context.CreateInstance(extensions,config.validation,e)) return false;
    auto create=reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(context.Proc()(context.instance,"vkCreateWin32SurfaceKHR"));
    if(!create) {e={"Win32 surface",VK_ERROR_EXTENSION_NOT_PRESENT,"vkCreateWin32SurfaceKHR unavailable"};return false;}
    VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};surface_info.hinstance=window.hinstance();surface_info.hwnd=window.hwnd();VkSurfaceKHR surface=VK_NULL_HANDLE;
    if(!Check(create(context.instance,&surface_info,nullptr,&surface),"Create SDK window surface",e)) return false;
    VkPhysicalDeviceFeatures features{};features.shaderSampledImageArrayDynamicIndexing=features.shaderStorageBufferArrayDynamicIndexing=features.independentBlend=features.shaderClipDistance=features.shaderCullDistance=features.robustBufferAccess=VK_TRUE;
    // Forced anisotropy must skip the render targets the game reads back (shadow
    // map, depth, glow); the Vulkan descriptor path cannot tell them apart yet, so
    // it keeps the game's filtering (D3D12: Renderer::GetSamplerIndex).
    if(config.anisotropy>1) REXLOG_INFO("native Vulkan: forced anisotropy not applied yet (render targets), game filtering");
    features.samplerAnisotropy=VK_FALSE;
    if(!context.OpenDevice(surface,config.gpu_uuid,e,&features,true,config.frame_generation.enabled)) return false;
    context.sampler_anisotropy=-1;
    if(!(context.selected.queues[context.graphics_family].flags&VK_QUEUE_COMPUTE_BIT)) {e={"Game queue",VK_ERROR_FEATURE_NOT_PRESENT,"Graphics queue must also support compute resolves"};return false;}
    config.shaders.compiler_workers=2;
    shaders=std::make_unique<shaders::VulkanShaderService>(config.shaders,WindowsShaderProcess());
    REXLOG_INFO("native Vulkan: {} precompiled shaders from {}",shaders->PrecompiledCount(),config.shaders.library.string());
    if(!shaders->LibraryDiagnostic().empty()) REXLOG_WARN("native Vulkan: pre-shader library unavailable: {}; compiling misses at runtime",shaders->LibraryDiagnostic());
    ShaderLookup lookup=[this](const guest::ShaderCapture& capture) {auto key=shaders->Request(capture.container,capture.vertex?shaders::ShaderStage::kVertex:shaders::ShaderStage::kPixel);return shaders->Poll(key);};
    TextureDecoder decode=[](const guest::TextureCapture& capture,guest::LinearTexture& result,std::string& error){return guest::DecodeTextureLayout(capture.fetch,capture.memory,result,error);};
    game=std::make_unique<GameFrame>(context,gpu_mutex,std::move(lookup),std::move(decode));
    game->compilation_progress=[] {native::HangWatchdogBeat();};
    resources=std::make_unique<ResourceStore>(context);compositor=std::make_unique<FrontbufferCompositor>(context);immediate=std::make_unique<ImmediateRenderer>(context);
    return game->Initialize(config.driver_cache,e) && compositor->Initialize(e) && immediate->Initialize(e);
  }
  bool Prepare(VkCommandBuffer command,uint64_t serial,Error& e) {
    if(!resources->BeginSubmission(command,serial,e)) return false;
    if(!dummies) {if(!resources->CreateDummies(e)) return false;dummies=true;}
    std::lock_guard lock(texture_mutex);
    for(auto it=textures.begin();it!=textures.end();) {
      auto texture=it->second.lock();if(!texture) {resources->ForgetTexture(it->first);it=textures.erase(it);continue;}
      Error missing;if(!resources->Texture(texture->id,missing) && !resources->UploadTexture(texture->id,texture->pixels,1,e)) return false;
      ++it;
    }
    return true;
  }
  void Retire(uint64_t serial) {immediate->Retire(serial);compositor->Retire(serial);resources->Retire(serial);}
};
struct DrawContext final:rex::ui::UIDrawContext {
  Host& host;VkCommandBuffer command;TargetPass& pass;
  DrawContext(rex::ui::Presenter& presenter,Host& h,VkCommandBuffer cmd,TargetPass& p):UIDrawContext(presenter,p.extent.width,p.extent.height),host(h),command(cmd),pass(p) {}
};
struct Texture final:rex::ui::ImmediateTexture {
  std::shared_ptr<HostTexture> owned;
  Texture(std::shared_ptr<HostTexture> texture):ImmediateTexture(texture->pixels.width,texture->pixels.height),owned(std::move(texture)) {}
};
class Drawer final:public rex::ui::ImmediateDrawer {
public:
  explicit Drawer(std::shared_ptr<Host> host):host_(std::move(host)) {}
  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width,uint32_t height,rex::ui::ImmediateTextureFilter filter,bool repeated,const uint8_t* data) override {
    if(!width || !height || width>8192 || height>8192 || !data) return {};
    auto texture=std::make_shared<HostTexture>();texture->linear=filter==rex::ui::ImmediateTextureFilter::kLinear;texture->repeated=repeated;
    auto& pixels=texture->pixels;pixels.width=width;pixels.height=height;pixels.format=guest::LinearFormat::kRGBA8Unorm;pixels.levels={{width,height,width*4,height,0}};pixels.data.assign(data,data+size_t(width)*height*4);
    std::lock_guard lock(host_->texture_mutex);texture->id=++host_->texture_serial;host_->textures[texture->id]=texture;return std::make_unique<Texture>(std::move(texture));
  }
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override {
    batch_ready_=false;if(!ui_draw_context() || batch.vertex_count<=0 || batch.index_count<0 || !batch.vertices || (batch.index_count && !batch.indices)) return;
    static_assert(sizeof(UIVertex)==sizeof(rex::ui::ImmediateVertex));auto& context=static_cast<DrawContext&>(*ui_draw_context());Error e;
    batch_ready_=host_->immediate->SetBatch(*host_->resources,{reinterpret_cast<const UIVertex*>(batch.vertices),size_t(batch.vertex_count)},{batch.indices,size_t(batch.index_count)},e);if(!batch_ready_) Report(e);
  }
  void Draw(const rex::ui::ImmediateDraw& draw) override {
    if(!batch_ready_ || draw.count<=0 || draw.index_offset<0) return;
    auto& context=static_cast<DrawContext&>(*ui_draw_context());UIDraw request;request.count=uint32_t(draw.count);request.first=uint32_t(draw.index_offset);request.base_vertex=draw.base_vertex;request.lines=draw.primitive_type==rex::ui::ImmediatePrimitiveType::kLines;
    uint32_t x,y,w,h;if(!ScissorToRenderTarget(draw,x,y,w,h)) return;request.scissor={{int32_t(x),int32_t(y)},{w,h}};
    if(draw.texture) {auto& texture=static_cast<Texture&>(*draw.texture);request.texture=texture.owned->id;request.linear=texture.owned->linear;request.repeated=texture.owned->repeated;}
    Error e;if(!host_->immediate->Draw(context.command,context.pass,*host_->resources,coordinate_space_width(),coordinate_space_height(),request,e)) Report(e);
  }
  void EndDrawBatch() override {batch_ready_=false;}
private:
  std::shared_ptr<Host> host_;bool batch_ready_=false;
};
struct Mailbox {std::shared_ptr<TextureResource> image;std::array<uint32_t,256> gamma{};bool gamma_enabled=false;uint64_t source_serial=0;};
class RefreshContext final:public rex::ui::Presenter::GuestOutputRefreshContext {
public:
  Mailbox output;
  explicit RefreshContext(bool& is8):GuestOutputRefreshContext(is8) {}
};
class Presenter final:public rex::ui::Presenter {
public:
  Presenter(std::shared_ptr<Host> host,HostGpuLossCallback loss):rex::ui::Presenter(std::move(loss)),host_(std::move(host)) {}
  ~Presenter() override {
    // The recording thread calls back into this presenter: stop it first.
    if(host_->game) host_->game->StopAsync();
    SetWindowSurfaceFromUIThread(nullptr,nullptr);DisconnectPaintingFromSurfaceFromUIThreadImpl();for(auto& mailbox:mailbox_) mailbox={};}
  bool Initialize() {return InitializeCommonSurfaceIndependent();}
  rex::ui::Surface::TypeFlags GetSupportedSurfaceTypes() const override {return rex::ui::Surface::kTypeFlag_Win32Hwnd;}
  bool CaptureGuestOutput(rex::ui::RawImage& image) override;
  bool Submit(guest::RenderPacket&& packet,std::string& diagnostic) {
    auto* swap=std::get_if<guest::SwapPacket>(&packet);auto gamma=swap?swap->gamma:nullptr;bool gamma_enabled=swap && swap->gamma_enabled;uint64_t source_serial=swap?swap->guest_swap:0;
    Error e;std::shared_ptr<TextureResource> image;
    GameFrame* game;
    game=host_->ready_game.load(std::memory_order_acquire);
    if(!game) {
      std::lock_guard lock(host_->gpu_mutex);
      if(!host_->initialization_error.message.empty()) {diagnostic=host_->initialization_error.operation+": "+host_->initialization_error.message;return false;}
      game=host_->game.get();
      if(!game) {diagnostic="Vulkan window/device has not been initialized";return false;}
    }
    // Frames record on GameFrame's thread while this worker replays the next.
    std::call_once(async_started_,[&] {game->StartAsync([this](std::shared_ptr<TextureResource> image,const guest::SwapPacket& swap) {
      if(!Publish(image,swap.gamma,swap.gamma_enabled,swap.guest_swap)) REXLOG_WARN("native Vulkan: presenter rejected a submitted frontbuffer");
    });});
    if(!game->Enqueue(std::move(packet),image,e)) {diagnostic=e.operation+": "+e.message;return false;}
    if(!image) return true;
    if(!Publish(image,gamma,gamma_enabled,source_serial)) {diagnostic="Vulkan presenter rejected submitted frontbuffer";return false;}
    return true;
  }
  bool Publish(const std::shared_ptr<TextureResource>& image,const std::shared_ptr<const std::array<uint32_t,256>>& gamma,bool gamma_enabled,uint64_t source_serial) {
    return RefreshGuestOutput(image->extent.width,image->extent.height,1280,720,[&](GuestOutputRefreshContext& base) {
      auto& context=static_cast<RefreshContext&>(base);context.output.image=image;if(gamma) context.output.gamma=*gamma;context.output.gamma_enabled=gamma_enabled;context.output.source_serial=source_serial;context.SetIs8bpc(image->format==VK_FORMAT_R8G8B8A8_UNORM);return true;
    });
  }
protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(rex::ui::Surface& surface,uint32_t width,uint32_t height,bool was_paintable,bool& implicit_vsync) override {
    implicit_vsync=false;std::lock_guard lock(host_->gpu_mutex);Error e;
    auto& window=static_cast<rex::ui::Win32HwndSurface&>(surface);
    if(!host_->context.device) {
      if(!host_->Initialize(window,e)) {host_->initialization_error=e;Report(e);return SurfacePaintConnectResult::kFailure;}
      host_->initialization_error={};host_->ready_game.store(host_->game.get(),std::memory_order_release);
    }
    else if(hwnd_ && hwnd_!=window.hwnd()) {
      if(!frames_.Retire(host_->context,e)) {Report(e);return SurfacePaintConnectResult::kFailure;}swapchain_.Destroy();
      auto create=reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(host_->context.Proc()(host_->context.instance,"vkCreateWin32SurfaceKHR"));
      VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};info.hinstance=window.hinstance();info.hwnd=window.hwnd();VkSurfaceKHR replacement=VK_NULL_HANDLE;
      if(!Check(create(host_->context.instance,&info,nullptr,&replacement),"Reconnect SDK window surface",e)) {Report(e);return SurfacePaintConnectResult::kFailure;}
      VkBool32 supported=VK_FALSE;auto result=host_->context.f.vkGetPhysicalDeviceSurfaceSupportKHR(host_->context.physical,host_->context.present_family,replacement,&supported);
      if(result!=VK_SUCCESS || !supported) {host_->context.f.vkDestroySurfaceKHR(host_->context.instance,replacement,nullptr);e={"Reconnect surface",result==VK_SUCCESS?VK_ERROR_FEATURE_NOT_PRESENT:result,"Existing presentation queue cannot present on the new window"};Report(e);return SurfacePaintConnectResult::kFailure;}
      host_->context.f.vkDestroySurfaceKHR(host_->context.instance,host_->context.surface,nullptr);host_->context.surface=replacement;
    }
    hwnd_=window.hwnd();
    if(was_paintable && swapchain_.handle && requested_extent_.width==width && requested_extent_.height==height) return SurfacePaintConnectResult::kSuccessUnchanged;
    if(!frames_.Retire(host_->context,e) || !swapchain_.Recreate(host_->context,{width,height},host_->config.vsync,e) || !frames_.Initialize(host_->context,swapchain_,e)) {Report(e);swapchain_.Destroy();return SurfacePaintConnectResult::kFailure;}
    generation_policy_.Reset();
#if SR_HAS_LSFG
    if(generation_) generation_->ResetAfterRetire();
    else if(host_->config.frame_generation.enabled) {
      generation_=std::make_unique<FrameGenerationSession>(host_->context,host_->config.frame_generation);
      if(!generation_->Initialize(e)) {REXLOG_WARN("native Vulkan: frame generation disabled: {}",e.message);generation_.reset();host_->config.frame_generation.enabled=false;}
    }
#endif
    requested_extent_={width,height};return SurfacePaintConnectResult::kSuccess;
  }
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override {
    std::lock_guard lock(host_->gpu_mutex);if(!host_->context.device) return;Error e;frames_.Retire(host_->context,e);
#if SR_HAS_LSFG
    generation_.reset();
#endif
    generation_policy_.Reset();pending_original_={};swapchain_.Destroy();requested_extent_={};
  }
  bool RefreshGuestOutputImpl(uint32_t index,uint32_t,uint32_t,std::function<bool(GuestOutputRefreshContext&)> callback,bool& is8) override {
    // The SDK hands the refresher an exclusive writable slot; consumers only read
    // acquired/ready slots, so gpu_mutex (held by painting) is not needed here.
    RefreshContext context(is8);if(!callback(context) || !context.output.image) return false;mailbox_[index]=std::move(context.output);return true;
  }
  PaintResult PaintAndPresentImpl(bool ui) override {
    uint32_t index;GuestOutputProperties properties{};auto consumer=ConsumeGuestOutput(index,&properties,nullptr);Mailbox source;if(index!=UINT32_MAX) source=mailbox_[index];consumer.unlock();
    const bool pending=bool(pending_original_.image);
    if(pending) {source=pending_original_;properties.display_aspect_ratio_x=pending_aspect_x_;properties.display_aspect_ratio_y=pending_aspect_y_;}
    if(!swapchain_.handle) return PaintResult::kNotPresented;
    Error e;TargetPass pass;pass.owns_handles=false;pass.render_pass=swapchain_.render_pass;pass.extent=swapchain_.choice.extent;pass.color_count=1;pass.formats[0]=swapchain_.choice.format.format;
    const auto now=std::chrono::steady_clock::now();
    const auto source_time=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    auto action=generation_policy_.PreviewSource(source.source_serial,source_time);
    const bool new_source=pending || action==GenerationAction::kWarmup || action==GenerationAction::kInterpolate;
    bool generate=false;
#if SR_HAS_LSFG
    FrameGenerationInput input{};input.source_serial=source.source_serial;input.image=source.image;input.gamma=source.gamma;input.gamma_enabled=source.gamma_enabled;
    input.extent=swapchain_.choice.extent;input.aspect_x=properties.display_aspect_ratio_x;input.aspect_y=properties.display_aspect_ratio_y;input.force_warmup=action==GenerationAction::kWarmup;
    if(generation_ && generation_->active() && source.image && new_source && !pending) {
      std::lock_guard lock(host_->gpu_mutex);
      if(generation_->NeedsReset(input.extent,source.image->format)) {
        if(!frames_.Retire(host_->context,e) || !frames_.Initialize(host_->context,swapchain_,e)) {Report(e);return PaintResult::kNotPresented;}
        generation_->ResetAfterRetire();input.force_warmup=true;
      }
      if(generation_->Prepare(input,e)) generate=true;
      else {REXLOG_WARN("native Vulkan: frame generation disabled: {}",e.message);frames_.Retire(host_->context,e);generation_.reset();host_->config.frame_generation.enabled=false;if(!frames_.Initialize(host_->context,swapchain_,e)) {Report(e);return PaintResult::kNotPresented;}}
    }
#endif
    std::shared_ptr<CompositionDraw> composition;
    std::shared_ptr<TextureResource> displayed=source.image;
    bool generated=false;
    FrameWork work;
    work.queue_mutex=&host_->gpu_mutex;  // DrawGame locks it only around recording, submit, present and retire
    work.prepare=[&](VkCommandBuffer command,uint64_t serial,Error& error) {
      if(!host_->Prepare(command,serial,error)) return false;
#if SR_HAS_LSFG
      if(generate) {
        FrameGenerationOutput output;
        if(!generation_->Record(command,serial,input,*source.image->state,output,error))return false;
        generated=bool(output.generated);displayed=generated?output.generated:output.original;
      }
#endif
      composition.reset();
      if(displayed) {composition=host_->compositor->Prepare(command,pass,displayed,*host_->resources,*displayed->state,source.gamma,generated?false:source.gamma_enabled,generated?pass.extent.width:properties.display_aspect_ratio_x,generated?pass.extent.height:properties.display_aspect_ratio_y,error);if(!composition) return false;}
      return true;
    };
    work.paint=[&](VkCommandBuffer command,uint32_t) {if(composition) host_->compositor->Record(command,*composition);if(ui) {DrawContext context(*this,*host_,command,pass);ExecuteUIDrawersFromUIThread(context);}};
    work.retire=[this,host=host_](uint64_t serial) {host->Retire(serial);
#if SR_HAS_LSFG
      if(generation_)generation_->Retire(serial);
#endif
    };
    auto presented_before=frames_.presented;
    auto outcome=frames_.DrawGame(host_->context,swapchain_,work,e);
    if(outcome!=FrameOutcome::kFailed && frames_.presented>presented_before) {
      // Also seed history after a reconnect while an original was pending.
      // Committing an already known serial is idempotent.
      if(new_source)generation_policy_.OnSource(source.source_serial,source_time);
      generation_policy_.OnPresented(generated?FramePresentationKind::kGenerated:(new_source?FramePresentationKind::kReal:FramePresentationKind::kRepeat));
      if(generated) {
        pending_original_=source;pending_aspect_x_=properties.display_aspect_ratio_x;pending_aspect_y_=properties.display_aspect_ratio_y;
      } else if(pending)pending_original_={};
      if(generated && outcome==FrameOutcome::kPresented) {
        // Pacing never holds gpu_mutex: the guest worker can submit the next real frame.
        std::this_thread::sleep_until(std::chrono::steady_clock::now()+std::chrono::nanoseconds(generation_policy_.HalfIntervalNs()));
        generate=false;generated=false;displayed=source.image;
        presented_before=frames_.presented;
        outcome=frames_.DrawGame(host_->context,swapchain_,work,e);
        if(outcome!=FrameOutcome::kFailed && frames_.presented>presented_before) {generation_policy_.OnPresented(FramePresentationKind::kReal);pending_original_={};}
      }
      ReportPresentationStats();
      if(outcome==FrameOutcome::kPresented)return PaintResult::kPresented;
    }
    if(outcome==FrameOutcome::kRecreate) return PaintResult::kNotPresentedConnectionOutdated;
    if(outcome==FrameOutcome::kSuspended) return PaintResult::kNotPresented;
    Report(e);return e.result==VK_ERROR_DEVICE_LOST?PaintResult::kGpuLostResponsible:PaintResult::kNotPresented;
  }
private:
  void ReportPresentationStats() {
    const auto now=std::chrono::steady_clock::now();
    if(!stats_started_) {stats_started_=true;stats_time_=now;stats_real_=generation_policy_.real();stats_generated_=generation_policy_.generated();stats_repeat_=generation_policy_.repeated();return;}
    const double seconds=std::chrono::duration<double>(now-stats_time_).count();if(seconds<2)return;
    const auto real=generation_policy_.real()-stats_real_,generated=generation_policy_.generated()-stats_generated_,repeat=generation_policy_.repeated()-stats_repeat_;
    double gpu_ms=0;
#if SR_HAS_LSFG
    if(generation_) {const auto stats=generation_->GpuStats();if(stats.samples)gpu_ms=double(stats.total_ns)/double(stats.samples)/1000000.0;}
#endif
    REXLOG_INFO("native Vulkan presentation: real_fps={:.2f} generated_fps={:.2f} repeat_fps={:.2f} presented_fps={:.2f} fg_gpu_ms={:.3f}",real/seconds,generated/seconds,repeat/seconds,(real+generated+repeat)/seconds,gpu_ms);
    stats_time_=now;stats_real_=generation_policy_.real();stats_generated_=generation_policy_.generated();stats_repeat_=generation_policy_.repeated();
  }
  std::shared_ptr<Host> host_;std::array<Mailbox,kGuestOutputMailboxSize> mailbox_;std::once_flag async_started_;
  FrameGenerationPolicy generation_policy_;
  Mailbox pending_original_;uint32_t pending_aspect_x_=1280,pending_aspect_y_=720;
#if SR_HAS_LSFG
  std::unique_ptr<FrameGenerationSession> generation_;
#endif
  bool stats_started_=false;std::chrono::steady_clock::time_point stats_time_{};uint64_t stats_real_=0,stats_generated_=0,stats_repeat_=0;
  Swapchain swapchain_;FrameLoop frames_;VkExtent2D requested_extent_{};HWND hwnd_=nullptr;
};
bool Presenter::CaptureGuestOutput(rex::ui::RawImage& output) {
  uint32_t index;GuestOutputProperties properties;auto consumer=ConsumeGuestOutput(index,&properties,nullptr);
  if(index==UINT32_MAX) return false;auto source=mailbox_[index];consumer.unlock();
  std::lock_guard lock(host_->gpu_mutex);auto& c=host_->context;if(!source.image || !c.device) return false;
  Error e;VkCommandPool pool=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;bool submitted=false;
  struct Cleanup {Context& c;VkCommandPool& pool;VkFence& fence;bool& submitted;~Cleanup(){if(submitted) c.f.vkDeviceWaitIdle(c.device);if(fence)c.f.vkDestroyFence(c.device,fence,nullptr);if(pool)c.f.vkDestroyCommandPool(c.device,pool,nullptr);}} cleanup{c,pool,fence,submitted};
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=c.graphics_family;
  VkCommandBuffer command;VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
  if(!Check(c.f.vkCreateCommandPool(c.device,&pi,nullptr,&pool),"Capture command pool",e)) {Report(e);return false;}ai.commandPool=pool;
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};if(!Check(c.f.vkAllocateCommandBuffers(c.device,&ai,&command),"Capture command",e) || !Check(c.f.vkCreateFence(c.device,&fi,nullptr,&fence),"Capture fence",e)) {Report(e);return false;}
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;if(!Check(c.f.vkBeginCommandBuffer(command,&bi),"Begin capture",e)) {Report(e);return false;}
  ImageState state(c.f);ResourceStore resources(c);TargetStore targets(c,state);FrontbufferCompositor compositor(c);
  if(!resources.BeginSubmission(command,1,e) || !resources.CreateDummies(e) || !targets.BeginSubmission(command,1,e) || !compositor.Initialize(e)) {Report(e);return false;}
  guest::DrawPacket draw;draw.colors[0]={1,false,{source.image->extent.width,source.image->extent.height,0,0,0,1}};
  auto pass=targets.PreparePass(PlanAttachments(draw,e),e);if(!pass) {Report(e);return false;}
  auto composition=compositor.Prepare(command,*pass,source.image,resources,host_->game->Renderer().Images(),source.gamma,source.gamma_enabled,source.image->extent.width,source.image->extent.height,e);if(!composition) {Report(e);return false;}
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass->render_pass;begin.framebuffer=pass->framebuffer;begin.renderArea={{0,0},pass->extent};c.f.vkCmdBeginRenderPass(command,&begin,VK_SUBPASS_CONTENTS_INLINE);compositor.Record(command,*composition);c.f.vkCmdEndRenderPass(command);
  auto image=targets.Get(targets.Acquire(draw.colors[0],e),e);auto buffer=resources.ReadbackBuffer(uint64_t(pass->extent.width)*pass->extent.height*4,e);
  if(!image || !buffer || !state.Transition(command,image->image,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},ImageUsage::TransferSource(),e)) {Report(e);return false;}
  VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={pass->extent.width,pass->extent.height,1};c.f.vkCmdCopyImageToBuffer(command,image->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer->handle,1,&copy);
  if(!Check(c.f.vkEndCommandBuffer(command),"End capture",e)) {Report(e);return false;}
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;if(!Check(c.f.vkQueueSubmit(c.graphics_queue,1,&submit,fence),"Submit capture",e)) {Report(e);return false;}submitted=true;
  if(!Check(c.f.vkWaitForFences(c.device,1,&fence,VK_TRUE,UINT64_MAX),"Capture completion",e) || !resources.Readback(buffer,output.data,e)) {Report(e);return false;}
  output.width=pass->extent.width;output.height=pass->extent.height;output.stride=size_t(output.width)*4;compositor.Retire(1);resources.Retire(1);targets.Retire(1);return true;
}
class Provider final:public rex::ui::GraphicsProvider {
public:
  explicit Provider(NativeProviderConfig config):host_(std::make_shared<Host>(std::move(config))) {}
  std::unique_ptr<rex::ui::Presenter> CreatePresenter(rex::ui::Presenter::HostGpuLossCallback loss) override {
    auto presenter=std::make_unique<Presenter>(host_,std::move(loss));if(!presenter->Initialize()) return {};
    auto* pointer=presenter.get();
    if(!native::Renderer::Get().InstallPacketSink([pointer](guest::RenderPacket&& packet,std::string& error){return pointer->Submit(std::move(packet),error);},[host=host_]{if(host->game) host->game->Cancel();})) {REXLOG_ERROR("native Vulkan: renderer worker already started or packet sink unavailable");return {};}
    return presenter;
  }
  std::unique_ptr<rex::ui::ImmediateDrawer> CreateImmediateDrawer() override {return std::make_unique<Drawer>(host_);}
private:
  std::shared_ptr<Host> host_;
};
}
std::unique_ptr<rex::ui::GraphicsProvider> CreateNativeVulkanProvider(NativeProviderConfig config) {
  for(auto& path:{config.shaders.python,config.shaders.script,config.shaders.emitter,config.shaders.common,config.shaders.dxc}) if(!std::filesystem::is_regular_file(path)) {REXLOG_ERROR("native Vulkan: required shader tool missing: {}",path.string());return {};}
  return std::make_unique<Provider>(std::move(config));
}
}

#include "openxr.hpp"

#if defined(AURORA_ENABLE_OPENXR) && defined(_WIN32)
#include "logging.hpp"
#include "webgpu/gpu.hpp"

#include <dawn/native/D3D12Backend.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <algorithm>
#include <cstring>
#include <vector>
#include <windows.h>

namespace aurora::openxr {
namespace {
Module Log("OpenXR");
XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_localSpace = XR_NULL_HANDLE;
XrSwapchain g_swapchain = XR_NULL_HANDLE;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
bool g_running = false;
constexpr uint32_t kWidth = 1600;
constexpr uint32_t kHeight = 900;

struct Image {
  XrSwapchainImageD3D12KHR xr{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
  Microsoft::WRL::ComPtr<ID3D12Resource> intermediate;
  wgpu::SharedTextureMemory memory;
  wgpu::Texture texture;
  wgpu::TextureView view;
  bool initialized = false;
};
std::vector<Image> g_images;
Microsoft::WRL::ComPtr<ID3D12Device> g_d3dDevice;
Microsoft::WRL::ComPtr<ID3D12CommandQueue> g_d3dQueue;
Microsoft::WRL::ComPtr<ID3D12CommandAllocator> g_copyAllocator;
Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> g_copyList;
Microsoft::WRL::ComPtr<ID3D12Fence> g_copyFence;
HANDLE g_copyEvent = nullptr;
uint64_t g_copyFenceValue = 0;

bool ok(XrResult result, const char* operation) noexcept {
  if (XR_SUCCEEDED(result)) return true;
  char resultName[XR_MAX_RESULT_STRING_SIZE]{};
  if (g_instance != XR_NULL_HANDLE &&
      XR_SUCCEEDED(xrResultToString(g_instance, result, resultName))) {
    Log.error("{} failed: {} ({})", operation, resultName, static_cast<int>(result));
  } else {
    Log.error("{} failed with OpenXR result {}", operation, static_cast<int>(result));
  }
  return false;
}

void destroy_handles() noexcept {
  g_images.clear();
  g_copyFence.Reset();
  g_copyList.Reset();
  g_copyAllocator.Reset();
  g_d3dQueue.Reset();
  g_d3dDevice.Reset();
  if (g_copyEvent != nullptr) CloseHandle(g_copyEvent);
  g_copyEvent = nullptr;
  if (g_swapchain != XR_NULL_HANDLE) xrDestroySwapchain(g_swapchain);
  if (g_localSpace != XR_NULL_HANDLE) xrDestroySpace(g_localSpace);
  if (g_session != XR_NULL_HANDLE) xrDestroySession(g_session);
  if (g_instance != XR_NULL_HANDLE) xrDestroyInstance(g_instance);
  g_swapchain = XR_NULL_HANDLE;
  g_localSpace = XR_NULL_HANDLE;
  g_session = XR_NULL_HANDLE;
  g_instance = XR_NULL_HANDLE;
  g_system = XR_NULL_SYSTEM_ID;
  g_state = XR_SESSION_STATE_UNKNOWN;
  g_running = false;
}

void poll_events() noexcept {
  if (g_instance == XR_NULL_HANDLE) return;
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(g_instance, &event) == XR_SUCCESS) {
    if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
      g_state = changed.state;
      if (g_state == XR_SESSION_STATE_READY && !g_running) {
        const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr,
                                       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
        if (ok(xrBeginSession(g_session, &begin), "xrBeginSession")) g_running = true;
      } else if (g_state == XR_SESSION_STATE_STOPPING && g_running) {
        xrEndSession(g_session);
        g_running = false;
      } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
        g_running = false;
      }
    }
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}
} // namespace

bool initialize() noexcept {
  if (g_instance != XR_NULL_HANDLE) return true;
  if (webgpu::g_backendType != wgpu::BackendType::D3D12) {
    Log.error("OpenXR requires the D3D12 backend in this build");
    return false;
  }

  // Virtual Desktop installs this Oculus-specific compatibility API layer as a global implicit
  // layer.  It is useful for OculusXR applications, but WiiCompiled does not use OculusXR and the
  // layer can make SteamVR's xrCreateInstance return XR_ERROR_INITIALIZATION_FAILED before our
  // application reaches the compositor.  Its manifest explicitly provides this opt-out.
  SetEnvironmentVariableW(L"DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY", L"1");

  const char* extension = XR_KHR_D3D12_ENABLE_EXTENSION_NAME;
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strncpy(create.applicationInfo.applicationName, g_config.appName,
               XR_MAX_APPLICATION_NAME_SIZE - 1);
  std::strncpy(create.applicationInfo.engineName, "Aurora", XR_MAX_ENGINE_NAME_SIZE - 1);
  create.applicationInfo.applicationVersion = 1;
  create.applicationInfo.engineVersion = 1;
  // SteamVR 2.x still exposes an OpenXR 1.0 runtime on some headset paths.  An application may
  // use newer loader headers while requesting the 1.0 core API; all functionality used here is
  // part of 1.0.  Requesting XR_CURRENT_API_VERSION (currently 1.1) makes those runtime builds
  // reject xrCreateInstance before a session can be created.
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionCount = 1;
  create.enabledExtensionNames = &extension;
  if (!ok(xrCreateInstance(&create, &g_instance), "xrCreateInstance")) {
    destroy_handles();
    return false;
  }

  XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
  systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  if (!ok(xrGetSystem(g_instance, &systemInfo, &g_system), "xrGetSystem")) {
    destroy_handles();
    return false;
  }

  PFN_xrGetD3D12GraphicsRequirementsKHR getRequirements = nullptr;
  if (!ok(xrGetInstanceProcAddr(g_instance, "xrGetD3D12GraphicsRequirementsKHR",
                                reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)),
          "xrGetInstanceProcAddr(D3D12 requirements)")) {
    destroy_handles();
    return false;
  }
  XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
  if (!ok(getRequirements(g_instance, g_system, &requirements),
          "xrGetD3D12GraphicsRequirementsKHR")) {
    destroy_handles();
    return false;
  }

  // The distributed Dawn DLL is built with MSVC while WiiCompiled uses llvm-mingw. Resolve the
  // native bridge exports by their stable MSVC names so C++ name mangling does not cross ABIs.
  using GetDeviceFn = Microsoft::WRL::ComPtr<ID3D12Device> (*)(WGPUDevice);
  using GetQueueFn = Microsoft::WRL::ComPtr<ID3D12CommandQueue> (*)(WGPUDevice);
  const HMODULE dawnModule = GetModuleHandleW(L"webgpu_dawn.dll");
  const auto getDevice = reinterpret_cast<GetDeviceFn>(GetProcAddress(
      dawnModule, "?GetD3D12Device@d3d12@native@dawn@@YA?AV?$ComPtr@UID3D12Device@@@WRL@Microsoft@@PEAUWGPUDeviceImpl@@@Z"));
  const auto getQueue = reinterpret_cast<GetQueueFn>(GetProcAddress(
      dawnModule, "?GetD3D12CommandQueue@d3d12@native@dawn@@YA?AV?$ComPtr@UID3D12CommandQueue@@@WRL@Microsoft@@PEAUWGPUDeviceImpl@@@Z"));
  if (getDevice == nullptr || getQueue == nullptr) {
    Log.error("Dawn D3D12 native bridge exports are unavailable");
    destroy_handles();
    return false;
  }
  g_d3dDevice = getDevice(webgpu::g_device.Get());
  g_d3dQueue = getQueue(webgpu::g_device.Get());
  const LUID actualLuid = g_d3dDevice->GetAdapterLuid();
  if (std::memcmp(&actualLuid, &requirements.adapterLuid, sizeof(LUID)) != 0) {
    Log.error("SteamVR requires a different graphics adapter than Dawn selected");
    destroy_handles();
    return false;
  }

  const XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR, nullptr,
                                          g_d3dDevice.Get(), g_d3dQueue.Get()};
  const XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, g_system};
  if (!ok(xrCreateSession(g_instance, &sessionInfo, &g_session), "xrCreateSession")) {
    destroy_handles();
    return false;
  }
  const XrReferenceSpaceCreateInfo spaceInfo{
      XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL,
      {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}}};
  if (!ok(xrCreateReferenceSpace(g_session, &spaceInfo, &g_localSpace),
          "xrCreateReferenceSpace")) {
    destroy_handles();
    return false;
  }

  uint32_t formatCount = 0;
  xrEnumerateSwapchainFormats(g_session, 0, &formatCount, nullptr);
  std::vector<int64_t> formats(formatCount);
  xrEnumerateSwapchainFormats(g_session, formatCount, &formatCount, formats.data());
  constexpr int64_t bgra8 = DXGI_FORMAT_B8G8R8A8_UNORM;
  if (std::find(formats.begin(), formats.end(), bgra8) == formats.end()) {
    Log.error("OpenXR runtime does not support BGRA8 swapchains");
    destroy_handles();
    return false;
  }
  const XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO,
      nullptr, 0, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
      bgra8, 1, kWidth, kHeight, 1, 1, 1};
  if (!ok(xrCreateSwapchain(g_session, &swapchainInfo, &g_swapchain), "xrCreateSwapchain")) {
    destroy_handles();
    return false;
  }
  uint32_t imageCount = 0;
  xrEnumerateSwapchainImages(g_swapchain, 0, &imageCount, nullptr);
  g_images.resize(imageCount);
  // Xr structs must be enumerated into a tightly packed array; Image also owns Dawn wrappers.
  std::vector<XrSwapchainImageD3D12KHR> xrImages(imageCount,
      {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
  xrEnumerateSwapchainImages(g_swapchain, imageCount, &imageCount,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(xrImages.data()));
  const D3D12_HEAP_PROPERTIES heapProperties{
      D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
      D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
  const D3D12_RESOURCE_DESC nativeTextureDesc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kWidth, kHeight, 1, 1,
      DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
          D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS};
  for (uint32_t i = 0; i < imageCount; ++i) {
    g_images[i].xr = xrImages[i];
    if (FAILED(g_d3dDevice->CreateCommittedResource(
            &heapProperties, D3D12_HEAP_FLAG_NONE, &nativeTextureDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_images[i].intermediate)))) {
      Log.error("Failed to create typed OpenXR intermediate texture");
      destroy_handles();
      return false;
    }
    dawn::native::d3d12::SharedTextureMemoryD3D12ResourceDescriptor resource;
    resource.resource = g_images[i].intermediate;
    const wgpu::SharedTextureMemoryDescriptor memoryDesc{.nextInChain = &resource,
                                                         .label = "OpenXR swapchain image"};
    g_images[i].memory = webgpu::g_device.ImportSharedTextureMemory(&memoryDesc);
    const wgpu::TextureDescriptor textureDesc{
        .label = "OpenXR texture", .usage = wgpu::TextureUsage::RenderAttachment,
        .dimension = wgpu::TextureDimension::e2D,
        .size = {kWidth, kHeight, 1}, .format = wgpu::TextureFormat::BGRA8Unorm,
        .mipLevelCount = 1, .sampleCount = 1};
    g_images[i].texture = g_images[i].memory.CreateTexture(&textureDesc);
    g_images[i].view = g_images[i].texture.CreateView();
  }
  if (FAILED(g_d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&g_copyAllocator))) ||
      FAILED(g_d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            g_copyAllocator.Get(), nullptr,
                                            IID_PPV_ARGS(&g_copyList))) ||
      FAILED(g_copyList->Close()) ||
      FAILED(g_d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                      IID_PPV_ARGS(&g_copyFence)))) {
    Log.error("Failed to create OpenXR D3D12 copy resources");
    destroy_handles();
    return false;
  }
  g_copyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (g_copyEvent == nullptr) {
    Log.error("Failed to create OpenXR D3D12 copy event");
    destroy_handles();
    return false;
  }
  Log.info("OpenXR session created; SteamVR headset compositor is available");
  return true;
}

void shutdown() noexcept { destroy_handles(); }
bool active() noexcept { return g_instance != XR_NULL_HANDLE; }

void present(wgpu::BindGroup source) noexcept {
  poll_events();
  if (!g_running || !source) return;
  XrFrameState frame{XR_TYPE_FRAME_STATE};
  const XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
  if (!ok(xrWaitFrame(g_session, &wait, &frame), "xrWaitFrame")) return;
  const XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
  if (!ok(xrBeginFrame(g_session, &begin), "xrBeginFrame")) return;

  std::array<const XrCompositionLayerBaseHeader*, 1> layers{};
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  if (frame.shouldRender) {
    uint32_t index = 0;
    const XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrAcquireSwapchainImage(g_swapchain, &acquire, &index);
    const XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr,
                                             XR_INFINITE_DURATION};
    xrWaitSwapchainImage(g_swapchain, &imageWait);
    auto& image = g_images[index];
    wgpu::SharedTextureMemoryD3DSwapchainBeginState swapchainState;
    wgpu::SharedTextureMemoryBeginAccessDescriptor access{
        .nextInChain = &swapchainState, .concurrentRead = false,
        .initialized = image.initialized, .fenceCount = 0};
    if (image.memory.BeginAccess(image.texture, &access) == wgpu::Status::Success) {
      const wgpu::CommandEncoderDescriptor encoderDesc{.label = "OpenXR encoder"};
      auto encoder = webgpu::g_device.CreateCommandEncoder(&encoderDesc);
      const std::array attachments{wgpu::RenderPassColorAttachment{
          .view = image.view, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store}};
      const wgpu::RenderPassDescriptor passDesc{.label = "OpenXR quad copy",
          .colorAttachmentCount = 1, .colorAttachments = attachments.data()};
      auto pass = encoder.BeginRenderPass(&passDesc);
      pass.SetPipeline(webgpu::g_CopyPipeline);
      pass.SetBindGroup(0, source);
      pass.Draw(3);
      pass.End();
      auto commands = encoder.Finish();
      webgpu::g_queue.Submit(1, &commands);
      // OpenXR may consume the resource immediately after release. EndAccess plus a synchronous
      // queue completion makes that ownership handoff explicit and correct before xrEndFrame.
      wgpu::SharedTextureMemoryEndAccessState end;
      image.memory.EndAccess(image.texture, &end);
      const auto done = webgpu::g_queue.OnSubmittedWorkDone(
          wgpu::CallbackMode::WaitAnyOnly,
          [](wgpu::QueueWorkDoneStatus, wgpu::StringView) {});
      webgpu::g_instance.WaitAny(done, UINT64_MAX);
      if (SUCCEEDED(g_copyAllocator->Reset()) &&
          SUCCEEDED(g_copyList->Reset(g_copyAllocator.Get(), nullptr))) {
        std::array<D3D12_RESOURCE_BARRIER, 2> before{};
        before[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        before[0].Transition = {image.intermediate.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
        before[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        before[1].Transition = {image.xr.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
        g_copyList->ResourceBarrier(before.size(), before.data());
        g_copyList->CopyResource(image.xr.texture, image.intermediate.Get());
        std::array<D3D12_RESOURCE_BARRIER, 2> after{before};
        std::swap(after[0].Transition.StateBefore, after[0].Transition.StateAfter);
        std::swap(after[1].Transition.StateBefore, after[1].Transition.StateAfter);
        g_copyList->ResourceBarrier(after.size(), after.data());
        if (SUCCEEDED(g_copyList->Close())) {
          ID3D12CommandList* lists[] = {g_copyList.Get()};
          g_d3dQueue->ExecuteCommandLists(1, lists);
          const uint64_t fenceValue = ++g_copyFenceValue;
          if (SUCCEEDED(g_d3dQueue->Signal(g_copyFence.Get(), fenceValue)) &&
              g_copyFence->GetCompletedValue() < fenceValue &&
              SUCCEEDED(g_copyFence->SetEventOnCompletion(fenceValue, g_copyEvent))) {
            WaitForSingleObject(g_copyEvent, INFINITE);
          }
        }
      }
      image.initialized = true;
    }
    const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_swapchain, &release);
    // The game frame is opaque. Blending its incidental alpha lets SteamVR's gray environment
    // wash out the image.
    quad.layerFlags = 0;
    quad.space = g_localSpace;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage = {g_swapchain, {{0, 0}, {static_cast<int32_t>(kWidth),
                                           static_cast<int32_t>(kHeight)}}, 0};
    // LOCAL space is established from the headset pose at session start. Keep the cinema screen
    // centered on that origin instead of adding an assumed standing-eye-height offset.
    quad.pose = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, -2.f}};
    quad.size = {3.2f, 1.8f};
    layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
  }
  const XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, frame.predictedDisplayTime,
                           XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                           frame.shouldRender ? 1u : 0u, layers.data()};
  xrEndFrame(g_session, &end);
}
} // namespace aurora::openxr
#else
namespace aurora::openxr {
bool initialize() noexcept { return false; }
void shutdown() noexcept {}
bool active() noexcept { return false; }
void present(wgpu::BindGroup) noexcept {}
}
#endif

#include "Application.h"

#include "controllers/AlarmController.h"
#include "imgui_impl_win32.h"
#include "views/AlarmView.h"
#include <cstdio>
#include <cstdlib>

// ─── Constants ───────────────────────────────────────────────────────────────
namespace {
constexpr UINT WM_TRAYICON				 = WM_APP + 1; // Custom message: tray icon mouse event callback
constexpr UINT IDM_SHOW						 = 1001;			 // Context menu: show / hide main window
constexpr UINT IDM_EXIT						 = 1002;			 // Context menu: exit application
constexpr UINT TRAY_UID						 = 1;					 // Icon ID passed to Shell_NotifyIcon
constexpr const wchar_t *APP_NAME	 = L"HanabiAlarm";
constexpr const wchar_t *WND_CLASS = L"HanabiAlarmWndClass";
constexpr int INIT_W							 = 1280;
constexpr int INIT_H							 = 800;
} // namespace

// ─── Constructor / Destructor (defined here so unique_ptr member can see complete types) ─
Application::Application()	= default;
Application::~Application() = default;

// ─── Static WndProc dispatch ─────────────────────────────────────────────────
Application *g_App = nullptr;

static LRESULT WINAPI WndProcDispatch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (g_App)
		return g_App->handleMessage(hwnd, msg, wp, lp);
	return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ─── run ─────────────────────────────────────────────────────────────────────
int Application::run()
{
	// Load settings first so initWindow_ can use the saved window size.
	ctrl_ = std::make_unique<alarm::controller::AlarmController>();
	ctrl_->load();

	initWindow_();
	initVulkan_();
	initImGui_();

	view_ = std::make_unique<alarm::view::AlarmView>(*ctrl_);

	initTray_();
	mainLoop_();

	// Release alarm subsystem before ImGui/Vulkan teardown
	view_.reset();
	ctrl_.reset();

	cleanup_();
	return 0;
}

// ─── initWindow_ ─────────────────────────────────────────────────────────────
void Application::initWindow_()
{
	wc_								= {};
	wc_.cbSize				= sizeof(wc_);
	wc_.style					= CS_CLASSDC;
	wc_.lpfnWndProc		= WndProcDispatch;
	wc_.hInstance			= ::GetModuleHandleW(nullptr);
	wc_.hIcon					= ::LoadIconW(nullptr, MAKEINTRESOURCEW(IDI_APPLICATION));
	wc_.hCursor				= ::LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
	wc_.lpszClassName = WND_CLASS;
	::RegisterClassExW(&wc_);

	const auto &savedSize = ctrl_->settings();
	hwnd_									= ::CreateWindowExW(0,
																						WND_CLASS,
																						APP_NAME,
																						WS_OVERLAPPEDWINDOW,
																						CW_USEDEFAULT,
																						CW_USEDEFAULT,
																						savedSize.window_width,
																						savedSize.window_height,
																						nullptr,
																						nullptr,
																						wc_.hInstance,
																						nullptr);

	::ShowWindow(hwnd_, SW_SHOWDEFAULT);
	::UpdateWindow(hwnd_);
}

// ─── Vulkan helpers ──────────────────────────────────────────────────────────
void Application::checkVkResult_(VkResult err) noexcept
{
	if (err == VK_SUCCESS)
		return;
	fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
	if (err < 0)
		abort();
}

bool Application::isExtensionAvailable_(const ImVector<VkExtensionProperties> &props, const char *name) noexcept
{
	for (const auto &p : props)
		if (strcmp(p.extensionName, name) == 0)
			return true;
	return false;
}

// ─── initVulkan_ ─────────────────────────────────────────────────────────────
void Application::initVulkan_()
{
	VkResult err;

	// Enumerate available instance extensions.
	uint32_t extCount = 0;
	ImVector<VkExtensionProperties> extProps;
	vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
	extProps.resize(static_cast<int>(extCount));
	vkEnumerateInstanceExtensionProperties(nullptr, &extCount, extProps.Data);

	// Build the extension list.
	ImVector<const char *> instExts;
	instExts.push_back("VK_KHR_surface");
	instExts.push_back("VK_KHR_win32_surface");
	if (isExtensionAvailable_(extProps, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
		instExts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef APP_USE_VULKAN_DEBUG_REPORT
	instExts.push_back("VK_EXT_debug_report");
#endif

	// Create Vulkan instance.
	VkInstanceCreateInfo instCI		 = {};
	instCI.sType									 = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instCI.enabledExtensionCount	 = static_cast<uint32_t>(instExts.Size);
	instCI.ppEnabledExtensionNames = instExts.Data;
#ifdef APP_USE_VULKAN_DEBUG_REPORT
	const char *validationLayers[] = {"VK_LAYER_KHRONOS_validation"};
	instCI.enabledLayerCount			 = 1;
	instCI.ppEnabledLayerNames		 = validationLayers;
#endif
	err = vkCreateInstance(&instCI, allocator_, &instance_);
	checkVkResult_(err);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
	// Register debug report callback.
	static auto debugCallback = [](VkDebugReportFlagsEXT,
																 VkDebugReportObjectTypeEXT,
																 uint64_t,
																 size_t,
																 int32_t,
																 const char *,
																 const char *msg,
																 void *) -> VkBool32 {
		fprintf(stderr, "[vulkan] %s\n", msg);
		return VK_FALSE;
	};
	auto fnCreate = reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(
			vkGetInstanceProcAddr(instance_, "vkCreateDebugReportCallbackEXT"));
	if (fnCreate) {
		VkDebugReportCallbackCreateInfoEXT dbgCI = {};
		dbgCI.sType															 = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
		dbgCI.flags =
				VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
		dbgCI.pfnCallback = debugCallback;
		fnCreate(instance_, &dbgCI, allocator_, &debugReport_);
	}
#endif

	// Select physical device and queue family.
	physDev_		 = ImGui_ImplVulkanH_SelectPhysicalDevice(instance_);
	queueFamily_ = ImGui_ImplVulkanH_SelectQueueFamilyIndex(physDev_);

	// Create logical device.
	{
		const char *devExts[]		= {"VK_KHR_swapchain"};
		const float queuePrio[] = {1.0f};

		VkDeviceQueueCreateInfo queueCI = {};
		queueCI.sType										= VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCI.queueFamilyIndex				= queueFamily_;
		queueCI.queueCount							= 1;
		queueCI.pQueuePriorities				= queuePrio;

		VkDeviceCreateInfo devCI			= {};
		devCI.sType										= VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		devCI.queueCreateInfoCount		= 1;
		devCI.pQueueCreateInfos				= &queueCI;
		devCI.enabledExtensionCount		= 1;
		devCI.ppEnabledExtensionNames = devExts;
		err														= vkCreateDevice(physDev_, &devCI, allocator_, &device_);
		checkVkResult_(err);
		vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
	}

	// Create descriptor pool.
	{
		VkDescriptorPoolSize poolSz				= {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
																				 IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE};
		VkDescriptorPoolCreateInfo poolCI = {};
		poolCI.sType											= VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolCI.flags											= VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		poolCI.maxSets										= poolSz.descriptorCount;
		poolCI.poolSizeCount							= 1;
		poolCI.pPoolSizes									= &poolSz;
		err																= vkCreateDescriptorPool(device_, &poolCI, allocator_, &descriptorPool_);
		checkVkResult_(err);
	}

	// Create Win32 surface.
	VkSurfaceKHR surface;
	VkWin32SurfaceCreateInfoKHR surfCI = {};
	surfCI.sType											 = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	surfCI.hwnd												 = hwnd_;
	surfCI.hinstance									 = ::GetModuleHandleW(nullptr);
	err																 = vkCreateWin32SurfaceKHR(instance_, &surfCI, allocator_, &surface);
	checkVkResult_(err);

	// Create swapchain / render pass / framebuffers.
	RECT rect;
	::GetClientRect(hwnd_, &rect);
	setupVulkanWindow_(surface, rect.right - rect.left, rect.bottom - rect.top);
}

// ─── setupVulkanWindow_ ──────────────────────────────────────────────────────
void Application::setupVulkanWindow_(VkSurfaceKHR surface, int w, int h)
{
	// Verify WSI support.
	VkBool32 wsiSupported;
	vkGetPhysicalDeviceSurfaceSupportKHR(physDev_, queueFamily_, surface, &wsiSupported);
	if (wsiSupported != VK_TRUE) {
		fprintf(stderr, "[vulkan] No WSI support on selected physical device.\n");
		abort();
	}

	wndData_.Surface = surface;

	const VkFormat requestFmts[] = {
			VK_FORMAT_B8G8R8A8_UNORM,
			VK_FORMAT_R8G8B8A8_UNORM,
			VK_FORMAT_B8G8R8_UNORM,
			VK_FORMAT_R8G8B8_UNORM,
	};
	wndData_.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
			physDev_, surface, requestFmts, static_cast<int>(IM_COUNTOF(requestFmts)), VK_COLORSPACE_SRGB_NONLINEAR_KHR);

	VkPresentModeKHR presentModes[] = {VK_PRESENT_MODE_FIFO_KHR};
	wndData_.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(physDev_, surface, presentModes, IM_COUNTOF(presentModes));

	ImGui_ImplVulkanH_CreateOrResizeWindow(
			instance_, physDev_, device_, &wndData_, queueFamily_, allocator_, w, h, minImageCount_, 0);
}

// ─── initImGui_ ──────────────────────────────────────────────────────────────
void Application::initImGui_()
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	// Load a font that covers CJK characters.
	// Microsoft JhengHei (Traditional Chinese) ships with every Windows 7+.
	// Fall back to the built-in Proggy font if the file is missing.
	if (!io.Fonts->AddFontFromFileTTF("data/msjh.ttc", 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull()))
		io.Fonts->AddFontDefault();

	ImGui::StyleColorsDark();

	ImGui_ImplWin32_Init(hwnd_);

	ImGui_ImplVulkan_InitInfo initInfo		= {};
	initInfo.Instance											= instance_;
	initInfo.PhysicalDevice								= physDev_;
	initInfo.Device												= device_;
	initInfo.QueueFamily									= queueFamily_;
	initInfo.Queue												= queue_;
	initInfo.PipelineCache								= pipelineCache_;
	initInfo.DescriptorPool								= descriptorPool_;
	initInfo.MinImageCount								= minImageCount_;
	initInfo.ImageCount										= wndData_.ImageCount;
	initInfo.Allocator										= allocator_;
	initInfo.PipelineInfoMain.RenderPass	= wndData_.RenderPass;
	initInfo.PipelineInfoMain.Subpass			= 0;
	initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	initInfo.CheckVkResultFn							= checkVkResult_;
	ImGui_ImplVulkan_Init(&initInfo);
}

// ─── initTray_ ───────────────────────────────────────────────────────────────
void Application::initTray_()
{
	nid_									= {};
	nid_.cbSize						= sizeof(nid_);
	nid_.hWnd							= hwnd_;
	nid_.uID							= TRAY_UID;
	nid_.uFlags						= NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid_.uCallbackMessage = WM_TRAYICON;
	nid_.hIcon						= ::LoadIconW(nullptr, MAKEINTRESOURCEW(IDI_APPLICATION));
	wcscpy_s(nid_.szTip, APP_NAME);
	Shell_NotifyIconW(NIM_ADD, &nid_);
}

// ─── mainLoop_ ───────────────────────────────────────────────────────────────
void Application::mainLoop_()
{
	while (running_) {
		// Drain Win32 message queue.
		MSG msg;
		while (::PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
			::TranslateMessage(&msg);
			::DispatchMessageW(&msg);
			if (msg.message == WM_QUIT)
				running_ = false;
		}
		if (!running_)
			break;

		// Rebuild swapchain when the window is resized.
		if (swapChainRebuild_) {
			RECT r;
			::GetClientRect(hwnd_, &r);
			int w = r.right - r.left;
			int h = r.bottom - r.top;
			if (w > 0 && h > 0) {
				ImGui_ImplVulkan_SetMinImageCount(minImageCount_);
				ImGui_ImplVulkanH_CreateOrResizeWindow(
						instance_, physDev_, device_, &wndData_, queueFamily_, allocator_, w, h, minImageCount_, 0);
				wndData_.FrameIndex = 0;
				swapChainRebuild_		= false;
			}
		}

		// Skip rendering while minimised.
		if (!windowVisible_)
			continue;

		// ── New ImGui frame ──────────────────────────────────────────────
		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		// ── UI content ───────────────────────────────────────────────────
		view_->render();

		// ── Handle window resize request from Settings ────────────────────
		{
			int rw, rh;
			if (view_->pollWindowResize(rw, rh)) {
				RECT rc = {0, 0, rw, rh};
				::AdjustWindowRectEx(&rc,
														 static_cast<DWORD>(::GetWindowLongW(hwnd_, GWL_STYLE)),
														 FALSE,
														 static_cast<DWORD>(::GetWindowLongW(hwnd_, GWL_EXSTYLE)));
				::SetWindowPos(hwnd_, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
			}
		}

		// ── Handle close decision from hint dialog ────────────────────────
		using CD = alarm::view::AlarmView::CloseDecision;
		switch (view_->pollCloseDecision()) {
		case CD::Minimize:
			setWindowVisible_(false);
			break;
		case CD::Exit:
			running_ = false;
			::PostQuitMessage(0);
			break;
		default:
			break;
		}

		// ── Render ───────────────────────────────────────────────────────
		ImGui::Render();
		ImDrawData *draw		 = ImGui::GetDrawData();
		const bool minimised = (draw->DisplaySize.x <= 0.0f || draw->DisplaySize.y <= 0.0f);
		if (!minimised) {
			wndData_.ClearValue.color = {{0.12f, 0.12f, 0.12f, 1.0f}};
			frameRender_(draw);
			framePresent_();
		}
	}
}

// ─── frameRender_ ────────────────────────────────────────────────────────────
void Application::frameRender_(ImDrawData *draw)
{
	VkSemaphore acqSem	= wndData_.FrameSemaphores[wndData_.SemaphoreIndex].ImageAcquiredSemaphore;
	VkSemaphore rendSem = wndData_.FrameSemaphores[wndData_.SemaphoreIndex].RenderCompleteSemaphore;

	VkResult err =
			vkAcquireNextImageKHR(device_, wndData_.Swapchain, UINT64_MAX, acqSem, VK_NULL_HANDLE, &wndData_.FrameIndex);
	if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
		swapChainRebuild_ = true;
		return;
	}
	checkVkResult_(err);

	ImGui_ImplVulkanH_Frame *fd = &wndData_.Frames[wndData_.FrameIndex];
	checkVkResult_(vkWaitForFences(device_, 1, &fd->Fence, VK_TRUE, UINT64_MAX));
	checkVkResult_(vkResetFences(device_, 1, &fd->Fence));
	checkVkResult_(vkResetCommandPool(device_, fd->CommandPool, 0));

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType										 = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags										 = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	checkVkResult_(vkBeginCommandBuffer(fd->CommandBuffer, &beginInfo));

	VkRenderPassBeginInfo rpInfo		= {};
	rpInfo.sType										= VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rpInfo.renderPass								= wndData_.RenderPass;
	rpInfo.framebuffer							= fd->Framebuffer;
	rpInfo.renderArea.extent.width	= static_cast<uint32_t>(wndData_.Width);
	rpInfo.renderArea.extent.height = static_cast<uint32_t>(wndData_.Height);
	rpInfo.clearValueCount					= 1;
	rpInfo.pClearValues							= &wndData_.ClearValue;
	vkCmdBeginRenderPass(fd->CommandBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

	ImGui_ImplVulkan_RenderDrawData(draw, fd->CommandBuffer);

	vkCmdEndRenderPass(fd->CommandBuffer);
	checkVkResult_(vkEndCommandBuffer(fd->CommandBuffer));

	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo si								 = {};
	si.sType											 = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.waitSemaphoreCount					 = 1;
	si.pWaitSemaphores						 = &acqSem;
	si.pWaitDstStageMask					 = &waitStage;
	si.commandBufferCount					 = 1;
	si.pCommandBuffers						 = &fd->CommandBuffer;
	si.signalSemaphoreCount				 = 1;
	si.pSignalSemaphores					 = &rendSem;
	checkVkResult_(vkQueueSubmit(queue_, 1, &si, fd->Fence));
}

// ─── framePresent_ ───────────────────────────────────────────────────────────
void Application::framePresent_()
{
	if (swapChainRebuild_)
		return;

	VkSemaphore rendSem		= wndData_.FrameSemaphores[wndData_.SemaphoreIndex].RenderCompleteSemaphore;
	VkPresentInfoKHR pi		= {};
	pi.sType							= VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores		= &rendSem;
	pi.swapchainCount			= 1;
	pi.pSwapchains				= &wndData_.Swapchain;
	pi.pImageIndices			= &wndData_.FrameIndex;

	VkResult err = vkQueuePresentKHR(queue_, &pi);
	if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
		swapChainRebuild_ = true;
		return;
	}
	checkVkResult_(err);
	wndData_.SemaphoreIndex = (wndData_.SemaphoreIndex + 1) % wndData_.SemaphoreCount;
}

// ─── showTrayMenu_ ───────────────────────────────────────────────────────────
void Application::showTrayMenu_()
{
	HMENU menu = ::CreatePopupMenu();
	::AppendMenuW(menu, MF_STRING, IDM_SHOW, windowVisible_ ? L"Hide" : L"Show");
	::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	::AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

	POINT pt;
	::GetCursorPos(&pt);
	::SetForegroundWindow(hwnd_);
	UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
	::DestroyMenu(menu);

	if (cmd == IDM_SHOW)
		setWindowVisible_(!windowVisible_);
	else if (cmd == IDM_EXIT) {
		running_ = false;
		::PostQuitMessage(0);
	}
}

// ─── setWindowVisible_ ───────────────────────────────────────────────────────
void Application::setWindowVisible_(bool v) noexcept
{
	windowVisible_ = v;
	::ShowWindow(hwnd_, v ? SW_SHOW : SW_HIDE);
}

// Explicit redeclaration: MSVC somehow loses the extern from imgui_impl_win32.h
// when large headers (nlohmann/json) are included between Application.h and this point.
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ─── handleMessage ───────────────────────────────────────────────────────────
LRESULT Application::handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
		return true;

	switch (msg) {
	case WM_SHOWWINDOW:
		// Sync our flag when the window is shown/hidden externally
		// (e.g. a second instance calling ShowWindow on our HWND).
		windowVisible_ = (wp != 0);
		break;

	case WM_EXITSIZEMOVE:
		// Auto-save window size after the user finishes resizing/moving.
		if (ctrl_) {
			RECT r;
			::GetWindowRect(hwnd_, &r);
			auto s					= ctrl_->settings();
			s.window_width	= r.right - r.left;
			s.window_height = r.bottom - r.top;
			ctrl_->saveSettings(std::move(s));
		}
		break;

	case WM_SIZE:
		if (device_ != VK_NULL_HANDLE && wp != SIZE_MINIMIZED) {
			int w = static_cast<int>(LOWORD(lp));
			int h = static_cast<int>(HIWORD(lp));
			if (w > 0 && h > 0)
				swapChainRebuild_ = true;
		}
		return 0;

	case WM_SYSCOMMAND:
		// Suppress the Alt key menu.
		if ((wp & 0xFFF0) == SC_KEYMENU)
			return 0;
		break;

	case WM_CLOSE:
		if (view_) {
			const auto &s = ctrl_->settings();
			if (!s.close_to_tray) {
				running_ = false;
				::PostQuitMessage(0);
			}
			else if (s.suppress_minimize_hint) {
				setWindowVisible_(false);
			}
			else {
				view_->triggerCloseHint();
			}
		}
		else {
			setWindowVisible_(false);
		}
		return 0;

	case WM_TRAYICON:
		switch (static_cast<UINT>(lp)) {
		case WM_RBUTTONUP:
			showTrayMenu_();
			break;
		case WM_LBUTTONDBLCLK:
			setWindowVisible_(!windowVisible_);
			break;
		}
		return 0;

	case WM_DESTROY:
		::PostQuitMessage(0);
		return 0;
	}

	return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ─── Cleanup ─────────────────────────────────────────────────────────────────
void Application::cleanupVulkan_()
{
	vkDestroyDescriptorPool(device_, descriptorPool_, allocator_);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
	auto fnDestroy = reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>(
			vkGetInstanceProcAddr(instance_, "vkDestroyDebugReportCallbackEXT"));
	if (fnDestroy && debugReport_ != VK_NULL_HANDLE)
		fnDestroy(instance_, debugReport_, allocator_);
#endif

	vkDestroyDevice(device_, allocator_);
	vkDestroyInstance(instance_, allocator_);
}

void Application::cleanupVulkanWindow_()
{
	ImGui_ImplVulkanH_DestroyWindow(instance_, device_, &wndData_, allocator_);
	vkDestroySurfaceKHR(instance_, wndData_.Surface, allocator_);
}

void Application::cleanup_()
{
	vkDeviceWaitIdle(device_);

	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	cleanupVulkanWindow_();
	cleanupVulkan_();

	Shell_NotifyIconW(NIM_DELETE, &nid_);

	::DestroyWindow(hwnd_);
	::UnregisterClassW(WND_CLASS, wc_.hInstance);
}

#include <windows.h>
#include "graphics-hook.h"

#define VK_USE_PLATFORM_WIN32_KHR

#include <malloc.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#undef VK_LAYER_EXPORT
#if defined(WIN32)
#define VK_LAYER_EXPORT __declspec(dllexport)
#else
#define VK_LAYER_EXPORT
#endif

#include <vulkan/vulkan_win32.h>

#define COBJMACROS
#include <dxgi.h>
#include <d3d11.h>

#include "vulkan-capture.h"

//#define ENABLE_FOR_UNEXPLAINABLE_AMD_CRASH 1

/* ======================================================================== */
/* defs/statics                                                                  */

/* shorten stuff because dear GOD is vulkan unclean. */
#define VKAPI VKAPI_CALL
#define VkFunc PFN_vkVoidFunction
#define EXPORT VK_LAYER_EXPORT

#define OBJ_MAX 16

/* use the loader's dispatch table pointer as a key for internal data maps */
#define GET_LDT(x) (*(void **)x)

#define DUMMY_WINDOW_CLASS_NAME L"graphics_hook_vk_dummy_window"

/* clang-format off */
static const GUID dxgi_factory1_guid =
{0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
static const GUID dxgi_resource_guid =
{0x035f3ab4, 0x482e, 0x4e50, {0xb4, 0x1f, 0x8a, 0x7f, 0x8b, 0xd8, 0x96, 0x0b}};
/* clang-format on */

static bool vulkan_seen = false;
static CRITICAL_SECTION mutex;

/* ======================================================================== */
/* hook data                                                                */

struct vk_swap_data {
	VkSwapchainKHR sc;
	VkExtent2D image_extent;
	VkFormat format;
	VkSurfaceKHR surf;
	VkImage export_image;
	VkDeviceMemory export_mem;
	VkImage swap_images[OBJ_MAX];

	HANDLE handle;
	struct shtex_data *shtex_info;
	ID3D11Texture2D *d3d11_tex;
	bool captured;
};

struct vk_data {
	bool valid;

	struct vk_device_funcs funcs;
	VkPhysicalDevice phy_device;
	VkDevice device;
	struct vk_swap_data swaps[OBJ_MAX];
	struct vk_swap_data *cur_swap;
	uint32_t swap_idx;

	uint32_t queue_fam_idx;
	VkCommandPool cmd_pool;
	VkCommandBuffer cmd_buffer;
	VkQueue queue;
	VkExternalMemoryPropertiesKHR external_mem_props;

	ID3D11Device *d3d11_device;
	ID3D11DeviceContext *d3d11_context;
	IDXGISwapChain *dxgi_swap;
	HWND dummy_hwnd;
};

static struct vk_swap_data *get_swap_data(struct vk_data *data,
					  VkSwapchainKHR sc)
{
	for (int i = 0; i < OBJ_MAX; i++) {
		if (data->swaps[i].sc == sc) {
			return &data->swaps[i];
		}
	}

	debug("get_swap_data failed, swapchain not found");
	return NULL;
}

static struct vk_swap_data *get_new_swap_data(struct vk_data *data)
{
	for (int i = 0; i < OBJ_MAX; i++) {
		if (data->swaps[i].surf == NULL && data->swaps[i].sc == NULL) {
			return &data->swaps[i];
		}
	}

	debug("get_new_swap_data failed, no more free slot");
	return NULL;
}

/* ------------------------------------------------------------------------- */

static inline size_t find_obj_idx(void *objs[], void *obj)
{
	EnterCriticalSection(&mutex);
	for (size_t i = 0; i < OBJ_MAX; i++) {
		if (objs[i] == obj) {
			return i;
		}
	}
	LeaveCriticalSection(&mutex);
	return SIZE_MAX;
}

static size_t get_obj_idx(void *objs[], void *obj)
{
	size_t idx = SIZE_MAX;

	EnterCriticalSection(&mutex);
	for (size_t i = 0; i < OBJ_MAX; i++) {
		if (objs[i] == obj) {
			idx = i;
			break;
		}
		if (!objs[i] && idx == SIZE_MAX) {
			idx = i;
		}
	}
	LeaveCriticalSection(&mutex);
	return idx;
}

/* ------------------------------------------------------------------------- */

static struct vk_data device_data[OBJ_MAX] = {0};
static void *devices[OBJ_MAX] = {0};

static inline struct vk_data *get_device_data(void *dev)
{
	size_t idx = get_obj_idx(devices, GET_LDT(dev));
	if (idx == SIZE_MAX) {
		debug("out of device slots");
		return NULL;
	}

	return &device_data[idx];
}

static void vk_shtex_free(struct vk_data *data)
{
	capture_free();

	for (int i = 0; i < OBJ_MAX; i++) {
		struct vk_swap_data *swap = &data->swaps[i];

		if (swap->export_image)
			data->funcs.DestroyImage(data->device,
						 swap->export_image, NULL);

		if (swap->export_mem)
			data->funcs.FreeMemory(data->device, swap->export_mem,
					       NULL);

		if (swap->d3d11_tex) {
			ID3D11Resource_Release(swap->d3d11_tex);
		}

		swap->handle = INVALID_HANDLE_VALUE;
		swap->d3d11_tex = NULL;
		swap->export_mem = NULL;
		swap->export_image = NULL;

		swap->captured = false;
	}

	if (data->d3d11_context) {
		ID3D11DeviceContext_Release(data->d3d11_context);
		data->d3d11_context = NULL;
	}
	if (data->d3d11_device) {
		ID3D11Device_Release(data->d3d11_device);
		data->d3d11_device = NULL;
	}
	if (data->dxgi_swap) {
		IDXGISwapChain_Release(data->dxgi_swap);
		data->dxgi_swap = NULL;
	}

	data->cur_swap = NULL;

	hlog("------------------ vulkan capture freed ------------------");
}

static void vk_remove_device(void *dev)
{
	size_t idx = find_obj_idx(devices, GET_LDT(dev));
	if (idx == SIZE_MAX) {
		return;
	}

	struct vk_data *data = &device_data[idx];

	memset(data, 0, sizeof(*data));

	EnterCriticalSection(&mutex);
	devices[idx] = NULL;
	LeaveCriticalSection(&mutex);
}

/* ------------------------------------------------------------------------- */

struct vk_surf_data {
	VkSurfaceKHR surf;
	HINSTANCE hinstance;
	HWND hwnd;
};

struct vk_inst_data {
	bool valid;

	struct vk_inst_funcs funcs;
	struct vk_surf_data surfaces[OBJ_MAX];
};

static struct vk_surf_data *find_surf_data(struct vk_inst_data *data,
					   VkSurfaceKHR surf)
{
	int idx = OBJ_MAX;
	for (int i = 0; i < OBJ_MAX; i++) {
		if (data->surfaces[i].surf == surf) {
			return &data->surfaces[i];
		} else if (data->surfaces[i].surf == NULL && idx == OBJ_MAX) {
			idx = i;
		}
	}
	if (idx != OBJ_MAX) {
		data->surfaces[idx].surf = surf;
		return &data->surfaces[idx];
	}

	debug("find_surf_data failed, no more free slots");
	return NULL;
}

/* ------------------------------------------------------------------------- */

static struct vk_inst_data inst_data[OBJ_MAX] = {0};
static void *instances[OBJ_MAX] = {0};

static struct vk_inst_data *get_inst_data(void *inst)
{
	size_t idx = get_obj_idx(instances, GET_LDT(inst));
	if (idx == SIZE_MAX) {
		debug("out of instance slots");
		return NULL;
	}

	vulkan_seen = true;
	return &inst_data[idx];
}

static inline struct vk_inst_funcs *get_inst_funcs(void *inst)
{
	struct vk_inst_data *data = get_inst_data(inst);
	return &data->funcs;
}

static void remove_instance(void *inst)
{
	size_t idx = find_obj_idx(instances, inst);
	if (idx == SIZE_MAX) {
		return;
	}

	struct vk_inst_data *data = &inst_data[idx];
	memset(data, 0, sizeof(*data));

	EnterCriticalSection(&mutex);
	instances[idx] = NULL;
	LeaveCriticalSection(&mutex);
}

/* ======================================================================== */
/* capture                                                                  */

static bool vk_register_window(void)
{
	WNDCLASSW wc = {0};
	wc.style = CS_OWNDC;
	wc.hInstance = GetModuleHandle(NULL);
	wc.lpfnWndProc = DefWindowProc;
	wc.lpszClassName = DUMMY_WINDOW_CLASS_NAME;

	if (!RegisterClassW(&wc)) {
		flog("failed to register window class: %d", GetLastError());
		return false;
	}

	return true;
}

static inline bool vk_shtex_init_window(struct vk_data *data)
{
	static bool registered = false;
	if (!registered) {
		static bool failure = false;
		if (failure || !vk_register_window()) {
			failure = true;
			return false;
		}
		registered = true;
	}

	data->dummy_hwnd = CreateWindowExW(
		0, DUMMY_WINDOW_CLASS_NAME, L"Dummy VK window, ignore",
		WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 2, 2, NULL,
		NULL, GetModuleHandle(NULL), NULL);
	if (!data->dummy_hwnd) {
		flog("failed to create window: %d", GetLastError());
		return false;
	}

	return true;
}

static inline bool vk_shtex_init_d3d11(struct vk_data *data)
{
	D3D_FEATURE_LEVEL level_used;
	IDXGIFactory1 *factory;
	IDXGIAdapter *adapter;
	HRESULT hr;

	HMODULE d3d11 = load_system_library("d3d11.dll");
	if (!d3d11) {
		flog("failed to load d3d11: %d", GetLastError());
		return false;
	}

	HMODULE dxgi = load_system_library("dxgi.dll");
	if (!dxgi) {
		flog("failed to load dxgi: %d", GetLastError());
		return false;
	}

	DXGI_SWAP_CHAIN_DESC desc = {0};
	desc.BufferCount = 2;
	desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.BufferDesc.Width = 2;
	desc.BufferDesc.Height = 2;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.SampleDesc.Count = 1;
	desc.Windowed = true;
	desc.OutputWindow = data->dummy_hwnd;

	HRESULT(WINAPI * create_factory)
	(REFIID, void **) = (void *)GetProcAddress(dxgi, "CreateDXGIFactory1");
	if (!create_factory) {
		flog("failed to get CreateDXGIFactory1 address: %d",
		     GetLastError());
		return false;
	}

	PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN create =
		(void *)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
	if (!create) {
		flog("failed to get D3D11CreateDeviceAndSwapChain address: %d",
		     GetLastError());
		return false;
	}

	hr = create_factory(&dxgi_factory1_guid, (void **)&factory);
	if (FAILED(hr)) {
		flog_hr("failed to create factory", hr);
		return false;
	}

	hr = IDXGIFactory1_EnumAdapters1(factory, 0,
					 (IDXGIAdapter1 **)&adapter);
	IDXGIFactory1_Release(factory);

	if (FAILED(hr)) {
		flog_hr("failed to create adapter", hr);
		return false;
	}

	static const D3D_FEATURE_LEVEL feature_levels[] = {
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_3,
	};

	hr = create(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, feature_levels,
		    sizeof(feature_levels) / sizeof(D3D_FEATURE_LEVEL),
		    D3D11_SDK_VERSION, &desc, &data->dxgi_swap,
		    &data->d3d11_device, &level_used, &data->d3d11_context);
	IDXGIAdapter_Release(adapter);

	if (FAILED(hr)) {
		flog_hr("failed to create device", hr);
		return false;
	}

	return true;
}

static inline bool vk_shtex_init_d3d11_tex(struct vk_data *data,
					   struct vk_swap_data *swap)
{
	IDXGIResource *dxgi_res;
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc = {0};
	desc.Width = swap->image_extent.width;
	desc.Height = swap->image_extent.height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;

	flog("OBS requesting %s texture format",
	     vk_format_to_str(swap->format));

	desc.Format = vk_format_to_dxgi(swap->format);
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	hr = ID3D11Device_CreateTexture2D(data->d3d11_device, &desc, NULL,
					  &swap->d3d11_tex);
	if (FAILED(hr)) {
		flog_hr("failed to create texture", hr);
		return false;
	}

	hr = ID3D11Device_QueryInterface(swap->d3d11_tex, &dxgi_resource_guid,
					 (void **)&dxgi_res);
	if (FAILED(hr)) {
		flog_hr("failed to get IDXGIResource", hr);
		return false;
	}

	hr = IDXGIResource_GetSharedHandle(dxgi_res, &swap->handle);
	IDXGIResource_Release(dxgi_res);

	if (FAILED(hr)) {
		flog_hr("failed to get shared handle", hr);
		return false;
	}

	return true;
}

static inline bool vk_shtex_init_vulkan_tex(struct vk_data *data,
					    struct vk_swap_data *swap)
{
	struct vk_device_funcs *funcs = &data->funcs;
	VkExternalMemoryFeatureFlags f =
		data->external_mem_props.externalMemoryFeatures;

	/* -------------------------------------------------------- */
	/* create texture                                           */

	VkExternalMemoryImageCreateInfoKHR emici;
	emici.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_KHR;
	emici.pNext = NULL;
	emici.handleTypes =
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

	VkImageCreateInfo ici;
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.pNext = &emici;
	ici.flags = 0;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = swap->format;
	ici.extent.width = swap->image_extent.width;
	ici.extent.height = swap->image_extent.height;
	ici.extent.depth = 1;
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.queueFamilyIndexCount = 0;
	ici.pQueueFamilyIndices = 0;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkResult res;
	res = funcs->CreateImage(data->device, &ici, NULL, &swap->export_image);
	if (VK_SUCCESS != res) {
		flog("failed to CreateImage: %s", result_to_str(res));
		swap->export_image = NULL;
		return false;
	}

	/* -------------------------------------------------------- */
	/* get image memory requirements                            */

	VkMemoryRequirements mr;
	bool use_gimr2 = f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT_KHR;

	if (use_gimr2) {
		VkMemoryDedicatedRequirementsKHR mdr = {0};
		mdr.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS_KHR;
		mdr.pNext = NULL;

		VkMemoryRequirements2KHR mr2 = {0};
		mr2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2_KHR;
		mr2.pNext = &mdr;

		VkImageMemoryRequirementsInfo2KHR imri2 = {0};
		imri2.sType =
			VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2_KHR;
		imri2.pNext = NULL;
		imri2.image = swap->export_image;

		funcs->GetImageMemoryRequirements2KHR(data->device, &imri2,
						      &mr2);
		mr = mr2.memoryRequirements;
	} else {
		funcs->GetImageMemoryRequirements(data->device,
						  swap->export_image, &mr);
	}

	/* -------------------------------------------------------- */
	/* get memory type index                                    */

	struct vk_inst_funcs *ifuncs = get_inst_funcs(data->phy_device);

	VkPhysicalDeviceMemoryProperties pdmp;
	ifuncs->GetPhysicalDeviceMemoryProperties(data->phy_device, &pdmp);

	uint32_t mem_type_idx = 0;

	for (; mem_type_idx < pdmp.memoryTypeCount; mem_type_idx++) {
		if ((mr.memoryTypeBits & (1 << mem_type_idx)) &&
		    (pdmp.memoryTypes[mem_type_idx].propertyFlags &
		     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ==
			    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
			break;
		}
	}

	if (mem_type_idx == pdmp.memoryTypeCount) {
		flog("failed to get memory type index");
		funcs->DestroyImage(data->device, swap->export_image, NULL);
		swap->export_image = NULL;
		return false;
	}

	/* -------------------------------------------------------- */
	/* allocate memory                                          */

	VkImportMemoryWin32HandleInfoKHR imw32hi;
	imw32hi.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
	imw32hi.pNext = NULL;
	imw32hi.name = NULL;
	imw32hi.handleType =
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
	imw32hi.handle = swap->handle;

	VkMemoryAllocateInfo mai;
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.pNext = &imw32hi;
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = mem_type_idx;

	VkMemoryDedicatedAllocateInfoKHR mdai;
	mdai.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	mdai.pNext = NULL;
	mdai.buffer = VK_NULL_HANDLE;

	if (data->external_mem_props.externalMemoryFeatures &
	    VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT_KHR) {
		mdai.image = swap->export_image;
		imw32hi.pNext = &mdai;
	}

	res = funcs->AllocateMemory(data->device, &mai, NULL,
				    &swap->export_mem);
	if (VK_SUCCESS != res) {
		flog("failed to AllocateMemory: %s", result_to_str(res));
		funcs->DestroyImage(data->device, swap->export_image, NULL);
		swap->export_image = NULL;
		return false;
	}

	/* -------------------------------------------------------- */
	/* bind image memory                                        */

	bool use_bi2 = f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT_KHR;

	if (use_bi2) {
		VkBindImageMemoryInfoKHR bimi = {0};
		bimi.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
		bimi.image = swap->export_image;
		bimi.memory = swap->export_mem;
		bimi.memoryOffset = 0;
		res = funcs->BindImageMemory2KHR(data->device, 1, &bimi);
	} else {
		res = funcs->BindImageMemory(data->device, swap->export_image,
					     swap->export_mem, 0);
	}
	if (VK_SUCCESS != res) {
		flog("%s failed: %s",
		     use_bi2 ? "BindImageMemory2KHR" : "BindImageMemory",
		     result_to_str(res));
		funcs->DestroyImage(data->device, swap->export_image, NULL);
		swap->export_image = NULL;
		return false;
	}
	return true;
}

static bool vk_shtex_init(struct vk_data *data, HWND window,
			  struct vk_swap_data *swap)
{
	if (!vk_shtex_init_window(data)) {
		return false;
	}
	if (!vk_shtex_init_d3d11(data)) {
		return false;
	}
	if (!vk_shtex_init_d3d11_tex(data, swap)) {
		return false;
	}
	if (!vk_shtex_init_vulkan_tex(data, swap)) {
		return false;
	}

	data->cur_swap = swap;

	swap->captured = capture_init_shtex(
		&swap->shtex_info, window, swap->image_extent.width,
		swap->image_extent.height, swap->image_extent.width,
		swap->image_extent.height, (uint32_t)swap->format, false,
		(uintptr_t)swap->handle);

	if (swap->captured) {
		if (global_hook_info->force_shmem) {
			flog("shared memory capture currently "
			     "unsupported; ignoring");
		}

		hlog("vulkan shared texture capture successful");
		return true;
	}

	return false;
}

static void vk_shtex_capture(struct vk_data *data,
			     struct vk_device_funcs *funcs,
			     struct vk_swap_data *swap, uint32_t idx,
			     const VkPresentInfoKHR *info)
{
	VkResult res = VK_SUCCESS;

	VkCommandBufferBeginInfo begin_info;
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.pNext = NULL;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	begin_info.pInheritanceInfo = NULL;

	/* ------------------------------------------------------ */
	/* do image copy                                          */

	res = funcs->BeginCommandBuffer(data->cmd_buffer, &begin_info);
	debug_res("BeginCommandBuffer", res);

	VkImage cur_backbuffer = swap->swap_images[info->pImageIndices[idx]];

	/* ------------------------------------------------------ */
	/* transition cur_backbuffer to transfer source state     */

	VkImageMemoryBarrier src_mb;
	src_mb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	src_mb.pNext = NULL;
	src_mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	src_mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	src_mb.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	src_mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	src_mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	src_mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	src_mb.image = cur_backbuffer;
	src_mb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	src_mb.subresourceRange.baseMipLevel = 0;
	src_mb.subresourceRange.levelCount = 1;
	src_mb.subresourceRange.baseArrayLayer = 0;
	src_mb.subresourceRange.layerCount = 1;

	VkPipelineStageFlags src_stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkPipelineStageFlags dst_stages = VK_PIPELINE_STAGE_TRANSFER_BIT;

	funcs->CmdPipelineBarrier(data->cmd_buffer, src_stages, dst_stages, 0,
				  0, NULL, 0, NULL, 1, &src_mb);

	/* ------------------------------------------------------ */
	/* transition exportedTexture to transfer dest state      */

	VkImageMemoryBarrier dst_mb;
	dst_mb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	dst_mb.pNext = NULL;
	dst_mb.srcAccessMask = 0;
	dst_mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	dst_mb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	dst_mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	dst_mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dst_mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dst_mb.image = swap->export_image;
	dst_mb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	dst_mb.subresourceRange.baseMipLevel = 0;
	dst_mb.subresourceRange.levelCount = 1;
	dst_mb.subresourceRange.baseArrayLayer = 0;
	dst_mb.subresourceRange.layerCount = 1;

	funcs->CmdPipelineBarrier(data->cmd_buffer, src_stages, dst_stages, 0,
				  0, NULL, 0, NULL, 1, &dst_mb);

	/* ------------------------------------------------------ */
	/* copy cur_backbuffer's content to our interop image     */

	VkImageCopy cpy;
	cpy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	cpy.srcSubresource.mipLevel = 0;
	cpy.srcSubresource.baseArrayLayer = 0;
	cpy.srcSubresource.layerCount = 1;
	cpy.srcOffset.x = 0;
	cpy.srcOffset.y = 0;
	cpy.srcOffset.z = 0;
	cpy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	cpy.dstSubresource.mipLevel = 0;
	cpy.dstSubresource.baseArrayLayer = 0;
	cpy.dstSubresource.layerCount = 1;
	cpy.dstOffset.x = 0;
	cpy.dstOffset.y = 0;
	cpy.dstOffset.z = 0;
	cpy.extent.width = swap->image_extent.width;
	cpy.extent.height = swap->image_extent.height;
	cpy.extent.depth = 1;
	funcs->CmdCopyImage(data->cmd_buffer, cur_backbuffer,
			    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    swap->export_image,
			    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);

	/* ------------------------------------------------------ */
	/* Restore the swap chain image layout to what it was 
	 * before.  This may not be strictly needed, but it is
	 * generally good to restore things to their original
	 * state.  */

	src_mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	src_mb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	src_mb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	src_mb.dstAccessMask = 0;
	funcs->CmdPipelineBarrier(data->cmd_buffer, src_stages, dst_stages, 0,
				  0, NULL, 0, NULL, 1, &src_mb);

	dst_mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	dst_mb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dst_mb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	dst_mb.dstAccessMask = 0;
	funcs->CmdPipelineBarrier(data->cmd_buffer, src_stages, dst_stages, 0,
				  0, NULL, 0, NULL, 1, &dst_mb);

	funcs->EndCommandBuffer(data->cmd_buffer);

	/* ------------------------------------------------------ */

	VkSubmitInfo submit_info;
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext = NULL;
	submit_info.waitSemaphoreCount = 0;
	submit_info.pWaitSemaphores = NULL;
	submit_info.pWaitDstStageMask = NULL;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &data->cmd_buffer;
	submit_info.signalSemaphoreCount = 0;
	submit_info.pSignalSemaphores = NULL;

	VkFence null_fence = {VK_NULL_HANDLE};

	res = funcs->QueueSubmit(data->queue, 1, &submit_info, null_fence);
	debug_res("QueueSubmit", res);
}

static inline HWND get_swap_window(struct vk_swap_data *swap)
{
	for (size_t i = 0; i < OBJ_MAX; i++) {
		struct vk_surf_data *surf_data =
			find_surf_data(&inst_data[i], swap->surf);

		if (!!surf_data && surf_data->surf == swap->surf) {
			return surf_data->hwnd;
		}
	}

	return NULL;
}

static void vk_capture(struct vk_data *data, const VkPresentInfoKHR *info)
{
	struct vk_swap_data *swap = NULL;
	HWND window = NULL;
	uint32_t idx = 0;

	debug("QueuePresentKHR called on "
	      "devicekey %p, swapchain count %d",
	      &data->funcs, info->swapchainCount);

	/* use first swap chain associated with a window */
	for (; idx < info->swapchainCount; idx++) {
		struct vk_swap_data *cur_swap =
			get_swap_data(data, info->pSwapchains[idx]);
		window = get_swap_window(cur_swap);
		if (!!window) {
			swap = cur_swap;
			break;
		}
	}

	if (!window) {
		return;
	}

	if (capture_should_stop()) {
		vk_shtex_free(data);
	}
	if (capture_should_init()) {
		if (!vk_shtex_init(data, window, swap)) {
			vk_shtex_free(data);
			data->valid = false;
		}
	}
	if (capture_ready()) {
		if (swap != data->cur_swap) {
			vk_shtex_free(data);
			return;
		}

		vk_shtex_capture(data, &data->funcs, swap, idx, info);
	}
}

static VkResult VKAPI OBS_QueuePresentKHR(VkQueue queue,
					  const VkPresentInfoKHR *info)
{
	struct vk_data *data = get_device_data(queue);
	struct vk_device_funcs *funcs = &data->funcs;

	if (data->valid) {
		vk_capture(data, info);
	}
	return funcs->QueuePresentKHR(queue, info);
}

/* ======================================================================== */
/* setup hooks                                                              */

struct ext_info {
	const char *name;
	bool found;
	bool enabled;
};

struct ext_spec {
	uint32_t count;
	const char *const *names;
};

#define get_ext_spec(x) ((struct ext_spec *)(&(x)->enabledExtensionCount))

static void *vk_enable_exts(struct ext_spec *spec, struct ext_info *exts,
			    size_t count)
{
	size_t enable_count = count;

	for (uint32_t i = 0; i < spec->count; i++) {
		for (size_t j = 0; j < count; j++) {
			const char *name = spec->names[i];
			struct ext_info *ext = &exts[j];

			if (!ext->enabled && strcmp(ext->name, name) == 0) {
				ext->enabled = true;
				enable_count--;
			}
		}
	}

	if (!enable_count) {
		return NULL;
	}

	uint32_t idx = spec->count;
	spec->count += (uint32_t)enable_count;

	const char **new_names = malloc(sizeof(const char *) * spec->count);
	for (uint32_t i = 0; i < idx; i++) {
		new_names[i] = (const char *)(spec->names[i]);
	}

	for (size_t i = 0; i < count; i++) {
		struct ext_info *ext = &exts[i];
		if (!ext->enabled) {
			new_names[idx++] = ext->name;
		}
	}

	spec->names = new_names;
	return (void *)new_names;
}

static inline bool is_inst_link_info(VkLayerInstanceCreateInfo *lici)
{
	return lici->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
	       lici->function == VK_LAYER_LINK_INFO;
}

static VkResult VKAPI OBS_CreateInstance(const VkInstanceCreateInfo *cinfo,
					 const VkAllocationCallbacks *ac,
					 VkInstance *p_inst)
{
	VkInstanceCreateInfo info = *cinfo;
	bool funcs_not_found = false;

	/* -------------------------------------------------------- */
	/* step through chain until we get to the link info         */

	VkLayerInstanceCreateInfo *lici = (void *)info.pNext;
	while (lici && !is_inst_link_info(lici)) {
		lici = (VkLayerInstanceCreateInfo *)lici->pNext;
	}

	if (lici == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	PFN_vkGetInstanceProcAddr gpa =
		lici->u.pLayerInfo->pfnNextGetInstanceProcAddr;

	/* -------------------------------------------------------- */
	/* move chain on for next layer                             */

	lici->u.pLayerInfo = lici->u.pLayerInfo->pNext;

	/* -------------------------------------------------------- */
	/* enable instance extensions we need                       */

	struct ext_info req_ext[] = {
		{.name = VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME},
		{.name = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME},
	};

	const size_t req_ext_count = sizeof(req_ext) / sizeof(req_ext[0]);

	void *a = vk_enable_exts(get_ext_spec(&info), req_ext, req_ext_count);

	/* -------------------------------------------------------- */
	/* create instance                                          */

	PFN_vkCreateInstance create = (void *)gpa(NULL, "vkCreateInstance");

	VkResult res = create(&info, ac, p_inst);
	VkInstance inst = *p_inst;
	free(a);

	/* -------------------------------------------------------- */
	/* fetch the functions we need                              */

	struct vk_inst_data *data = get_inst_data(inst);
	struct vk_inst_funcs *funcs = &data->funcs;

#define GETADDR(x)                                     \
	do {                                           \
		funcs->x = (void *)gpa(inst, "vk" #x); \
		if (!funcs->x) {                       \
			flog("could not get instance " \
			     "address for %s",         \
			     #x);                      \
			funcs_not_found = true;        \
		}                                      \
	} while (false)

	GETADDR(GetInstanceProcAddr);
	GETADDR(DestroyInstance);
	GETADDR(EnumerateDeviceExtensionProperties);
	GETADDR(CreateWin32SurfaceKHR);
	GETADDR(GetPhysicalDeviceQueueFamilyProperties);
	GETADDR(GetPhysicalDeviceMemoryProperties);
	GETADDR(GetPhysicalDeviceImageFormatProperties2KHR);
#undef GETADDR

	data->valid = !funcs_not_found;
	return res;
}

static VkResult VKAPI OBS_DestroyInstance(VkInstance instance,
					  const VkAllocationCallbacks *ac)
{
	struct vk_inst_funcs *funcs = get_inst_funcs(instance);
	funcs->DestroyInstance(instance, ac);
	remove_instance(instance);
	return VK_SUCCESS;
}

static VkResult VKAPI OBS_EnumerateInstanceLayerProperties(
	uint32_t *p_count, VkLayerProperties *props)
{
	if (p_count)
		*p_count = 1;

	if (props) {
		strcpy(props->layerName, "VK_LAYER_OBS_HOOK");
		strcpy(props->description, "Open Broadcaster Software hook");
		props->implementationVersion = 1;
		props->specVersion = VK_API_VERSION_1_1;
	}
	return VK_SUCCESS;
}

static VkResult VKAPI OBS_EnumerateInstanceExtensionProperties(
	const char *name, uint32_t *p_count, VkExtensionProperties *props)
{
	if (name == NULL || strcmp(name, "VK_LAYER_OBS_HOOK"))
		return VK_ERROR_LAYER_NOT_PRESENT;

	if (p_count)
		*p_count = 1;

	if (props) {
		strcpy(props[0].extensionName,
		       VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
		props[0].specVersion = VK_API_VERSION_1_1;
	}

	return VK_SUCCESS;
}

static VkResult VKAPI OBS_EnumerateDeviceExtensionProperties(
	VkPhysicalDevice phy_device, const char *name, uint32_t *p_count,
	VkExtensionProperties *props)
{
	/* pass through any queries that aren't to us */
	if (name == NULL || strcmp(name, "VK_LAYER_OBS_HOOK")) {
		if (phy_device == VK_NULL_HANDLE)
			return VK_SUCCESS;

		struct vk_inst_funcs *funcs = get_inst_funcs(phy_device);
		return funcs->EnumerateDeviceExtensionProperties(
			phy_device, name, p_count, props);
	}

	if (p_count)
		*p_count = 2;

	if (props) {
		strcpy(props[0].extensionName,
		       VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
		props[0].specVersion = VK_API_VERSION_1_1;
		strcpy(props[1].extensionName,
		       VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
		props[1].specVersion = VK_API_VERSION_1_1;
	}

	return VK_SUCCESS;
}

#if ENABLE_FOR_UNEXPLAINABLE_AMD_CRASH
static bool
vk_shared_tex_supported(struct vk_inst_funcs *funcs,
			VkPhysicalDevice phy_device, VkFormat format,
			VkImageUsageFlags usage,
			VkExternalMemoryPropertiesKHR *external_mem_props)
{
	VkPhysicalDeviceImageFormatInfo2KHR info;
	VkPhysicalDeviceExternalImageFormatInfoKHR external_info;

	external_info.sType =
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO_KHR;
	external_info.pNext = NULL;
	external_info.handleType =
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT_KHR;

	info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2_KHR;
	info.pNext = &external_info;
	info.format = format;
	info.type = VK_IMAGE_TYPE_2D;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.flags = 0;
	info.usage = usage;

	VkExternalImageFormatPropertiesKHR external_props = {0};
	external_props.sType =
		VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES_KHR;
	external_props.pNext = NULL;

	VkImageFormatProperties2KHR props = {0};
	props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2_KHR;
	props.pNext = &external_props;

	VkResult result = funcs->GetPhysicalDeviceImageFormatProperties2KHR(
		phy_device, &info, &props);

	*external_mem_props = external_props.externalMemoryProperties;

	const VkExternalMemoryFeatureFlags features =
		external_mem_props->externalMemoryFeatures;

	return ((VK_SUCCESS == result) &&
		(features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT_KHR));
}
#endif

static bool vk_init_req_extensions(VkPhysicalDevice phy_device,
				   VkDeviceCreateInfo *info,
				   struct vk_inst_funcs *funcs, void **a)
{
	struct ext_info req_ext[] = {
		{.name = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME},
		{.name = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME},
		{.name = VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME},
		{.name = VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME},
		{.name = VK_KHR_BIND_MEMORY_2_EXTENSION_NAME},
	};

	const size_t req_ext_count = sizeof(req_ext) / sizeof(req_ext[0]);
	size_t req_ext_found = 0;

	uint32_t count = 0;
	funcs->EnumerateDeviceExtensionProperties(phy_device, NULL, &count,
						  NULL);

	VkExtensionProperties *props =
		(VkExtensionProperties *)alloca(sizeof(*props) * count);
	funcs->EnumerateDeviceExtensionProperties(phy_device, NULL, &count,
						  props);

	for (uint32_t i = 0; i < count; i++) {
		for (size_t j = 0; j < req_ext_count; j++) {
			const char *name = props[i].extensionName;
			struct ext_info *ext = &req_ext[j];

			if (!ext->found && strcmp(ext->name, name) == 0) {
				ext->found = true;
				req_ext_found++;
			}
		}
	}

	if (req_ext_found != req_ext_count) {
		debug("device extensions needed to perform capture "
		      "are not available");
		return false;
	}

	*a = vk_enable_exts(get_ext_spec(info), req_ext, req_ext_count);
	return true;
}

static bool vk_get_usable_queue(VkPhysicalDevice phy_device,
				VkDeviceCreateInfo *info,
				struct vk_inst_funcs *funcs,
				uint32_t *p_fam_idx, void **b)
{
	uint32_t fam_idx = 0;
	uint32_t prop_count = 0;
	VkQueueFamilyProperties queue_fam_props[OBJ_MAX];

	funcs->GetPhysicalDeviceQueueFamilyProperties(phy_device, &prop_count,
						      NULL);
	if (prop_count > OBJ_MAX)
		prop_count = OBJ_MAX;

	funcs->GetPhysicalDeviceQueueFamilyProperties(phy_device, &prop_count,
						      queue_fam_props);

	VkQueueFlags search = (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
	VkDeviceQueueCreateInfo *mod_queues = NULL;
	bool found = false;

	for (uint32_t i = 0; i < info->queueCreateInfoCount; i++) {
		uint32_t idx = info->pQueueCreateInfos[i].queueFamilyIndex;
		if (idx >= prop_count) {
			continue;
		}

		/* this requested queue is one we can use too */
		if ((queue_fam_props[idx].queueFlags & search) == search &&
		    info->pQueueCreateInfos[i].queueCount > 0) {
			fam_idx = idx;
			found = true;
			break;
		}
	}
	/* if we didn't find it, search for which queue family we should add a
	 * request for */
	if (!found) {
		for (uint32_t i = 0; i < prop_count; i++) {
			if ((queue_fam_props[i].queueFlags & search) ==
			    search) {
				fam_idx = i;
				found = true;
				break;
			}
		}

		if (!found) {
			return false;
		}

		uint32_t count = info->queueCreateInfoCount;
		float one = 1.0f;

		/* we found the queue family, add it */
		mod_queues = malloc(sizeof(*mod_queues) * (count + 1));
		memcpy(mod_queues, info->pQueueCreateInfos,
		       sizeof(*mod_queues) * count);

		mod_queues[count].queueFamilyIndex = fam_idx;
		mod_queues[count].queueCount = 1;
		mod_queues[count].pQueuePriorities = &one;
		mod_queues[count].sType =
			VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		mod_queues[count].pNext = NULL;
		mod_queues[count].flags = 0;

		info->pQueueCreateInfos = mod_queues;
		info->queueCreateInfoCount++;

		*b = mod_queues;
	}

	*p_fam_idx = fam_idx;
	return true;
}

static inline bool is_device_link_info(VkLayerDeviceCreateInfo *lici)
{
	return lici->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
	       lici->function == VK_LAYER_LINK_INFO;
}

static VkResult VKAPI OBS_CreateDevice(VkPhysicalDevice phy_device,
				       const VkDeviceCreateInfo *cinfo,
				       const VkAllocationCallbacks *ac,
				       VkDevice *p_device)
{
	VkDeviceCreateInfo info = *cinfo;
	struct vk_inst_data *idata = get_inst_data(phy_device);
	struct vk_inst_funcs *ifuncs = &idata->funcs;
	struct vk_data *data = NULL;

	uint32_t fam_idx = 0;
	void *a = NULL, *b = NULL;
	VkResult ret = VK_ERROR_INITIALIZATION_FAILED;

	if (!vk_init_req_extensions(phy_device, &info, ifuncs, &a)) {
		goto fail;
	}
	if (!vk_get_usable_queue(phy_device, &info, ifuncs, &fam_idx, &b)) {
		goto fail;
	}

	VkLayerDeviceCreateInfo *ldci = (void *)info.pNext;

	/* -------------------------------------------------------- */
	/* step through chain until we get to the link info         */

	while (ldci && !is_device_link_info(ldci)) {
		ldci = (VkLayerDeviceCreateInfo *)ldci->pNext;
	}

	if (!ldci) {
		goto fail;
	}

	PFN_vkGetInstanceProcAddr gipa;
	PFN_vkGetDeviceProcAddr gdpa;

	gipa = ldci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	gdpa = ldci->u.pLayerInfo->pfnNextGetDeviceProcAddr;

	/* -------------------------------------------------------- */
	/* move chain on for next layer                             */

	ldci->u.pLayerInfo = ldci->u.pLayerInfo->pNext;

	/* -------------------------------------------------------- */
	/* create device and initialize hook data                   */

	PFN_vkCreateDevice createFunc =
		(PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");

	ret = createFunc(phy_device, idata->valid ? &info : cinfo, ac,
			 p_device);
	if (ret != VK_SUCCESS) {
		goto fail;
	}

	VkDevice device = *p_device;

	data = get_device_data(*p_device);
	struct vk_device_funcs *dfuncs = &data->funcs;

	data->valid = false; /* set true below if it doesn't go to fail */
	data->queue_fam_idx = fam_idx;
	data->phy_device = phy_device;
	data->device = device;

	/* -------------------------------------------------------- */
	/* fetch the functions we need                              */

	bool funcs_not_found = false;

#define GETADDR(x)                                         \
	do {                                               \
		dfuncs->x = (void *)gdpa(device, "vk" #x); \
		if (!dfuncs->x) {                          \
			flog("could not get device "       \
			     "address for %s",             \
			     #x);                          \
			funcs_not_found = true;            \
		}                                          \
	} while (false)

#define GETADDR_OPTIONAL(x)                                \
	do {                                               \
		dfuncs->x = (void *)gdpa(device, "vk" #x); \
	} while (false)

	GETADDR(GetDeviceProcAddr);
	GETADDR(DestroyDevice);
	GETADDR(CreateSwapchainKHR);
	GETADDR(DestroySwapchainKHR);
	GETADDR(QueuePresentKHR);
	GETADDR(AllocateMemory);
	GETADDR(FreeMemory);
	GETADDR(BindImageMemory);
	GETADDR_OPTIONAL(BindImageMemory2KHR);
	GETADDR(GetSwapchainImagesKHR);
	GETADDR(CreateImage);
	GETADDR(DestroyImage);
	GETADDR(GetImageMemoryRequirements);
	GETADDR_OPTIONAL(GetImageMemoryRequirements2KHR);
	GETADDR(BeginCommandBuffer);
	GETADDR(EndCommandBuffer);
	GETADDR(CmdCopyImage);
	GETADDR(CmdPipelineBarrier);
	GETADDR(GetDeviceQueue);
	GETADDR(QueueSubmit);
	GETADDR(QueueWaitIdle);
	GETADDR(DeviceWaitIdle);
	GETADDR(CreateCommandPool);
	GETADDR(AllocateCommandBuffers);
#undef GETADDR_OPTIONAL
#undef GETADDR

	if (funcs_not_found) {
		goto fail;
	}
	if (!idata->valid) {
		goto fail;
	}

	/* -------------------------------------------------------- */
	/* retrieve the queue                                       */

	dfuncs->GetDeviceQueue(*p_device, fam_idx, 0, &data->queue);

	if (data->cmd_pool == VK_NULL_HANDLE) {
		VkCommandPoolCreateInfo cpci;
		cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		cpci.pNext = NULL;
		cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		cpci.queueFamilyIndex = fam_idx;

		VkResult res = dfuncs->CreateCommandPool(device, &cpci, NULL,
							 &data->cmd_pool);
		debug_res("CreateCommandPool", res);

		VkCommandBufferAllocateInfo cbai;
		cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cbai.pNext = NULL;
		cbai.commandPool = data->cmd_pool;
		cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cbai.commandBufferCount = 1;

		res = dfuncs->AllocateCommandBuffers(device, &cbai,
						     &data->cmd_buffer);
		debug_res("AllocateCommandBuffers", res);
	}

#if ENABLE_FOR_UNEXPLAINABLE_AMD_CRASH
	VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				  VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	if (!vk_shared_tex_supported(ifuncs, phy_device, format, usage,
				     &data->external_mem_props)) {
		flog("texture sharing is not supported");
		goto fail;
	}
#endif

	data->valid = true;

fail:
	free(a);
	free(b);
	return ret;
}

static void VKAPI OBS_DestroyDevice(VkDevice device,
				    const VkAllocationCallbacks *ac)
{
	struct vk_data *data = get_device_data(device);
	if (data) {
		vk_remove_device(device);
		data->funcs.DestroyDevice(device, ac);
	}
}

static VkResult VKAPI
OBS_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
		       const VkAllocationCallbacks *ac, VkSwapchainKHR *p_sc)
{
	struct vk_data *data = get_device_data(device);
	struct vk_device_funcs *funcs = &data->funcs;

	struct vk_swap_data *swap = get_new_swap_data(data);

	swap->surf = info->surface;
	swap->image_extent = info->imageExtent;
	swap->format = info->imageFormat;

	VkResult res = funcs->CreateSwapchainKHR(device, info, ac, p_sc);
	VkSwapchainKHR sc = *p_sc;

	uint32_t count = 0;
	res = funcs->GetSwapchainImagesKHR(data->device, sc, &count, NULL);
	debug_res("GetSwapchainImagesKHR", res);

	if (count > 0) {
		if (count > OBJ_MAX)
			count = OBJ_MAX;

		res = funcs->GetSwapchainImagesKHR(data->device, sc, &count,
						   swap->swap_images);
		debug_res("GetSwapchainImagesKHR", res);
	}

	swap->sc = sc;
	return res;
}

static void VKAPI OBS_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR sc,
					  const VkAllocationCallbacks *ac)
{
	struct vk_data *data = get_device_data(device);
	struct vk_device_funcs *funcs = &data->funcs;

	struct vk_swap_data *swap = get_swap_data(data, sc);
	if (swap) {
		if (data->cur_swap == swap) {
			vk_shtex_free(data);
		}

		swap->sc = VK_NULL_HANDLE;
		swap->surf = NULL;
	}

	funcs->DestroySwapchainKHR(device, sc, ac);
}

static VkResult VKAPI OBS_CreateWin32SurfaceKHR(
	VkInstance inst, const VkWin32SurfaceCreateInfoKHR *info,
	const VkAllocationCallbacks *ac, VkSurfaceKHR *surf)
{
	struct vk_inst_data *data = get_inst_data(inst);
	struct vk_inst_funcs *funcs = &data->funcs;

	VkResult res = funcs->CreateWin32SurfaceKHR(inst, info, ac, surf);
	if (NULL != surf && VK_NULL_HANDLE != *surf) {
		struct vk_surf_data *surf_data = find_surf_data(data, *surf);

		surf_data->hinstance = info->hinstance;
		surf_data->hwnd = info->hwnd;
	}
	return res;
}

#define GETPROCADDR(func)              \
	if (!strcmp(name, "vk" #func)) \
		return (VkFunc)&OBS_##func;

static VkFunc VKAPI OBS_GetDeviceProcAddr(VkDevice dev, const char *name)
{
	struct vk_data *data = get_device_data(dev);
	struct vk_device_funcs *funcs = &data->funcs;

	debug_procaddr("vkGetDeviceProcAddr(%p, \"%s\")", dev, name);

	GETPROCADDR(GetDeviceProcAddr);
	GETPROCADDR(EnumerateDeviceExtensionProperties);
	GETPROCADDR(CreateDevice);
	GETPROCADDR(DestroyDevice);
	GETPROCADDR(CreateSwapchainKHR);
	GETPROCADDR(DestroySwapchainKHR);
	GETPROCADDR(QueuePresentKHR);

	if (funcs->GetDeviceProcAddr == NULL)
		return NULL;
	return funcs->GetDeviceProcAddr(dev, name);
}

static VkFunc VKAPI OBS_GetInstanceProcAddr(VkInstance inst, const char *name)
{
	debug_procaddr("vkGetInstanceProcAddr(%p, \"%s\")", inst, name);

	/* instance chain functions we intercept */
	GETPROCADDR(GetInstanceProcAddr);
	GETPROCADDR(EnumerateInstanceLayerProperties);
	GETPROCADDR(EnumerateInstanceExtensionProperties);
	GETPROCADDR(CreateInstance);
	GETPROCADDR(DestroyInstance);
	GETPROCADDR(CreateWin32SurfaceKHR);

	/* device chain functions we intercept */
	GETPROCADDR(GetDeviceProcAddr);
	GETPROCADDR(EnumerateDeviceExtensionProperties);
	GETPROCADDR(CreateDevice);
	GETPROCADDR(DestroyDevice);

	struct vk_inst_funcs *funcs = get_inst_funcs(inst);
	if (funcs->GetInstanceProcAddr == NULL)
		return NULL;
	return funcs->GetInstanceProcAddr(inst, name);
}

#undef GETPROCADDR

EXPORT VkResult VKAPI OBS_Negotiate(VkNegotiateLayerInterface *nli)
{
	if (nli->loaderLayerInterfaceVersion >= 2) {
		nli->sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
		nli->pNext = NULL;
		nli->pfnGetInstanceProcAddr = OBS_GetInstanceProcAddr;
		nli->pfnGetDeviceProcAddr = OBS_GetDeviceProcAddr;
		nli->pfnGetPhysicalDeviceProcAddr = NULL;
	}

	const uint32_t cur_ver = CURRENT_LOADER_LAYER_INTERFACE_VERSION;

	if (nli->loaderLayerInterfaceVersion > cur_ver) {
		nli->loaderLayerInterfaceVersion = cur_ver;
	}

	return VK_SUCCESS;
}

bool hook_vulkan(void)
{
	static bool hooked = false;
	if (!hooked && vulkan_seen) {
		hlog("Hooked Vulkan");
		hooked = true;
	}
	return hooked;
}

static bool vulkan_initialized = false;

bool init_vk_layer()
{
	if (!vulkan_initialized) {
		InitializeCriticalSection(&mutex);
		vulkan_initialized = true;
	}
	return true;
}

bool shutdown_vk_layer()
{
	if (vulkan_initialized) {
		DeleteCriticalSection(&mutex);
	}
	return true;
}

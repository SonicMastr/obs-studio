#include <windows.h>
#include "graphics-hook.h"

#if COMPILE_VULKAN_HOOK

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

#define EXPORT VK_LAYER_EXPORT

#include <vulkan/vulkan_win32.h>
#include <../Source/layers/vk_layer_dispatch_table.h>

#define VKAPI VKAPI_CALL

#define COBJMACROS
#include <dxgi.h>
#include <d3d11.h>

#include "vulkan-capture.h"

#define MAX_INSTANCE_COUNT 16
#define MAX_SURFACE_PER_INSTANCE 16
#define MAX_DEVICE_COUNT 16
#define MAX_SWAPCHAIN_PER_DEVICE 16
#define MAX_PHYSICALDEVICE_COUNT 16
#define MAX_IMAGES_PER_SWAPCHAIN 16

static bool initialized = false;
static bool hooked = false;
static CRITICAL_SECTION mutex;

/* use the loader's dispatch table pointer as a key for dispatch map lookups */
#define TOKEY(x) (*(void **)x)

struct swap_data {
	VkSwapchainKHR swap;
	VkExtent2D img_extent;
	VkFormat format;
	VkSurfaceKHR surface;
	VkImage export_image;
	VkDeviceMemory export_mem;
	HANDLE handle;
	struct shtex_data *shtex_info;
	ID3D11Texture2D *d3d11_tex;
	bool captured;
	VkImage swap_images[MAX_IMAGES_PER_SWAPCHAIN];
};

struct vk_data {
	VkLayerDispatchTable dispatch_table;
	VkPhysicalDevice phy_device;
	VkDevice device;
	struct swap_data swaps[MAX_SWAPCHAIN_PER_DEVICE];

	uint32_t queue_family_idx;
	VkCommandPool cmd_pool;
	VkCommandBuffer cmd_buffer;
	VkQueue queue;
	VkExternalMemoryPropertiesKHR external_mem_props;

	ID3D11Device *d3d11_device;
	ID3D11DeviceContext *d3d11_context;
	IDXGISwapChain *dxgi_swap;
	HWND dummy_hwnd;
};

static struct swap_data *get_swap_data(struct vk_data *data,
				       VkSwapchainKHR swapchain)
{
	for (int i = 0; i < MAX_SWAPCHAIN_PER_DEVICE; ++i) {
		if (data->swaps[i].swap == swapchain) {
			return &data->swaps[i];
		}
	}
	DbgOut("# OBS_Layer # get_swap_data failed, swapchain not found\n");
	return NULL;
}

static struct swap_data *get_new_swap_data(struct vk_data *data)
{
	for (int i = 0; i < MAX_SWAPCHAIN_PER_DEVICE; ++i) {
		if (data->swaps[i].surface == NULL &&
		    data->swaps[i].swap == NULL) {
			return &data->swaps[i];
		}
	}
	DbgOut("# OBS_Layer # get_new_swap_data failed, no more free slot\n");
	return NULL;
}

/* devices storage: devices/device_table share the same index maintain those on
 * the leading device_count elements */
struct vk_data device_table[MAX_DEVICE_COUNT];
static void *devices[MAX_DEVICE_COUNT];
static uint8_t device_count;

static inline uint8_t get_device_idx(VkDevice *dev)
{
	for (uint8_t i = 0; i < device_count; ++i) {
		if (devices[i] == dev) {
			return i;
		}
	}
	return UINT8_MAX;
};

static struct vk_data *get_device_data(void *dev)
{
	EnterCriticalSection(&mutex);

	struct vk_data *data = NULL;
	uint8_t idx = get_device_idx(dev);

	if (idx < device_count) {
		data = &device_table[idx];
	} else if (device_count >= MAX_DEVICE_COUNT - 1) {
		DbgOut("# OBS_Layer # Out of Device Slot\n");
	} else {
		struct vk_data *new_device_data = &device_table[device_count];
		devices[device_count] = dev;
		device_count++;
		data = new_device_data;
	}

	LeaveCriticalSection(&mutex);
	return data;
}

static inline VkLayerDispatchTable *GetDeviceDispatchTable(void *dev)
{
	struct vk_data *data = get_device_data(dev);
	return &data->dispatch_table;
}

static void vk_remove_device(void *dev)
{
	EnterCriticalSection(&mutex);
	uint8_t idx = get_device_idx(dev);
	struct vk_data *data = (struct vk_data *)(&devices[idx]);

	for (int i = 0; i < MAX_SWAPCHAIN_PER_DEVICE; ++i) {
		struct swap_data *swap = &data->swaps[i];

		if (swap->export_image)
			data->dispatch_table.DestroyImage(
				data->device, swap->export_image, NULL);

		if (swap->export_mem)
			data->dispatch_table.FreeMemory(data->device,
							swap->export_mem, NULL);

		swap->handle = INVALID_HANDLE_VALUE;
		swap->swap = VK_NULL_HANDLE;
		swap->surface = NULL;

		if (swap->d3d11_tex)
			ID3D11Resource_Release(swap->d3d11_tex);

		swap->captured = 0;
	}

	if (data->d3d11_context)
		ID3D11DeviceContext_Release(data->d3d11_context);
	if (data->d3d11_device)
		ID3D11Device_Release(data->d3d11_device);
	if (data->dxgi_swap)
		IDXGISwapChain_Release(data->dxgi_swap);

	if (idx > 0 && idx < device_count - 1) {
		devices[idx] = devices[device_count - 1];
		memcpy(&device_table[idx], &device_table[device_count - 1],
		       sizeof(struct vk_data));
	}

	devices[device_count - 1] = NULL;
	memset(&device_table[device_count - 1], 0, sizeof(struct vk_data));
	device_count--;
	LeaveCriticalSection(&mutex);
}

struct vk_surf_data {
	VkSurfaceKHR surface;
	HINSTANCE hinstance;
	HWND hwnd;
};

struct vk_inst_data {
	VkLayerInstanceDispatchTable dispatch_table;
	uint32_t phy_device_count;
	VkPhysicalDevice *availablePhysicalDevices[MAX_PHYSICALDEVICE_COUNT];

	struct vk_surf_data surfaces[MAX_SURFACE_PER_INSTANCE];
};

static struct vk_surf_data *FindSurfaceData(struct vk_inst_data *inst_data,
					    VkSurfaceKHR surface)
{
	int idx = MAX_SURFACE_PER_INSTANCE;
	for (int i = 0; i < MAX_SURFACE_PER_INSTANCE; ++i) {
		if (inst_data->surfaces[i].surface == surface) {
			return &inst_data->surfaces[i];
		} else if (inst_data->surfaces[i].surface == NULL &&
			   idx == MAX_SWAPCHAIN_PER_DEVICE) {
			idx = i;
		}
	}
	if (idx != MAX_SWAPCHAIN_PER_DEVICE) {
		inst_data->surfaces[idx].surface = surface;
		return &inst_data->surfaces[idx];
	}

	DbgOut("# OBS_Layer # FindSurfaceData failed, no more free slot\n");
	return NULL;
}

/* instances level disptach table storage: inst_keys/inst_table share the same
 * index maintain those on the leading inst_count elements */
static struct vk_inst_data inst_table[MAX_INSTANCE_COUNT];
static void *inst_keys[MAX_INSTANCE_COUNT];
static uint8_t inst_count;

static inline uint8_t GetInstanceIndex(void *inst)
{
	for (uint8_t i = 0; i < inst_count; ++i) {
		if (inst_keys[i] == inst) {
			return i;
		}
	}
	return UINT8_MAX;
};

static struct vk_inst_data *GetInstanceData(void *inst)
{
	EnterCriticalSection(&mutex);

	struct vk_inst_data *inst_data = NULL;
	uint8_t idx = GetInstanceIndex(inst);
	if (idx < inst_count) {
		inst_data = &inst_table[idx];
	} else if (inst_count >= MAX_INSTANCE_COUNT - 1) {
		DbgOut("# OBS_Layer # Out of Instance Slot\n");
	} else {

		struct vk_inst_data *newInstanceData = &inst_table[inst_count];
		inst_keys[inst_count] = inst;
		inst_count++;
		inst_data = newInstanceData;
	}
	LeaveCriticalSection(&mutex);
	return inst_data;
}

static VkLayerInstanceDispatchTable *GetInstanceDispatchTable(void *inst)
{
	struct vk_inst_data *inst_data = GetInstanceData(inst);
	return &inst_data->dispatch_table;
}

static void remove_instance(void *inst)
{
	EnterCriticalSection(&mutex);
	uint8_t idx = GetInstanceIndex(inst);

	if (idx > 0 && idx < inst_count - 1) {
		inst_keys[idx] = inst_keys[inst_count - 1];
		memcpy(&inst_table[idx], &inst_table[inst_count - 1],
		       sizeof(struct vk_inst_data));
	}

	inst_keys[inst_count - 1] = NULL;
	memset(&inst_table[inst_count - 1], 0, sizeof(struct vk_inst_data));
	inst_count--;
	LeaveCriticalSection(&mutex);
}

#define DUMMY_WINDOW_CLASS_NAME L"graphics_hook_vk_dummy_window"

/* clang-format off */
static const GUID GUID_IDXGIFactory1 =
{0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
static const GUID GUID_IDXGIResource =
{0x035f3ab4, 0x482e, 0x4e50, {0xb4, 0x1f, 0x8a, 0x7f, 0x8b, 0xd8, 0x96, 0x0b}};
/* clang-format on */

static inline bool vk_shtex_init_window(struct vk_data *data)
{
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

typedef HRESULT(WINAPI *create_dxgi_factory1_t)(REFIID, void **);

static const D3D_FEATURE_LEVEL feature_levels[] = {
	D3D_FEATURE_LEVEL_11_0,
	D3D_FEATURE_LEVEL_10_1,
	D3D_FEATURE_LEVEL_10_0,
	D3D_FEATURE_LEVEL_9_3,
};

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

	create_dxgi_factory1_t create_factory =
		(void *)GetProcAddress(dxgi, "CreateDXGIFactory1");
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

	hr = create_factory(&GUID_IDXGIFactory1, (void **)&factory);
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

static inline bool vk_shtex_init_d3d11_tex(struct vk_data *dev_data,
					   struct swap_data *swpch_data)
{
	IDXGIResource *dxgi_res;
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc = {0};
	desc.Width = swpch_data->img_extent.width;
	desc.Height = swpch_data->img_extent.height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;

	flog("OBS requesting %s texture format",
	     vk_format_to_str(swpch_data->format));

	desc.Format = vk_format_to_dxgi(swpch_data->format);
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	hr = ID3D11Device_CreateTexture2D(dev_data->d3d11_device, &desc, NULL,
					  &swpch_data->d3d11_tex);
	if (FAILED(hr)) {
		flog_hr("failed to create texture", hr);
		return false;
	}

	hr = ID3D11Device_QueryInterface(
		swpch_data->d3d11_tex, &GUID_IDXGIResource, (void **)&dxgi_res);
	if (FAILED(hr)) {
		flog_hr("failed to get IDXGIResource", hr);
		return false;
	}

	hr = IDXGIResource_GetSharedHandle(dxgi_res, &swpch_data->handle);
	IDXGIResource_Release(dxgi_res);

	if (FAILED(hr)) {
		flog_hr("failed to get shared handle", hr);
		return false;
	}

	return true;
}

static inline bool vk_shtex_init_vulkan_tex(struct vk_data *data,
					    struct swap_data *swap)
{

	VkLayerDispatchTable *dispatch_table = &data->dispatch_table;

	VkExternalMemoryImageCreateInfoKHR externalMemoryImageInfo;
	externalMemoryImageInfo.sType =
		VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_KHR;
	externalMemoryImageInfo.pNext = NULL;
	externalMemoryImageInfo.handleTypes =
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

	VkImageCreateInfo create_info;
	create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create_info.pNext = &externalMemoryImageInfo;
	create_info.flags = 0;
	create_info.imageType = VK_IMAGE_TYPE_2D;
	create_info.format = swap->format;
	create_info.extent.width = swap->img_extent.width;
	create_info.extent.height = swap->img_extent.height;
	create_info.extent.depth = 1;
	create_info.mipLevels = 1;
	create_info.arrayLayers = 1;
	create_info.samples = VK_SAMPLE_COUNT_1_BIT;
	create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	create_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
			    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create_info.queueFamilyIndexCount = 0;
	create_info.pQueueFamilyIndices = 0;
	create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkResult res;
	res = dispatch_table->CreateImage(data->device, &create_info, NULL,
					  &swap->export_image);
	DbgOutRes("# OBS_Layer # CreateImage %s\n", res);

	VkExportMemoryAllocateInfo export_info;
	export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
	export_info.pNext = NULL;
	export_info.handleTypes =
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

	VkMemoryRequirements req;

	if (data->external_mem_props.externalMemoryFeatures &
	    VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT_KHR) {
		VkMemoryDedicatedRequirementsKHR dedicated_req;
		memset(&dedicated_req, 0, sizeof(dedicated_req));
		dedicated_req.sType =
			VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS_KHR;
		dedicated_req.pNext = NULL;

		VkMemoryRequirements2KHR mem_req2;
		memset(&mem_req2, 0, sizeof(VkMemoryRequirements2KHR));
		mem_req2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2_KHR;
		mem_req2.pNext = &dedicated_req;

		VkImageMemoryRequirementsInfo2KHR img_req_info2;
		memset(&img_req_info2, 0,
		       sizeof(VkImageMemoryRequirementsInfo2KHR));
		img_req_info2.sType =
			VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2_KHR;
		img_req_info2.pNext = NULL;
		img_req_info2.image = swap->export_image;

		dispatch_table->GetImageMemoryRequirements2KHR(
			data->device, &img_req_info2, &mem_req2);
		req = mem_req2.memoryRequirements;
	} else {
		dispatch_table->GetImageMemoryRequirements(
			data->device, swap->export_image, &req);
	}

	uint32_t mem_type_idx = 0;

	VkLayerInstanceDispatchTable *inst_dispatch =
		GetInstanceDispatchTable(TOKEY(data->phy_device));

	VkPhysicalDeviceMemoryProperties mem_props;
	inst_dispatch->GetPhysicalDeviceMemoryProperties(data->phy_device,
							 &mem_props);

	for (; mem_type_idx < mem_props.memoryTypeCount; mem_type_idx++) {
		if ((req.memoryTypeBits & (1 << mem_type_idx)) &&
		    (mem_props.memoryTypes[mem_type_idx].propertyFlags &
		     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ==
			    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
			break;
		}
	}

	VkImportMemoryWin32HandleInfoKHR import_info;
	import_info.sType =
		VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
	import_info.pNext = NULL;
	import_info.name = NULL;
	import_info.handleType =
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
	import_info.handle = swap->handle;

	VkMemoryAllocateInfo mem_info;
	mem_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mem_info.pNext = &import_info;
	mem_info.allocationSize = req.size;
	mem_info.memoryTypeIndex = mem_type_idx;

	VkMemoryDedicatedAllocateInfoKHR alloc_info;
	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.buffer = VK_NULL_HANDLE;

	if (data->external_mem_props.externalMemoryFeatures &
	    VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT_KHR) {
		alloc_info.image = swap->export_image;
		import_info.pNext = &alloc_info;
	}

	res = dispatch_table->AllocateMemory(data->device, &mem_info, NULL,
					     &swap->export_mem);
	DbgOutRes("# OBS_Layer # AllocateMemory %s\n", res);

	if (VK_SUCCESS != res) {
		hlog("vk_shtex_init_vulkan_tex: failed to AllocateMemory : %s",
		     result_to_str(res));
		dispatch_table->DestroyImage(data->device, swap->export_image,
					     NULL);
		swap->export_image = NULL;
		return false;
	}

	if (!(data->external_mem_props.externalMemoryFeatures &
	      VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT_KHR)) {
		res = dispatch_table->BindImageMemory(
			data->device, swap->export_image, swap->export_mem, 0);
		DbgOutRes("# OBS_Layer # BindImageMemory %s\n", res);
	} else {
		VkBindImageMemoryInfoKHR bind_info;
		memset(&bind_info, 0, sizeof(VkBindImageMemoryInfoKHR));
		bind_info.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
		bind_info.image = swap->export_image;
		bind_info.memory = swap->export_mem;
		bind_info.memoryOffset = 0;
		res = dispatch_table->BindImageMemory2KHR(data->device, 1,
							  &bind_info);
		DbgOutRes("# OBS_Layer # BindImageMemory2KHR %s\n", res);
	}
	if (VK_SUCCESS != res) {
		dispatch_table->DestroyImage(data->device, swap->export_image,
					     NULL);
		return false;
	}
	return true;
}

bool init_vk_layer()
{
	if (!initialized) {
		InitializeCriticalSection(&mutex);

		inst_count = 0;
		memset(&inst_table, 0,
		       sizeof(struct vk_inst_data) * MAX_INSTANCE_COUNT);
		memset(&inst_keys, 0, sizeof(void *) * MAX_INSTANCE_COUNT);

		device_count = 0;
		memset(&device_table, 0,
		       sizeof(struct vk_data) * MAX_DEVICE_COUNT);
		memset(&devices, 0, sizeof(void *) * MAX_DEVICE_COUNT);

		initialized = true;
	}
	return true;
}

bool shutdown_vk_layer()
{
	if (initialized) {
		DeleteCriticalSection(&mutex);
		initialized = false;
	}
	return true;
}

EXPORT VkResult VKAPI OBS_CreateInstance(const VkInstanceCreateInfo *info,
					 const VkAllocationCallbacks *allocator,
					 VkInstance *p_inst)
{
	VkLayerInstanceCreateInfo *create_info =
		(VkLayerInstanceCreateInfo *)info->pNext;

	/* step through the chain of pNext until we get to the link info */
	while (create_info &&
	       (create_info->sType !=
			VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
		create_info->function != VK_LAYER_LINK_INFO)) {
		create_info = (VkLayerInstanceCreateInfo *)create_info->pNext;
	}

	if (create_info == NULL) {
		/* No loader instance create info */
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	PFN_vkGetInstanceProcAddr gpa =
		create_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	/* move chain on for next layer */
	create_info->u.pLayerInfo = create_info->u.pLayerInfo->pNext;

	PFN_vkCreateInstance create =
		(PFN_vkCreateInstance)gpa(VK_NULL_HANDLE, "vkCreateInstance");

	bool external_memory_capabilities_enabled = false;
	bool get_phy_device_properties2_enabled = false;
	for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
		external_memory_capabilities_enabled |=
			(0 ==
			 strcmp(info->ppEnabledExtensionNames[i],
				VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME));
		get_phy_device_properties2_enabled |=
			(0 ==
			 strcmp(info->ppEnabledExtensionNames[i],
				VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME));
	}

	if (!external_memory_capabilities_enabled ||
	    !get_phy_device_properties2_enabled) {
		uint32_t count = info->enabledExtensionCount;
		uint32_t new_count = info->enabledExtensionCount;

		new_count += external_memory_capabilities_enabled ? 0 : 1;
		new_count += get_phy_device_properties2_enabled ? 0 : 1;

		const char **ext_names =
			(const char **)alloca(sizeof(const char *) * new_count);
		for (uint32_t i = 0; i < count; ++i) {
			ext_names[i] =
				(const char *)(info->ppEnabledExtensionNames[i]);
		}
		if (!external_memory_capabilities_enabled) {
			ext_names[count++] =
				VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME;
		}
		if (!get_phy_device_properties2_enabled) {
			ext_names[count++] =
				VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
		}
		VkInstanceCreateInfo *create_info =
			(VkInstanceCreateInfo *)(info);
		create_info->enabledExtensionCount = count;
		create_info->ppEnabledExtensionNames = ext_names;
	}

	VkResult res = create(info, allocator, p_inst);
	DbgOutRes("# OBS_Layer # CreateInstance %s\n", res);

	VkLayerInstanceDispatchTable *dispatch_table =
		GetInstanceDispatchTable(TOKEY(*p_inst));

	/* fetch our own dispatch table for the functions we need, into the
	 * next layer */
	dispatch_table->GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)gpa(
		*p_inst, "vkGetInstanceProcAddr");
	dispatch_table->DestroyInstance =
		(PFN_vkDestroyInstance)gpa(*p_inst, "vkDestroyInstance");
	dispatch_table->EnumerateDeviceExtensionProperties =
		(PFN_vkEnumerateDeviceExtensionProperties)gpa(
			*p_inst, "vkEnumerateDeviceExtensionProperties");
	dispatch_table->EnumeratePhysicalDevices =
		(PFN_vkEnumeratePhysicalDevices)gpa(
			*p_inst, "vkEnumeratePhysicalDevices");
	dispatch_table->CreateWin32SurfaceKHR =
		(PFN_vkCreateWin32SurfaceKHR)gpa(*p_inst,
						 "vkCreateWin32SurfaceKHR");

	dispatch_table->GetPhysicalDeviceQueueFamilyProperties =
		(PFN_vkGetPhysicalDeviceQueueFamilyProperties)gpa(
			*p_inst, "vkGetPhysicalDeviceQueueFamilyProperties");
	dispatch_table->GetPhysicalDeviceMemoryProperties =
		(PFN_vkGetPhysicalDeviceMemoryProperties)gpa(
			*p_inst, "vkGetPhysicalDeviceMemoryProperties");
	dispatch_table->GetPhysicalDeviceImageFormatProperties2KHR =
		(PFN_vkGetPhysicalDeviceImageFormatProperties2KHR)gpa(
			*p_inst,
			"vkGetPhysicalDeviceImageFormatProperties2KHR");

	return res;
}

EXPORT VkResult VKAPI
OBS_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator)
{
	VkLayerInstanceDispatchTable *dispatch_table =
		GetInstanceDispatchTable(TOKEY(instance));
	dispatch_table->DestroyInstance(instance, allocator);
	remove_instance(instance);
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI OBS_EnumerateInstanceLayerProperties(
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

VKAPI_ATTR VkResult VKAPI OBS_EnumerateInstanceExtensionProperties(
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

VKAPI_ATTR VkResult VKAPI OBS_EnumerateDeviceExtensionProperties(
	VkPhysicalDevice phy_device, const char *name, uint32_t *p_count,
	VkExtensionProperties *props)
{
	/* pass through any queries that aren't to us */
	if (name == NULL || strcmp(name, "VK_LAYER_OBS_HOOK")) {
		if (phy_device == VK_NULL_HANDLE)
			return VK_SUCCESS;

		VkLayerInstanceDispatchTable *disp =
			GetInstanceDispatchTable(TOKEY(phy_device));
		return disp->EnumerateDeviceExtensionProperties(
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

VKAPI_ATTR VkResult VKAPI OBS_EnumeratePhysicalDevices(
	VkInstance instance, uint32_t *p_count, VkPhysicalDevice *phy_devices)
{
	struct vk_inst_data *inst_data = GetInstanceData(TOKEY(instance));
	VkLayerInstanceDispatchTable *dispatch_table =
		&inst_data->dispatch_table;

	VkResult res = dispatch_table->EnumeratePhysicalDevices(
		instance, p_count, phy_devices);

	if (res == VK_SUCCESS) {
		uint32_t physical_count = *p_count;
		if (physical_count >= MAX_PHYSICALDEVICE_COUNT) {
			physical_count = MAX_PHYSICALDEVICE_COUNT;
			DbgOut2("# OBS_Layer # Out of physical device "
				"storage for instance %p, clamping to %d\n",
				instance, physical_count);
		}
		inst_data->phy_device_count = physical_count;

		if (phy_devices != NULL) {
			for (uint32_t i = 0; i < physical_count; ++i) {
				inst_data->availablePhysicalDevices[i] =
					(VkPhysicalDevice *)phy_devices[i];
			}
		}
	}
	return res;
}

static bool
shared_tex_supported(VkLayerInstanceDispatchTable *inst_disp,
		     VkPhysicalDevice phy_device, VkFormat format,
		     VkImageUsageFlags usage,
		     VkExternalMemoryPropertiesKHR *external_mem_props)
{
	VkPhysicalDeviceImageFormatInfo2KHR format_info;
	VkPhysicalDeviceExternalImageFormatInfoKHR external_img_format;

	external_img_format.sType =
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO_KHR;
	external_img_format.pNext = NULL;
#if 0
	external_img_format.handleType =
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT_KHR;
#endif
	external_img_format.handleType =
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT_KHR;

	format_info.sType =
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2_KHR;
	format_info.pNext = &external_img_format;
	format_info.format = format;
	format_info.type = VK_IMAGE_TYPE_2D;
	format_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	format_info.flags = 0;
	format_info.usage = usage;

	VkExternalImageFormatPropertiesKHR external_img_format_props;
	memset(&external_img_format_props, 0,
	       sizeof(VkExternalImageFormatPropertiesKHR));
	external_img_format_props.sType =
		VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES_KHR;
	external_img_format_props.pNext = NULL;

	VkImageFormatProperties2KHR format_props;
	memset(&format_props, 0, sizeof(VkImageFormatProperties2KHR));
	format_props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2_KHR;
	format_props.pNext = &external_img_format_props;

	VkResult result = inst_disp->GetPhysicalDeviceImageFormatProperties2KHR(
		phy_device, &format_info, &format_props);

	*external_mem_props =
		external_img_format_props.externalMemoryProperties;

	return ((VK_SUCCESS == result) &&
		(external_mem_props->externalMemoryFeatures &
		 VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT_KHR));
}

static bool vk_init_req_extensions(VkPhysicalDevice phy_device,
				   const VkDeviceCreateInfo *info,
				   VkDeviceCreateInfo *create_info,
				   VkLayerInstanceDispatchTable *inst_disp)
{
	struct ext_info {
		const char *name;
		bool found;
		bool enabled;
	};

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
	inst_disp->EnumerateDeviceExtensionProperties(phy_device, NULL, &count,
						      NULL);

	VkExtensionProperties *props =
		(VkExtensionProperties *)alloca(sizeof(*props) * count);
	inst_disp->EnumerateDeviceExtensionProperties(phy_device, NULL, &count,
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
		DbgOut("# OBS_Layer # Needed device extensions are not available\n");
		return false;
	}

	size_t req_enable_count = req_ext_count;

	for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
		for (size_t j = 0; j < req_ext_count; j++) {
			const char *name = info->ppEnabledExtensionNames[i];
			struct ext_info *ext = &req_ext[j];

			if (!ext->enabled && strcmp(ext->name, name) == 0) {
				ext->enabled = true;
				req_enable_count--;
			}
		}
	}

	uint32_t idx = info->enabledExtensionCount;
	create_info->enabledExtensionCount += (uint32_t)req_enable_count;

	const char **ext_names = (const char **)alloca(
		sizeof(const char *) * info->enabledExtensionCount);
	for (uint32_t i = 0; i < idx; i++) {
		ext_names[i] = (const char *)(info->ppEnabledExtensionNames[i]);
	}

	for (size_t i = 0; i < req_ext_count; i++) {
		struct ext_info *ext = &req_ext[i];
		if (!ext->enabled) {
			ext_names[idx++] = ext->name;
		}
	}

	create_info->ppEnabledExtensionNames = ext_names;
	return true;
}

VKAPI_ATTR VkResult VKAPI
OBS_CreateDevice(VkPhysicalDevice phy_device, const VkDeviceCreateInfo *info,
		 const VkAllocationCallbacks *allocator, VkDevice *p_device)
{
	VkDeviceCreateInfo *create_info = (VkDeviceCreateInfo *)(info);
	VkLayerInstanceDispatchTable *inst_disp =
		GetInstanceDispatchTable(TOKEY(phy_device));

	if (!vk_init_req_extensions(phy_device, info, create_info, inst_disp)) {
		/* TODO */
	}

	/* retrieve a usable queue, in order to issue our copy command */
	uint32_t family_idx = 0;
#pragma region(usablequeue)
	/* find or create a usable queue */
	uint32_t prop_count = 0;
	VkQueueFamilyProperties queue_fam_props[16];

	inst_disp->GetPhysicalDeviceQueueFamilyProperties(phy_device,
							  &prop_count, NULL);
	/* only support 16 queue family */
	prop_count = (prop_count > 16) ? 16 : prop_count;

	inst_disp->GetPhysicalDeviceQueueFamilyProperties(
		phy_device, &prop_count, queue_fam_props);

	/* find a queue that supports all capabilities, and if one doesn't
	 * exist, add it. */
	bool found = false;

	/* we need graphics, and if there is a graphics queue there must be a
	 * graphics & compute queue. */
	VkQueueFlags search = (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);

	/* for queue priorities, if we need it */
	float one = 1.0f;

	/* if we need to change the requested queues, it will point to this */
	VkDeviceQueueCreateInfo *mod_queues = NULL;

	for (uint32_t i = 0; i < info->queueCreateInfoCount; i++) {
		uint32_t idx = info->pQueueCreateInfos[i].queueFamilyIndex;
		/* this requested queue is one we can use too */
		if ((idx < prop_count) &&
		    (queue_fam_props[idx].queueFlags & search) == search &&
		    info->pQueueCreateInfos[i].queueCount > 0) {
			family_idx = idx;
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
				family_idx = i;
				found = true;
				break;
			}
		}

		if (!found) {
			return VK_ERROR_INITIALIZATION_FAILED;
		}

		/* we found the queue family, add it */
		mod_queues = (VkDeviceQueueCreateInfo *)alloca(
			sizeof(VkDeviceQueueCreateInfo) *
			(info->queueCreateInfoCount + 1));
		memcpy(mod_queues, info->pQueueCreateInfos,
		       sizeof(VkDeviceQueueCreateInfo) *
			       info->queueCreateInfoCount);

		mod_queues[info->queueCreateInfoCount].queueFamilyIndex =
			family_idx;
		mod_queues[info->queueCreateInfoCount].queueCount = 1;
		mod_queues[info->queueCreateInfoCount].pQueuePriorities = &one;
		mod_queues[info->queueCreateInfoCount].sType =
			VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		mod_queues[info->queueCreateInfoCount].pNext = NULL;
		mod_queues[info->queueCreateInfoCount].flags = 0;

		create_info->pQueueCreateInfos = mod_queues;
		create_info->queueCreateInfoCount++;
	}
#pragma endregion

	VkLayerDeviceCreateInfo *layer_create_info =
		(VkLayerDeviceCreateInfo *)info->pNext;

	/* step through the chain of pNext until we get to the link info */
	while (layer_create_info &&
	       (layer_create_info->sType !=
			VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
		layer_create_info->function != VK_LAYER_LINK_INFO)) {
		layer_create_info =
			(VkLayerDeviceCreateInfo *)layer_create_info->pNext;
	}

	if (layer_create_info == NULL) {
		/* No loader instance create info */
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	PFN_vkGetInstanceProcAddr gipa =
		layer_create_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr gdpa =
		layer_create_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	/* move chain on for next layer */
	layer_create_info->u.pLayerInfo =
		layer_create_info->u.pLayerInfo->pNext;

	PFN_vkCreateDevice createFunc =
		(PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");

	createFunc(phy_device, info, allocator, p_device);

	/* store the table by key */
	struct vk_data *data = get_device_data(TOKEY(*p_device));
	VkLayerDispatchTable *dispatch_table = &data->dispatch_table;

	/* store the queue_family_idx needed for graphics command */
	data->queue_family_idx = family_idx;

	/* store the phy_device on which device is created */
	data->phy_device = phy_device;

	/* store the device */
	data->device = *p_device;

	/* feed our dispatch table for the functions we need (function pointer
	 * into the next layer) */
	dispatch_table->GetDeviceProcAddr =
		(PFN_vkGetDeviceProcAddr)gdpa(*p_device, "vkGetDeviceProcAddr");
	dispatch_table->DestroyDevice =
		(PFN_vkDestroyDevice)gdpa(*p_device, "vkDestroyDevice");

	dispatch_table->CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)gdpa(
		*p_device, "vkCreateSwapchainKHR");
	dispatch_table->DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)gdpa(
		*p_device, "vkDestroySwapchainKHR");
	dispatch_table->QueuePresentKHR =
		(PFN_vkQueuePresentKHR)gdpa(*p_device, "vkQueuePresentKHR");

	dispatch_table->AllocateMemory =
		(PFN_vkAllocateMemory)gdpa(*p_device, "vkAllocateMemory");
	dispatch_table->FreeMemory =
		(PFN_vkFreeMemory)gdpa(*p_device, "vkFreeMemory");
	dispatch_table->BindImageMemory =
		(PFN_vkBindImageMemory)gdpa(*p_device, "vkBindImageMemory");
	dispatch_table->BindImageMemory2KHR = (PFN_vkBindImageMemory2KHR)gdpa(
		*p_device, "vkBindImageMemory2KHR");

	dispatch_table->GetSwapchainImagesKHR =
		(PFN_vkGetSwapchainImagesKHR)gdpa(*p_device,
						  "vkGetSwapchainImagesKHR");

	dispatch_table->CreateImage =
		(PFN_vkCreateImage)gdpa(*p_device, "vkCreateImage");
	dispatch_table->DestroyImage =
		(PFN_vkDestroyImage)gdpa(*p_device, "vkDestroyImage");
	dispatch_table->GetImageMemoryRequirements =
		(PFN_vkGetImageMemoryRequirements)gdpa(
			*p_device, "vkGetImageMemoryRequirements");
	dispatch_table->GetImageMemoryRequirements2KHR =
		(PFN_vkGetImageMemoryRequirements2KHR)gdpa(
			*p_device, "vkGetImageMemoryRequirements2KHR");

	dispatch_table->BeginCommandBuffer = (PFN_vkBeginCommandBuffer)gdpa(
		*p_device, "vkBeginCommandBuffer");
	dispatch_table->EndCommandBuffer =
		(PFN_vkEndCommandBuffer)gdpa(*p_device, "vkEndCommandBuffer");

	dispatch_table->CmdCopyImage =
		(PFN_vkCmdCopyImage)gdpa(*p_device, "vkCmdCopyImage");
#if 0
	/* might help handling formats */
	dispatch_table->CmdBlitImage = (PFN_vkCmdBlitImage)gdpa(*p_device,
			"vkCmdBlitImage");
#endif

	dispatch_table->CmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)gdpa(
		*p_device, "vkCmdPipelineBarrier");
	dispatch_table->GetDeviceQueue =
		(PFN_vkGetDeviceQueue)gdpa(*p_device, "vkGetDeviceQueue");
	dispatch_table->QueueSubmit =
		(PFN_vkQueueSubmit)gdpa(*p_device, "vkQueueSubmit");

	dispatch_table->QueueWaitIdle =
		(PFN_vkQueueWaitIdle)gdpa(*p_device, "vkQueueWaitIdle");
	dispatch_table->DeviceWaitIdle =
		(PFN_vkDeviceWaitIdle)gdpa(*p_device, "vkDeviceWaitIdle");

	dispatch_table->CreateCommandPool =
		(PFN_vkCreateCommandPool)gdpa(*p_device, "vkCreateCommandPool");
	dispatch_table->AllocateCommandBuffers =
		(PFN_vkAllocateCommandBuffers)gdpa(*p_device,
						   "vkAllocateCommandBuffers");

	/* retrieve the queue */
	dispatch_table->GetDeviceQueue(*p_device, family_idx, 0, &data->queue);

	if (data->cmd_pool == VK_NULL_HANDLE) {
		VkCommandPoolCreateInfo pool_info;
		pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool_info.pNext = NULL;
		pool_info.flags =
			VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pool_info.queueFamilyIndex = family_idx;

		VkResult res = dispatch_table->CreateCommandPool(
			*p_device, &pool_info, NULL, &data->cmd_pool);
		DbgOutRes("# OBS_Layer # CreateCommandPool %s\n", res);

		VkCommandBufferAllocateInfo cmd_info;
		cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cmd_info.pNext = NULL;
		cmd_info.commandPool = data->cmd_pool;
		cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cmd_info.commandBufferCount = 1;

		res = dispatch_table->AllocateCommandBuffers(
			*p_device, &cmd_info, &data->cmd_buffer);
		DbgOutRes("# OBS_Layer # AllocateCommandBuffers %s\n", res);
	}

	VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				  VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	if (!shared_tex_supported(inst_disp, phy_device, format, usage,
				  &data->external_mem_props)) {
		hlog(" Vulkan CreateDevice : texture sharing is not supported\n");
	}

	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI OBS_DestroyDevice(VkDevice device,
					const VkAllocationCallbacks *allocator)
{
	(void)allocator;
	vk_remove_device(&device);
}

VKAPI_ATTR VkResult VKAPI OBS_CreateSwapchainKHR(
	VkDevice device, const VkSwapchainCreateInfoKHR *info,
	const VkAllocationCallbacks *allocator, VkSwapchainKHR *p_swap)
{
	struct vk_data *data = get_device_data(TOKEY(device));
	VkLayerDispatchTable *dispatch_table = &data->dispatch_table;

	struct swap_data *swap = get_new_swap_data(data);

	swap->surface = info->surface;
	swap->img_extent = info->imageExtent;
	swap->format = info->imageFormat;

	VkResult res = dispatch_table->CreateSwapchainKHR(device, info,
							  allocator, p_swap);

	uint32_t count = 0;
	res = dispatch_table->GetSwapchainImagesKHR(data->device, *p_swap,
						    &count, NULL);
	DbgOutRes("# OBS_Layer # GetSwapchainImagesKHR %s\n", res);

	if (count > 0) {
		count = (count < MAX_IMAGES_PER_SWAPCHAIN)
				? count
				: MAX_IMAGES_PER_SWAPCHAIN;
		res = dispatch_table->GetSwapchainImagesKHR(
			data->device, *p_swap, &count, swap->swap_images);
		DbgOutRes("# OBS_Layer # GetSwapchainImagesKHR %s\n", res);
	}

	swap->swap = *p_swap;

	return res;
}

VKAPI_ATTR void VKAPI
OBS_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
			const VkAllocationCallbacks *allocator)
{
	struct vk_data *data = get_device_data(TOKEY(device));
	VkLayerDispatchTable *dispatch_table = &data->dispatch_table;

	struct swap_data *swap = get_swap_data(data, swapchain);
	if (swap) {
		if (swap->export_image)
			dispatch_table->DestroyImage(device, swap->export_image,
						     NULL);

		if (swap->export_mem)
			dispatch_table->FreeMemory(device, swap->export_mem,
						   NULL);

		swap->handle = INVALID_HANDLE_VALUE;
		swap->swap = VK_NULL_HANDLE;
		swap->surface = NULL;

		if (swap->d3d11_tex)
			ID3D11Resource_Release(swap->d3d11_tex);

		swap->captured = false;
	}

	dispatch_table->DestroySwapchainKHR(device, swapchain, allocator);
}

VKAPI_ATTR VkResult VKAPI OBS_QueuePresentKHR(VkQueue queue,
					      const VkPresentInfoKHR *info)
{
	VkResult res = VK_SUCCESS;
	struct vk_data *data = get_device_data(TOKEY(queue));
	VkLayerDispatchTable *dispatch_table = &data->dispatch_table;
	DbgOut2("# OBS_Layer # QueuePresentKHR called on "
		"devicekey %p, swapchaincount %d\n",
		dispatch_table, info->swapCount);

	for (uint32_t i = 0; i < info->swapchainCount; ++i) {
		struct swap_data *swap =
			get_swap_data(data, info->pSwapchains[i]);
		if (hooked) {

			HWND window = NULL;
			for (int inst = 0; inst < inst_count; ++inst) {
				struct vk_surf_data *surf_data =
					FindSurfaceData(&inst_table[inst],
							swap->surface);
				if (surf_data != NULL &&
				    surf_data->surface == swap->surface) {
					window = surf_data->hwnd;
				}
			}
			if (window != NULL && !swap->captured) {
				if (vk_shtex_init_window(data)) {
					if (vk_shtex_init_d3d11(data)) {
						if (vk_shtex_init_d3d11_tex(
							    data, swap)) {
							if (vk_shtex_init_vulkan_tex(
								    data,
								    swap)) {
								swap->captured = capture_init_shtex(
									&swap->shtex_info,
									window,
									swap->img_extent
										.width,
									swap->img_extent
										.height,
									swap->img_extent
										.width,
									swap->img_extent
										.height,
									(uint32_t)swap
										->format,
									false,
									(uintptr_t)swap
										->handle);
							}
						}
					}
				}
			}
		}

		if (swap->captured) {
			VkCommandBufferBeginInfo begin_info;
			begin_info.sType =
				VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			begin_info.pNext = NULL;
			begin_info.flags =
				VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			begin_info.pInheritanceInfo = NULL;

			/* do image copy */
			res = dispatch_table->BeginCommandBuffer(
				data->cmd_buffer, &begin_info);
			DbgOutRes("# OBS_Layer # BeginCommandBuffer %s\n", res);

			VkImage cur_backbuffer =
				swap->swap_images[info->pImageIndices[i]];

			/* transition cur_backbuffer to transfer source state */
			VkImageMemoryBarrier present_mem_barrier;
			present_mem_barrier.sType =
				VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			present_mem_barrier.pNext = NULL;
			present_mem_barrier.srcAccessMask =
				VK_ACCESS_TRANSFER_WRITE_BIT;
			present_mem_barrier.dstAccessMask =
				VK_ACCESS_TRANSFER_READ_BIT;
			present_mem_barrier.oldLayout =
				VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			present_mem_barrier.newLayout =
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			present_mem_barrier.srcQueueFamilyIndex =
				VK_QUEUE_FAMILY_IGNORED;
			present_mem_barrier.dstQueueFamilyIndex =
				VK_QUEUE_FAMILY_IGNORED;
			present_mem_barrier.image = cur_backbuffer;
			present_mem_barrier.subresourceRange.aspectMask =
				VK_IMAGE_ASPECT_COLOR_BIT;
			present_mem_barrier.subresourceRange.baseMipLevel = 0;
			present_mem_barrier.subresourceRange.levelCount = 1;
			present_mem_barrier.subresourceRange.baseArrayLayer = 0;
			present_mem_barrier.subresourceRange.layerCount = 1;

			/* transition exportedTexture to transfer dest state */
			VkImageMemoryBarrier dst_mem_barrier;
			dst_mem_barrier.sType =
				VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			dst_mem_barrier.pNext = NULL;
			dst_mem_barrier.srcAccessMask = 0;
			dst_mem_barrier.dstAccessMask =
				VK_ACCESS_TRANSFER_WRITE_BIT;
			dst_mem_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			dst_mem_barrier.newLayout =
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			dst_mem_barrier.srcQueueFamilyIndex =
				VK_QUEUE_FAMILY_IGNORED;
			dst_mem_barrier.dstQueueFamilyIndex =
				VK_QUEUE_FAMILY_IGNORED;
			dst_mem_barrier.image = swap->export_image;
			dst_mem_barrier.subresourceRange.aspectMask =
				VK_IMAGE_ASPECT_COLOR_BIT;
			dst_mem_barrier.subresourceRange.baseMipLevel = 0;
			dst_mem_barrier.subresourceRange.levelCount = 1;
			dst_mem_barrier.subresourceRange.baseArrayLayer = 0;
			dst_mem_barrier.subresourceRange.layerCount = 1;

			VkPipelineStageFlags src_stages =
				VK_PIPELINE_STAGE_TRANSFER_BIT;
			VkPipelineStageFlags dst_stages =
				VK_PIPELINE_STAGE_TRANSFER_BIT;

			dispatch_table->CmdPipelineBarrier(
				data->cmd_buffer, src_stages, dst_stages, 0, 0,
				NULL, 0, NULL, 1, &present_mem_barrier);

			dispatch_table->CmdPipelineBarrier(
				data->cmd_buffer, src_stages, dst_stages, 0, 0,
				NULL, 0, NULL, 1, &dst_mem_barrier);

			/* copy cur_backbuffer's content to our interop
			 * image */

			VkImageCopy cpy;
			cpy.srcSubresource.aspectMask =
				VK_IMAGE_ASPECT_COLOR_BIT;
			cpy.srcSubresource.mipLevel = 0;
			cpy.srcSubresource.baseArrayLayer = 0;
			cpy.srcSubresource.layerCount = 1;
			cpy.srcOffset.x = 0;
			cpy.srcOffset.y = 0;
			cpy.srcOffset.z = 0;
			cpy.dstSubresource.aspectMask =
				VK_IMAGE_ASPECT_COLOR_BIT;
			cpy.dstSubresource.mipLevel = 0;
			cpy.dstSubresource.baseArrayLayer = 0;
			cpy.dstSubresource.layerCount = 1;
			cpy.dstOffset.x = 0;
			cpy.dstOffset.y = 0;
			cpy.dstOffset.z = 0;
			cpy.extent.width = swap->img_extent.width;
			cpy.extent.height = swap->img_extent.height;
			cpy.extent.depth = 1;
			dispatch_table->CmdCopyImage(
				data->cmd_buffer, cur_backbuffer,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				swap->export_image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);

			/* Restore the swap chain image layout to what it was
			 * before.  This may not be strictly needed, but it is
			 * generally good to restore things to their original
			 * state.  */
			present_mem_barrier.oldLayout =
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			present_mem_barrier.newLayout =
				VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			present_mem_barrier.srcAccessMask =
				VK_ACCESS_TRANSFER_READ_BIT;
			present_mem_barrier.dstAccessMask = 0;
			dispatch_table->CmdPipelineBarrier(
				data->cmd_buffer, src_stages, dst_stages, 0, 0,
				NULL, 0, NULL, 1, &present_mem_barrier);

			dst_mem_barrier.oldLayout =
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			dst_mem_barrier.newLayout =
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			dst_mem_barrier.srcAccessMask =
				VK_ACCESS_TRANSFER_READ_BIT;
			dst_mem_barrier.dstAccessMask = 0;
			dispatch_table->CmdPipelineBarrier(
				data->cmd_buffer, src_stages, dst_stages, 0, 0,
				NULL, 0, NULL, 1, &dst_mem_barrier);

			dispatch_table->EndCommandBuffer(data->cmd_buffer);

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

			res = dispatch_table->QueueSubmit(
				data->queue, 1, &submit_info, null_fence);
			DbgOutRes("# OBS_Layer # QueueSubmit %s\n", res);
		}
	}

	return dispatch_table->QueuePresentKHR(queue, info);
}

VKAPI_ATTR VkResult VKAPI OBS_GetSwapchainImagesKHR(VkDevice device,
						    VkSwapchainKHR swapchain,
						    uint32_t *count,
						    VkImage *images)
{
	VkLayerDispatchTable *dispatch_table =
		GetDeviceDispatchTable(TOKEY(device));
	VkResult res = dispatch_table->GetSwapchainImagesKHR(device, swapchain,
							     count, images);
	return res;
}

VKAPI_ATTR VkResult VKAPI OBS_CreateWin32SurfaceKHR(
	VkInstance inst, const VkWin32SurfaceCreateInfoKHR *info,
	const VkAllocationCallbacks *allocator, VkSurfaceKHR *surf)
{
	struct vk_inst_data *inst_data = GetInstanceData(TOKEY(inst));
	DbgOut("# OBS_Layer # CreateWin32SurfaceKHR\n");

	VkResult res = inst_data->dispatch_table.CreateWin32SurfaceKHR(
		inst, info, allocator, surf);
	if (NULL != surf && VK_NULL_HANDLE != *surf) {
		struct vk_surf_data *surf_data =
			FindSurfaceData(inst_data, *surf);

		surf_data->hinstance = info->hinstance;
		surf_data->hwnd = info->hwnd;
	}
	return res;
}

#define GETPROCADDR(func)              \
	if (!strcmp(name, "vk" #func)) \
		return (PFN_vkVoidFunction)&OBS_##func;

EXPORT PFN_vkVoidFunction VKAPI OBS_GetDeviceProcAddr(VkDevice dev,
						      const char *name)
{
	DbgOutProcAddr(
		"# OBS_Layer # vkGetDeviceProcAddr [%s] called on device %p\n",
		name, dev);
	GETPROCADDR(GetDeviceProcAddr);
	GETPROCADDR(EnumerateDeviceExtensionProperties);
	GETPROCADDR(CreateDevice);
	GETPROCADDR(DestroyDevice);
	GETPROCADDR(CreateSwapchainKHR);
	GETPROCADDR(DestroySwapchainKHR);
	GETPROCADDR(QueuePresentKHR);
	GETPROCADDR(GetSwapchainImagesKHR);

	VkLayerDispatchTable *dispatch_table =
		GetDeviceDispatchTable(TOKEY(dev));
	if (dispatch_table->GetDeviceProcAddr == NULL)
		return NULL;
	return dispatch_table->GetDeviceProcAddr(dev, name);
}

EXPORT PFN_vkVoidFunction VKAPI OBS_GetInstanceProcAddr(VkInstance instance,
							const char *name)
{
	DbgOutProcAddr(
		"# OBS_Layer # vkGetInstanceProcAddr [%s] called on instance %p\n",
		name, instance);
	/* instance chain functions we intercept */
	GETPROCADDR(GetInstanceProcAddr);
	GETPROCADDR(EnumerateInstanceLayerProperties);
	GETPROCADDR(EnumerateInstanceExtensionProperties);
	GETPROCADDR(EnumeratePhysicalDevices);
	GETPROCADDR(CreateInstance);
	GETPROCADDR(DestroyInstance);
	GETPROCADDR(CreateWin32SurfaceKHR);

	/* device chain functions we intercept */
	GETPROCADDR(GetDeviceProcAddr);
	GETPROCADDR(EnumerateDeviceExtensionProperties);
	GETPROCADDR(CreateDevice);
	GETPROCADDR(DestroyDevice);

	VkLayerInstanceDispatchTable *dispatch_table =
		GetInstanceDispatchTable(TOKEY(instance));
	if (dispatch_table->GetInstanceProcAddr == NULL)
		return NULL;
	return dispatch_table->GetInstanceProcAddr(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI
OBS_GetPhysicalDeviceProcAddr(VkInstance instance, const char *name)
{
	VkLayerInstanceDispatchTable *instdt =
		GetInstanceDispatchTable(TOKEY(instance));
	if (instdt->GetPhysicalDeviceProcAddr == NULL) {
		return NULL;
	}

	return instdt->GetPhysicalDeviceProcAddr(instance, name);
}

EXPORT PFN_vkVoidFunction VKAPI
OBS_layerGetPhysicalDeviceProcAddr(VkInstance instance, const char *name)
{
	return OBS_GetPhysicalDeviceProcAddr(instance, name);
}

EXPORT VkResult VKAPI OBS_NegotiateLoaderLayerInterfaceVersion(
	VkNegotiateLayerInterface *layer_interface)
{
	if (layer_interface->loaderLayerInterfaceVersion >= 2) {
		layer_interface->sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
		layer_interface->pNext = NULL;
		layer_interface->pfnGetInstanceProcAddr =
			OBS_GetInstanceProcAddr;
		layer_interface->pfnGetDeviceProcAddr = OBS_GetDeviceProcAddr;
		layer_interface->pfnGetPhysicalDeviceProcAddr =
			OBS_layerGetPhysicalDeviceProcAddr;
	}

	if (layer_interface->loaderLayerInterfaceVersion >
	    CURRENT_LOADER_LAYER_INTERFACE_VERSION) {
		layer_interface->loaderLayerInterfaceVersion =
			CURRENT_LOADER_LAYER_INTERFACE_VERSION;
	}

	return VK_SUCCESS;
}

#include "graphics-hook.h"

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

bool hook_vulkan(void)
{
	if (inst_count > 0) {
		if (!vk_register_window()) {
			return true;
		}
		hlog("Hooked Vulkan");
		hooked = true;
	} else {
		hooked = false;
	}
	return hooked;
}

#endif

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

#include <vulkan/vulkan_win32.h>
#include <../Source/layers/vk_layer_dispatch_table.h>

/* shorten stuff because dear GOD is vulkan unclean. */
#define VKAPI VKAPI_CALL
#define VkFunc PFN_vkVoidFunction
#define EXPORT VK_LAYER_EXPORT

#define COBJMACROS
#include <dxgi.h>
#include <d3d11.h>

#include "vulkan-capture.h"

#define MAX_INSTANCE_COUNT 16
#define MAX_SURFACE_PER_INSTANCE 16
#define MAX_DEVICE_COUNT 16
#define MAX_QUEUES 16
#define MAX_SWAPCHAIN_PER_DEVICE 16
#define MAX_PHYSICALDEVICE_COUNT 16
#define MAX_IMAGES_PER_SWAPCHAIN 16

static bool initialized = false;
static bool hooked = false;
static CRITICAL_SECTION mutex;

/* use the loader's dispatch table pointer as a key for dispatch map lookups */
#define TOKEY(x) (*(void **)x)

struct swap_data {
	VkSwapchainKHR sc;
	VkExtent2D image_extent;
	VkFormat format;
	VkSurfaceKHR surf;
	VkImage export_image;
	VkDeviceMemory export_mem;
	VkImage swap_images[MAX_IMAGES_PER_SWAPCHAIN];

	HANDLE handle;
	struct shtex_data *shtex_info;
	ID3D11Texture2D *d3d11_tex;
	bool captured;
};

struct vk_data {
	VkLayerDispatchTable table;
	VkPhysicalDevice phy_device;
	VkDevice device;
	struct swap_data swaps[MAX_SWAPCHAIN_PER_DEVICE];

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

static struct swap_data *get_swap_data(struct vk_data *data, VkSwapchainKHR sc)
{
	for (int i = 0; i < MAX_SWAPCHAIN_PER_DEVICE; i++) {
		if (data->swaps[i].sc == sc) {
			return &data->swaps[i];
		}
	}
	DbgOut("# OBS_Layer # get_swap_data failed, swapchain not found\n");
	return NULL;
}

static struct swap_data *get_new_swap_data(struct vk_data *data)
{
	for (int i = 0; i < MAX_SWAPCHAIN_PER_DEVICE; i++) {
		if (data->swaps[i].surf == NULL && data->swaps[i].sc == NULL) {
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
	for (uint8_t i = 0; i < device_count; i++) {
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

static inline VkLayerDispatchTable *get_table(void *dev)
{
	struct vk_data *data = get_device_data(dev);
	return &data->table;
}

static void vk_remove_device(void *dev)
{
	EnterCriticalSection(&mutex);
	uint8_t idx = get_device_idx(dev);
	struct vk_data *data = (struct vk_data *)(&devices[idx]);

	for (int i = 0; i < MAX_SWAPCHAIN_PER_DEVICE; i++) {
		struct swap_data *swap = &data->swaps[i];

		if (swap->export_image)
			data->table.DestroyImage(data->device,
						 swap->export_image, NULL);

		if (swap->export_mem)
			data->table.FreeMemory(data->device, swap->export_mem,
					       NULL);

		swap->handle = INVALID_HANDLE_VALUE;
		swap->sc = VK_NULL_HANDLE;
		swap->surf = NULL;

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
	VkSurfaceKHR surf;
	HINSTANCE hinstance;
	HWND hwnd;
};

struct vk_inst_data {
	VkLayerInstanceDispatchTable table;
	uint32_t phy_device_count;
	VkPhysicalDevice *phy_devices[MAX_PHYSICALDEVICE_COUNT];

	struct vk_surf_data surfaces[MAX_SURFACE_PER_INSTANCE];
};

static struct vk_surf_data *find_surf_data(struct vk_inst_data *inst_data,
					   VkSurfaceKHR surf)
{
	int idx = MAX_SURFACE_PER_INSTANCE;
	for (int i = 0; i < MAX_SURFACE_PER_INSTANCE; i++) {
		if (inst_data->surfaces[i].surf == surf) {
			return &inst_data->surfaces[i];
		} else if (inst_data->surfaces[i].surf == NULL &&
			   idx == MAX_SWAPCHAIN_PER_DEVICE) {
			idx = i;
		}
	}
	if (idx != MAX_SWAPCHAIN_PER_DEVICE) {
		inst_data->surfaces[idx].surf = surf;
		return &inst_data->surfaces[idx];
	}

	DbgOut("# OBS_Layer # find_surf_data failed, no more free slot\n");
	return NULL;
}

/* instances level disptach table storage: inst_keys/inst_table share the same
 * index maintain those on the leading inst_count elements */
static struct vk_inst_data inst_table[MAX_INSTANCE_COUNT];
static void *inst_keys[MAX_INSTANCE_COUNT];
static uint8_t inst_count;

static inline uint8_t get_inst_idx(void *inst)
{
	for (uint8_t i = 0; i < inst_count; i++) {
		if (inst_keys[i] == inst) {
			return i;
		}
	}
	return UINT8_MAX;
};

static struct vk_inst_data *get_inst_data(void *inst)
{
	EnterCriticalSection(&mutex);

	struct vk_inst_data *inst_data = NULL;
	uint8_t idx = get_inst_idx(inst);
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

static VkLayerInstanceDispatchTable *get_inst_table(void *inst)
{
	struct vk_inst_data *inst_data = get_inst_data(inst);
	return &inst_data->table;
}

static void remove_instance(void *inst)
{
	EnterCriticalSection(&mutex);
	uint8_t idx = get_inst_idx(inst);

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
static const GUID dxgi_factory1_guid =
{0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
static const GUID dxgi_resource_guid =
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
	desc.Width = swpch_data->image_extent.width;
	desc.Height = swpch_data->image_extent.height;
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
		swpch_data->d3d11_tex, &dxgi_resource_guid, (void **)&dxgi_res);
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
	VkLayerDispatchTable *table = &data->table;

	VkExternalMemoryImageCreateInfoKHR external_mem_image_info;
	external_mem_image_info.sType =
		VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_KHR;
	external_mem_image_info.pNext = NULL;
	external_mem_image_info.handleTypes =
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

	VkImageCreateInfo create_info;
	create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create_info.pNext = &external_mem_image_info;
	create_info.flags = 0;
	create_info.imageType = VK_IMAGE_TYPE_2D;
	create_info.format = swap->format;
	create_info.extent.width = swap->image_extent.width;
	create_info.extent.height = swap->image_extent.height;
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
	res = table->CreateImage(data->device, &create_info, NULL,
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
		VkMemoryDedicatedRequirementsKHR dedicated_req = {0};
		dedicated_req.sType =
			VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS_KHR;
		dedicated_req.pNext = NULL;

		VkMemoryRequirements2KHR mem_req2 = {0};
		mem_req2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2_KHR;
		mem_req2.pNext = &dedicated_req;

		VkImageMemoryRequirementsInfo2KHR img_req_info2 = {0};
		img_req_info2.sType =
			VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2_KHR;
		img_req_info2.pNext = NULL;
		img_req_info2.image = swap->export_image;

		table->GetImageMemoryRequirements2KHR(
			data->device, &img_req_info2, &mem_req2);
		req = mem_req2.memoryRequirements;
	} else {
		table->GetImageMemoryRequirements(data->device,
						  swap->export_image, &req);
	}

	uint32_t mem_type_idx = 0;

	VkLayerInstanceDispatchTable *inst_dispatch =
		get_inst_table(TOKEY(data->phy_device));

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

	res = table->AllocateMemory(data->device, &mem_info, NULL,
				    &swap->export_mem);
	DbgOutRes("# OBS_Layer # AllocateMemory %s\n", res);

	if (VK_SUCCESS != res) {
		flog("failed to AllocateMemory: %s", result_to_str(res));
		table->DestroyImage(data->device, swap->export_image, NULL);
		swap->export_image = NULL;
		return false;
	}

	if (!(data->external_mem_props.externalMemoryFeatures &
	      VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT_KHR)) {
		res = table->BindImageMemory(data->device, swap->export_image,
					     swap->export_mem, 0);
		DbgOutRes("# OBS_Layer # BindImageMemory %s\n", res);
	} else {
		VkBindImageMemoryInfoKHR bind_info = {0};
		bind_info.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
		bind_info.image = swap->export_image;
		bind_info.memory = swap->export_mem;
		bind_info.memoryOffset = 0;
		res = table->BindImageMemory2KHR(data->device, 1, &bind_info);
		DbgOutRes("# OBS_Layer # BindImageMemory2KHR %s\n", res);
	}
	if (VK_SUCCESS != res) {
		table->DestroyImage(data->device, swap->export_image, NULL);
		return false;
	}
	return true;
}

bool init_vk_layer()
{
	if (!initialized) {
		InitializeCriticalSection(&mutex);

		inst_count = 0;
		memset(&inst_table, 0, sizeof(inst_table));
		memset(&inst_keys, 0, sizeof(inst_keys));

		device_count = 0;
		memset(&device_table, 0, sizeof(device_table));
		memset(&devices, 0, sizeof(devices));

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

struct ext_info {
	const char *name;
	bool found;
	bool enabled;
};

struct ext_spec {
	uint32_t count;
	const char *const *names;
};

#define get_ext_spec(x) ((struct ext_spec *)(&x->enabledExtensionCount))

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

EXPORT VkResult VKAPI OBS_CreateInstance(const VkInstanceCreateInfo *cinfo,
					 const VkAllocationCallbacks *allocator,
					 VkInstance *p_inst)
{
	VkInstanceCreateInfo *info = (void *)cinfo;

	/* -------------------------------------------------------- */
	/* step through chain until we get to the link info         */

	VkLayerInstanceCreateInfo *lici = (void *)info->pNext;
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

	void *a = vk_enable_exts(get_ext_spec(info), req_ext, req_ext_count);

	/* -------------------------------------------------------- */
	/* create instance                                          */

	PFN_vkCreateInstance create = (void *)gpa(NULL, "vkCreateInstance");

	VkResult res = create(info, allocator, p_inst);
	VkInstance inst = *p_inst;
	free(a);

	/* -------------------------------------------------------- */
	/* fetch the functions we need                              */

	VkLayerInstanceDispatchTable *table = get_inst_table(TOKEY(inst));

#define GETADDR(x)                                     \
	do {                                           \
		table->x = (void *)gpa(inst, "vk" #x); \
	} while (false)

	GETADDR(GetInstanceProcAddr);
	GETADDR(DestroyInstance);
	GETADDR(EnumerateDeviceExtensionProperties);
	GETADDR(EnumeratePhysicalDevices);
	GETADDR(CreateWin32SurfaceKHR);
	GETADDR(GetPhysicalDeviceQueueFamilyProperties);
	GETADDR(GetPhysicalDeviceMemoryProperties);
	GETADDR(GetPhysicalDeviceImageFormatProperties2KHR);
#undef GETADDR

	return res;
}

EXPORT VkResult VKAPI
OBS_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator)
{
	VkLayerInstanceDispatchTable *table = get_inst_table(TOKEY(instance));
	table->DestroyInstance(instance, allocator);
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

		VkLayerInstanceDispatchTable *disp =
			get_inst_table(TOKEY(phy_device));
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

static VkResult VKAPI OBS_EnumeratePhysicalDevices(
	VkInstance inst, uint32_t *p_count, VkPhysicalDevice *phy_devices)
{
	struct vk_inst_data *inst_data = get_inst_data(TOKEY(inst));
	VkLayerInstanceDispatchTable *table = &inst_data->table;

	VkResult res =
		table->EnumeratePhysicalDevices(inst, p_count, phy_devices);
	uint32_t count = *p_count;

	if (res == VK_SUCCESS) {
		if (count > MAX_PHYSICALDEVICE_COUNT) {
			count = MAX_PHYSICALDEVICE_COUNT;
			DbgOut2("# OBS_Layer # Out of physical device "
				"storage for instance %p, clamping to %d\n",
				inst, physical_count);
		}
		inst_data->phy_device_count = count;

		if (phy_devices != NULL) {
			for (uint32_t i = 0; i < count; i++) {
				inst_data->phy_devices[i] =
					(VkPhysicalDevice *)phy_devices[i];
			}
		}
	}
	return res;
}

static bool
vk_shared_tex_supported(VkLayerInstanceDispatchTable *table,
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

	VkResult result = table->GetPhysicalDeviceImageFormatProperties2KHR(
		phy_device, &info, &props);

	*external_mem_props = external_props.externalMemoryProperties;

	const VkExternalMemoryFeatureFlags features =
		external_mem_props->externalMemoryFeatures;

	return ((VK_SUCCESS == result) &&
		(features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT_KHR));
}

static bool vk_init_req_extensions(VkPhysicalDevice phy_device,
				   VkDeviceCreateInfo *info,
				   VkLayerInstanceDispatchTable *table,
				   void **a)
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
	table->EnumerateDeviceExtensionProperties(phy_device, NULL, &count,
						  NULL);

	VkExtensionProperties *props =
		(VkExtensionProperties *)alloca(sizeof(*props) * count);
	table->EnumerateDeviceExtensionProperties(phy_device, NULL, &count,
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
		DbgOut("# OBS_Layer # Needed device extensions "
		       "are not available\n");
		return false;
	}

	*a = vk_enable_exts(get_ext_spec(info), req_ext, req_ext_count);
	return true;
}

static bool vk_get_usable_queue(VkPhysicalDevice phy_device,
				VkDeviceCreateInfo *info,
				VkLayerInstanceDispatchTable *table,
				uint32_t *p_fam_idx, void **b)
{
	uint32_t fam_idx = 0;
	uint32_t prop_count = 0;
	VkQueueFamilyProperties queue_fam_props[MAX_QUEUES];

	table->GetPhysicalDeviceQueueFamilyProperties(phy_device, &prop_count,
						      NULL);
	if (prop_count > MAX_QUEUES)
		prop_count = MAX_QUEUES;

	table->GetPhysicalDeviceQueueFamilyProperties(phy_device, &prop_count,
						      queue_fam_props);

	bool found = false;

	VkQueueFlags search = (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);

	VkDeviceQueueCreateInfo *mod_queues = NULL;

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
			return VK_ERROR_INITIALIZATION_FAILED;
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
				       const VkAllocationCallbacks *allocator,
				       VkDevice *p_device)
{
	VkDeviceCreateInfo *info = (VkDeviceCreateInfo *)(cinfo);
	VkLayerInstanceDispatchTable *inst_disp =
		get_inst_table(TOKEY(phy_device));

	uint32_t fam_idx = 0;
	void *a = NULL, *b = NULL;
	VkResult ret = VK_ERROR_INITIALIZATION_FAILED;

	if (!vk_init_req_extensions(phy_device, info, inst_disp, &a)) {
		goto fail;
	}
	if (!vk_get_usable_queue(phy_device, info, inst_disp, &fam_idx, &b)) {
		goto fail;
	}

	VkLayerDeviceCreateInfo *ldci = (void *)info->pNext;

	/* -------------------------------------------------------- */
	/* step through chain until we get to the link info         */

	while (ldci && is_device_link_info(ldci)) {
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

	createFunc(phy_device, info, allocator, p_device);
	VkDevice device = *p_device;

	struct vk_data *data = get_device_data(TOKEY(*p_device));
	VkLayerDispatchTable *table = &data->table;

	data->queue_fam_idx = fam_idx;
	data->phy_device = phy_device;
	data->device = device;

	/* -------------------------------------------------------- */
	/* fetch the functions we need                              */

#define GETADDR(x)                                        \
	do {                                              \
		table->x = (void *)gdpa(device, "vk" #x); \
	} while (false)

	GETADDR(GetDeviceProcAddr);
	GETADDR(DestroyDevice);

	GETADDR(CreateSwapchainKHR);
	GETADDR(DestroySwapchainKHR);
	GETADDR(QueuePresentKHR);

	GETADDR(AllocateMemory);
	GETADDR(FreeMemory);
	GETADDR(BindImageMemory);
	GETADDR(BindImageMemory2KHR);

	GETADDR(GetSwapchainImagesKHR);

	GETADDR(CreateImage);
	GETADDR(DestroyImage);
	GETADDR(GetImageMemoryRequirements);
	GETADDR(GetImageMemoryRequirements2KHR);

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
#undef GETADDR

	/* -------------------------------------------------------- */
	/* retrieve the queue                                       */

	table->GetDeviceQueue(*p_device, fam_idx, 0, &data->queue);

	if (data->cmd_pool == VK_NULL_HANDLE) {
		VkCommandPoolCreateInfo cpci;
		cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		cpci.pNext = NULL;
		cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		cpci.queueFamilyIndex = fam_idx;

		VkResult res = table->CreateCommandPool(device, &cpci, NULL,
							&data->cmd_pool);
		DbgOutRes("# OBS_Layer # CreateCommandPool %s\n", res);

		VkCommandBufferAllocateInfo cbai;
		cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cbai.pNext = NULL;
		cbai.commandPool = data->cmd_pool;
		cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cbai.commandBufferCount = 1;

		res = table->AllocateCommandBuffers(device, &cbai,
						    &data->cmd_buffer);
		DbgOutRes("# OBS_Layer # AllocateCommandBuffers %s\n", res);
	}

	VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				  VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	if (!vk_shared_tex_supported(inst_disp, phy_device, format, usage,
				     &data->external_mem_props)) {
		flog("texture sharing is not supported\n");
	} else {
		ret = VK_SUCCESS;
	}

fail:
	free(a);
	free(b);
	return ret;
}

static void VKAPI OBS_DestroyDevice(VkDevice device,
				    const VkAllocationCallbacks *unused)
{
	(void)unused;
	vk_remove_device(&device);
}

static VkResult VKAPI
OBS_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
		       const VkAllocationCallbacks *ac, VkSwapchainKHR *p_sc)
{
	struct vk_data *data = get_device_data(TOKEY(device));
	VkLayerDispatchTable *table = &data->table;

	struct swap_data *swap = get_new_swap_data(data);

	swap->surf = info->surface;
	swap->image_extent = info->imageExtent;
	swap->format = info->imageFormat;

	VkResult res = table->CreateSwapchainKHR(device, info, ac, p_sc);
	VkSwapchainKHR sc = *p_sc;

	uint32_t count = 0;
	res = table->GetSwapchainImagesKHR(data->device, sc, &count, NULL);
	DbgOutRes("# OBS_Layer # GetSwapchainImagesKHR %s\n", res);

	if (count > 0) {
		if (count > MAX_IMAGES_PER_SWAPCHAIN)
			count = MAX_IMAGES_PER_SWAPCHAIN;

		res = table->GetSwapchainImagesKHR(data->device, sc, &count,
						   swap->swap_images);
		DbgOutRes("# OBS_Layer # GetSwapchainImagesKHR %s\n", res);
	}

	swap->sc = sc;
	return res;
}

static void VKAPI OBS_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR sc,
					  const VkAllocationCallbacks *ac)
{
	struct vk_data *data = get_device_data(TOKEY(device));
	VkLayerDispatchTable *table = &data->table;

	struct swap_data *swap = get_swap_data(data, sc);
	if (swap) {
		if (swap->export_image)
			table->DestroyImage(device, swap->export_image, NULL);
		if (swap->export_mem)
			table->FreeMemory(device, swap->export_mem, NULL);

		swap->handle = INVALID_HANDLE_VALUE;
		swap->sc = VK_NULL_HANDLE;
		swap->surf = NULL;
		swap->captured = false;

		if (swap->d3d11_tex)
			ID3D11Resource_Release(swap->d3d11_tex);
	}

	table->DestroySwapchainKHR(device, sc, ac);
}

/* ======================================================================== */

static void vk_capture(struct vk_data *data, VkLayerDispatchTable *table,
		       struct swap_data *swap, uint32_t idx,
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

	res = table->BeginCommandBuffer(data->cmd_buffer, &begin_info);
	DbgOutRes("# OBS_Layer # BeginCommandBuffer %s\n", res);

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

	table->CmdPipelineBarrier(data->cmd_buffer, src_stages, dst_stages, 0,
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

	table->CmdPipelineBarrier(data->cmd_buffer, src_stages, dst_stages, 0,
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
	table->CmdCopyImage(data->cmd_buffer, cur_backbuffer,
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
	table->CmdPipelineBarrier(data->cmd_buffer, src_stages, dst_stages, 0,
				  0, NULL, 0, NULL, 1, &src_mb);

	dst_mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	dst_mb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dst_mb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	dst_mb.dstAccessMask = 0;
	table->CmdPipelineBarrier(data->cmd_buffer, src_stages, dst_stages, 0,
				  0, NULL, 0, NULL, 1, &dst_mb);

	table->EndCommandBuffer(data->cmd_buffer);

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

	res = table->QueueSubmit(data->queue, 1, &submit_info, null_fence);
	DbgOutRes("# OBS_Layer # QueueSubmit %s\n", res);
}

static VkResult VKAPI OBS_QueuePresentKHR(VkQueue queue,
					  const VkPresentInfoKHR *info)
{
	struct vk_data *data = get_device_data(TOKEY(queue));
	VkLayerDispatchTable *table = &data->table;

	DbgOut2("# OBS_Layer # QueuePresentKHR called on "
		"devicekey %p, swapchaincount %d\n",
		table, info->swapCount);

	for (uint32_t i = 0; i < info->swapchainCount; i++) {
		struct swap_data *swap =
			get_swap_data(data, info->pSwapchains[i]);
		if (hooked) {
			HWND window = NULL;
			for (int inst = 0; inst < inst_count; inst++) {
				struct vk_surf_data *surf_data = find_surf_data(
					&inst_table[inst], swap->surf);
				if (surf_data != NULL &&
				    surf_data->surf == swap->surf) {
					window = surf_data->hwnd;
				}
			}

			if (window != NULL && !swap->captured &&
			    vk_shtex_init_window(data) &&
			    vk_shtex_init_d3d11(data) &&
			    vk_shtex_init_d3d11_tex(data, swap) &&
			    vk_shtex_init_vulkan_tex(data, swap)) {
				swap->captured = capture_init_shtex(
					&swap->shtex_info, window,
					swap->image_extent.width,
					swap->image_extent.height,
					swap->image_extent.width,
					swap->image_extent.height,
					(uint32_t)swap->format, false,
					(uintptr_t)swap->handle);
			}
		}

		if (swap->captured) {
			vk_capture(data, table, swap, i, info);
		}
	}

	return table->QueuePresentKHR(queue, info);
}

/* ======================================================================== */

static VkResult VKAPI OBS_GetSwapchainImagesKHR(VkDevice device,
						VkSwapchainKHR sc,
						uint32_t *count,
						VkImage *images)
{
	VkLayerDispatchTable *table = get_table(TOKEY(device));
	VkResult res = table->GetSwapchainImagesKHR(device, sc, count, images);
	return res;
}

static VkResult VKAPI OBS_CreateWin32SurfaceKHR(
	VkInstance inst, const VkWin32SurfaceCreateInfoKHR *info,
	const VkAllocationCallbacks *ac, VkSurfaceKHR *surf)
{
	struct vk_inst_data *inst_data = get_inst_data(TOKEY(inst));
	DbgOut("# OBS_Layer # CreateWin32SurfaceKHR\n");

	VkResult res =
		inst_data->table.CreateWin32SurfaceKHR(inst, info, ac, surf);
	if (NULL != surf && VK_NULL_HANDLE != *surf) {
		struct vk_surf_data *surf_data =
			find_surf_data(inst_data, *surf);

		surf_data->hinstance = info->hinstance;
		surf_data->hwnd = info->hwnd;
	}
	return res;
}

#define GETPROCADDR(func)              \
	if (!strcmp(name, "vk" #func)) \
		return (VkFunc)&OBS_##func;

EXPORT VkFunc VKAPI OBS_GetDeviceProcAddr(VkDevice dev, const char *name)
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

	VkLayerDispatchTable *table = get_table(TOKEY(dev));
	if (table->GetDeviceProcAddr == NULL)
		return NULL;
	return table->GetDeviceProcAddr(dev, name);
}

EXPORT VkFunc VKAPI OBS_GetInstanceProcAddr(VkInstance inst, const char *name)
{
	DbgOutProcAddr("# OBS_Layer # vkGetInstanceProcAddr [%s] "
		       "called on instance %p\n",
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

	VkLayerInstanceDispatchTable *table = get_inst_table(TOKEY(inst));
	if (table->GetInstanceProcAddr == NULL)
		return NULL;
	return table->GetInstanceProcAddr(inst, name);
}

#undef GETPROCADDR

static VkFunc VKAPI vk_get_phys_proc(VkInstance inst, const char *name)
{
	VkLayerInstanceDispatchTable *table = get_inst_table(TOKEY(inst));
	if (table->GetPhysicalDeviceProcAddr == NULL) {
		return NULL;
	}

	return table->GetPhysicalDeviceProcAddr(inst, name);
}

EXPORT VkResult VKAPI OBS_Negotiate(VkNegotiateLayerInterface *nli)
{
	if (nli->loaderLayerInterfaceVersion >= 2) {
		nli->sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
		nli->pNext = NULL;
		nli->pfnGetInstanceProcAddr = OBS_GetInstanceProcAddr;
		nli->pfnGetDeviceProcAddr = OBS_GetDeviceProcAddr;
		nli->pfnGetPhysicalDeviceProcAddr = vk_get_phys_proc;
	}

	const uint32_t cur_ver = CURRENT_LOADER_LAYER_INTERFACE_VERSION;

	if (nli->loaderLayerInterfaceVersion > cur_ver) {
		nli->loaderLayerInterfaceVersion = cur_ver;
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

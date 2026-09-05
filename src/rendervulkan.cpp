// Initialize Vulkan and composite stuff with a compute queue

#include <cassert>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <algorithm>
#include <bit>
#include <array>
#include <bitset>
#include <dlfcn.h>
#include "vulkan_include.h"
#include "Utils/Algorithm.h"

#if defined(__linux__)
#include <sys/sysmacros.h>
#endif

// Used to remove the config struct alignment specified by the NIS header
#define NIS_ALIGNED(x)
// NIS_Config needs to be included before the X11 headers because of conflicting defines introduced by X11
#include "shaders/NVIDIAImageScaling/NIS/NIS_Config.h"

#include <drm_fourcc.h>
#include "hdmi.h"
#if HAVE_DRM
#include "drm_include.h"
#endif
#include "wlr_begin.hpp"
#include <wlr/render/drm_format_set.h>
#include "wlr_end.hpp"

#include "rendervulkan.hpp"
#include "main.hpp"
#include "steamcompmgr.hpp"
#include "log.hpp"
#include "Utils/Process.h"
#include "SettingsOverlay.h"
#include "Overlay/FpsDisplay.h"
#include "Overlay/Notifications.h"

#include "cs_composite_blit.h"
#include "cs_composite_blur.h"
#include "cs_composite_blur_cond.h"
#include "cs_composite_rcas.h"
#include "cs_easu.h"
#include "cs_easu_fp16.h"
#include "cs_effects_layer0.h"
#include "cs_effects_measure.h"
#include "cs_gaussian_blur_horizontal.h"
#include "cs_nis.h"
#include "cs_nis_fp16.h"
#include "cs_rgb_to_nv12.h"

#define A_CPU
#include "shaders/ffx_a.h"
#include "shaders/ffx_fsr1.h"

#include "reshade_effect_manager.hpp"

extern bool g_bWasPartialComposite;
extern bool g_bAllowDeferredBackend;

static constexpr mat3x4 g_rgb2yuv_srgb_to_bt601_limited = {{
  { 0.257f, 0.504f, 0.098f, 0.0625f },
  { -0.148f, -0.291f, 0.439f, 0.5f },
  { 0.439f, -0.368f, -0.071f, 0.5f },
}};

static constexpr mat3x4 g_rgb2yuv_srgb_to_bt601 = {{
  { 0.299f, 0.587f, 0.114f, 0.0f },
  { -0.169f, -0.331f, 0.500f, 0.5f },
  { 0.500f, -0.419f, -0.081f, 0.5f },
}};

static constexpr mat3x4 g_rgb2yuv_srgb_to_bt709_limited = {{
  { 0.1826f, 0.6142f, 0.0620f, 0.0625f },
  { -0.1006f, -0.3386f, 0.4392f, 0.5f },
  { 0.4392f, -0.3989f, -0.0403f, 0.5f },
}};

static constexpr mat3x4 g_rgb2yuv_srgb_to_bt709_full = {{
  { 0.2126f, 0.7152f, 0.0722f, 0.0f },
  { -0.1146f, -0.3854f, 0.5000f, 0.5f },
  { 0.5000f, -0.4542f, -0.0458f, 0.5f },
}};

static const mat3x4& colorspace_to_conversion_from_srgb_matrix(EStreamColorspace colorspace) {
	switch (colorspace) {
		default:
		case k_EStreamColorspace_BT601:			return g_rgb2yuv_srgb_to_bt601_limited;
		case k_EStreamColorspace_BT601_Full:	return g_rgb2yuv_srgb_to_bt601;
		case k_EStreamColorspace_BT709:			return g_rgb2yuv_srgb_to_bt709_limited;
		case k_EStreamColorspace_BT709_Full:	return g_rgb2yuv_srgb_to_bt709_full;
	}
}

PFN_vkGetInstanceProcAddr g_pfn_vkGetInstanceProcAddr;
PFN_vkCreateInstance g_pfn_vkCreateInstance;

static VkResult vulkan_load_module()
{
	static VkResult s_result = []()
	{
		void* pModule = dlopen( "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL );
		if ( !pModule )
			pModule = dlopen( "libvulkan.so", RTLD_NOW | RTLD_LOCAL );
		if ( !pModule )
			return VK_ERROR_INITIALIZATION_FAILED;

		g_pfn_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym( pModule, "vkGetInstanceProcAddr" );
		if ( !g_pfn_vkGetInstanceProcAddr )
			return VK_ERROR_INITIALIZATION_FAILED;

		g_pfn_vkCreateInstance = (PFN_vkCreateInstance) g_pfn_vkGetInstanceProcAddr( nullptr, "vkCreateInstance" );
		if ( !g_pfn_vkCreateInstance )
			return VK_ERROR_INITIALIZATION_FAILED;

		return VK_SUCCESS;
	}();

	return s_result;
}

VulkanOutput_t g_output;

uint32_t g_uCompositeDebug = 0u;
gamescope::ConVar<uint32_t> cv_composite_debug{ "composite_debug", 0, "Debug composition flags" };

static std::map< VkFormat, std::map< uint64_t, VkDrmFormatModifierPropertiesEXT > > DRMModifierProps = {};
static std::unordered_map<uint32_t, std::vector<uint64_t>> s_SampledModifierFormats = {};
static struct wlr_drm_format_set sampledShmFormats = {};
static struct wlr_drm_format_set sampledDRMFormats = {};

std::span<const uint64_t> GetSupportedSampleModifiers( uint32_t uDrmFormat )
{
	auto iter = s_SampledModifierFormats.find( uDrmFormat );
	if ( iter == s_SampledModifierFormats.end() )
		return std::span<const uint64_t>{};

	return std::span<const uint64_t>{ iter->second.begin(), iter->second.end() };
}

static LogScope vk_log("vulkan");

static void vk_errorf(VkResult result, const char *fmt, ...) {
	static char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	vk_log.errorf("%s (VkResult: %d)", buf, result);
}

// For when device is up and it would be totally fatal to fail
#define vk_check( x ) \
	do \
	{ \
		VkResult check_res = VK_SUCCESS; \
		if ( ( check_res = ( x ) ) != VK_SUCCESS ) \
		{ \
			vk_errorf( check_res, #x " failed!" ); \
			abort(); \
		} \
	} while ( 0 )

template<typename Target, typename Base>
Target *pNextFind(const Base *base, VkStructureType sType)
{
	for ( ; base; base = (const Base *)base->pNext )
	{
		if (base->sType == sType)
			return (Target *) base;
	}
	return nullptr;
}

#define VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA (VkStructureType)1000001002
#define VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA (VkStructureType)1000001003

struct wsi_image_create_info {
	VkStructureType sType;
	const void *pNext;
	bool scanout;

	uint32_t modifier_count;
	const uint64_t *modifiers;
};

struct wsi_memory_allocate_info {
    VkStructureType sType;
    const void *pNext;
    bool implicit_sync;
};

// DRM doesn't always have 32bit floating point formats, so add our own if necessary

#ifndef DRM_FORMAT_ABGR32323232F
#define DRM_FORMAT_ABGR32323232F fourcc_code('A', 'B', '8', 'F')
#endif

#ifndef DRM_FORMAT_R16F
#define DRM_FORMAT_R16F fourcc_code('R', '1', '6', 'F')
#endif

#ifndef DRM_FORMAT_R32F
#define DRM_FORMAT_R32F fourcc_code('R', '3', '2', 'F')
#endif

struct {
	uint32_t DRMFormat;
	VkFormat vkFormat;
	VkFormat vkFormatSrgb;
	uint32_t bpp;
	bool bHasAlpha;
	bool internal;
} s_DRMVKFormatTable[] = {
	{ DRM_FORMAT_ARGB8888, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, 4, true, false },
	{ DRM_FORMAT_XRGB8888, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, 4, false, false },
	{ DRM_FORMAT_ABGR8888, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, 4, true, false },
	{ DRM_FORMAT_XBGR8888, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, 4, false, false },
	{ DRM_FORMAT_RGB565, VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_R5G6B5_UNORM_PACK16, 1, false, false },
	{ DRM_FORMAT_NV12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, 0, false, false },
	{ DRM_FORMAT_ABGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8, true, false },
	{ DRM_FORMAT_XBGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8, false, false },
	{ DRM_FORMAT_ABGR16161616, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, 8, true, false },
	{ DRM_FORMAT_XBGR16161616, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, 8, false, false },
	{ DRM_FORMAT_ABGR2101010, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, true, false },
	{ DRM_FORMAT_XBGR2101010, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, false, false },
	{ DRM_FORMAT_ARGB2101010, VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32, 4, true, false },
	{ DRM_FORMAT_XRGB2101010, VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32, 4, false, false },

	{ DRM_FORMAT_R8, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, 1, false, true },
	{ DRM_FORMAT_R16, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, 2, false, true },
	{ DRM_FORMAT_GR88, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, 2, false, true },
	{ DRM_FORMAT_GR1616, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_UNORM, 4, false, true },
	{ DRM_FORMAT_ABGR32323232F, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, 16,true, true },
	{ DRM_FORMAT_R16F, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_SFLOAT, 2, false, true },
	{ DRM_FORMAT_R32F, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SFLOAT, 4, false, true },
	{ DRM_FORMAT_INVALID, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, false, true },
};

uint32_t VulkanFormatToDRM( VkFormat vkFormat, std::optional<bool> obHasAlphaOverride )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( ( s_DRMVKFormatTable[i].vkFormat == vkFormat || s_DRMVKFormatTable[i].vkFormatSrgb == vkFormat ) && ( !obHasAlphaOverride || s_DRMVKFormatTable[i].bHasAlpha == *obHasAlphaOverride ) )
		{
			return s_DRMVKFormatTable[i].DRMFormat;
		}
	}
	
	return DRM_FORMAT_INVALID;
}

VkFormat DRMFormatToVulkan( uint32_t nDRMFormat, bool bSrgb )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].DRMFormat == nDRMFormat )
		{
			return bSrgb ? s_DRMVKFormatTable[i].vkFormatSrgb : s_DRMVKFormatTable[i].vkFormat;
		}
	}
	
	return VK_FORMAT_UNDEFINED;
}

bool DRMFormatHasAlpha( uint32_t nDRMFormat )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].DRMFormat == nDRMFormat )
		{
			return s_DRMVKFormatTable[i].bHasAlpha;
		}
	}
	
	return false;
}

uint32_t DRMFormatGetBPP( uint32_t nDRMFormat )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].DRMFormat == nDRMFormat )
		{
			return s_DRMVKFormatTable[i].bpp;
		}
	}

	return false;
}

bool CVulkanDevice::BInit(VkInstance instance, VkSurfaceKHR surface)
{
	assert(instance);
	assert(!m_bInitialized);

	g_output.surface = surface;

	m_instance = instance;
	#define VK_FUNC(x) vk.x = (PFN_vk##x) g_pfn_vkGetInstanceProcAddr(instance, "vk"#x);
	VULKAN_INSTANCE_FUNCTIONS
	#undef VK_FUNC

	if (!selectPhysDev(surface))
		return false;
	if (!createDevice())
		return false;
	if (!createLayouts())
		return false;
	if (!createPools())
		return false;
	if (!createShaders())
		return false;
	if (!createScratchResources())
		return false;

	m_bInitialized = true;

	m_pipelineThread = std::jthread([this](std::stop_token st){compileAllPipelines(st);});

	g_reshadeManager.init(this);

	return true;
}

extern bool env_to_bool(const char *env);

bool CVulkanDevice::selectPhysDev(VkSurfaceKHR surface)
{
	uint32_t deviceCount = 0;
	vk.EnumeratePhysicalDevices(instance(), &deviceCount, nullptr);
	std::vector<VkPhysicalDevice> physDevs(deviceCount);
	vk.EnumeratePhysicalDevices(instance(), &deviceCount, physDevs.data());
	if (deviceCount < physDevs.size())
		physDevs.resize(deviceCount);

	bool bTryComputeOnly = true;

	// In theory vkBasalt might want to filter out compute-only queue families to force our hand here
	const char *pchEnableVkBasalt = getenv( "ENABLE_VKBASALT" );
	if ( pchEnableVkBasalt != nullptr && pchEnableVkBasalt[0] == '1' )
	{
		bTryComputeOnly = false;
	}

	for (auto cphysDev : physDevs)
	{
		VkPhysicalDeviceProperties deviceProperties;
		vk.GetPhysicalDeviceProperties(cphysDev, &deviceProperties);

		if (deviceProperties.apiVersion < VK_API_VERSION_1_2)
			continue;

		uint32_t queueFamilyCount = 0;
		vk.GetPhysicalDeviceQueueFamilyProperties(cphysDev, &queueFamilyCount, nullptr);
		std::vector<VkQueueFamilyProperties> queueFamilyProperties(queueFamilyCount);
		vk.GetPhysicalDeviceQueueFamilyProperties(cphysDev, &queueFamilyCount, queueFamilyProperties.data());

		uint32_t generalIndex = ~0u;
		uint32_t computeOnlyIndex = ~0u;
		for (uint32_t i = 0; i < queueFamilyCount; ++i) {
			const VkQueueFlags generalBits = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT;
			if ((queueFamilyProperties[i].queueFlags & generalBits) == generalBits )
				generalIndex = std::min(generalIndex, i);
			else if (bTryComputeOnly && queueFamilyProperties[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
				computeOnlyIndex = std::min(computeOnlyIndex, i);
		}

		if (generalIndex != ~0u || computeOnlyIndex != ~0u)
		{
			// Select the device if it's the first one or the preferred one
			if (!m_physDev ||
			    (g_preferVendorID == deviceProperties.vendorID && g_preferDeviceID == deviceProperties.deviceID))
			{
				// if we have a surface, check that the queue family can actually present on it
				if (surface) {
					VkBool32 canPresent = false;
					vk.GetPhysicalDeviceSurfaceSupportKHR( cphysDev, generalIndex, surface, &canPresent );
					if ( !canPresent )
					{
						vk_log.infof( "physical device %04x:%04x queue doesn't support presenting on our surface, testing next one..", deviceProperties.vendorID, deviceProperties.deviceID );
						continue;
					}
					if (computeOnlyIndex != ~0u)
					{
						vk.GetPhysicalDeviceSurfaceSupportKHR( cphysDev, computeOnlyIndex, surface, &canPresent );
						if ( !canPresent )
						{
							vk_log.infof( "physical device %04x:%04x compute queue doesn't support presenting on our surface, using graphics queue", deviceProperties.vendorID, deviceProperties.deviceID );
							computeOnlyIndex = ~0u;
						}
					}
				}

				m_queueFamily = computeOnlyIndex == ~0u ? generalIndex : computeOnlyIndex;
				m_generalQueueFamily = generalIndex;
				m_physDev = cphysDev;

				/* When Intel uses compute-only queue for Gamescope composition, some games
				 * experience performance loss. Using the general queue alleviates the issue
				 * for now.
				 * See: https://gitlab.freedesktop.org/drm/xe/kernel/-/issues/4452
				 *
				 * TODO: Remove vendorID check for Intel once issue is resolved.
				 */
				if (deviceProperties.vendorID == 0x8086) /* Intel */
				{
					vk_log.infof("Intel device detected, forcing general queue family instead of compute-only queue");
					m_queueFamily = generalIndex;
				}
				else if ( env_to_bool( getenv( "GAMESCOPE_FORCE_GENERAL_QUEUE" ) ) )
					m_queueFamily = generalIndex;
			}
		}
	}

	if (!m_physDev)
	{
		vk_log.errorf("failed to find physical device");
		return false;
	}

	VkPhysicalDeviceProperties props;
	vk.GetPhysicalDeviceProperties( m_physDev, &props );
	vk_log.infof( "selecting physical device '%s': queue family %x (general queue family %x)", props.deviceName, m_queueFamily, m_generalQueueFamily );

	return true;
}

bool CVulkanDevice::createDevice()
{
	uint32_t supportedExtensionCount;
	vk.EnumerateDeviceExtensionProperties( physDev(), NULL, &supportedExtensionCount, NULL );

	m_supportedExts.resize(supportedExtensionCount);
	vk.EnumerateDeviceExtensionProperties( physDev(), NULL, &supportedExtensionCount, m_supportedExts.data() );

	if ( !GetBackend()->ValidPhysicalDevice( physDev() ) ) {
		vk_log.errorf( "not a valid physical device" );
		return false;
	}

	vk.GetPhysicalDeviceMemoryProperties( physDev(), &m_memoryProperties );

	bool hasDrmProps = vulkan_has_drm_props();
	bool supportsForeignQueue = false;
	bool supportsHDRMetadata = false;
	for (const auto& ext : m_supportedExts) {
		if ( strcmp(ext.extensionName, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) == 0 )
			m_bSupportsModifiers = true;

		if ( strcmp(ext.extensionName, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) == 0 )
			supportsForeignQueue = true;

		if ( strcmp(ext.extensionName, VK_EXT_HDR_METADATA_EXTENSION_NAME) == 0 )
			supportsHDRMetadata = true;
	}

	vk_log.infof( "physical device %s DRM format modifiers", m_bSupportsModifiers ? "supports" : "does not support" );

	if ( !hasDrmProps ) {
		// This could happen when e.g. running the lavapipe driver
		// (without an actual physical device)
		vk_log.warnf( "physical device doesn't support VK_EXT_physical_device_drm" );
	} else {
#if HAVE_DRM
		VkPhysicalDeviceDrmPropertiesEXT drmProps = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
		};
		VkPhysicalDeviceProperties2 props2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &drmProps,
		};
		vk.GetPhysicalDeviceProperties2( physDev(), &props2 );

		if ( !GetBackend()->UsesVulkanSwapchain() && !drmProps.hasPrimary ) {
			vk_log.errorf( "physical device has no primary node" );
			return false;
		}
		if ( !drmProps.hasRender ) {
			vk_log.errorf( "physical device has no render node" );
			return false;
		}

		dev_t renderDevId = makedev( drmProps.renderMajor, drmProps.renderMinor );
		drmDevice *drmDev = nullptr;
		if (drmGetDeviceFromDevId(renderDevId, 0, &drmDev) != 0) {
			vk_log.errorf( "drmGetDeviceFromDevId() failed" );
			return false;
		}
		assert(drmDev->available_nodes & (1 << DRM_NODE_RENDER));
		const char *drmRenderName = drmDev->nodes[DRM_NODE_RENDER];

		m_drmRendererFd = open( drmRenderName, O_RDWR | O_CLOEXEC );
		drmFreeDevice(&drmDev);
		if ( m_drmRendererFd < 0 ) {
			vk_log.errorf_errno( "failed to open DRM render node" );
			return false;
		}

		if ( drmProps.hasPrimary ) {
			m_bHasDrmPrimaryDevId = true;
			m_drmPrimaryDevId = makedev( drmProps.primaryMajor, drmProps.primaryMinor );
		}
#else
		vk_log.warnf( "built without DRM support" );
#endif
	}

	if ( m_bSupportsModifiers && !supportsForeignQueue ) {
		vk_log.infof( "The vulkan driver does not support foreign queues,"
		              " disabling modifier support.");
		m_bSupportsModifiers = false;
	}

	{
		VkPhysicalDeviceVulkan12Features vulkan12Features = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		};
		VkPhysicalDeviceFeatures2 features2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
			.pNext = &vulkan12Features,
		};
		vk.GetPhysicalDeviceFeatures2( physDev(), &features2 );

		m_bSupportsFp16 = vulkan12Features.shaderFloat16 && features2.features.shaderInt16;
	}

	float queuePriorities = 1.0f;

	VkDeviceQueueGlobalPriorityCreateInfoEXT queueCreateInfoEXT = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT,
		.pNext = nullptr,
		.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_REALTIME_EXT
	};

	VkDeviceQueueCreateInfo queueCreateInfos[2] = 
	{
		{
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.pNext = gamescope::Process::HasCapSysNice() ? &queueCreateInfoEXT : nullptr,
			.queueFamilyIndex = m_queueFamily,
			.queueCount = 1,
			.pQueuePriorities = &queuePriorities
		},
		{
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.pNext = gamescope::Process::HasCapSysNice() ? &queueCreateInfoEXT : nullptr,
			.queueFamilyIndex = m_generalQueueFamily,
			.queueCount = 1,
			.pQueuePriorities = &queuePriorities
		},
	};

	std::vector< const char * > enabledExtensions;

	if ( GetBackend()->UsesVulkanSwapchain() )
	{
		enabledExtensions.push_back( VK_KHR_SWAPCHAIN_EXTENSION_NAME );
		enabledExtensions.push_back( VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME );

		enabledExtensions.push_back( VK_KHR_PRESENT_ID_EXTENSION_NAME );
		enabledExtensions.push_back( VK_KHR_PRESENT_WAIT_EXTENSION_NAME );

		if ( supportsHDRMetadata )
			enabledExtensions.push_back( VK_EXT_HDR_METADATA_EXTENSION_NAME );
	}

	if ( m_bSupportsModifiers )
	{
		enabledExtensions.push_back( VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME );
		enabledExtensions.push_back( VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME );
	}

	enabledExtensions.push_back( VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME );
	enabledExtensions.push_back( VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME );

	enabledExtensions.push_back( VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME );

	enabledExtensions.push_back( VK_EXT_ROBUSTNESS_2_EXTENSION_NAME );

	// We chain VkPhysicalDeviceVulkan13Features::dynamicRendering = VK_TRUE below,
	// but never actually enabled the extension string -- ImGui's Vulkan backend
	// (imgui_impl_vulkan.h) documents needing VK_KHR_dynamic_rendering enabled
	// explicitly "even for Vulkan 1.3", and conformant 1.3 implementations still
	// enumerate promoted-to-core extensions, so this is safe to always request.
	//
	// A/B flicker test kit (superdoc/planning/flicker-ab-test-plan.md): this is
	// one of the few device-creation changes that touches every frame, not just
	// the overlay's, so GAMESCOPE_RITZ_AB_NO_DYNAMIC_RENDERING_EXT=1 lets a
	// variant run skip requesting the string while leaving the 1.3 feature bit
	// (already core, above) untouched -- isolating "did enabling this string
	// specifically change anything" from the overlay entirely.
	const char *pszNoDynamicRenderingExt = getenv( "GAMESCOPE_RITZ_AB_NO_DYNAMIC_RENDERING_EXT" );
	if ( !pszNoDynamicRenderingExt || pszNoDynamicRenderingExt[0] != '1' )
		enabledExtensions.push_back( VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME );
#if 0
	enabledExtensions.push_back( VK_KHR_MAINTENANCE_5_EXTENSION_NAME );
#endif

	for ( auto& extension : GetBackend()->GetDeviceExtensions( physDev() ) )
		enabledExtensions.push_back( extension );

	uint32_t devExtPropCount = 0;
	vk.EnumerateDeviceExtensionProperties( physDev(), nullptr, &devExtPropCount, nullptr );
	std::vector<VkExtensionProperties> devExtProp( devExtPropCount );
	vk.EnumerateDeviceExtensionProperties( physDev(), nullptr, &devExtPropCount, devExtProp.data() );
	bool anyMissing = false;
	for ( auto& requiredExt : enabledExtensions ) {
		bool extFound = false;
		for ( auto & availableExt : devExtProp ) {
			if ( strcmp( requiredExt, availableExt.extensionName ) == 0 ) {
				extFound = true;
				break;
			}
		}
		if ( !extFound ) {
			vk_log.errorf( "Missing required extension: %s", requiredExt );
			anyMissing = true;
		}
	}
	if ( anyMissing )
		return false;

#if 0
	VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
		.maintenance5 = VK_TRUE,
	};
#endif

	VkPhysicalDeviceVulkan13Features features13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
#if 0
		.pNext = &maintenance5,
#endif
		.dynamicRendering = VK_TRUE,
	};

	VkPhysicalDevicePresentWaitFeaturesKHR presentWaitFeatures = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
		.pNext = &features13,
		.presentWait = VK_TRUE,
	};

	VkPhysicalDevicePresentIdFeaturesKHR presentIdFeatures = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
		.pNext = &presentWaitFeatures,
		.presentId = VK_TRUE,
	};

	VkPhysicalDeviceFeatures2 features2 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &presentIdFeatures,
		.features = {
			.shaderInt16 = m_bSupportsFp16,
		},
	};

	VkDeviceCreateInfo deviceCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &features2,
		.queueCreateInfoCount = m_queueFamily == m_generalQueueFamily ? 1u : 2u,
		.pQueueCreateInfos = queueCreateInfos,
		.enabledExtensionCount = (uint32_t)enabledExtensions.size(),
		.ppEnabledExtensionNames = enabledExtensions.data(),
	};

	VkPhysicalDeviceVulkan12Features vulkan12Features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = std::exchange(features2.pNext, &vulkan12Features),
		.shaderFloat16 = m_bSupportsFp16,
		.scalarBlockLayout = VK_TRUE,
		.timelineSemaphore = VK_TRUE,
	};

	VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeatures = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
		.pNext = std::exchange(features2.pNext, &ycbcrFeatures),
		.samplerYcbcrConversion = VK_TRUE,
	};

	VkPhysicalDeviceRobustness2FeaturesEXT robustness2Features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
		.pNext = std::exchange(features2.pNext, &robustness2Features),
		.nullDescriptor = VK_TRUE,
	};

	VkResult res = vk.CreateDevice(physDev(), &deviceCreateInfo, nullptr, &m_device);
	if ( res == VK_ERROR_NOT_PERMITTED_KHR && gamescope::Process::HasCapSysNice() )
	{
		fprintf(stderr, "vkCreateDevice failed with a high-priority queue (general + compute). Falling back to regular priority (general).\n");
		queueCreateInfos[1].pNext = nullptr;
		res = vk.CreateDevice(physDev(), &deviceCreateInfo, nullptr, &m_device);


		if ( res == VK_ERROR_NOT_PERMITTED_KHR && gamescope::Process::HasCapSysNice() )
		{
			fprintf(stderr, "vkCreateDevice failed with a high-priority queue (compute). Falling back to regular priority (all).\n");
			queueCreateInfos[0].pNext = nullptr;
			res = vk.CreateDevice(physDev(), &deviceCreateInfo, nullptr, &m_device);
		}
	}

	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateDevice failed" );
		return false;
	}

	#define VK_FUNC(x) vk.x = (PFN_vk##x) vk.GetDeviceProcAddr(device(), "vk"#x);
	VULKAN_DEVICE_FUNCTIONS
	#undef VK_FUNC

	vk.GetDeviceQueue(device(), m_queueFamily, 0, &m_queue);
	if ( m_queueFamily == m_generalQueueFamily )
		m_generalQueue = m_queue;
	else
		vk.GetDeviceQueue(device(), m_generalQueueFamily, 0, &m_generalQueue);

	return true;
}

static VkSamplerYcbcrModelConversion colorspaceToYCBCRModel( EStreamColorspace colorspace )
{
	switch (colorspace)
	{
		default:
		case k_EStreamColorspace_Unknown:
			return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;

		case k_EStreamColorspace_BT601:
		case k_EStreamColorspace_BT601_Full:
			return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;

		case k_EStreamColorspace_BT709:
		case k_EStreamColorspace_BT709_Full:
			return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
	}
}

static VkSamplerYcbcrRange colorspaceToYCBCRRange( EStreamColorspace colorspace )
{
	switch (colorspace)
	{
		default:
		case k_EStreamColorspace_Unknown:
			return VK_SAMPLER_YCBCR_RANGE_ITU_FULL;

		case k_EStreamColorspace_BT709:
		case k_EStreamColorspace_BT601:
			return VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;

		case k_EStreamColorspace_BT601_Full:
		case k_EStreamColorspace_BT709_Full:
			return VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
	}
}

bool CVulkanDevice::createLayouts()
{
	VkFormatProperties nv12Properties;
	vk.GetPhysicalDeviceFormatProperties(physDev(), VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &nv12Properties);
	bool cosited = nv12Properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT;

	VkSamplerYcbcrConversionCreateInfo ycbcrSamplerConversionCreateInfo = 
	{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
		.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
		.ycbcrModel = colorspaceToYCBCRModel( g_ForcedNV12ColorSpace ),
		.ycbcrRange = colorspaceToYCBCRRange( g_ForcedNV12ColorSpace ),
		.xChromaOffset = cosited ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT,
		.yChromaOffset = cosited ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT,
		.chromaFilter = VK_FILTER_LINEAR,
		.forceExplicitReconstruction = VK_FALSE,
	};

	vk.CreateSamplerYcbcrConversion( device(), &ycbcrSamplerConversionCreateInfo, nullptr, &m_ycbcrConversion );

	VkSamplerYcbcrConversionInfo ycbcrSamplerConversionInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
		.conversion = m_ycbcrConversion,
	};

	VkSamplerCreateInfo ycbcrSamplerInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = &ycbcrSamplerConversionInfo,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
	};
	
	vk.CreateSampler( device(), &ycbcrSamplerInfo, nullptr, &m_ycbcrSampler );

	// Create an array of our ycbcrSampler to fill up
	std::array<VkSampler, VKR_SAMPLER_SLOTS> ycbcrSamplers;
	for (auto& sampler : ycbcrSamplers)
		sampler = m_ycbcrSampler;

	std::array<VkDescriptorSetLayoutBinding, 7 > layoutBindings = {
		VkDescriptorSetLayoutBinding {
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		},
		VkDescriptorSetLayoutBinding {
			.binding = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		},
		VkDescriptorSetLayoutBinding {
			.binding = 2,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		},
		VkDescriptorSetLayoutBinding {
			.binding = 3,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = VKR_SAMPLER_SLOTS,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		},
		VkDescriptorSetLayoutBinding {
			.binding = 4,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = VKR_SAMPLER_SLOTS,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.pImmutableSamplers = ycbcrSamplers.data(),
		},
		VkDescriptorSetLayoutBinding {
			.binding = 5,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = VKR_LUT3D_COUNT,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		},
		VkDescriptorSetLayoutBinding {
			.binding = 6,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = VKR_LUT3D_COUNT,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		},
	};

	VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = (uint32_t)layoutBindings.size(),
		.pBindings = layoutBindings.data()
	};

	VkResult res = vk.CreateDescriptorSetLayout(device(), &descriptorSetLayoutCreateInfo, 0, &m_descriptorSetLayout);
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateDescriptorSetLayout failed" );
		return false;
	}

	VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &m_descriptorSetLayout,
	};
	
	res = vk.CreatePipelineLayout(device(), &pipelineLayoutCreateInfo, nullptr, &m_pipelineLayout);
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreatePipelineLayout failed" );
		return false;
	}

	return true;
}

bool CVulkanDevice::createPools()
{
	VkCommandPoolCreateInfo commandPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = m_queueFamily,
	};

	VkResult res = vk.CreateCommandPool(device(), &commandPoolCreateInfo, nullptr, &m_commandPool);
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateCommandPool failed" );
		return false;
	}

	VkCommandPoolCreateInfo generalCommandPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = m_generalQueueFamily,
	};

	res = vk.CreateCommandPool(device(), &generalCommandPoolCreateInfo, nullptr, &m_generalCommandPool);
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateCommandPool failed" );
		return false;
	}

	VkPhysicalDeviceImageFormatInfo2 imageFormatInfo = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
		.type = VK_IMAGE_TYPE_2D,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	};

	VkSamplerYcbcrConversionImageFormatProperties ycbcrProps = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES,
	};

	VkImageFormatProperties2 imageFormatProps = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
		.pNext = &ycbcrProps,
	};

	res = vk.GetPhysicalDeviceImageFormatProperties2( physDev(), &imageFormatInfo, &imageFormatProps );

	VkDescriptorPoolSize poolSizes[3] {
		{
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			uint32_t(m_descriptorSets.size()),
		},
		{
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			uint32_t(m_descriptorSets.size()) * 2,
		},
		{
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			uint32_t(m_descriptorSets.size()) * (((ycbcrProps.combinedImageSamplerDescriptorCount + 1) * VKR_SAMPLER_SLOTS) + (2 * VKR_LUT3D_COUNT)),
		},
	};
	
	VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = uint32_t(m_descriptorSets.size()),
		.poolSizeCount = sizeof(poolSizes) / sizeof(poolSizes[0]),
		.pPoolSizes = poolSizes,
	};
	
	res = vk.CreateDescriptorPool(device(), &descriptorPoolCreateInfo, nullptr, &m_descriptorPool);
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateDescriptorPool failed" );
		return false;
	}

	return true;
}

bool CVulkanDevice::createShaders()
{
	struct ShaderInfo_t
	{
		const uint32_t* spirv;
		uint32_t size;
	};

	std::array<ShaderInfo_t, SHADER_TYPE_COUNT> shaderInfos;
#define SHADER(type, array) shaderInfos[SHADER_TYPE_##type] = {array , sizeof(array)}
	SHADER(BLIT, cs_composite_blit);
	SHADER(BLUR, cs_composite_blur);
	SHADER(BLUR_COND, cs_composite_blur_cond);
	SHADER(BLUR_FIRST_PASS, cs_gaussian_blur_horizontal);
	SHADER(RCAS, cs_composite_rcas);
	if (m_bSupportsFp16)
	{
		SHADER(EASU, cs_easu_fp16);
		SHADER(NIS, cs_nis_fp16);
	}
	else
	{
		SHADER(EASU, cs_easu);
		SHADER(NIS, cs_nis);
	}
	SHADER(RGB_TO_NV12, cs_rgb_to_nv12);
	SHADER(EFFECTS_LAYER0, cs_effects_layer0);
	SHADER(EFFECTS_MEASURE, cs_effects_measure);
#undef SHADER

	for (uint32_t i = 0; i < shaderInfos.size(); i++)
	{
		VkShaderModuleCreateInfo shaderCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = shaderInfos[i].size,
			.pCode = shaderInfos[i].spirv,
		};

		VkResult res = vk.CreateShaderModule(device(), &shaderCreateInfo, nullptr, &m_shaderModules[i]);
		if ( res != VK_SUCCESS )
		{
			vk_errorf( res, "vkCreateShaderModule failed" );
			return false;
		}
	}

	return true;
}

bool CVulkanDevice::createScratchResources()
{
	std::vector<VkDescriptorSetLayout> descriptorSetLayouts(m_descriptorSets.size(), m_descriptorSetLayout);
	
	VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = m_descriptorPool,
		.descriptorSetCount = (uint32_t)descriptorSetLayouts.size(),
		.pSetLayouts = descriptorSetLayouts.data(),
	};
	
	VkResult res = vk.AllocateDescriptorSets(device(), &descriptorSetAllocateInfo, m_descriptorSets.data());
	if ( res != VK_SUCCESS )
	{
		vk_log.errorf( "vkAllocateDescriptorSets failed" );
		return false;
	}

	// Make and map upload buffer
	
	VkBufferCreateInfo bufferCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = upload_buffer_size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
	};

	res = vk.CreateBuffer( device(), &bufferCreateInfo, nullptr, &m_uploadBuffer );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateBuffer failed" );
		return false;
	}
	
	VkMemoryRequirements memRequirements;
	vk.GetBufferMemoryRequirements(device(), m_uploadBuffer, &memRequirements);
	
	uint32_t memTypeIndex =  findMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, memRequirements.memoryTypeBits );
	if ( memTypeIndex == ~0u )
	{
		vk_log.errorf( "findMemoryType failed" );
		return false;
	}
	
	VkMemoryAllocateInfo allocInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = memRequirements.size,
		.memoryTypeIndex = memTypeIndex,
	};
	
	vk.AllocateMemory( device(), &allocInfo, nullptr, &m_uploadBufferMemory);
	
	vk.BindBufferMemory( device(), m_uploadBuffer, m_uploadBufferMemory, 0 );

	res = vk.MapMemory( device(), m_uploadBufferMemory, 0, VK_WHOLE_SIZE, 0, (void**)&m_uploadBufferData );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkMapMemory failed" );
		return false;
	}

	VkSemaphoreTypeCreateInfo timelineCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	};

	VkSemaphoreCreateInfo semCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &timelineCreateInfo,
	};

	res = vk.CreateSemaphore( device(), &semCreateInfo, NULL, &m_scratchTimelineSemaphore );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateSemaphore failed" );
		return false;
	}

	return true;
}

VkSampler CVulkanDevice::sampler( SamplerState key )
{
	if ( m_samplerCache.count(key) != 0 )
		return m_samplerCache[key];

	VkSampler ret = VK_NULL_HANDLE;

	VkSamplerCreateInfo samplerCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = key.bNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
		.minFilter = key.bNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
		.unnormalizedCoordinates = key.bUnnormalized,
	};

	vk.CreateSampler( device(), &samplerCreateInfo, nullptr, &ret );

	m_samplerCache[key] = ret;

	return ret;
}

VkPipeline CVulkanDevice::compilePipeline(uint32_t layerCount, uint32_t ycbcrMask, ShaderType type, uint32_t blur_layer_count, uint32_t composite_debug, uint32_t colorspace_mask, uint32_t output_eotf, bool itm_enable)
{
	const std::array<VkSpecializationMapEntry, 7> specializationEntries = {{
		{
			.constantID = 0,
			.offset     = sizeof(uint32_t) * 0,
			.size       = sizeof(uint32_t)
		},
		{
			.constantID = 1,
			.offset     = sizeof(uint32_t) * 1,
			.size       = sizeof(uint32_t)
		},
		{
			.constantID = 2,
			.offset     = sizeof(uint32_t) * 2,
			.size       = sizeof(uint32_t)
		},
		{
			.constantID = 3,
			.offset     = sizeof(uint32_t) * 3,
			.size       = sizeof(uint32_t)
		},
		{
			.constantID = 4,
			.offset     = sizeof(uint32_t) * 4,
			.size       = sizeof(uint32_t)
		},

		{
			.constantID = 5,
			.offset     = sizeof(uint32_t) * 5,
			.size       = sizeof(uint32_t)
		},

		{
			.constantID = 6,
			.offset     = sizeof(uint32_t) * 6,
			.size       = sizeof(uint32_t)
		},
	}};

	struct {
		uint32_t layerCount;
		uint32_t ycbcrMask;
		uint32_t debug;
		uint32_t blur_layer_count;
		uint32_t colorspace_mask;
		uint32_t output_eotf;
		uint32_t itm_enable;
	} specializationData = {
		.layerCount   = layerCount,
		.ycbcrMask    = ycbcrMask,
		.debug        = composite_debug,
		.blur_layer_count = blur_layer_count,
		.colorspace_mask = colorspace_mask,
		.output_eotf = output_eotf,
		.itm_enable = itm_enable,
	};

	VkSpecializationInfo specializationInfo = {
		.mapEntryCount = uint32_t(specializationEntries.size()),
		.pMapEntries   = specializationEntries.data(),
		.dataSize      = sizeof(specializationData),
		.pData		   = &specializationData,
	};

	VkComputePipelineCreateInfo computePipelineCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = m_shaderModules[type],
			.pName = "main",
			.pSpecializationInfo = &specializationInfo
		},
		.layout = m_pipelineLayout,
	};

	VkPipeline result;

	VkResult res = vk.CreateComputePipelines(device(), VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &result);
	if (res != VK_SUCCESS) {
		vk_errorf( res, "vkCreateComputePipelines failed" );
		return VK_NULL_HANDLE;
	}

	return result;
}

void CVulkanDevice::compileAllPipelines(std::stop_token st)
{
	pthread_setname_np( pthread_self(), "gamescope-shdr" );

	std::array<PipelineInfo_t, SHADER_TYPE_COUNT> pipelineInfos;
#define SHADER(type, layer_count, max_ycbcr, blur_layers) pipelineInfos[SHADER_TYPE_##type] = {SHADER_TYPE_##type, layer_count, max_ycbcr, blur_layers}
	SHADER(BLIT, k_nMaxLayers, k_nMaxYcbcrMask_ToPreCompile, 1);
	SHADER(BLUR, k_nMaxLayers, k_nMaxYcbcrMask_ToPreCompile, k_nMaxBlurLayers);
	SHADER(BLUR_COND, k_nMaxLayers, k_nMaxYcbcrMask_ToPreCompile, k_nMaxBlurLayers);
	SHADER(BLUR_FIRST_PASS, 1, 2, 1);
	SHADER(RCAS, k_nMaxLayers, k_nMaxYcbcrMask_ToPreCompile, 1);
	SHADER(EASU, 1, 1, 1);
	SHADER(NIS, 1, 1, 1);
	SHADER(RGB_TO_NV12, 1, 1, 1);
	SHADER(EFFECTS_LAYER0, 1, 1, 1);
	SHADER(EFFECTS_MEASURE, 1, 1, 1);
#undef SHADER

	for (auto& info : pipelineInfos) {
		for (uint32_t layerCount = 1; layerCount <= info.layerCount; layerCount++) {
			for (uint32_t ycbcrMask = 0; ycbcrMask < info.ycbcrMask; ycbcrMask++) {
				for (uint32_t blur_layers = 1; blur_layers <= info.blurLayerCount; blur_layers++) {
					if (ycbcrMask >= (1u << (layerCount + 1)))
						continue;
					if (blur_layers > layerCount)
						continue;

					VkPipeline newPipeline = compilePipeline(layerCount, ycbcrMask, info.shaderType, blur_layers, info.compositeDebug, info.colorspaceMask, info.outputEOTF, info.itmEnable);
					{
						std::lock_guard<std::mutex> lock(m_pipelineMutex);
						PipelineInfo_t key = {info.shaderType, layerCount, ycbcrMask, blur_layers, info.compositeDebug};
						if (st.stop_requested()) return;
						auto result = m_pipelineMap.emplace(std::make_pair(key, newPipeline));
						if (!result.second)
							vk.DestroyPipeline(device(), newPipeline, nullptr);
					}
				}
			}
		}
	}
}

extern bool g_bSteamIsActiveWindow;

VkPipeline CVulkanDevice::pipeline(ShaderType type, uint32_t layerCount, uint32_t ycbcrMask, uint32_t blur_layers, uint32_t colorspace_mask, uint32_t output_eotf, bool itm_enable)
{
	uint32_t effective_debug = g_uCompositeDebug;
	if ( g_bSteamIsActiveWindow )
		effective_debug &= ~(CompositeDebugFlag::Heatmap | CompositeDebugFlag::Heatmap_MSWCG | CompositeDebugFlag::Heatmap_Hard);

	std::lock_guard<std::mutex> lock(m_pipelineMutex);
	PipelineInfo_t key = {type, layerCount, ycbcrMask, blur_layers, effective_debug, colorspace_mask, output_eotf, itm_enable};
	auto search = m_pipelineMap.find(key);
	if (search == m_pipelineMap.end())
	{
		VkPipeline result = compilePipeline(layerCount, ycbcrMask, type, blur_layers, effective_debug, colorspace_mask, output_eotf, itm_enable);
		m_pipelineMap[key] = result;
		return result;
	}
	else
	{
		return search->second;
	}
}


int32_t CVulkanDevice::findMemoryType( VkMemoryPropertyFlags properties, uint32_t requiredTypeBits )
{
	for ( uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; i++ )
	{
		if ( ( ( 1 << i ) & requiredTypeBits ) == 0 )
			continue;
		
		if ( ( properties & m_memoryProperties.memoryTypes[ i ].propertyFlags ) != properties )
			continue;
		
		return i;
	}
	
	return -1;
}

std::unique_ptr<CVulkanCmdBuffer> CVulkanDevice::commandBuffer()
{
	std::unique_ptr<CVulkanCmdBuffer> cmdBuffer;
	if (m_unusedCmdBufs.empty())
	{
		VkCommandBuffer rawCmdBuffer;
		VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = m_commandPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1
		};

		VkResult res = vk.AllocateCommandBuffers( device(), &commandBufferAllocateInfo, &rawCmdBuffer );
		if ( res != VK_SUCCESS )
		{
			vk_errorf( res, "vkAllocateCommandBuffers failed" );
			return nullptr;
		}

		cmdBuffer = std::make_unique<CVulkanCmdBuffer>(this, rawCmdBuffer, m_commandPool, queue(), queueFamily());
	}
	else
	{
		cmdBuffer = std::move(m_unusedCmdBufs.back());
		m_unusedCmdBufs.pop_back();
	}

	cmdBuffer->begin();
	return cmdBuffer;
}

std::unique_ptr<CVulkanCmdBuffer> CVulkanDevice::generalCommandBuffer()
{
	VkCommandBuffer rawCmdBuffer;
	VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = m_generalCommandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1
	};

	VkResult res = vk.AllocateCommandBuffers( device(), &commandBufferAllocateInfo, &rawCmdBuffer );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkAllocateCommandBuffers failed" );
		return nullptr;
	}

	// ponytail: unlike commandBuffer() above, this always allocates fresh rather
	// than reusing m_unusedCmdBufs -- that freelist assumes every entry came from
	// the same VkCommandPool/queue family (m_commandPool/queue()), and mixing in
	// general-queue buffers would hand them back out through the compute-only
	// commandBuffer() factory. This path is only exercised once per frame while
	// the settings overlay is visible/fading, so a fresh alloc+free per frame is
	// cheap. If this ever needs to be hot, give it its own dedicated freelist
	// mirroring m_unusedCmdBufs/m_pendingCmdBufs instead.
	auto cmdBuffer = std::make_unique<CVulkanCmdBuffer>(this, rawCmdBuffer, m_generalCommandPool, generalQueue(), generalQueueFamily());
	cmdBuffer->begin();
	return cmdBuffer;
}

uint64_t CVulkanDevice::submitInternal( CVulkanCmdBuffer* cmdBuffer )
{
	cmdBuffer->end();

	// The seq no of the last submission.
	const uint64_t lastSubmissionSeqNo = m_submissionSeqNo++;

	// This is the seq no of the command buffer we are going to submit.
	const uint64_t nextSeqNo = lastSubmissionSeqNo + 1;

	std::vector<VkSemaphore> pSignalSemaphores;
	std::vector<uint64_t> ulSignalPoints;

	std::vector<VkPipelineStageFlags> uWaitStageFlags;
	std::vector<VkSemaphore> pWaitSemaphores;
	std::vector<uint64_t> ulWaitPoints;

	pSignalSemaphores.push_back( m_scratchTimelineSemaphore );
	ulSignalPoints.push_back( nextSeqNo );

	for ( auto &dep : cmdBuffer->GetExternalSignals() )
	{
		pSignalSemaphores.push_back( dep.pTimelineSemaphore->pVkSemaphore );
		ulSignalPoints.push_back( dep.ulPoint );
	}

	// Wait stages must be supported by the queue family this buffer is submitted
	// to (VUID-vkQueueSubmit-pWaitDstStageMask-00066). COLOR_ATTACHMENT_OUTPUT is
	// a graphics-only stage, so naming it unconditionally is invalid usage on the
	// compute-only queue that vulkan_composite() submits to -- which on this GPU
	// (RADV/Navi31: compute family 1, general family 0) is every composite.
	//
	// Upstream carries the unconditional form; it is latent there because upstream
	// rarely attaches external dependencies to a composite submission. Our overlay
	// attaches one EVERY frame (SettingsOverlay_WaitForRender /
	// FpsDisplay_WaitForRender), so the violation fires continuously. Caught by
	// VK_LAYER_KHRONOS_validation, not by inspection.
	const bool bGeneralQueue = cmdBuffer->queueFamily() == generalQueueFamily();
	const VkPipelineStageFlags uWaitStages = bGeneralQueue
		? ( VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT )
		: ( VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT );

	for ( auto &dep : cmdBuffer->GetExternalDependencies() )
	{
		pWaitSemaphores.push_back( dep.pTimelineSemaphore->pVkSemaphore );
		ulWaitPoints.push_back( dep.ulPoint );
		uWaitStageFlags.push_back( uWaitStages );
	}

	VkTimelineSemaphoreSubmitInfo timelineInfo = {
		.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
		// no need to ensure order of cmd buffer submission, we only have one queue
		.waitSemaphoreValueCount = static_cast<uint32_t>( ulWaitPoints.size() ),
		.pWaitSemaphoreValues = ulWaitPoints.data(),
		.signalSemaphoreValueCount = static_cast<uint32_t>( ulSignalPoints.size() ),
		.pSignalSemaphoreValues = ulSignalPoints.data(),
	};

	VkCommandBuffer rawCmdBuffer = cmdBuffer->rawBuffer();

	VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pNext = &timelineInfo,
		.waitSemaphoreCount = static_cast<uint32_t>( pWaitSemaphores.size() ),
		.pWaitSemaphores = pWaitSemaphores.data(),
		.pWaitDstStageMask = uWaitStageFlags.data(),
		.commandBufferCount = 1,
		.pCommandBuffers = &rawCmdBuffer,
		.signalSemaphoreCount = static_cast<uint32_t>( pSignalSemaphores.size() ),
		.pSignalSemaphores = pSignalSemaphores.data(),
	};

	vk_check( vk.QueueSubmit( cmdBuffer->queue(), 1, &submitInfo, VK_NULL_HANDLE ) );

	return nextSeqNo;
}

uint64_t CVulkanDevice::submit( std::unique_ptr<CVulkanCmdBuffer> cmdBuffer)
{
	uint64_t nextSeqNo = submitInternal(cmdBuffer.get());
	m_pendingCmdBufs.emplace(nextSeqNo, std::move(cmdBuffer));
	return nextSeqNo;
}

void CVulkanDevice::garbageCollect( void )
{
	uint64_t currentSeqNo;
	vk_check( vk.GetSemaphoreCounterValue(device(), m_scratchTimelineSemaphore, &currentSeqNo) );

	resetCmdBuffers(currentSeqNo);
}

VulkanTimelineSemaphore_t::~VulkanTimelineSemaphore_t()
{
	if ( pVkSemaphore != VK_NULL_HANDLE )
	{
		pDevice->vk.DestroySemaphore( pDevice->device(), pVkSemaphore, nullptr );
		pVkSemaphore = VK_NULL_HANDLE;
	}
}

int VulkanTimelineSemaphore_t::GetFd() const
{
	const VkSemaphoreGetFdInfoKHR semaphoreGetInfo =
	{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
		.semaphore = pVkSemaphore,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
	};

	int32_t nFd = -1;
	VkResult res = VK_SUCCESS;
	if ( ( res = pDevice->vk.GetSemaphoreFdKHR( pDevice->device(), &semaphoreGetInfo, &nFd ) ) != VK_SUCCESS )
	{
		vk_errorf( res, "vkGetSemaphoreFdKHR failed" );
		return -1;
	}

	return nFd;
}

std::shared_ptr<VulkanTimelineSemaphore_t> CVulkanDevice::CreateTimelineSemaphore( uint64_t ulStartPoint, bool bShared )
{
	std::shared_ptr<VulkanTimelineSemaphore_t> pSemaphore = std::make_unique<VulkanTimelineSemaphore_t>();
	pSemaphore->pDevice = this;

	VkSemaphoreCreateInfo createInfo =
	{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};

	VkSemaphoreTypeCreateInfo typeInfo =
	{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.pNext = std::exchange( createInfo.pNext, &typeInfo ),
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
		.initialValue = ulStartPoint,
	};

	VkExportSemaphoreCreateInfo exportInfo =
	{
		.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
		.pNext = bShared ? std::exchange( createInfo.pNext, &exportInfo ) : nullptr,
		// This is a syncobj fd for any drivers using syncobj.
		.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
	};

	VkResult res;
	if ( ( res = vk.CreateSemaphore( m_device, &createInfo, nullptr, &pSemaphore->pVkSemaphore ) ) != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateSemaphore failed" );
		return nullptr;
	}

	return pSemaphore;
}

std::shared_ptr<VulkanTimelineSemaphore_t> CVulkanDevice::ImportTimelineSemaphore( gamescope::CTimeline *pTimeline )
{
	std::shared_ptr<VulkanTimelineSemaphore_t> pSemaphore = std::make_unique<VulkanTimelineSemaphore_t>();
	pSemaphore->pDevice = this;

	const VkSemaphoreTypeCreateInfo typeInfo =
	{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	};

	const VkSemaphoreCreateInfo createInfo =
	{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &typeInfo,
	};

	VkResult res;
	if ( ( res = vk.CreateSemaphore( m_device, &createInfo, nullptr, &pSemaphore->pVkSemaphore ) ) != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateSemaphore failed" );
		return nullptr;
	}

    // "Importing a semaphore payload from a file descriptor transfers
    // ownership of the file descriptor from the application to the Vulkan
    // implementation. The application must not perform any operations on
    // the file descriptor after a successful import."
	//
	// Thus, we must dup.

	VkImportSemaphoreFdInfoKHR importFdInfo =
	{
		.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
		.pNext = nullptr,
		.semaphore = pSemaphore->pVkSemaphore,
		.flags = 0, // not temporary
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
		.fd = dup( pTimeline->GetSyncobjFd() ),
	};
	if ( ( res = vk.ImportSemaphoreFdKHR( m_device, &importFdInfo ) ) != VK_SUCCESS )
	{
		vk_errorf( res, "vkImportSemaphoreFdKHR failed" );
		return nullptr;
	}

	return pSemaphore;
}

void CVulkanCmdBuffer::AddDependency( std::shared_ptr<VulkanTimelineSemaphore_t> pTimelineSemaphore, uint64_t ulPoint )
{
	m_ExternalDependencies.emplace_back( std::move( pTimelineSemaphore ), ulPoint );
}

void CVulkanCmdBuffer::AddSignal( std::shared_ptr<VulkanTimelineSemaphore_t> pTimelineSemaphore, uint64_t ulPoint )
{
	m_ExternalSignals.emplace_back( std::move( pTimelineSemaphore ), ulPoint );
}

void CVulkanDevice::wait(uint64_t sequence, bool reset)
{
	if (m_submissionSeqNo == sequence)
		m_uploadBufferOffset = 0;

	VkSemaphoreWaitInfo waitInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		.semaphoreCount = 1,
		.pSemaphores = &m_scratchTimelineSemaphore,
		.pValues = &sequence,
	} ;

	vk_check( vk.WaitSemaphores( device(), &waitInfo, ~0ull ) );

	if (reset)
		resetCmdBuffers(sequence);
}

void CVulkanDevice::waitIdle(bool reset)
{
	wait(m_submissionSeqNo, reset);
}

void CVulkanDevice::resetCmdBuffers(uint64_t sequence)
{
	auto last = m_pendingCmdBufs.find(sequence);
	if (last == m_pendingCmdBufs.end())
		return;

	for (auto it = m_pendingCmdBufs.begin(); ; it++)
	{
		it->second->reset();
		m_unusedCmdBufs.push_back(std::move(it->second));
		if (it == last)
			break;
	}

	m_pendingCmdBufs.erase(m_pendingCmdBufs.begin(), ++last);
}

CVulkanCmdBuffer::CVulkanCmdBuffer(CVulkanDevice *parent, VkCommandBuffer cmdBuffer, VkCommandPool cmdPool, VkQueue queue, uint32_t queueFamily)
	: m_cmdBuffer(cmdBuffer), m_device(parent), m_cmdPool(cmdPool), m_queue(queue), m_queueFamily(queueFamily)
{
}

CVulkanCmdBuffer::~CVulkanCmdBuffer()
{
	// Must free from the pool this buffer was actually allocated from -- was
	// unconditionally m_device->commandPool() (the compute pool) before, which
	// only happened to be correct because nothing but commandBuffer() ever
	// constructed one of these. generalCommandBuffer() breaks that assumption.
	m_device->vk.FreeCommandBuffers(m_device->device(), m_cmdPool, 1, &m_cmdBuffer);
}

void CVulkanCmdBuffer::reset()
{
	vk_check( m_device->vk.ResetCommandBuffer(m_cmdBuffer, 0) );
	m_textureRefs.clear();
	m_textureState.clear();

	m_ExternalDependencies.clear();
	m_ExternalSignals.clear();
}

void CVulkanCmdBuffer::begin()
{
	VkCommandBufferBeginInfo commandBufferBeginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};

	vk_check( m_device->vk.BeginCommandBuffer(m_cmdBuffer, &commandBufferBeginInfo) );

	clearState();
}

void CVulkanCmdBuffer::end()
{
	insertBarrier(true);
	vk_check( m_device->vk.EndCommandBuffer(m_cmdBuffer) );
}

void CVulkanCmdBuffer::bindTexture(uint32_t slot, gamescope::Rc<CVulkanTexture> texture)
{
	m_boundTextures[slot] = texture.get();
	if (texture)
		m_textureRefs.emplace_back(std::move(texture));
}

void CVulkanCmdBuffer::bindColorMgmtLuts(uint32_t slot, gamescope::Rc<CVulkanTexture> lut1d, gamescope::Rc<CVulkanTexture> lut3d)
{
	m_shaperLut[slot] = lut1d.get();
	m_lut3D[slot] = lut3d.get();

	if (lut1d != nullptr)
		m_textureRefs.emplace_back(std::move(lut1d));
	if (lut3d != nullptr)
		m_textureRefs.emplace_back(std::move(lut3d));
}

void CVulkanCmdBuffer::setTextureSrgb(uint32_t slot, bool srgb)
{
	m_useSrgb[slot] = srgb;
}

void CVulkanCmdBuffer::setSamplerNearest(uint32_t slot, bool nearest)
{
	m_samplerState[slot].bNearest = nearest;
}

void CVulkanCmdBuffer::setSamplerUnnormalized(uint32_t slot, bool unnormalized)
{
	m_samplerState[slot].bUnnormalized = unnormalized;
}

void CVulkanCmdBuffer::bindTarget(gamescope::Rc<CVulkanTexture> target)
{
	m_target = target.get();
	if (target)
		m_textureRefs.emplace_back(std::move(target));
}

void CVulkanCmdBuffer::clearState()
{
	for (auto& texture : m_boundTextures)
		texture = nullptr;

	for (auto& sampler : m_samplerState)
		sampler = {};

	m_target = nullptr;
	m_useSrgb.reset();
}

template<class PushData, class... Args>
void CVulkanCmdBuffer::uploadConstants(Args&&... args)
{
	PushData data(std::forward<Args>(args)...);

	auto [ptr, offset] = m_device->uploadBufferData(sizeof(data));
	m_renderBufferOffset = offset;
	memcpy(ptr, &data, sizeof(data));
}

void CVulkanCmdBuffer::bindPipeline(VkPipeline pipeline)
{
	m_device->vk.CmdBindPipeline(m_cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
}

void CVulkanCmdBuffer::dispatch(uint32_t x, uint32_t y, uint32_t z)
{
	for (auto src : m_boundTextures)
	{
		if (src)
			prepareSrcImage(src);
	}
	assert(m_target != nullptr);
	prepareDestImage(m_target);
	insertBarrier();

	VkDescriptorSet descriptorSet = m_device->descriptorSet();

	std::array<VkWriteDescriptorSet, 7> writeDescriptorSets;
	std::array<VkDescriptorImageInfo, VKR_SAMPLER_SLOTS> imageDescriptors = {};
	std::array<VkDescriptorImageInfo, VKR_SAMPLER_SLOTS> ycbcrImageDescriptors = {};
	std::array<VkDescriptorImageInfo, VKR_TARGET_SLOTS> targetDescriptors = {};
	std::array<VkDescriptorImageInfo, VKR_LUT3D_COUNT> shaperLutDescriptor = {};
	std::array<VkDescriptorImageInfo, VKR_LUT3D_COUNT> lut3DDescriptor = {};
	VkDescriptorBufferInfo scratchDescriptor = {};

	writeDescriptorSets[0] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = descriptorSet,
		.dstBinding = 0,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pBufferInfo = &scratchDescriptor,
	};

	writeDescriptorSets[1] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = descriptorSet,
		.dstBinding = 1,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		.pImageInfo = &targetDescriptors[0],
	};

	writeDescriptorSets[2] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = descriptorSet,
		.dstBinding = 2,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		.pImageInfo = &targetDescriptors[1],
	};

	writeDescriptorSets[3] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = descriptorSet,
		.dstBinding = 3,
		.dstArrayElement = 0,
		.descriptorCount = imageDescriptors.size(),
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = imageDescriptors.data(),
	};

	writeDescriptorSets[4] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = descriptorSet,
		.dstBinding = 4,
		.dstArrayElement = 0,
		.descriptorCount = ycbcrImageDescriptors.size(),
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = ycbcrImageDescriptors.data(),
	};

	writeDescriptorSets[5] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = descriptorSet,
		.dstBinding = 5,
		.dstArrayElement = 0,
		.descriptorCount = shaperLutDescriptor.size(),
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = shaperLutDescriptor.data(),
	};

	writeDescriptorSets[6] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = descriptorSet,
		.dstBinding = 6,
		.dstArrayElement = 0,
		.descriptorCount = lut3DDescriptor.size(),
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = lut3DDescriptor.data(),
	};

	scratchDescriptor.buffer = m_device->m_uploadBuffer;
	scratchDescriptor.offset = m_renderBufferOffset;
	scratchDescriptor.range = VK_WHOLE_SIZE;

	for (uint32_t i = 0; i < VKR_SAMPLER_SLOTS; i++)
	{
		imageDescriptors[i].sampler = m_device->sampler(m_samplerState[i]);
		imageDescriptors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		ycbcrImageDescriptors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		if (m_boundTextures[i] == nullptr)
			continue;

		VkImageView view = m_useSrgb[i] ? m_boundTextures[i]->srgbView() : m_boundTextures[i]->linearView();

		if (m_boundTextures[i]->format() == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
			ycbcrImageDescriptors[i].imageView = view;
		else
			imageDescriptors[i].imageView = view;
	}

	for (uint32_t i = 0; i < VKR_LUT3D_COUNT; i++)
	{
		SamplerState linearState;
		linearState.bNearest = false;
		linearState.bUnnormalized = false;
		SamplerState nearestState; // TODO(Josh): Probably want to do this when I bring in tetrahedral interpolation.
		nearestState.bNearest = true;
		nearestState.bUnnormalized = false;

		shaperLutDescriptor[i].sampler = m_device->sampler(linearState);
		shaperLutDescriptor[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		// TODO(Josh): I hate the fact that srgbView = view *as* raw srgb and treat as linear.
		// I need to change this, it's so utterly stupid and confusing.
		shaperLutDescriptor[i].imageView = m_shaperLut[i] ? m_shaperLut[i]->srgbView() : VK_NULL_HANDLE;

		lut3DDescriptor[i].sampler = m_device->sampler(nearestState);
		lut3DDescriptor[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		lut3DDescriptor[i].imageView = m_lut3D[i] ? m_lut3D[i]->srgbView() : VK_NULL_HANDLE;
	}

	if (!m_target->isYcbcr())
	{
		targetDescriptors[0].imageView = m_target->srgbView();
		targetDescriptors[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	}
	else
	{
		targetDescriptors[0].imageView = m_target->lumaView();
		targetDescriptors[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		targetDescriptors[1].imageView = m_target->chromaView();
		targetDescriptors[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	}

	m_device->vk.UpdateDescriptorSets(m_device->device(), writeDescriptorSets.size(), writeDescriptorSets.data(), 0, nullptr);

	m_device->vk.CmdBindDescriptorSets(m_cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_device->pipelineLayout(), 0, 1, &descriptorSet, 0, nullptr);

	m_device->vk.CmdDispatch(m_cmdBuffer, x, y, z);

	markDirty(m_target);
}

void CVulkanCmdBuffer::copyImage(gamescope::Rc<CVulkanTexture> src, gamescope::Rc<CVulkanTexture> dst)
{
	assert(src->width() == dst->width());
	assert(src->height() == dst->height());
	prepareSrcImage(src.get());
	prepareDestImage(dst.get());
	insertBarrier();

	VkImageCopy region = {
		.srcSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1
		},
		.dstSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1
		},
		.extent = {
			.width = src->width(),
			.height = src->height(),
			.depth = 1
		},
	};

	m_device->vk.CmdCopyImage(m_cmdBuffer, src->vkImage(), VK_IMAGE_LAYOUT_GENERAL, dst->vkImage(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);

	markDirty(dst.get());
	m_textureRefs.emplace_back(std::move(src));
	m_textureRefs.emplace_back(std::move(dst));
}

void CVulkanCmdBuffer::copyBufferToImage(VkBuffer buffer, VkDeviceSize offset, uint32_t stride, gamescope::Rc<CVulkanTexture> dst)
{
	prepareDestImage(dst.get());
	insertBarrier();
	VkBufferImageCopy region = {
		.bufferOffset = offset,
		.bufferRowLength = stride,
		.imageSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1,
		},
		.imageExtent = {
			.width = dst->width(),
			.height = dst->height(),
			.depth = dst->depth(),
		},
	};

	m_device->vk.CmdCopyBufferToImage(m_cmdBuffer, buffer, dst->vkImage(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);

	markDirty(dst.get());

	m_textureRefs.emplace_back(std::move(dst));
}

void CVulkanCmdBuffer::prepareSrcImage(CVulkanTexture *image)
{
	auto result = m_textureState.emplace(image, TextureState());
	// no need to reimport if the image didn't change
	if (!result.second)
		return;
	// using the swapchain image as a source without writing to it doesn't make any sense
	assert(image->outputImage() == false);
	result.first->second.needsImport = image->externalImage();
	result.first->second.needsExport = image->externalImage();
}

void CVulkanCmdBuffer::prepareDestImage(CVulkanTexture *image)
{
	auto result = m_textureState.emplace(image, TextureState());
	// no need to discard if the image is already image/in the correct layout
	if (!result.second)
		return;
	result.first->second.discarded = true;
	result.first->second.needsExport = image->externalImage();
	result.first->second.needsPresentLayout = image->outputImage();
}

void CVulkanCmdBuffer::discardImage(CVulkanTexture *image)
{
	auto result = m_textureState.emplace(image, TextureState());
	if (!result.second)
		return;
	result.first->second.discarded = true;
}

void CVulkanCmdBuffer::markDirty(CVulkanTexture *image)
{
	auto result = m_textureState.find(image);
	// image should have been prepared already
	assert(result !=  m_textureState.end());
	result->second.dirty = true;
}

void CVulkanCmdBuffer::insertBarrier(bool flush)
{
	std::vector<VkImageMemoryBarrier> barriers;

	uint32_t externalQueue = m_device->supportsModifiers() ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL_KHR;

	VkImageSubresourceRange subResRange =
	{
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.levelCount = 1,
		.layerCount = 1
	};

	for (auto& pair : m_textureState)
	{
		CVulkanTexture *image = pair.first;
		TextureState& state = pair.second;
		assert(!flush || !state.needsImport);

		bool isExport = flush && state.needsExport;
		bool isPresent = flush && state.needsPresentLayout;

		if (!state.discarded && !state.dirty && !state.needsImport && !isExport && !isPresent)
			continue;

		const VkAccessFlags write_bits = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		const VkAccessFlags read_bits = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

		if (image->queueFamily == VK_QUEUE_FAMILY_IGNORED)
			image->queueFamily = m_queueFamily;

		// A CONCURRENT image (createFlags::bGeneralQueueShared) is usable from
		// every family it was created with, so it has no owner to transfer.
		// Emitting a transfer for one is invalid usage, and the tracked
		// image->queueFamily would be a lie anyway: these images are written
		// from the general queue behind insertBarrier()'s back.
		const bool bConcurrent = image->generalQueueShared();

		VkImageMemoryBarrier memoryBarrier =
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = state.dirty ? write_bits : 0u,
			.dstAccessMask = flush ? 0u : read_bits | write_bits,
			.oldLayout = state.discarded ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
			.newLayout = isPresent ? GetBackend()->GetPresentLayout() : VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = bConcurrent ? VK_QUEUE_FAMILY_IGNORED : isExport ? image->queueFamily : state.needsImport ? externalQueue : image->queueFamily,
			.dstQueueFamilyIndex = bConcurrent ? VK_QUEUE_FAMILY_IGNORED : isExport ? externalQueue : state.needsImport ? m_queueFamily : m_queueFamily,
			.image = image->vkImage(),
			.subresourceRange = subResRange
		};

		barriers.push_back(memoryBarrier);

		state.discarded = false;
		state.dirty = false;
		state.needsImport = false;
	}

	// TODO replace VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
	m_device->vk.CmdPipelineBarrier(m_cmdBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
									0, 0, nullptr, 0, nullptr, barriers.size(), barriers.data());
}

CVulkanDevice g_device;

static bool allDMABUFsEqual( wlr_dmabuf_attributes *pDMA )
{
	if ( pDMA->n_planes == 1 )
		return true;

	struct stat first_stat;
	if ( fstat( pDMA->fd[0], &first_stat ) != 0 )
	{
		vk_log.errorf_errno( "fstat failed" );
		return false;
	}

	for ( int i = 1; i < pDMA->n_planes; ++i )
	{
		struct stat plane_stat;
		if ( fstat( pDMA->fd[i], &plane_stat ) != 0 )
		{
			vk_log.errorf_errno( "fstat failed" );
			return false;
		}
		if ( plane_stat.st_ino != first_stat.st_ino )
			return false;
	}

	return true;
}

static VkResult getModifierProps( const VkImageCreateInfo *imageInfo, uint64_t modifier, VkExternalImageFormatProperties *externalFormatProps)
{
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierFormatInfo = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
		.drmFormatModifier = modifier,
		.sharingMode = imageInfo->sharingMode,
	};

	VkPhysicalDeviceExternalImageFormatInfo externalImageFormatInfo = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
		.pNext = &modifierFormatInfo,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};

	VkPhysicalDeviceImageFormatInfo2 imageFormatInfo = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		.pNext = &externalImageFormatInfo,
		.format = imageInfo->format,
		.type = imageInfo->imageType,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = imageInfo->usage,
		.flags = imageInfo->flags,
	};

	const VkImageFormatListCreateInfo *readonlyList = pNextFind<VkImageFormatListCreateInfo>(imageInfo, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO);
	VkImageFormatListCreateInfo formatList = {};
	if ( readonlyList != nullptr )
	{
		formatList = *readonlyList;
		formatList.pNext = std::exchange(imageFormatInfo.pNext, &formatList);
	}

	VkImageFormatProperties2 imageProps = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
		.pNext = externalFormatProps,
	};

	return g_device.vk.GetPhysicalDeviceImageFormatProperties2(g_device.physDev(), &imageFormatInfo, &imageProps);
}

static VkImageViewType VulkanImageTypeToViewType(VkImageType type)
{
	switch (type)
	{
		case VK_IMAGE_TYPE_1D: return VK_IMAGE_VIEW_TYPE_1D;
		case VK_IMAGE_TYPE_2D: return VK_IMAGE_VIEW_TYPE_2D;
		case VK_IMAGE_TYPE_3D: return VK_IMAGE_VIEW_TYPE_3D;
		default: abort();
	}
}

bool CVulkanTexture::BInit( uint32_t width, uint32_t height, uint32_t depth, uint32_t drmFormat, createFlags flags, wlr_dmabuf_attributes *pDMA /* = nullptr */,  uint32_t contentWidth /* = 0 */, uint32_t contentHeight /* =  0 */, CVulkanTexture *pExistingImageToReuseMemory, gamescope::OwningRc<gamescope::IBackendFb> pBackendFb )
{
	m_pBackendFb = std::move( pBackendFb );
	m_drmFormat = drmFormat;
	VkResult res = VK_ERROR_INITIALIZATION_FAILED;

	VkImageTiling tiling = (flags.bMappable || flags.bLinear) ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
	VkImageUsageFlags usage = 0;
	VkMemoryPropertyFlags properties;

	if ( flags.bSampled == true )
	{
		usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	}

	if ( flags.bStorage == true )
	{
		usage |= VK_IMAGE_USAGE_STORAGE_BIT;
	}

	if ( flags.bColorAttachment == true )
	{
		usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	}

	if ( flags.bFlippable == true )
	{
		flags.bExportable = true;
	}

	if ( flags.bTransferSrc == true )
	{
		usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}

	if ( flags.bTransferDst == true )
	{
		usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	}

	if ( flags.bMappable == true )
	{
		properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	}
	else
	{
		properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	}

	if ( flags.bOutputImage == true )
	{
		m_bOutputImage = true;	
	}

	m_bExternal = pDMA || flags.bExportable == true;

	// See createFlags::bGeneralQueueShared. Only actually go CONCURRENT when
	// the two families really are distinct -- when they are the same family
	// (some Intel/NVIDIA setups) EXCLUSIVE is already correct, and CONCURRENT
	// with duplicate indices is invalid usage.
	const uint32_t uSharedQueueFamilies[2] = { g_device.queueFamily(), g_device.generalQueueFamily() };
	m_bGeneralQueueShared = flags.bGeneralQueueShared && uSharedQueueFamilies[0] != uSharedQueueFamilies[1];

	// Possible extensions for below
	wsi_image_create_info wsiImageCreateInfo = {};
	VkExternalMemoryImageCreateInfo externalImageCreateInfo = {};
	VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo = {};
	VkSubresourceLayout modifierPlaneLayouts[4] = {};
	VkImageDrmFormatModifierListCreateInfoEXT modifierListInfo = {};
	
	VkImageCreateInfo imageInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = flags.imageType,
		.format = DRMFormatToVulkan(drmFormat, false),
		.extent = {
			.width = width,
			.height = height,
			.depth = depth,
		},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = tiling,
		.usage = usage,
		.sharingMode = m_bGeneralQueueShared ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = m_bGeneralQueueShared ? 2u : 0u,
		.pQueueFamilyIndices = m_bGeneralQueueShared ? uSharedQueueFamilies : nullptr,
	};

	assert( imageInfo.format != VK_FORMAT_UNDEFINED );

	std::array<VkFormat, 2> formats = {
		DRMFormatToVulkan(drmFormat, false),
		DRMFormatToVulkan(drmFormat, true),
	};

	VkImageFormatListCreateInfo formatList = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
		.viewFormatCount = (uint32_t)formats.size(),
		.pViewFormats = formats.data(),
	};

	if ( formats[0] != formats[1] )
	{
		formatList.pNext = std::exchange(imageInfo.pNext, &formatList);
		imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	}

	if ( pDMA != nullptr )
	{
		assert( drmFormat == pDMA->format );
	}

	if ( g_device.supportsModifiers() && pDMA && pDMA->modifier != DRM_FORMAT_MOD_INVALID )
	{
		VkExternalImageFormatProperties externalImageProperties = {
			.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
		};

		res = getModifierProps( &imageInfo, pDMA->modifier, &externalImageProperties );
		if ( res != VK_SUCCESS && res != VK_ERROR_FORMAT_NOT_SUPPORTED ) {
			vk_errorf( res, "getModifierProps failed" );
			return false;
		}

		if ( res == VK_SUCCESS &&
		     ( externalImageProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT ) )
		{
			modifierInfo = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
				.pNext = std::exchange(imageInfo.pNext, &modifierInfo),
				.drmFormatModifier = pDMA->modifier,
				.drmFormatModifierPlaneCount = uint32_t(pDMA->n_planes),
				.pPlaneLayouts = modifierPlaneLayouts,
			};

			for ( int i = 0; i < pDMA->n_planes; ++i )
			{
				modifierPlaneLayouts[i].offset = pDMA->offset[i];
				modifierPlaneLayouts[i].rowPitch = pDMA->stride[i];
			}

			imageInfo.tiling = tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		}
	}

	std::vector<uint64_t> modifiers = {};
	// TODO(JoshA): Move this code to backend for making flippable image.
	if ( GetBackend()->UsesModifiers() && flags.bFlippable && g_device.supportsModifiers() && !pDMA )
	{
		assert( drmFormat != DRM_FORMAT_INVALID );

		uint64_t linear = DRM_FORMAT_MOD_LINEAR;

		const uint64_t *possibleModifiers;
		size_t numPossibleModifiers;
		if ( flags.bLinear )
		{
			possibleModifiers = &linear;
			numPossibleModifiers = 1;
		}
		else
		{
			std::span<const uint64_t> modifiers = GetBackend()->GetSupportedModifiers( drmFormat );
			assert( !modifiers.empty() );
			possibleModifiers = modifiers.data();
			numPossibleModifiers = modifiers.size();
		}

		for ( size_t i = 0; i < numPossibleModifiers; i++ )
		{
			uint64_t modifier = possibleModifiers[i];

			VkExternalImageFormatProperties externalFormatProps = {
				.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
			};
			res = getModifierProps( &imageInfo, modifier, &externalFormatProps );
			if ( res == VK_ERROR_FORMAT_NOT_SUPPORTED )
				continue;
			else if ( res != VK_SUCCESS ) {
				vk_errorf( res, "getModifierProps failed" );
				return false;
			}

			if ( !( externalFormatProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT ) )
				continue;

			modifiers.push_back( modifier );
		}

		assert( modifiers.size() > 0 );

		modifierListInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
			.pNext = std::exchange(imageInfo.pNext, &modifierListInfo),
			.drmFormatModifierCount = uint32_t(modifiers.size()),
			.pDrmFormatModifiers = modifiers.data(),
		};

		externalImageCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
			.pNext = std::exchange(imageInfo.pNext, &externalImageCreateInfo),
			.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		};

		imageInfo.tiling = tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	}

	if ( flags.bFlippable == true && tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT )
	{
		// We want to scan-out the image
		wsiImageCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA,
			.pNext = std::exchange(imageInfo.pNext, &wsiImageCreateInfo),
			.scanout = VK_TRUE,
		};
	}
	
	if ( pDMA != nullptr )
	{
		externalImageCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
			.pNext = std::exchange(imageInfo.pNext, &externalImageCreateInfo),
			.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		};
	}

	m_width = width;
	m_height = height;
	m_depth = depth;

	if (contentWidth && contentHeight)
	{
		m_contentWidth = contentWidth;
		m_contentHeight = contentHeight;
	}
	else
	{
		m_contentWidth = width;
		m_contentHeight = height;
	}

	m_format = imageInfo.format;

	res = g_device.vk.CreateImage(g_device.device(), &imageInfo, nullptr, &m_vkImage);
	if (res != VK_SUCCESS) {
		vk_errorf( res, "vkCreateImage failed" );
		return false;
	}
	
	VkMemoryRequirements memRequirements;
	g_device.vk.GetImageMemoryRequirements(g_device.device(), m_vkImage, &memRequirements);

	VkMemoryAllocateInfo allocInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = memRequirements.size,
		.memoryTypeIndex = uint32_t(g_device.findMemoryType(properties, memRequirements.memoryTypeBits)),
	};

	m_size = allocInfo.allocationSize;

	VkDeviceMemory memoryHandle = VK_NULL_HANDLE;

	if ( pExistingImageToReuseMemory == nullptr )
	{
		// Possible pNexts
		VkImportMemoryFdInfoKHR importMemoryInfo = {};
		VkExportMemoryAllocateInfo memory_export_info = {};
		VkMemoryDedicatedAllocateInfo memory_dedicated_info = {};
		struct wsi_memory_allocate_info memory_wsi_info = {};

		if ( flags.bFlippable == true )
		{
			memory_wsi_info = {
				.sType = VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA,
				.pNext = std::exchange(allocInfo.pNext, &memory_wsi_info),
			};
		}

		if ( flags.bExportable == true || pDMA != nullptr )
		{
			memory_dedicated_info = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
				.pNext = std::exchange(allocInfo.pNext, &memory_dedicated_info),
				.image = m_vkImage,
			};
		}
		
		if ( flags.bExportable == true && pDMA == nullptr )
		{
			// We'll export it to DRM
			memory_export_info = {
				.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
				.pNext = std::exchange(allocInfo.pNext, &memory_export_info),
				.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
			};
		}
		
		if ( pDMA != nullptr )
		{
			// TODO: multi-planar DISTINCT DMA-BUFs support (see vkBindImageMemory2
			// and VkBindImagePlaneMemoryInfo)
			assert( allDMABUFsEqual( pDMA ) );

			// Importing memory from a FD transfers ownership of the FD
			int fd = dup( pDMA->fd[0] );
			if ( fd < 0 )
			{
				vk_log.errorf_errno( "dup failed" );
				return false;
			}

			// Memory already provided by pDMA
			importMemoryInfo = {
					.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
					.pNext = std::exchange(allocInfo.pNext, &importMemoryInfo),
					.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
					.fd = fd,
			};
		}
		
		res = g_device.vk.AllocateMemory( g_device.device(), &allocInfo, nullptr, &memoryHandle );
		if ( res != VK_SUCCESS )
		{
			vk_errorf( res, "vkAllocateMemory failed" );
			if ( pDMA != nullptr )
			{
				// When import doesn't succeed, the driver hasn't taken ownership of the FD
				// Close it so it doesn't become an FD leak
				close( importMemoryInfo.fd );
			}
			return false;
		}

		m_vkImageMemory = memoryHandle;
	}
	else
	{
		vk_log.infof("%d vs %d!", (int)pExistingImageToReuseMemory->m_size, (int)m_size);
		assert(pExistingImageToReuseMemory->m_size >= m_size);

		memoryHandle = pExistingImageToReuseMemory->m_vkImageMemory;
		m_vkImageMemory = VK_NULL_HANDLE;
	}
	
	res = g_device.vk.BindImageMemory( g_device.device(), m_vkImage, memoryHandle, 0 );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkBindImageMemory failed" );
		return false;
	}

	if ( flags.bMappable == true )
	{
		assert( tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT );
		const VkImageSubresource image_subresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		};
		VkSubresourceLayout image_layout;
		g_device.vk.GetImageSubresourceLayout(g_device.device(), m_vkImage, &image_subresource, &image_layout);

		m_unRowPitch = image_layout.rowPitch;

		if (isYcbcr())
		{
			const VkImageSubresource lumaSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
			};
			VkSubresourceLayout lumaLayout;
			g_device.vk.GetImageSubresourceLayout(g_device.device(), m_vkImage, &lumaSubresource, &lumaLayout);

			m_lumaOffset = lumaLayout.offset;
			m_lumaPitch = lumaLayout.rowPitch;

			const VkImageSubresource chromaSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
			};
			VkSubresourceLayout chromaLayout;
			g_device.vk.GetImageSubresourceLayout(g_device.device(), m_vkImage, &chromaSubresource, &chromaLayout);

			m_chromaOffset = chromaLayout.offset;
			m_chromaPitch = chromaLayout.rowPitch;
		}
	}
	
	if ( flags.bExportable == true )
	{
		// We assume we own the memory when doing this right now.
		// We could support the import scenario as well if needed (but we
		// already have a DMA-BUF in that case).
		assert( pDMA == nullptr );

		struct wlr_dmabuf_attributes dmabuf = {
			.width = int(width),
			.height = int(height),
			.format = drmFormat,
		};
		assert( dmabuf.format != DRM_FORMAT_INVALID );

		// TODO: disjoint planes support
		const VkMemoryGetFdInfoKHR memory_get_fd_info = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
			.memory = memoryHandle,
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		};
		res = g_device.vk.GetMemoryFdKHR(g_device.device(), &memory_get_fd_info, &dmabuf.fd[0]);
		if ( res != VK_SUCCESS ) {
			vk_errorf( res, "vkGetMemoryFdKHR failed" );
			return false;
		}

		if ( tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT )
		{
			assert( g_device.vk.GetImageDrmFormatModifierPropertiesEXT != nullptr );

			VkImageDrmFormatModifierPropertiesEXT imgModifierProps = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
			};

			res = g_device.vk.GetImageDrmFormatModifierPropertiesEXT( g_device.device(), m_vkImage, &imgModifierProps );
			if ( res != VK_SUCCESS ) {
				vk_errorf( res, "vkGetImageDrmFormatModifierPropertiesEXT failed" );
				return false;
			}
			dmabuf.modifier = imgModifierProps.drmFormatModifier;

			assert( DRMModifierProps.count( m_format ) > 0);
			assert( DRMModifierProps[ m_format ].count( dmabuf.modifier ) > 0);

			dmabuf.n_planes = DRMModifierProps[ m_format ][ dmabuf.modifier ].drmFormatModifierPlaneCount;

			const VkImageAspectFlagBits planeAspects[] = {
				VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
				VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
				VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
				VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
			};
			assert( dmabuf.n_planes <= 4 );

			for ( int i = 0; i < dmabuf.n_planes; i++ )
			{
				const VkImageSubresource subresource = {
					.aspectMask = planeAspects[i],
				};
				VkSubresourceLayout subresourceLayout = {};
				g_device.vk.GetImageSubresourceLayout( g_device.device(), m_vkImage, &subresource, &subresourceLayout );
				dmabuf.offset[i] = subresourceLayout.offset;
				dmabuf.stride[i] = subresourceLayout.rowPitch;
			}

			// Copy the first FD to all other planes
			for ( int i = 1; i < dmabuf.n_planes; i++ )
			{
				dmabuf.fd[i] = dup( dmabuf.fd[0] );
				if ( dmabuf.fd[i] < 0 ) {
					vk_log.errorf_errno( "dup failed" );
					return false;
				}
			}
		}
		else
		{
			const VkImageSubresource subresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			};
			VkSubresourceLayout subresourceLayout = {};
			g_device.vk.GetImageSubresourceLayout( g_device.device(), m_vkImage, &subresource, &subresourceLayout );

			dmabuf.n_planes = 1;
			dmabuf.modifier = DRM_FORMAT_MOD_INVALID;
			dmabuf.offset[0] = 0;
			dmabuf.stride[0] = subresourceLayout.rowPitch;
		}

		m_dmabuf = dmabuf;
	}

	if ( flags.bFlippable == true )
	{
		m_pBackendFb = GetBackend()->ImportDmabufToBackend( &m_dmabuf );
	}

	bool bHasAlpha = pDMA ? DRMFormatHasAlpha( pDMA->format ) : true;

	if (!bHasAlpha )
	{
		// not compatible with with swizzles
		assert ( flags.bStorage == false );
	}

	if ( flags.bStorage || flags.bSampled || flags.bColorAttachment )
	{
		VkImageViewCreateInfo createInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = m_vkImage,
			.viewType = VulkanImageTypeToViewType(flags.imageType),
			.format = DRMFormatToVulkan(drmFormat, false),
			.components = {
				.r = VK_COMPONENT_SWIZZLE_IDENTITY,
				.g = VK_COMPONENT_SWIZZLE_IDENTITY,
				.b = VK_COMPONENT_SWIZZLE_IDENTITY,
				.a = bHasAlpha ? VK_COMPONENT_SWIZZLE_IDENTITY : VK_COMPONENT_SWIZZLE_ONE,
			},
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1,
			},
		};

		res = g_device.vk.CreateImageView(g_device.device(), &createInfo, nullptr, &m_srgbView);
		if ( res != VK_SUCCESS ) {
			vk_errorf( res, "vkCreateImageView failed" );
			return false;
		}

		if ( flags.bSampled )
		{
			VkImageViewUsageCreateInfo viewUsageInfo = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
				.usage = usage & ~VK_IMAGE_USAGE_STORAGE_BIT,
			};
			createInfo.pNext = &viewUsageInfo;
			createInfo.format = DRMFormatToVulkan(drmFormat, true);
			res = g_device.vk.CreateImageView(g_device.device(), &createInfo, nullptr, &m_linearView);
			if ( res != VK_SUCCESS ) {
				vk_errorf( res, "vkCreateImageView failed" );
				return false;
			}
		}


		if ( isYcbcr() )
		{
			createInfo.pNext = NULL;
			createInfo.format = VK_FORMAT_R8_UNORM;

			createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
			res = g_device.vk.CreateImageView(g_device.device(), &createInfo, nullptr, &m_lumaView);
			if ( res != VK_SUCCESS ) {
				vk_errorf( res, "vkCreateImageView failed" );
				return false;
			}

			createInfo.pNext = NULL;
			createInfo.format = VK_FORMAT_R8G8_UNORM;
			createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
			res = g_device.vk.CreateImageView(g_device.device(), &createInfo, nullptr, &m_chromaView);
			if ( res != VK_SUCCESS ) {
				vk_errorf( res, "vkCreateImageView failed" );
				return false;
			}

			createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		}
	}

	if ( flags.bMappable )
	{
		if (pExistingImageToReuseMemory)
		{
			m_pMappedData = pExistingImageToReuseMemory->m_pMappedData;
		}
		else
		{
			void *pData = nullptr;
			res = g_device.vk.MapMemory( g_device.device(), memoryHandle, 0, VK_WHOLE_SIZE, 0, &pData );
			if ( res != VK_SUCCESS )
			{
				vk_errorf( res, "vkMapMemory failed" );
				return false;
			}
			m_pMappedData = (uint8_t*)pData;
		}
	}
	
	m_bInitialized = true;
	
	return true;
}

bool CVulkanTexture::BInitFromSwapchain( VkImage image, uint32_t width, uint32_t height, VkFormat format )
{
	m_drmFormat = VulkanFormatToDRM( format );
	m_vkImage = image;
	m_vkImageMemory = VK_NULL_HANDLE;
	m_width = width;
	m_height = height;
	m_depth = 1;
	m_format = format;
	m_contentWidth = width;
	m_contentHeight = height;
	m_bOutputImage = true;

	VkImageViewCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = ToLinearVulkanFormat( format ),
		.components = {
			.r = VK_COMPONENT_SWIZZLE_IDENTITY,
			.g = VK_COMPONENT_SWIZZLE_IDENTITY,
			.b = VK_COMPONENT_SWIZZLE_IDENTITY,
			.a = VK_COMPONENT_SWIZZLE_IDENTITY,
		},
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
	};

	VkResult res = g_device.vk.CreateImageView(g_device.device(), &createInfo, nullptr, &m_srgbView);
	if ( res != VK_SUCCESS ) {
		vk_errorf( res, "vkCreateImageView failed" );
		return false;
	}

	VkImageViewUsageCreateInfo viewUsageInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	};

	createInfo.pNext = &viewUsageInfo;
	createInfo.format = ToSrgbVulkanFormat( format );

	res = g_device.vk.CreateImageView(g_device.device(), &createInfo, nullptr, &m_linearView);
	if ( res != VK_SUCCESS ) {
		vk_errorf( res, "vkCreateImageView failed" );
		return false;
	}

	m_bInitialized = true;

	return true;
}

uint32_t CVulkanTexture::IncRef()
{
	uint32_t uRefCount = gamescope::RcObject::IncRef();
	if ( m_pBackendFb && !uRefCount )
	{
		m_pBackendFb->IncRef();
	}
	return uRefCount;
}
uint32_t CVulkanTexture::DecRef()
{
	// Need to pull it out as we could be destroyed in DecRef.
	gamescope::IBackendFb *pBackendFb = m_pBackendFb.get();

	uint32_t uRefCount = gamescope::RcObject::DecRef();
	if ( pBackendFb && !uRefCount )
	{
		pBackendFb->DecRef();
	}
	return uRefCount;
}

bool CVulkanTexture::IsInUse()
{
	if ( m_pBackendFb && m_pBackendFb->GetRefCount() != 0 )
		return true;

	return GetRefCount() != 0;
}

CVulkanTexture::CVulkanTexture( void )
{
}

CVulkanTexture::~CVulkanTexture( void )
{
	wlr_dmabuf_attributes_finish( &m_dmabuf );

	if ( m_pMappedData != nullptr && m_vkImageMemory )
	{
		g_device.vk.UnmapMemory( g_device.device(), m_vkImageMemory );
		m_pMappedData = nullptr;
	}

	if ( m_srgbView != VK_NULL_HANDLE )
	{
		g_device.vk.DestroyImageView( g_device.device(), m_srgbView, nullptr );
		m_srgbView = VK_NULL_HANDLE;
	}

	if ( m_linearView != VK_NULL_HANDLE )
	{
		g_device.vk.DestroyImageView( g_device.device(), m_linearView, nullptr );
		m_linearView = VK_NULL_HANDLE;
	}

	if ( m_pBackendFb != nullptr )
		m_pBackendFb = nullptr;

	if ( m_vkImageMemory != VK_NULL_HANDLE )
	{
		if ( m_vkImage != VK_NULL_HANDLE )
		{
			g_device.vk.DestroyImage( g_device.device(), m_vkImage, nullptr );
			m_vkImage = VK_NULL_HANDLE;
		}

		g_device.vk.FreeMemory( g_device.device(), m_vkImageMemory, nullptr );
		m_vkImageMemory = VK_NULL_HANDLE;
	}

	m_bInitialized = false;
}

int CVulkanTexture::memoryFence()
{
	const VkMemoryGetFdInfoKHR memory_get_fd_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
		.memory = m_vkImageMemory,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	int fence = -1;
	VkResult res = g_device.vk.GetMemoryFdKHR(g_device.device(), &memory_get_fd_info, &fence);
	if ( res != VK_SUCCESS ) {
		fprintf( stderr, "vkGetMemoryFdKHR failed\n" );
	}

	return fence;
}

static bool is_image_format_modifier_supported(VkFormat format, uint32_t drmFormat, uint64_t modifier)
{
  VkPhysicalDeviceImageFormatInfo2 imageFormatInfo = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
    .format = format,
    .type = VK_IMAGE_TYPE_2D,
    .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
    .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
  };

  std::array<VkFormat, 2> formats = {
    DRMFormatToVulkan(drmFormat, false),
    DRMFormatToVulkan(drmFormat, true),
  };

  VkImageFormatListCreateInfo formatList = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
    .viewFormatCount = (uint32_t)formats.size(),
    .pViewFormats = formats.data(),
  };

  if ( formats[0] != formats[1] )
    {
      formatList.pNext = std::exchange(imageFormatInfo.pNext,
				       &formatList);
      imageFormatInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }

  VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierInfo = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
    .pNext = nullptr,
    .drmFormatModifier = modifier,
  };

  modifierInfo.pNext = std::exchange(imageFormatInfo.pNext, &modifierInfo);

  VkImageFormatProperties2 imageFormatProps = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
  };

  VkResult res = g_device.vk.GetPhysicalDeviceImageFormatProperties2( g_device.physDev(), &imageFormatInfo, &imageFormatProps );
  return res == VK_SUCCESS;
}

bool vulkan_init_format(VkFormat format, uint32_t drmFormat)
{
	// First, check whether the Vulkan format is supported
	VkPhysicalDeviceImageFormatInfo2 imageFormatInfo = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		.format = format,
		.type = VK_IMAGE_TYPE_2D,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	};

	std::array<VkFormat, 2> formats = {
		DRMFormatToVulkan(drmFormat, false),
		DRMFormatToVulkan(drmFormat, true),
	};

	VkImageFormatListCreateInfo formatList = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
		.viewFormatCount = (uint32_t)formats.size(),
		.pViewFormats = formats.data(),
	};

	if ( formats[0] != formats[1] )
	{
		formatList.pNext = std::exchange(imageFormatInfo.pNext,
						 &formatList);
		imageFormatInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	}


	VkImageFormatProperties2 imageFormatProps = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
	};

	VkResult res = g_device.vk.GetPhysicalDeviceImageFormatProperties2( g_device.physDev(), &imageFormatInfo, &imageFormatProps );
	if ( res == VK_ERROR_FORMAT_NOT_SUPPORTED )
	{
		return false;
	}
	else if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkGetPhysicalDeviceImageFormatProperties2 failed for DRM format 0x%" PRIX32, drmFormat );
		return false;
	}

	wlr_drm_format_set_add( &sampledShmFormats, drmFormat, DRM_FORMAT_MOD_LINEAR );

	if ( g_device.supportsModifiers() )
	{
		// Then, collect the list of modifiers supported for sampled usage
		VkDrmFormatModifierPropertiesListEXT modifierPropList = {
			.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
		};
		VkFormatProperties2 formatProps = {
			.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
			.pNext = &modifierPropList,
		};

		g_device.vk.GetPhysicalDeviceFormatProperties2( g_device.physDev(), format, &formatProps );

		if ( modifierPropList.drmFormatModifierCount == 0 )
		{
			vk_errorf( res, "vkGetPhysicalDeviceFormatProperties2 returned zero modifiers for DRM format 0x%" PRIX32, drmFormat );
			return false;
		}

		std::vector<VkDrmFormatModifierPropertiesEXT> modifierProps(modifierPropList.drmFormatModifierCount);
		modifierPropList.pDrmFormatModifierProperties = modifierProps.data();
		g_device.vk.GetPhysicalDeviceFormatProperties2( g_device.physDev(), format, &formatProps );

		std::map< uint64_t, VkDrmFormatModifierPropertiesEXT > map = {};

		for ( size_t j = 0; j < modifierProps.size(); j++ )
		{
			map[ modifierProps[j].drmFormatModifier ] = modifierProps[j];

			uint64_t modifier = modifierProps[j].drmFormatModifier;

			if ( !is_image_format_modifier_supported( format, drmFormat, modifier ) )
				continue;

			if ( ( modifierProps[j].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ) == 0 )
			{
				continue;
			}

			// The deferred backend exposes all sample-able formats as supported modifiers.
			if ( !g_bAllowDeferredBackend )
			{
				if ( GetBackend()->UsesModifiers() && !gamescope::Algorithm::Contains( GetBackend()->GetSupportedModifiers( drmFormat ), modifier ) )
					continue;
			}

			wlr_drm_format_set_add( &sampledDRMFormats, drmFormat, modifier );
			s_SampledModifierFormats[ drmFormat ].emplace_back( modifier );
		}

		DRMModifierProps[ format ] = map;
		return true;
	}
	else
	{
		if ( GetBackend()->UsesModifiers() && !GetBackend()->SupportsInvalidModifier( drmFormat ) )
			return false;

		wlr_drm_format_set_add( &sampledDRMFormats, drmFormat, DRM_FORMAT_MOD_INVALID );
		return false;
	}
}

bool vulkan_init_formats()
{
	for ( size_t i = 0; s_DRMVKFormatTable[i].DRMFormat != DRM_FORMAT_INVALID; i++ )
	{
		if (s_DRMVKFormatTable[i].internal)
			continue;

		VkFormat format = s_DRMVKFormatTable[i].vkFormat;
		VkFormat srgbFormat = s_DRMVKFormatTable[i].vkFormatSrgb;
		uint32_t drmFormat = s_DRMVKFormatTable[i].DRMFormat;

		vulkan_init_format(format, drmFormat);
		if (format != srgbFormat)
			vulkan_init_format(srgbFormat, drmFormat);
	}

	vk_log.infof( "supported DRM formats for sampling usage:" );
	for ( size_t i = 0; i < sampledDRMFormats.len; i++ )
	{
		uint32_t fmt = sampledDRMFormats.formats[ i ].format;
#if HAVE_DRM
		char *name = drmGetFormatName(fmt);
		vk_log.infof( "  %s (0x%" PRIX32 ")", name, fmt );
		free(name);
#endif
	}

	return true;
}

bool acquire_next_image( void )
{
	VkResult res = g_device.vk.AcquireNextImageKHR( g_device.device(), g_output.swapChain, UINT64_MAX, VK_NULL_HANDLE, g_output.acquireFence, &g_output.nOutImage );
	if ( res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR )
		return false;
	if ( g_device.vk.WaitForFences( g_device.device(), 1, &g_output.acquireFence, false, UINT64_MAX ) != VK_SUCCESS )
		return false;
	return g_device.vk.ResetFences( g_device.device(), 1, &g_output.acquireFence ) == VK_SUCCESS;
}


static std::atomic<uint64_t> g_currentPresentWaitId = {0u};
static std::mutex present_wait_lock;

extern void mangoapp_output_update( uint64_t vblanktime );
static void present_wait_thread_func( void )
{
	uint64_t present_wait_id = 0;

	while (true)
	{
		g_currentPresentWaitId.wait(present_wait_id);

		// Lock to make sure swapchain destruction is waited on and that
		// it's for this swapchain.
		{
			std::unique_lock lock(present_wait_lock);
			present_wait_id = g_currentPresentWaitId.load();

			if (present_wait_id != 0)
			{
				g_device.vk.WaitForPresentKHR( g_device.device(), g_output.swapChain, present_wait_id, 1'000'000'000lu );
				uint64_t vblanktime = get_time_in_nanos();
				GetVBlankTimer().MarkVBlank( vblanktime, true );
				mangoapp_output_update( vblanktime );
			}
		}
	}
}

void vulkan_update_swapchain_hdr_metadata( VulkanOutput_t *pOutput )
{
	if (!g_output.swapchainHDRMetadata)
		return;

	if ( !g_device.vk.SetHdrMetadataEXT )
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			vk_log.errorf("Unable to forward HDR metadata with Vulkan as vkSetMetadataEXT is not supported.");
			s_bWarned = true;
		}
		return;
	}

	const hdr_metadata_infoframe &infoframe = g_output.swapchainHDRMetadata->View<hdr_output_metadata>().hdmi_metadata_type1;
	VkHdrMetadataEXT metadata =
	{
		.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
		.displayPrimaryRed = VkXYColorEXT { color_xy_from_u16(infoframe.display_primaries[0].x), color_xy_from_u16(infoframe.display_primaries[0].y) },
		.displayPrimaryGreen = VkXYColorEXT { color_xy_from_u16(infoframe.display_primaries[1].x), color_xy_from_u16(infoframe.display_primaries[1].y), },
		.displayPrimaryBlue = VkXYColorEXT { color_xy_from_u16(infoframe.display_primaries[2].x), color_xy_from_u16(infoframe.display_primaries[2].y), },
		.whitePoint = VkXYColorEXT { color_xy_from_u16(infoframe.white_point.x), color_xy_from_u16(infoframe.white_point.y), },
		.maxLuminance = nits_from_u16(infoframe.max_display_mastering_luminance),
		.minLuminance = nits_from_u16_dark(infoframe.min_display_mastering_luminance),
		.maxContentLightLevel = nits_from_u16(infoframe.max_cll),
		.maxFrameAverageLightLevel = nits_from_u16(infoframe.max_fall),
	};
	g_device.vk.SetHdrMetadataEXT(g_device.device(), 1, &g_output.swapChain, &metadata);
}

void vulkan_present_to_window( void )
{
	static uint64_t s_lastPresentId = 0;

	uint64_t presentId = ++s_lastPresentId;
	
	auto feedback = steamcompmgr_get_base_layer_swapchain_feedback();
	if (feedback && feedback->hdr_metadata_blob)
	{
		if ( feedback->hdr_metadata_blob != g_output.swapchainHDRMetadata )
		{
			g_output.swapchainHDRMetadata = feedback->hdr_metadata_blob;
			vulkan_update_swapchain_hdr_metadata( &g_output );
		}
	}
	else if ( g_output.swapchainHDRMetadata != nullptr )
	{
		// Only way to clear hdr metadata for a swapchain in Vulkan
		// is to recreate the swapchain.
		g_output.swapchainHDRMetadata = nullptr;
		vulkan_remake_swapchain();
	}


	VkPresentIdKHR presentIdInfo = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
		.swapchainCount = 1,
		.pPresentIds = &presentId,
	};

	VkPresentInfoKHR presentInfo = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.pNext = &presentIdInfo,
		.swapchainCount = 1,
		.pSwapchains = &g_output.swapChain,
		.pImageIndices = &g_output.nOutImage,
	};

	if ( g_device.vk.QueuePresentKHR( g_device.queue(), &presentInfo ) == VK_SUCCESS )
	{
		g_currentPresentWaitId = presentId;
		g_currentPresentWaitId.notify_all();
	}
	else
		vulkan_remake_swapchain();

	while ( !acquire_next_image() )
		vulkan_remake_swapchain();
}

gamescope::Rc<CVulkanTexture> vulkan_create_1d_lut(uint32_t size)
{
	CVulkanTexture::createFlags flags;
	flags.bSampled = true;
	flags.bTransferDst = true;
	flags.imageType = VK_IMAGE_TYPE_1D;

	auto texture = new CVulkanTexture();
	auto drmFormat = VulkanFormatToDRM( VK_FORMAT_R16G16B16A16_UNORM );
	bool bRes = texture->BInit( size, 1u, 1u, drmFormat, flags );
	assert( bRes );

	return texture;
}

gamescope::Rc<CVulkanTexture> vulkan_create_3d_lut(uint32_t width, uint32_t height, uint32_t depth)
{
	CVulkanTexture::createFlags flags;
	flags.bSampled = true;
	flags.bTransferDst = true;
	flags.imageType = VK_IMAGE_TYPE_3D;

	auto texture = new CVulkanTexture();
	auto drmFormat = VulkanFormatToDRM( VK_FORMAT_R16G16B16A16_UNORM );
	bool bRes = texture->BInit( width, height, depth, drmFormat, flags );
	assert( bRes );

	return texture;
}

void vulkan_update_luts(const gamescope::Rc<CVulkanTexture>& lut1d, const gamescope::Rc<CVulkanTexture>& lut3d, void* lut1d_data, void* lut3d_data)
{
	size_t lut1d_size = lut1d->width() * sizeof(uint16_t) * 4;
	size_t lut3d_size = lut3d->width() * lut3d->height() * lut3d->depth() * sizeof(uint16_t) * 4;

	auto [base_dst, base_offset] = g_device.uploadBufferData(lut1d_size + lut3d_size);

	void* lut1d_dst = base_dst;
	void *lut3d_dst = ((uint8_t*)base_dst) + lut1d_size;
	memcpy(lut1d_dst, lut1d_data, lut1d_size);
	memcpy(lut3d_dst, lut3d_data, lut3d_size);

	auto cmdBuffer = g_device.commandBuffer();
	cmdBuffer->copyBufferToImage(g_device.uploadBuffer(), base_offset, 0, lut1d);
	cmdBuffer->copyBufferToImage(g_device.uploadBuffer(), base_offset + lut1d_size, 0, lut3d);
	g_device.submit(std::move(cmdBuffer));
	g_device.waitIdle(); // TODO: Sync this better
}

gamescope::Rc<CVulkanTexture> vulkan_get_hacky_blank_texture()
{
	return g_output.temporaryHackyBlankImage.get();
}

gamescope::OwningRc<CVulkanTexture> vulkan_create_flat_texture( uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b, uint8_t a )
{
	CVulkanTexture::createFlags flags;
	flags.bFlippable = true;
	flags.bSampled = true;
	flags.bTransferDst = true;

	gamescope::OwningRc<CVulkanTexture> texture = new CVulkanTexture();
	bool bRes = texture->BInit( width, height, 1u, VulkanFormatToDRM( VK_FORMAT_B8G8R8A8_UNORM ), flags );
	assert( bRes );

	auto [_dst, offset] = g_device.uploadBufferData( width * height * 4 );
	uint8_t *dst = (uint8_t *)_dst;
	for ( uint32_t i = 0; i < width * height * 4; i += 4 )
	{
		dst[i + 0] = b;
		dst[i + 1] = g;
		dst[i + 2] = r;
		dst[i + 3] = a;
	}

	auto cmdBuffer = g_device.commandBuffer();
	cmdBuffer->copyBufferToImage(g_device.uploadBuffer(), offset, 0, texture.get());
	g_device.submit(std::move(cmdBuffer));
	g_device.waitIdle();

	return texture;
}

gamescope::OwningRc<CVulkanTexture> vulkan_create_debug_blank_texture()
{
	// To match Steam's scaling, which is capped at 1080p
	int width = std::min<int>( g_nOutputWidth, 1920 );
	int height = std::min<int>( g_nOutputHeight, 1080 );

	return vulkan_create_flat_texture( width, height, 0, 0, 0, 0 );
}

bool vulkan_supports_hdr10()
{
	for ( auto& format : g_output.surfaceFormats )
	{
		if ( format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT )
			return true;
	}

	return false;
}

extern bool g_bOutputHDREnabled;

bool vulkan_make_swapchain( VulkanOutput_t *pOutput )
{
	uint32_t imageCount = pOutput->surfaceCaps.minImageCount + 1;
	uint32_t formatCount = pOutput->surfaceFormats.size();
	uint32_t surfaceFormat = formatCount;
	VkColorSpaceKHR preferredColorSpace = g_bOutputHDREnabled ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

	if ( surfaceFormat == formatCount )
	{
		for ( surfaceFormat = 0; surfaceFormat < formatCount; surfaceFormat++ )
		{
			if ( pOutput->surfaceFormats[ surfaceFormat ].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
				 pOutput->surfaceFormats[ surfaceFormat ].colorSpace == preferredColorSpace )
				break;
		}
	}

	if ( surfaceFormat == formatCount )
	{
		for ( surfaceFormat = 0; surfaceFormat < formatCount; surfaceFormat++ )
		{
			if ( pOutput->surfaceFormats[ surfaceFormat ].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32 &&
				 pOutput->surfaceFormats[ surfaceFormat ].colorSpace == preferredColorSpace )
				break;
		}
	}

	if ( surfaceFormat == formatCount )
	{
		for ( surfaceFormat = 0; surfaceFormat < formatCount; surfaceFormat++ )
		{
			if ( pOutput->surfaceFormats[ surfaceFormat ].format == VK_FORMAT_B8G8R8A8_UNORM &&
				 pOutput->surfaceFormats[ surfaceFormat ].colorSpace == preferredColorSpace )
				break;
		}
	}
	
	if ( surfaceFormat == formatCount )
		return false;

	VkFormat eVkFormat = pOutput->surfaceFormats[ surfaceFormat ].format;
	pOutput->uOutputFormat = VulkanFormatToDRM( pOutput->surfaceFormats[ surfaceFormat ].format );
	
	VkFormat formats[2] =
	{
		ToSrgbVulkanFormat( eVkFormat ),
		ToLinearVulkanFormat( eVkFormat ),
	};

	VkImageFormatListCreateInfo usageListInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
		.viewFormatCount = 2,
		.pViewFormats = formats,
	};

	vk_log.infof("Creating Gamescope nested swapchain with format %u and colorspace %u", eVkFormat, pOutput->surfaceFormats[surfaceFormat].colorSpace);

	VkSwapchainCreateInfoKHR createInfo = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.pNext = formats[0] != formats[1] ? &usageListInfo : nullptr,
		.flags = formats[0] != formats[1] ? VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR : (VkSwapchainCreateFlagBitsKHR )0,
		.surface = pOutput->surface,
		.minImageCount = imageCount,
		.imageFormat = eVkFormat,
		.imageColorSpace = pOutput->surfaceFormats[surfaceFormat].colorSpace,
		.imageExtent = {
			.width = g_nOutputWidth,
			.height = g_nOutputHeight,
		},
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = pOutput->surfaceCaps.currentTransform,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR,
		.clipped = VK_TRUE,
	};

	if (g_device.vk.CreateSwapchainKHR( g_device.device(), &createInfo, nullptr, &pOutput->swapChain) != VK_SUCCESS ) {
		return false;
	}

	g_device.vk.GetSwapchainImagesKHR( g_device.device(), pOutput->swapChain, &imageCount, nullptr );
	std::vector<VkImage> swapchainImages( imageCount );
	g_device.vk.GetSwapchainImagesKHR( g_device.device(), pOutput->swapChain, &imageCount, swapchainImages.data() );

	pOutput->outputImages.resize(imageCount);

	for ( uint32_t i = 0; i < pOutput->outputImages.size(); i++ )
	{
		pOutput->outputImages[i] = new CVulkanTexture();

		if ( !pOutput->outputImages[i]->BInitFromSwapchain(swapchainImages[i], g_nOutputWidth, g_nOutputHeight, eVkFormat))
			return false;
	}

	VkFenceCreateInfo fenceInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	};

	g_device.vk.CreateFence( g_device.device(), &fenceInfo, nullptr, &pOutput->acquireFence );

	vulkan_update_swapchain_hdr_metadata(pOutput);

	return true;
}

bool vulkan_remake_swapchain( void )
{
	std::unique_lock lock(present_wait_lock);
	g_currentPresentWaitId = 0;
	g_currentPresentWaitId.notify_all();

	VulkanOutput_t *pOutput = &g_output;
	g_device.waitIdle();
	g_device.vk.QueueWaitIdle( g_device.queue() );

	pOutput->outputImages.clear();

	g_device.vk.DestroySwapchainKHR( g_device.device(), pOutput->swapChain, nullptr );

	// Delete screenshot/capture textures to be remade if needed
	for (auto& pScreenshotTexture : pOutput->pScreenshotTextures)
		pScreenshotTexture = nullptr;
	for (auto& pCaptureTexture : pOutput->pCaptureTextures)
		pCaptureTexture = nullptr;

	bool bRet = vulkan_make_swapchain( pOutput );
	assert( bRet ); // Something has gone horribly wrong!
	return bRet;
}

static bool vulkan_make_output_images( VulkanOutput_t *pOutput )
{
	CVulkanTexture::createFlags outputImageflags;
	outputImageflags.bFlippable = true;
	outputImageflags.bStorage = true;
	outputImageflags.bTransferSrc = true; // for screenshots
	outputImageflags.bSampled = true; // for pipewire blits
	outputImageflags.bOutputImage = true;

	pOutput->outputImages.resize(3); // extra image for partial composition.
	pOutput->outputImagesPartialOverlay.resize(3);

	pOutput->outputImages[0] = nullptr;
	pOutput->outputImages[1] = nullptr;
	pOutput->outputImages[2] = nullptr;
	pOutput->outputImagesPartialOverlay[0] = nullptr;
	pOutput->outputImagesPartialOverlay[1] = nullptr;
	pOutput->outputImagesPartialOverlay[2] = nullptr;

	uint32_t uDRMFormat = pOutput->uOutputFormat;

	// Output images are physical-oriented; the panel scans out unrotated.
	uint32_t uOutputWidth = g_nOutputWidth;
	uint32_t uOutputHeight = g_nOutputHeight;
	if ( g_uOutputRotation & 1u )
		std::swap( uOutputWidth, uOutputHeight );

	pOutput->outputImages[0] = new CVulkanTexture();
	bool bSuccess = pOutput->outputImages[0]->BInit( uOutputWidth, uOutputHeight, 1u, uDRMFormat, outputImageflags );
	if ( bSuccess != true )
	{
		vk_log.errorf( "failed to allocate buffer for KMS" );
		return false;
	}

	pOutput->outputImages[1] = new CVulkanTexture();
	bSuccess = pOutput->outputImages[1]->BInit( uOutputWidth, uOutputHeight, 1u, uDRMFormat, outputImageflags );
	if ( bSuccess != true )
	{
		vk_log.errorf( "failed to allocate buffer for KMS" );
		return false;
	}

	pOutput->outputImages[2] = new CVulkanTexture();
	bSuccess = pOutput->outputImages[2]->BInit( uOutputWidth, uOutputHeight, 1u, uDRMFormat, outputImageflags );
	if ( bSuccess != true )
	{
		vk_log.errorf( "failed to allocate buffer for KMS" );
		return false;
	}

	// Oh no.
	pOutput->temporaryHackyBlankImage = vulkan_create_debug_blank_texture();

	if ( pOutput->uOutputFormatOverlay != VK_FORMAT_UNDEFINED && !kDisablePartialComposition )
	{
		uint32_t uPartialDRMFormat = pOutput->uOutputFormatOverlay;

		pOutput->outputImagesPartialOverlay[0] = new CVulkanTexture();
		bool bSuccess = pOutput->outputImagesPartialOverlay[0]->BInit( uOutputWidth, uOutputHeight, 1u, uPartialDRMFormat, outputImageflags, nullptr, 0, 0, pOutput->outputImages[0].get() );
		if ( bSuccess != true )
		{
			vk_log.errorf( "failed to allocate buffer for KMS" );
			return false;
		}

		pOutput->outputImagesPartialOverlay[1] = new CVulkanTexture();
		bSuccess = pOutput->outputImagesPartialOverlay[1]->BInit( uOutputWidth, uOutputHeight, 1u, uPartialDRMFormat, outputImageflags, nullptr, 0, 0, pOutput->outputImages[1].get() );
		if ( bSuccess != true )
		{
			vk_log.errorf( "failed to allocate buffer for KMS" );
			return false;
		}

		pOutput->outputImagesPartialOverlay[2] = new CVulkanTexture();
		bSuccess = pOutput->outputImagesPartialOverlay[2]->BInit( uOutputWidth, uOutputHeight, 1u, uPartialDRMFormat, outputImageflags, nullptr, 0, 0, pOutput->outputImages[2].get() );
		if ( bSuccess != true )
		{
			vk_log.errorf( "failed to allocate buffer for KMS" );
			return false;
		}
	}

	return true;
}

bool vulkan_remake_output_images()
{
	VulkanOutput_t *pOutput = &g_output;
	g_device.waitIdle();

	pOutput->nOutImage = 0;

	// Delete screenshot/capture textures to be remade if needed
	for (auto& pScreenshotTexture : pOutput->pScreenshotTextures)
		pScreenshotTexture = nullptr;
	for (auto& pCaptureTexture : pOutput->pCaptureTextures)
		pCaptureTexture = nullptr;

	bool bRet = vulkan_make_output_images( pOutput );
	assert( bRet );
	return bRet;
}

bool vulkan_make_output()
{
	VulkanOutput_t *pOutput = &g_output;

	VkResult result;
	
	if ( GetBackend()->UsesVulkanSwapchain() )
	{
		result = g_device.vk.GetPhysicalDeviceSurfaceCapabilitiesKHR( g_device.physDev(), pOutput->surface, &pOutput->surfaceCaps );
		if ( result != VK_SUCCESS )
		{
			vk_errorf( result, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed" );
			return false;
		}
		
		uint32_t formatCount = 0;
		result = g_device.vk.GetPhysicalDeviceSurfaceFormatsKHR( g_device.physDev(), pOutput->surface, &formatCount, nullptr );
		if ( result != VK_SUCCESS )
		{
			vk_errorf( result, "vkGetPhysicalDeviceSurfaceFormatsKHR failed" );
			return false;
		}
		
		if ( formatCount != 0 ) {
			pOutput->surfaceFormats.resize( formatCount );
			g_device.vk.GetPhysicalDeviceSurfaceFormatsKHR( g_device.physDev(), pOutput->surface, &formatCount, pOutput->surfaceFormats.data() );
			if ( result != VK_SUCCESS )
			{
				vk_errorf( result, "vkGetPhysicalDeviceSurfaceFormatsKHR failed" );
				return false;
			}
		}
		
		uint32_t presentModeCount = false;
		result = g_device.vk.GetPhysicalDeviceSurfacePresentModesKHR(g_device.physDev(), pOutput->surface, &presentModeCount, nullptr );
		if ( result != VK_SUCCESS )
		{
			vk_errorf( result, "vkGetPhysicalDeviceSurfacePresentModesKHR failed" );
			return false;
		}
		
		if ( presentModeCount != 0 ) {
			pOutput->presentModes.resize(presentModeCount);
			result = g_device.vk.GetPhysicalDeviceSurfacePresentModesKHR( g_device.physDev(), pOutput->surface, &presentModeCount, pOutput->presentModes.data() );
			if ( result != VK_SUCCESS )
			{
				vk_errorf( result, "vkGetPhysicalDeviceSurfacePresentModesKHR failed" );
				return false;
			}
		}
		
		if ( !vulkan_make_swapchain( pOutput ) )
			return false;

		while ( !acquire_next_image() )
			vulkan_remake_swapchain();
	}
	else
	{
		GetBackend()->GetPreferredOutputFormat( &pOutput->uOutputFormat, &pOutput->uOutputFormatOverlay );

		if ( pOutput->uOutputFormat == DRM_FORMAT_INVALID )
		{
			vk_log.errorf( "failed to find Vulkan format suitable for KMS" );
			return false;
		}

		if ( pOutput->uOutputFormatOverlay == DRM_FORMAT_INVALID )
		{
			vk_log.errorf( "failed to find Vulkan format suitable for KMS partial overlays" );
			return false;
		}

		if ( !vulkan_make_output_images( pOutput ) )
			return false;
	}

	return true;
}

static void update_tmp_images( uint32_t width, uint32_t height )
{
	if ( g_output.tmpOutput != nullptr
			&& width == g_output.tmpOutput->width()
			&& height == g_output.tmpOutput->height() )
	{
		return;
	}

	CVulkanTexture::createFlags createFlags;
	createFlags.bSampled = true;
	createFlags.bStorage = true;

	g_output.tmpOutput = new CVulkanTexture();
	bool bSuccess = g_output.tmpOutput->BInit( width, height, 1u, DRM_FORMAT_ARGB8888, createFlags, nullptr );

	if ( !bSuccess )
	{
		vk_log.errorf( "failed to create fsr output" );
		return;
	}
}

// Storage format for the native effects pre-pass output: the base layer's own
// DRM format when the device can use it as an optimal-tiled storage+sampled
// image (keeps a 10-bit game at 10 bits), else ARGB8888. Why the check: 10-bit
// storage images are an optional Vulkan feature -- see
// vulkan_get_rgb10_capture_format()'s note about NVIDIA -- and the answer is
// per-format and constant for the process, so it is cached per format.
static uint32_t effects_storage_format( uint32_t uInputDrmFormat )
{
	static std::unordered_map<uint32_t, uint32_t> s_cache;
	auto it = s_cache.find( uInputDrmFormat );
	if ( it != s_cache.end() )
		return it->second;

	uint32_t uResult = DRM_FORMAT_ARGB8888;
	VkFormat vkFormat = DRMFormatToVulkan( uInputDrmFormat, false );
	if ( vkFormat != VK_FORMAT_UNDEFINED && uInputDrmFormat != DRM_FORMAT_ARGB8888 )
	{
		VkFormatProperties props;
		g_device.vk.GetPhysicalDeviceFormatProperties( g_device.physDev(), vkFormat, &props );
		constexpr VkFormatFeatureFlags uNeeded = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		if ( ( props.optimalTilingFeatures & uNeeded ) == uNeeded )
			uResult = uInputDrmFormat;
	}
	s_cache[ uInputDrmFormat ] = uResult;
	return uResult;
}

// Pooled output for the native effects pre-pass, same shape as
// update_tmp_images() above: re-created only when the base layer's source
// size or format changes.
static bool update_effects_image( uint32_t width, uint32_t height, uint32_t uInputDrmFormat )
{
	const uint32_t uFormat = effects_storage_format( uInputDrmFormat );

	if ( g_output.effectsOutput != nullptr
			&& width == g_output.effectsOutput->width()
			&& height == g_output.effectsOutput->height()
			&& uFormat == g_output.effectsOutput->drmFormat() )
	{
		return true;
	}

	CVulkanTexture::createFlags createFlags;
	createFlags.bSampled = true;
	createFlags.bStorage = true;

	g_output.effectsOutput = new CVulkanTexture();
	if ( !g_output.effectsOutput->BInit( width, height, 1u, uFormat, createFlags, nullptr ) )
	{
		vk_log.errorf( "failed to create native effects output" );
		g_output.effectsOutput = nullptr;
		return false;
	}
	return true;
}

// Adaptive Brightness's persistent 1x1 history (see the field's comment in
// rendervulkan.hpp). Created once and kept: unlike effectsOutput it does not
// depend on the game's size or format, and its contents ARE the effect's
// cross-frame state. Sets bCreated when this call made the texture, so the
// caller can discard its undefined contents and tell the shader to start from
// the measurement (EFFECT_RESET_HISTORY) instead of blending with garbage.
//
// Format: ABGR8888 (VK_FORMAT_R8G8B8A8_UNORM), whose storage-image support is
// mandatory in Vulkan, holding one float spread over the four channels --
// effects_common.h's history_pack() explains the encoding and why it is
// lossless. Why not R32F: `dst` in descriptor_set.h is rgba8 and
// CVulkanCmdBuffer::dispatch() binds a single RGB target there; an r32f
// second-target path through the shared descriptor set for this one texel
// was judged more plumbing than four exact byte lanes.
static bool update_effects_history( bool &bCreated )
{
	bCreated = false;
	if ( g_output.effectsHistory != nullptr )
		return true;

	CVulkanTexture::createFlags createFlags;
	createFlags.bSampled = true;
	createFlags.bStorage = true;

	g_output.effectsHistory = new CVulkanTexture();
	if ( !g_output.effectsHistory->BInit( 1u, 1u, 1u, DRM_FORMAT_ABGR8888, createFlags, nullptr ) )
	{
		vk_log.errorf( "failed to create native effects history" );
		g_output.effectsHistory = nullptr;
		return false;
	}
	bCreated = true;
	return true;
}


static bool init_nis_data()
{
	// Create the NIS images
	// Select between the FP16 or FP32 coefficients

	void* coefScaleData = g_device.supportsFp16() ? (void*) coef_scale_fp16 : (void*) coef_scale;

	void* coefUsmData = g_device.supportsFp16() ? (void*) coef_usm_fp16 : (void*) coef_usm;

	uint32_t nisFormat = g_device.supportsFp16() ? DRM_FORMAT_ABGR16161616F : DRM_FORMAT_ABGR32323232F;

	uint32_t width = kFilterSize / 4;
	uint32_t height = kPhaseCount;

	g_output.nisScalerImage = vulkan_create_texture_from_bits( width, height, width, height, nisFormat, {}, coefScaleData );
	g_output.nisUsmImage = vulkan_create_texture_from_bits( width, height, width, height, nisFormat, {}, coefUsmData );

	return true;
}

VkInstance vulkan_get_instance( void )
{
	static VkInstance s_pVkInstance = []() -> VkInstance
	{
		VkResult result = VK_ERROR_INITIALIZATION_FAILED;

		if ( ( result = vulkan_load_module() ) != VK_SUCCESS )
		{
			vk_errorf( result, "Failed to load vulkan module." );
			return nullptr;
		}

		auto instanceExtensions = GetBackend()->GetInstanceExtensions();

		const VkApplicationInfo appInfo = {
			.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
			.pApplicationName   = "gamescope",
			.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
			.pEngineName        = "hopefully not just some code",
			.engineVersion      = VK_MAKE_VERSION(1, 0, 0),
			.apiVersion         = VK_API_VERSION_1_3,
		};

		const VkInstanceCreateInfo createInfo = {
			.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			.pApplicationInfo        = &appInfo,
			.enabledExtensionCount   = (uint32_t)instanceExtensions.size(),
			.ppEnabledExtensionNames = instanceExtensions.data(),
		};

		VkInstance instance = nullptr;
		result = g_pfn_vkCreateInstance(&createInfo, 0, &instance);
		if ( result != VK_SUCCESS )
		{
			vk_errorf( result, "vkCreateInstance failed" );
		}

		return instance;
	}();

	return s_pVkInstance;
}

bool vulkan_init( VkInstance instance, VkSurfaceKHR surface )
{
	static bool s_bInitted = false;
	if ( s_bInitted )
	{
		g_output.surface = surface;
		return true;
	}

	if (!g_device.BInit(instance, surface))
		return false;

	if (!init_nis_data())
		return false;

	if ( GetBackend()->UsesVulkanSwapchain() )
	{
		std::thread present_wait_thread( present_wait_thread_func );
		present_wait_thread.detach();
	}

	s_bInitted = true;

	return true;
}

gamescope::OwningRc<CVulkanTexture> vulkan_create_texture_from_dmabuf( struct wlr_dmabuf_attributes *pDMA, gamescope::OwningRc<gamescope::IBackendFb> pBackendFb )
{
	gamescope::OwningRc<CVulkanTexture> pTex = new CVulkanTexture();

	CVulkanTexture::createFlags texCreateFlags;
	texCreateFlags.bSampled = true;

	//fprintf(stderr, "pDMA->width: %d pDMA->height: %d pDMA->format: 0x%x pDMA->modifier: 0x%lx pDMA->n_planes: %d\n",
	//	pDMA->width, pDMA->height, pDMA->format, pDMA->modifier, pDMA->n_planes);
	
	if ( pTex->BInit( pDMA->width, pDMA->height, 1u, pDMA->format, texCreateFlags, pDMA, 0, 0, nullptr, pBackendFb ) == false )
		return nullptr;
	
	return pTex;
}

gamescope::OwningRc<CVulkanTexture> vulkan_create_texture_from_bits( uint32_t width, uint32_t height, uint32_t contentWidth, uint32_t contentHeight, uint32_t drmFormat, CVulkanTexture::createFlags texCreateFlags, void *bits )
{
	gamescope::OwningRc<CVulkanTexture> pTex = new CVulkanTexture();

	texCreateFlags.bSampled = true;
	texCreateFlags.bTransferDst = true;

	if ( pTex->BInit( width, height, 1u, drmFormat, texCreateFlags, nullptr,  contentWidth, contentHeight) == false )
		return nullptr;

	size_t size = width * height * DRMFormatGetBPP(drmFormat);
	auto [ dst, offset ] = g_device.uploadBufferData(size);
	memcpy( dst, bits, size );

	auto cmdBuffer = g_device.commandBuffer();

	cmdBuffer->copyBufferToImage(g_device.uploadBuffer(), offset, 0, pTex.get());
	// TODO: Sync this copyBufferToImage.

	g_device.submit(std::move(cmdBuffer));
	g_device.waitIdle();

	return pTex;
}

static uint32_t s_frameId = 0;

void vulkan_garbage_collect( void )
{
	g_device.garbageCollect();
}

static gamescope::Rc<CVulkanTexture> acquire_pooled_texture( auto& pool, uint32_t width, uint32_t height, bool exportable, uint32_t drmFormat, EStreamColorspace colorspace )
{
	for (auto& pTexture : pool)
	{
		// Evict a stale texture and reuse the slot
		if (pTexture && pTexture->GetRefCount() == 0 &&
			(width != pTexture->width() ||
			 height != pTexture->height() ||
			 drmFormat != pTexture->drmFormat()))
		{
			pTexture = nullptr;
		}

		if (pTexture == nullptr)
		{
			pTexture = new CVulkanTexture();

			CVulkanTexture::createFlags textureFlags;
			textureFlags.bMappable = true;
			textureFlags.bTransferDst = true;
			textureFlags.bStorage = true;
			textureFlags.bSampled = true; // required for RGB-to-NV12 shader to sample this texture
			if (exportable || drmFormat == DRM_FORMAT_NV12) {
				textureFlags.bExportable = true;
				textureFlags.bLinear = true; // TODO: support multi-planar DMA-BUF export via PipeWire
			}

			bool bSuccess = pTexture->BInit( width, height, 1u, drmFormat, textureFlags );
			pTexture->setStreamColorspace(colorspace);

			assert( bSuccess );
		}

		if (pTexture->GetRefCount() != 0 ||
			width != pTexture->width() ||
			height != pTexture->height() ||
			drmFormat != pTexture->drmFormat())
			continue;

		return pTexture.get();
	}

	return nullptr;
}

gamescope::Rc<CVulkanTexture> vulkan_acquire_screenshot_texture(uint32_t width, uint32_t height, bool exportable, uint32_t drmFormat, EStreamColorspace colorspace)
{
	auto texture = acquire_pooled_texture(g_output.pScreenshotTextures, width, height, exportable, drmFormat, colorspace);
	if (!texture)
		vk_log.errorf("Unable to acquire screenshot texture. Out of textures.");
	return texture;
}

gamescope::Rc<CVulkanTexture> vulkan_acquire_capture_texture(uint32_t width, uint32_t height, bool exportable, uint32_t drmFormat, EStreamColorspace colorspace)
{
	auto texture = acquire_pooled_texture(g_output.pCaptureTextures, width, height, exportable, drmFormat, colorspace);
	if (!texture)
		vk_log.errorf("Unable to acquire capture texture. Out of textures.");
	return texture;
}

// XRGB2101010 storage images are an optional Vulkan feature that NVIDIA hardware
// doesn't support, and imageStore lands in XBGR order there, swapping R/B.
uint32_t vulkan_get_rgb10_capture_format( void )
{
	static uint32_t s_uFormat = []()
	{
		// Pooled capture textures are mappable, hence linear-tiled.
		VkFormatProperties props;
		g_device.vk.GetPhysicalDeviceFormatProperties( g_device.physDev(), VK_FORMAT_A2R10G10B10_UNORM_PACK32, &props );
		constexpr VkFormatFeatureFlags uNeeded = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		if ( ( props.linearTilingFeatures & uNeeded ) == uNeeded )
			return DRM_FORMAT_XRGB2101010;

		vk_log.infof( "XRGB2101010 lacks linear storage/sampled support, capturing as XBGR2101010" );
		return DRM_FORMAT_XBGR2101010;
	}();
	return s_uFormat;
}

// Internal display's native brightness.
float g_flInternalDisplayBrightnessNits = 500.0f;

float g_flHDRItmSdrNits = 100.f;
float g_flHDRItmTargetNits = 1000.f;

#pragma pack(push, 1)
struct BlitPushData_t
{
	vec2_t scale[k_nMaxLayers];
	vec2_t offset[k_nMaxLayers];
	float opacity[k_nMaxLayers];
	glm::mat3x4 ctm[k_nMaxLayers];
	uint32_t borderMask;
	uint32_t frameId;
	uint32_t blurRadius;

	uint32_t u_shaderFilter;
	uint32_t u_alphaMode;

    float u_linearToNits; // unset
    float u_nitsToLinear; // unset
    float u_itmSdrNits; // unset
    float u_itmTargetNits; // unset

	uint32_t u_rotation;

	explicit BlitPushData_t(const struct FrameInfo_t *frameInfo, uint32_t rotation = 0)
	{
		u_shaderFilter = 0;
		u_alphaMode = 0;
		u_rotation = rotation;

		for (int i = 0; i < frameInfo->layers.count(); i++) {
			const FrameInfo_t::Layer_t *layer = &frameInfo->layers.get( i );
			scale[i] = layer->scale;
			offset[i] = layer->offsetPixelCenter();
			opacity[i] = layer->opacity;
            if (layer->isScreenSize() || (layer->filter == GamescopeUpscaleFilter::LINEAR && layer->viewConvertsToLinearAutomatically()))
                u_shaderFilter |= ((uint32_t)GamescopeUpscaleFilter::FROM_VIEW) << (i * 4);
            else
                u_shaderFilter |= ((uint32_t)layer->filter) << (i * 4);

			u_alphaMode |= ((uint32_t)layer->eAlphaBlendingMode) << ( i * 4 );

			if (layer->ctm)
			{
				ctm[i] = layer->ctm->View<glm::mat3x4>();
			}
			else
			{
				ctm[i] = glm::mat3x4
				{
					1, 0, 0, 0,
					0, 1, 0, 0,
					0, 0, 1, 0
				};
			}
		}

		borderMask = frameInfo->borderMask();
		frameId = s_frameId++;
		blurRadius = frameInfo->blurRadius ? ( frameInfo->blurRadius * 2 ) - 1 : 0;

		u_linearToNits = g_flInternalDisplayBrightnessNits;
		u_nitsToLinear = 1.0f / g_flInternalDisplayBrightnessNits;
		u_itmSdrNits = g_flHDRItmSdrNits;
		u_itmTargetNits = g_flHDRItmTargetNits;
	}

	explicit BlitPushData_t(float blit_scale) {
		scale[0] = { blit_scale, blit_scale };
		offset[0] = { 0.5f, 0.5f };
		opacity[0] = 1.0f;
        u_shaderFilter = (uint32_t)GamescopeUpscaleFilter::LINEAR;
		u_alphaMode = 0;
		u_rotation = 0;
		ctm[0] = glm::mat3x4
		{
			1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0
		};
		borderMask = 0;
		frameId = s_frameId;

		u_linearToNits = g_flInternalDisplayBrightnessNits;
		u_nitsToLinear = 1.0f / g_flInternalDisplayBrightnessNits;
		u_itmSdrNits = g_flHDRItmSdrNits;
		u_itmTargetNits = g_flHDRItmTargetNits;
	}
};

struct CaptureConvertBlitData_t
{
	vec2_t scale[1];
	vec2_t offset[1];
	float opacity[1];
	glm::mat3x4 ctm[1];
	mat3x4 outputCTM;
	uint32_t borderMask;
	uint32_t halfExtent[2];

	explicit CaptureConvertBlitData_t(float blit_scale, const mat3x4 &color_matrix) {
		scale[0] = { blit_scale, blit_scale };
		offset[0] = { 0.0f, 0.0f };
		opacity[0] = 1.0f;
		borderMask = 0;
		ctm[0] = glm::mat3x4
		{
			1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0
		};
		outputCTM = color_matrix;
	}
};

struct uvec4_t
{
	uint32_t  x;
	uint32_t  y;
	uint32_t  z;
	uint32_t  w;
};
struct uvec2_t
{
	uint32_t x;
	uint32_t y;
};

struct EasuPushData_t
{
	uvec4_t Const0;
	uvec4_t Const1;
	uvec4_t Const2;
	uvec4_t Const3;

	EasuPushData_t(uint32_t inputX, uint32_t inputY, uint32_t tempX, uint32_t tempY)
	{
		FsrEasuCon(&Const0.x, &Const1.x, &Const2.x, &Const3.x, inputX, inputY, inputX, inputY, tempX, tempY);
	}
};

// Uniform block for cs_effects_layer0.comp and cs_effects_measure.comp --
// mirrors effects_common.h's `effects_t` block field-for-field. One upload
// serves both dispatches of a frame (they read the same values).
struct EffectsPushData_t
{
	// Bit assignments are the contract with the shader's EFFECT_* consts.
	static constexpr uint32_t kShadowLift        = 1u << 0;
	static constexpr uint32_t kVibrancy          = 1u << 1;
	static constexpr uint32_t kVibrancySkin      = 1u << 2;
	static constexpr uint32_t kPreSharpen        = 1u << 3;
	static constexpr uint32_t kAdaptiveBrightness = 1u << 4;
	// The history texture was created this frame: the measure pass writes
	// the measurement straight in rather than blending with undefined bits.
	static constexpr uint32_t kResetHistory      = 1u << 31;

	uint32_t u_flags;
	float    u_vibrancy;
	float    u_shadowLift;
	uint32_t u_rcasCon;

	float    u_abTarget;
	float    u_abUp;
	float    u_abDown;
	float    u_abMin;
	float    u_abMax;
	float    u_abStrength;
	float    u_abDt;

	// Pre-Sharpen slider (0..2, 0.5 default) -> RCAS con.x. RCAS scales its
	// clip-limited lobe by con.x in 0..1 (FsrRcasCon() derives it as
	// exp2(-sharpness_in_stops)); mapping k -> k / (0.75 * (1 + k)) keeps
	// today's slider feel: 0 -> 0 (off), 0.5 -> 0.444, 1 -> 0.667, 2 -> 0.889,
	// monotonic and saturating so the top of the slider is "as sharp as RCAS
	// goes" rather than a cliff. Passed as float bits, exactly like
	// RcasPushData_t::u_c1 (RCAS reads con.x with AF1_AU1).
	static float RcasConX( float flStrength )
	{
		const float k = std::clamp( flStrength, 0.0f, 2.0f );
		return std::clamp( k / ( 0.75f * ( 1.0f + k ) ), 0.0f, 1.0f );
	}

	EffectsPushData_t( const NativeEffectsState_t &s, float flAbDtSeconds, bool bResetHistory )
	{
		u_flags = 0;
		if ( s.bShadowLift )          u_flags |= kShadowLift;
		if ( s.bVibrancy )            u_flags |= kVibrancy;
		if ( s.bVibrancyProtectSkin ) u_flags |= kVibrancySkin;
		if ( s.bPreSharpen )          u_flags |= kPreSharpen;
		if ( s.bAdaptiveBrightness )  u_flags |= kAdaptiveBrightness;
		if ( bResetHistory )          u_flags |= kResetHistory;

		u_vibrancy   = s.flVibrancy;
		u_shadowLift = s.flShadowLift;
		const float flCon = s.bPreSharpen ? RcasConX( s.flPreSharpen ) : 0.0f;
		u_rcasCon = std::bit_cast<uint32_t>( flCon );

		u_abTarget   = s.flAbTarget;
		u_abUp       = s.flAbUpSpeed;
		u_abDown     = s.flAbDownSpeed;
		u_abMin      = s.flAbMinGain;
		u_abMax      = s.flAbMaxGain;
		u_abStrength = s.flAbStrength;
		u_abDt       = flAbDtSeconds;
	}
};

struct RcasPushData_t
{
	uvec2_t u_layer0Offset;
	vec2_t u_scale[k_nMaxLayers - 1];
	vec2_t u_offset[k_nMaxLayers - 1];
	float u_opacity[k_nMaxLayers];
	glm::mat3x4 ctm[k_nMaxLayers];
	uint32_t u_borderMask;
	uint32_t u_frameId;
	uint32_t u_c1;

	uint32_t u_shaderFilter;
	uint32_t u_alphaMode;

    float u_linearToNits; // unset
    float u_nitsToLinear; // unset
    float u_itmSdrNits; // unset
    float u_itmTargetNits; // unset

	uint32_t u_rotation;

	RcasPushData_t(const struct FrameInfo_t *frameInfo, float sharpness, uint32_t rotation = 0)
	{
		uvec4_t tmp;
		FsrRcasCon(&tmp.x, sharpness);
		u_rotation = rotation;
		u_layer0Offset.x = uint32_t(int32_t(frameInfo->layers.get( 0 ).offset.x));
		u_layer0Offset.y = uint32_t(int32_t(frameInfo->layers.get( 0 ).offset.y));
		u_borderMask = frameInfo->borderMask() >> 1u;
		u_frameId = s_frameId++;
		u_c1 = tmp.x;
		u_shaderFilter = 0;
		u_alphaMode = 0;

		for (int i = 0; i < frameInfo->layers.count(); i++)
		{
			const FrameInfo_t::Layer_t *layer = &frameInfo->layers.get( i );

            if (i == 0 || layer->isScreenSize() || (layer->filter == GamescopeUpscaleFilter::LINEAR && layer->viewConvertsToLinearAutomatically()))
                u_shaderFilter |= ((uint32_t)GamescopeUpscaleFilter::FROM_VIEW) << (i * 4);
            else
                u_shaderFilter |= ((uint32_t)layer->filter) << (i * 4);

			u_alphaMode |= ((uint32_t)layer->eAlphaBlendingMode) << ( i * 4 );

			if (layer->ctm)
			{
				ctm[i] = layer->ctm->View<glm::mat3x4>();
			}
			else
			{
				ctm[i] = glm::mat3x4
				{
					1, 0, 0, 0,
					0, 1, 0, 0,
					0, 0, 1, 0
				};
			}

			u_opacity[i] = frameInfo->layers.get( i ).opacity;
		}

		u_linearToNits = g_flInternalDisplayBrightnessNits;
		u_nitsToLinear = 1.0f / g_flInternalDisplayBrightnessNits;
		u_itmSdrNits = g_flHDRItmSdrNits;
		u_itmTargetNits = g_flHDRItmTargetNits;

		for (int i = 1; i < frameInfo->layers.count(); i++)
		{
			u_scale[i - 1] = frameInfo->layers.get( i ).scale;
			u_offset[i - 1] = frameInfo->layers.get( i ).offsetPixelCenter();
		}
	}
};

struct NisPushData_t
{
	NISConfig nisConfig;

	NisPushData_t(uint32_t inputX, uint32_t inputY, uint32_t tempX, uint32_t tempY, float sharpness)
	{
		NVScalerUpdateConfig(
			nisConfig, sharpness,
			0, 0,
			inputX, inputY,
			inputX, inputY,
			0, 0,
			tempX, tempY,
			tempX, tempY);
	}
};
#pragma pack(pop)

void bind_all_layers(CVulkanCmdBuffer* cmdBuffer, const struct FrameInfo_t *frameInfo)
{
	for ( int i = 0; i < frameInfo->layers.count(); i++ )
	{
		const FrameInfo_t::Layer_t *layer = &frameInfo->layers.get( i );

		bool nearest = layer->isScreenSize()
                    || layer->filter == GamescopeUpscaleFilter::NEAREST
                    || (layer->filter == GamescopeUpscaleFilter::LINEAR && !layer->viewConvertsToLinearAutomatically());

		cmdBuffer->bindTexture(i, layer->tex);
		cmdBuffer->setTextureSrgb(i, layer->colorspace != GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR);
		cmdBuffer->setSamplerNearest(i, nearest);
		cmdBuffer->setSamplerUnnormalized(i, true);
	}
	for (uint32_t i = frameInfo->layers.count(); i < VKR_SAMPLER_SLOTS; i++)
	{
		cmdBuffer->bindTexture(i, nullptr);
	}
}

std::optional<uint64_t> vulkan_screenshot( const struct FrameInfo_t *frameInfo, gamescope::Rc<CVulkanTexture> pScreenshotTexture, gamescope::Rc<CVulkanTexture> pYUVOutTexture )
{
	EOTF outputTF = frameInfo->outputEncodingEOTF;
	if (!frameInfo->applyOutputColorMgmt)
		outputTF = EOTF_Count; //Disable blending stuff.

	auto cmdBuffer = g_device.commandBuffer();

	// Same cross-queue sync point as vulkan_composite() -- a screenshot taken
	// while the settings overlay is up is a second consumer of its texture,
	// on its own separate compute submission.
	gamescope::SettingsOverlay_WaitForRender( cmdBuffer.get() );
	gamescope::FpsDisplay_WaitForRender( cmdBuffer.get() );
	gamescope::Notifications::WaitForRender( cmdBuffer.get() );

	for (uint32_t i = 0; i < EOTF_Count; i++)
		cmdBuffer->bindColorMgmtLuts(i, frameInfo->shaperLut[i], frameInfo->lut3D[i]);

	cmdBuffer->bindPipeline( g_device.pipeline(SHADER_TYPE_BLIT, frameInfo->layers.count(), frameInfo->ycbcrMask(), 0u, frameInfo->colorspaceMask(), outputTF ));
	bind_all_layers(cmdBuffer.get(), frameInfo);
	cmdBuffer->bindTarget(pScreenshotTexture);
	cmdBuffer->uploadConstants<BlitPushData_t>(frameInfo);

	const int pixelsPerGroup = 8;

	cmdBuffer->dispatch(div_roundup(currentOutputWidth, pixelsPerGroup), div_roundup(currentOutputHeight, pixelsPerGroup));

	if ( pYUVOutTexture != nullptr )
	{
		float scale = (float)pScreenshotTexture->width() / pYUVOutTexture->width();

		CaptureConvertBlitData_t constants( scale, colorspace_to_conversion_from_srgb_matrix( pYUVOutTexture->streamColorspace() ) );
		constants.halfExtent[0] = pYUVOutTexture->width() / 2.0f;
		constants.halfExtent[1] = pYUVOutTexture->height() / 2.0f;
		cmdBuffer->uploadConstants<CaptureConvertBlitData_t>(constants);

		for (uint32_t i = 0; i < EOTF_Count; i++)
			cmdBuffer->bindColorMgmtLuts(i, nullptr, nullptr);

		cmdBuffer->bindPipeline(g_device.pipeline( SHADER_TYPE_RGB_TO_NV12, 1, 0, 0, GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB, EOTF_Count ));
		cmdBuffer->bindTexture(0, pScreenshotTexture);
		cmdBuffer->setTextureSrgb(0, true);
		cmdBuffer->setSamplerNearest(0, false);
		cmdBuffer->setSamplerUnnormalized(0, true);
		for (uint32_t i = 1; i < VKR_SAMPLER_SLOTS; i++)
		{
			cmdBuffer->bindTexture(i, nullptr);
		}
		cmdBuffer->bindTarget(pYUVOutTexture);

		const int pixelsPerGroup = 8;

		// For ycbcr, we operate on 2 pixels at a time, so use the half-extent.
		const int dispatchSize = pixelsPerGroup * 2;

		cmdBuffer->dispatch(div_roundup(pYUVOutTexture->width(), dispatchSize), div_roundup(pYUVOutTexture->height(), dispatchSize));
	}

	uint64_t sequence = g_device.submit(std::move(cmdBuffer));
	return sequence;
}

extern std::string g_reshade_effect;
extern uint32_t g_reshade_technique_idx;

ReshadeEffectPipeline *g_pLastReshadeEffect = nullptr;

// M6 (superdoc/planning/SPEC.md Feature 2, DECISIONS.md #15): the Shaders panel
// greys out ReShade effect controls -- with an explanation -- whenever the
// base/game layer isn't SDR (LINEAR/SRGB), rather than silently applying
// effect math that assumes clamped 0..1 SDR RGB to scRGB/HDR10_PQ content.
// Stashed here, on the same per-frame cadence this function already reads the
// base layer's colourspace at, independent of whether a ReShade effect is
// actually loaded (g_reshade_effect can be empty while this still needs to be
// current, so the panel knows what it *would* apply to). Read from
// PanelShaders.cpp on the same (steamcompmgr) thread -- relaxed atomic is the
// same pattern FpsDisplay.cpp's g_ulLastAppFrametimeNs (src/commit.cpp) uses
// for the same single-writer-thread, cross-file-read shape.
std::atomic<GamescopeAppTextureColorspace> g_eLastBaseLayerColorspace{ GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB };

NativeEffectsState_t g_nativeEffects;

bool vulkan_native_effects_active()
{
	return g_nativeEffects.AnyEnabled();
}

std::optional<uint64_t> vulkan_composite( const struct FrameInfo_t *pCallerFrameInfo, gamescope::Rc<CVulkanTexture> pPipewireTexture, bool partial, gamescope::Rc<CVulkanTexture> pOutputOverride, bool increment, std::unique_ptr<CVulkanCmdBuffer> pInCommandBuffer )
{
	// THE CALLER'S FrameInfo_t IS NEVER WRITTEN. The two layer-0 effect
	// passes below (ReShade, then the native pre-pass) hand the rest of this
	// function a processed texture in place of layer 0 -- but through a
	// private copy of the struct, never by swapping the caller's layer 0.
	//
	// Why: this function runs more than once on one frame's struct. The
	// backend's Present() composites paint_all()'s frameInfo (SDL/Wayland/
	// OpenVR pass the pointer straight through; DRM composites a copy), and
	// `gamescopectl screenshot` types 3 and 4 then composite that very struct
	// again into a mappable texture (steamcompmgr.cpp's screenshot block).
	// The previous design swapped layer 0 for the effects output in place and
	// set bBaseLayerEffectsApplied on the struct so the second call would skip
	// the pass -- which made every later composite depend on the FIRST
	// call's side effects: which struct it ran on (DRM's copy vs paint_all's
	// own), whether the flag travelled with the layer it described, and
	// whether the pooled texture layer 0 now pointed at still held THIS
	// frame. Measured live: the screenshot showed the effects twice under
	// the .fx (no flag) and not at all after the flag was added, while the
	// screen showed them once. With no writes to the caller's struct, every
	// composite starts from the same raw layer 0 and runs the passes fresh,
	// so each one applies them exactly once and a screenshot composite is
	// pixel-identical to the presented one (same source, same shaders, same
	// uniforms). The pass is a fraction of a millisecond of compute, and a
	// screenshot frame is the only time it runs twice.
	//
	// The one remaining input from the caller is bBaseLayerEffectsApplied
	// (rendervulkan.hpp), which steamcompmgr sets on a struct whose layer 0
	// IS the pre-emptively upscaled texture -- a texture the passes already
	// ran on, at source resolution, when it was built. That is a fact about
	// the texture in the struct, set by the code that put it there; nothing
	// here sets it.
	const FrameInfo_t *frameInfo = pCallerFrameInfo;
	FrameInfo_t effectsFrameInfo{};
	auto SubstituteLayer0 = [&]( gamescope::Rc<CVulkanTexture> pTex )
	{
		if ( frameInfo != &effectsFrameInfo )
		{
			effectsFrameInfo = *pCallerFrameInfo;
			frameInfo = &effectsFrameInfo;
		}
		effectsFrameInfo.layers.get( 0 ).tex = std::move( pTex );
	};

	EOTF outputTF = frameInfo->outputEncodingEOTF;
	if (!frameInfo->applyOutputColorMgmt)
		outputTF = EOTF_Count; //Disable blending stuff.

	if (frameInfo->layers.get( 0 ).tex)
		g_eLastBaseLayerColorspace.store( frameInfo->layers.get( 0 ).colorspace, std::memory_order_relaxed );

	if (g_reshade_effect.empty())
	{
		g_pLastReshadeEffect = nullptr;
		g_reshadeManager.clear();
	}
	// Issue #20 fix: skip re-applying ReShade to a base layer that was
	// already ReShaded by an earlier vulkan_composite() call this same
	// frame (steamcompmgr's preemptive-upscale path -- see the flag's
	// comment in rendervulkan.hpp). Re-running it here would double-apply
	// the effect on already-upscaled, output-resolution content, and would
	// build a ReshadeEffectKey with different buffer dimensions than the
	// first call's, defeating ReshadeEffectManager's single-entry pipeline
	// cache and forcing a full FX recompile on literally every composite
	// call. Deliberately leave g_pLastReshadeEffect/g_reshadeManager's
	// cached pipeline untouched here -- they still reflect the pipeline
	// the earlier call in this frame already built and ran.
	else if (!frameInfo->bBaseLayerEffectsApplied)
	{
		if (frameInfo->layers.get( 0 ).tex)
		{
			ReshadeEffectKey key
			{
				.path             = g_reshade_effect,
				.bufferWidth      = frameInfo->layers.get( 0 ).tex->width(),
				.bufferHeight     = frameInfo->layers.get( 0 ).tex->height(),
				.bufferColorSpace = frameInfo->layers.get( 0 ).colorspace,
				.bufferFormat     = frameInfo->layers.get( 0 ).tex->format(),
				.techniqueIdx     = g_reshade_technique_idx,
			};

			ReshadeEffectPipeline* pipeline = g_reshadeManager.pipeline(key);
			g_pLastReshadeEffect = pipeline;

			if (pipeline != nullptr)
			{
				// The effect's output replaces layer 0 for the rest of THIS
				// call only (see the head of the function): the caller's
				// struct keeps the raw texture, so a later composite of the
				// same frame runs the effect on the raw texture again, not on
				// this output (which was the gain^2 a gamescopectl screenshot
				// showed under the in-place swap). Same-size input, same key,
				// so the manager's cached pipeline is reused, not rebuilt.
				gamescope::Rc<CVulkanTexture> pReshadeOutput = frameInfo->layers.get( 0 ).tex;
				uint64_t seq = pipeline->execute(frameInfo->layers.get( 0 ).tex, &pReshadeOutput);
				g_device.wait(seq);
				SubstituteLayer0( std::move( pReshadeOutput ) );
			}
		}
	}

	gamescope::Rc<CVulkanTexture> compositeImage;
	if ( pOutputOverride )
		compositeImage = pOutputOverride;
	else
		compositeImage = partial ? g_output.outputImagesPartialOverlay[ g_output.nOutImage ] : g_output.outputImages[ g_output.nOutImage ];

	// Overrides (screenshots, upscale cache) are logical-sized, so never rotated.
	const uint32_t uOutputRotation = pOutputOverride ? 0u : g_uOutputRotation;

	auto cmdBuffer = pInCommandBuffer ? std::move( pInCommandBuffer ) : g_device.commandBuffer();

	// Cross-queue sync point: if the settings overlay drew a new frame on the
	// general queue this vblank, make this (compute-queue) submission wait on
	// its timeline semaphore before it can be recorded to sample that texture,
	// so vulkan_composite() never reads a torn/still-in-progress overlay frame.
	gamescope::SettingsOverlay_WaitForRender( cmdBuffer.get() );
	gamescope::FpsDisplay_WaitForRender( cmdBuffer.get() );
	gamescope::Notifications::WaitForRender( cmdBuffer.get() );

	for (uint32_t i = 0; i < EOTF_Count; i++)
		cmdBuffer->bindColorMgmtLuts(i, frameInfo->shaperLut[i], frameInfo->lut3D[i]);

	// Native layer-0 effects pre-pass (DECISIONS.md #27): the bundled
	// Shaders-area effects (Shadow Control, Vibrancy, Pre-Sharpen, Adaptive
	// Brightness) as one per-pixel compute dispatch over the base layer at
	// source resolution -- preceded by Adaptive Brightness's one-workgroup
	// measure/adapt dispatch -- whose output is swapped into layer 0 so
	// the FSR/NIS/blur/blit paths below consume it exactly as they consumed
	// the ReShade output before. Same command buffer, same compute queue --
	// no general-queue detour and no g_device.wait() CPU stall like the
	// ReShade block above pays.
	//
	// Why here and not in the composite shaders: layer 0 is read four
	// different ways there (composite.h, cs_composite_rcas.comp, blur.h,
	// cs_composite_blur*.comp), Pre-Sharpen needs its neighbours at SOURCE
	// resolution before scaling, and the composite chain has already
	// degamma'd by the time it has a colour -- these effects were authored
	// for encoded 0..1 values (see the shader's header).
	//
	// Gates: SDR only (DECISIONS.md #15 -- HDR/scRGB/passthru layers are
	// skipped, and the panel greys the switches via
	// g_eLastBaseLayerColorspace); no YCbCr (the shader samples one RGB
	// plane); at least one effect on; and layer 0 is not a texture the pass
	// already ran on.
	//
	// EXACTLY ONCE PER COMPOSITE. The rule is about the TEXTURE in layer 0,
	// never about which call this is: grade it unless it already carries the
	// effects. Two textures do -- the pre-emptively upscaled one, which
	// steamcompmgr marks with bBaseLayerEffectsApplied on the struct it puts
	// it in (the passes ran at source resolution while building it, and a
	// second run here would be post-upscale and visibly doubled), and the
	// pooled effects output itself, which no caller's struct can hold now
	// that this function never writes one (see the head of the function),
	// kept as a cheap identity check all the same. Everything else is raw
	// and gets the pass, so the present composite, DRM's copy, and a
	// gamescopectl screenshot's re-composite each apply it once from the
	// same source and agree pixel for pixel.
	if ( frameInfo->layers.get( 0 ).tex
		&& !frameInfo->bBaseLayerEffectsApplied
		&& frameInfo->layers.get( 0 ).tex.get() != g_output.effectsOutput.get()
		&& g_nativeEffects.AnyEnabled() )
	{
		const FrameInfo_t::Layer_t &layer0 = frameInfo->layers.get( 0 );
		const GamescopeAppTextureColorspace eColorspace = layer0.colorspace;
		const bool bSdr = !ColorspaceIsHDR( eColorspace ) && eColorspace != GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU;

		if ( bSdr && !layer0.isYcbcr() )
		{
			const uint32_t uWidth  = layer0.tex->width();
			const uint32_t uHeight = layer0.tex->height();

			if ( update_effects_image( uWidth, uHeight, layer0.tex->drmFormat() ) )
			{
				// Inside the success branch on purpose: a history created on
				// a frame whose pass then did not run would be read next
				// frame without its reset (bHistoryCreated is per call).
				bool bHistoryCreated = false;
				const bool bHaveHistory = update_effects_history( bHistoryCreated );

				// Adaptive Brightness's dt: wall time since the previous
				// effects dispatch, in seconds for the EMA's exp(-dt / tau).
				// Wall time, not a per-frame constant, so the EMA integrates
				// elapsed time however many composites a frame gets: a
				// gamescopectl screenshot's re-composite runs the measure
				// dispatch again a fraction of a millisecond after the
				// present one, and that second step is worth exactly that
				// fraction -- the next frame's step is shorter by the same
				// amount, and the adaptation over any interval is unchanged.
				// Clamped so a hitch, a pause, or the first frame after a
				// long idle nudges the history by at most a quarter-second's
				// worth instead of slamming it to the new measurement in one
				// step.
				static uint64_t s_ulLastEffectsDispatchNs = 0;
				const uint64_t ulNow = get_time_in_nanos();
				float flAbDt = 0.0f;
				if ( s_ulLastEffectsDispatchNs != 0 && ulNow > s_ulLastEffectsDispatchNs )
					flAbDt = std::min( float( ulNow - s_ulLastEffectsDispatchNs ) * 1e-9f, 0.25f );
				s_ulLastEffectsDispatchNs = ulNow;

				// Without a history texture (allocation failed) the visible
				// gain must not read an unbound slot: run the pass with the
				// effect masked off and skip the measure dispatch.
				NativeEffectsState_t state = g_nativeEffects;
				if ( !bHaveHistory )
					state.bAdaptiveBrightness = false;

				// One upload for both dispatches: the measure pass and the
				// per-pixel pass read the same effects_t block, and the
				// descriptor offset uploadConstants() records persists until
				// the next upload.
				cmdBuffer->uploadConstants<EffectsPushData_t>( state, flAbDt, bHistoryCreated );

				// Encoded in, encoded out: setTextureSrgb(0, true) selects the
				// raw UNORM view (srgbView() == "values still sRGB-encoded";
				// bind_all_layers does the same for SRGB layers), and the
				// storage targets are written through their UNORM views too.
				cmdBuffer->bindTexture( 0, layer0.tex );
				cmdBuffer->setTextureSrgb( 0, true );
				cmdBuffer->setSamplerUnnormalized( 0, true );
				cmdBuffer->setSamplerNearest( 0, true );

				if ( bHaveHistory )
				{
					// Adaptive Brightness measure/adapt: one workgroup that
					// averages the graded base layer and blends the result
					// into the 1x1 history, which the per-pixel pass below
					// reads THIS frame (no lag). Runs whether or not the
					// switch is on, as the .fx's Adapt pass did, so the
					// history keeps tracking and re-enabling is instant.
					//
					// HISTORY BARRIERS -- read carefully, this is the part a
					// different driver would break silently. The history is
					// bound to this dispatch both as sampler slot
					// VKR_EFFECTS_HISTORY_SLOT and as the storage target, and
					// dispatch() drives CVulkanCmdBuffer's per-command-buffer
					// m_textureState in this order:
					//   1. prepareSrcImage(history) -- first sight of the
					//      image this command buffer: tracked with
					//      discarded=false. Then prepareDestImage(history)
					//      returns early (already tracked), so it does NOT
					//      set discarded=true the way it does for a target it
					//      sees first. insertBarrier() therefore emits either
					//      nothing for the history (steady state: not dirty,
					//      not discarded) or GENERAL->GENERAL -- never
					//      oldLayout=UNDEFINED, which would license the driver
					//      to drop last frame's value. That is what makes the
					//      persistence a contract instead of the empirical
					//      luck the .fx relied on (DECISIONS.md #14 spike).
					//   2. The one exception is the creation frame: the
					//      VkImage is genuinely in UNDEFINED layout, so
					//      discardImage() below marks it discarded FIRST
					//      (emplace wins; prepareSrcImage's emplace then no-
					//      ops), insertBarrier() emits UNDEFINED->GENERAL, and
					//      kResetHistory tells the shader not to read it.
					//   3. dispatch() ends with markDirty(history). The per-
					//      pixel dispatch below binds the history as a source
					//      again; its insertBarrier() sees dirty=true and
					//      emits srcAccess=SHADER_WRITE -> dstAccess=SHADER_READ,
					//      GENERAL->GENERAL, ALL_COMMANDS->ALL_COMMANDS: the
					//      read-after-write hazard between the two dispatches,
					//      closed.
					//   4. Across frames: the barrier in step 3 made frame
					//      N's write available to every later shader read on
					//      this queue (and the fresh command buffer's
					//      m_textureState starts empty, so frame N+1 goes
					//      through step 1 again). Frame N+1's measure dispatch
					//      overwrites the texel frame N's per-pixel pass read
					//      (a WAR hazard, execution-only): insertBarrier()
					//      records a vkCmdPipelineBarrier ALL_COMMANDS ->
					//      ALL_COMMANDS unconditionally, even with zero image
					//      barriers, and pipeline barriers order against all
					//      earlier work on the same queue across command
					//      buffers, so N's read completes before N+1's write.
					//      Every vulkan_composite() runs on this compute
					//      queue; nothing else touches the history.
					if ( bHistoryCreated )
						cmdBuffer->discardImage( g_output.effectsHistory.get() );
					cmdBuffer->bindTexture( VKR_EFFECTS_HISTORY_SLOT, g_output.effectsHistory );
					cmdBuffer->setTextureSrgb( VKR_EFFECTS_HISTORY_SLOT, true );
					cmdBuffer->setSamplerUnnormalized( VKR_EFFECTS_HISTORY_SLOT, true );
					cmdBuffer->setSamplerNearest( VKR_EFFECTS_HISTORY_SLOT, true );

					cmdBuffer->bindPipeline( g_device.pipeline( SHADER_TYPE_EFFECTS_MEASURE ) );
					cmdBuffer->bindTarget( g_output.effectsHistory );
					cmdBuffer->dispatch( 1, 1, 1 );
				}

				cmdBuffer->bindPipeline( g_device.pipeline( SHADER_TYPE_EFFECTS_LAYER0 ) );
				cmdBuffer->bindTarget( g_output.effectsOutput );

				const int nPixelsPerGroup = 8;
				cmdBuffer->dispatch( div_roundup( uWidth, nPixelsPerGroup ), div_roundup( uHeight, nPixelsPerGroup ) );

				// Leave slot 1 clear for the FSR/NIS/blit dispatches that
				// follow; they bind layers 0..n-1 and would otherwise
				// carry a stray history descriptor on single-layer frames.
				cmdBuffer->bindTexture( VKR_EFFECTS_HISTORY_SLOT, nullptr );

				// The graded image stands in for layer 0 from here on, in
				// this call's private copy of the struct only.
				SubstituteLayer0( g_output.effectsOutput );
			}
		}
	}

	if ( frameInfo->useFSRLayer0 )
	{
		uint32_t inputX = frameInfo->layers.get( 0 ).tex->width();
		uint32_t inputY = frameInfo->layers.get( 0 ).tex->height();

		uint32_t tempX = frameInfo->layers.get( 0 ).integerWidth();
		uint32_t tempY = frameInfo->layers.get( 0 ).integerHeight();

		update_tmp_images(tempX, tempY);

		cmdBuffer->bindPipeline(g_device.pipeline(SHADER_TYPE_EASU));
		cmdBuffer->bindTarget(g_output.tmpOutput);
		cmdBuffer->bindTexture(0, frameInfo->layers.get( 0 ).tex);
		cmdBuffer->setTextureSrgb(0, true);
		cmdBuffer->setSamplerUnnormalized(0, false);
		cmdBuffer->setSamplerNearest(0, false);
		cmdBuffer->uploadConstants<EasuPushData_t>(inputX, inputY, tempX, tempY);

		int pixelsPerGroup = 16;

		cmdBuffer->dispatch(div_roundup(tempX, pixelsPerGroup), div_roundup(tempY, pixelsPerGroup));

		cmdBuffer->bindPipeline(g_device.pipeline(SHADER_TYPE_RCAS, frameInfo->layers.count(), frameInfo->ycbcrMask() & ~1, 0u, frameInfo->colorspaceMask(), outputTF ));
		bind_all_layers(cmdBuffer.get(), frameInfo);
		cmdBuffer->bindTexture(0, g_output.tmpOutput);
		cmdBuffer->setTextureSrgb(0, true);
		cmdBuffer->setSamplerUnnormalized(0, false);
		cmdBuffer->setSamplerNearest(0, false);
		cmdBuffer->bindTarget(compositeImage);
		cmdBuffer->uploadConstants<RcasPushData_t>(frameInfo, g_upscaleFilterSharpness / 10.0f, uOutputRotation);

		cmdBuffer->dispatch(div_roundup(currentOutputWidth, pixelsPerGroup), div_roundup(currentOutputHeight, pixelsPerGroup));
	}
	else if ( frameInfo->useNISLayer0 )
	{
		uint32_t inputX = frameInfo->layers.get( 0 ).tex->width();
		uint32_t inputY = frameInfo->layers.get( 0 ).tex->height();

		uint32_t tempX = frameInfo->layers.get( 0 ).integerWidth();
		uint32_t tempY = frameInfo->layers.get( 0 ).integerHeight();

		update_tmp_images(tempX, tempY);

		float nisSharpness = (20 - g_upscaleFilterSharpness) / 20.0f;

		cmdBuffer->bindPipeline(g_device.pipeline(SHADER_TYPE_NIS));
		cmdBuffer->bindTarget(g_output.tmpOutput);
		cmdBuffer->bindTexture(0, frameInfo->layers.get( 0 ).tex);
		cmdBuffer->setTextureSrgb(0, true);
		cmdBuffer->setSamplerUnnormalized(0, false);
		cmdBuffer->setSamplerNearest(0, false);
		cmdBuffer->bindTexture(VKR_NIS_COEF_SCALER_SLOT, g_output.nisScalerImage);
		cmdBuffer->setSamplerUnnormalized(VKR_NIS_COEF_SCALER_SLOT, false);
		cmdBuffer->setSamplerNearest(VKR_NIS_COEF_SCALER_SLOT, false);
		cmdBuffer->bindTexture(VKR_NIS_COEF_USM_SLOT, g_output.nisUsmImage);
		cmdBuffer->setSamplerUnnormalized(VKR_NIS_COEF_USM_SLOT, false);
		cmdBuffer->setSamplerNearest(VKR_NIS_COEF_USM_SLOT, false);
		cmdBuffer->uploadConstants<NisPushData_t>(inputX, inputY, tempX, tempY, nisSharpness);

		int pixelsPerGroupX = 32;
		int pixelsPerGroupY = 24;

		cmdBuffer->dispatch(div_roundup(tempX, pixelsPerGroupX), div_roundup(tempY, pixelsPerGroupY));

		struct FrameInfo_t nisFrameInfo = *frameInfo;
		nisFrameInfo.layers.get( 0 ).tex = g_output.tmpOutput;
		nisFrameInfo.layers.get( 0 ).scale.x = 1.0f;
		nisFrameInfo.layers.get( 0 ).scale.y = 1.0f;

		cmdBuffer->bindPipeline( g_device.pipeline(SHADER_TYPE_BLIT, nisFrameInfo.layers.count(), nisFrameInfo.ycbcrMask(), 0u, nisFrameInfo.colorspaceMask(), outputTF ));
		bind_all_layers(cmdBuffer.get(), &nisFrameInfo);
		cmdBuffer->bindTarget(compositeImage);
		cmdBuffer->uploadConstants<BlitPushData_t>(&nisFrameInfo, uOutputRotation);

		int pixelsPerGroup = 8;

		cmdBuffer->dispatch(div_roundup(currentOutputWidth, pixelsPerGroup), div_roundup(currentOutputHeight, pixelsPerGroup));
	}
	else if ( frameInfo->blurLayer0 )
	{
		update_tmp_images(currentOutputWidth, currentOutputHeight);

		ShaderType type = SHADER_TYPE_BLUR_FIRST_PASS;

		uint32_t blur_layer_count = 1;
		// Also blur the override on top if we have one.
		if (frameInfo->layers.count() >= 2 && frameInfo->layers.get( 1 ).zpos == g_zposOverride)
			blur_layer_count++;

		cmdBuffer->bindPipeline(g_device.pipeline(type, blur_layer_count, frameInfo->ycbcrMask() & 0x3u, 0, frameInfo->colorspaceMask(), outputTF ));
		cmdBuffer->bindTarget(g_output.tmpOutput);
		for (uint32_t i = 0; i < blur_layer_count; i++)
		{
			cmdBuffer->bindTexture(i, frameInfo->layers.get( i ).tex);
			cmdBuffer->setTextureSrgb(i, false);
			cmdBuffer->setSamplerUnnormalized(i, true);
			cmdBuffer->setSamplerNearest(i, false);
		}
		cmdBuffer->uploadConstants<BlitPushData_t>(frameInfo, uOutputRotation);

		int pixelsPerGroup = 8;

		cmdBuffer->dispatch(div_roundup(currentOutputWidth, pixelsPerGroup), div_roundup(currentOutputHeight, pixelsPerGroup));

		bool useSrgbView = frameInfo->layers.get( 0 ).colorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR;

		type = frameInfo->blurLayer0 == BLUR_MODE_COND ? SHADER_TYPE_BLUR_COND : SHADER_TYPE_BLUR;
		cmdBuffer->bindPipeline(g_device.pipeline(type, frameInfo->layers.count(), frameInfo->ycbcrMask(), blur_layer_count, frameInfo->colorspaceMask(), outputTF ));
		bind_all_layers(cmdBuffer.get(), frameInfo);
		cmdBuffer->bindTarget(compositeImage);
		cmdBuffer->bindTexture(VKR_BLUR_EXTRA_SLOT, g_output.tmpOutput);
		cmdBuffer->setTextureSrgb(VKR_BLUR_EXTRA_SLOT, !useSrgbView); // Inverted because it chooses whether to view as linear (sRGB view) or sRGB (raw view). It's horrible. I need to change it.
		cmdBuffer->setSamplerUnnormalized(VKR_BLUR_EXTRA_SLOT, true);
		cmdBuffer->setSamplerNearest(VKR_BLUR_EXTRA_SLOT, false);

		cmdBuffer->dispatch(div_roundup(currentOutputWidth, pixelsPerGroup), div_roundup(currentOutputHeight, pixelsPerGroup));
	}
	else
	{
		cmdBuffer->bindPipeline( g_device.pipeline(SHADER_TYPE_BLIT, frameInfo->layers.count(), frameInfo->ycbcrMask(), 0u, frameInfo->colorspaceMask(), outputTF ));
		bind_all_layers(cmdBuffer.get(), frameInfo);
		cmdBuffer->bindTarget(compositeImage);
		cmdBuffer->uploadConstants<BlitPushData_t>(frameInfo, uOutputRotation);

		const int pixelsPerGroup = 8;

		cmdBuffer->dispatch(div_roundup(currentOutputWidth, pixelsPerGroup), div_roundup(currentOutputHeight, pixelsPerGroup));
	}

	if ( pPipewireTexture != nullptr )
	{

		if (compositeImage->format() == pPipewireTexture->format() &&
			compositeImage->width() == pPipewireTexture->width() &&
		    compositeImage->height() == pPipewireTexture->height()) {
			cmdBuffer->copyImage(compositeImage, pPipewireTexture);
		} else {
			const bool ycbcr = pPipewireTexture->isYcbcr();

			float scale = (float)compositeImage->width() / pPipewireTexture->width();
			if ( ycbcr )
			{
				CaptureConvertBlitData_t constants( scale, colorspace_to_conversion_from_srgb_matrix( pPipewireTexture->streamColorspace() ) );
				constants.halfExtent[0] = pPipewireTexture->width() / 2.0f;
				constants.halfExtent[1] = pPipewireTexture->height() / 2.0f;
				cmdBuffer->uploadConstants<CaptureConvertBlitData_t>(constants);
			}
			else
			{
				BlitPushData_t constants( scale );
				cmdBuffer->uploadConstants<BlitPushData_t>(constants);
			}

			for (uint32_t i = 0; i < EOTF_Count; i++)
				cmdBuffer->bindColorMgmtLuts(i, nullptr, nullptr);

			cmdBuffer->bindPipeline(g_device.pipeline( ycbcr ? SHADER_TYPE_RGB_TO_NV12 : SHADER_TYPE_BLIT, 1, 0, 0, GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB, EOTF_Count ));
			cmdBuffer->bindTexture(0, compositeImage);
			cmdBuffer->setTextureSrgb(0, true);
			cmdBuffer->setSamplerNearest(0, false);
			cmdBuffer->setSamplerUnnormalized(0, true);
			for (uint32_t i = 1; i < VKR_SAMPLER_SLOTS; i++)
			{
				cmdBuffer->bindTexture(i, nullptr);
			}
			cmdBuffer->bindTarget(pPipewireTexture);

			const int pixelsPerGroup = 8;

			// For ycbcr, we operate on 2 pixels at a time, so use the half-extent.
			const int dispatchSize = ycbcr ? pixelsPerGroup * 2 : pixelsPerGroup;

			cmdBuffer->dispatch(div_roundup(pPipewireTexture->width(), dispatchSize), div_roundup(pPipewireTexture->height(), dispatchSize));
		}
	}

	uint64_t sequence = g_device.submit(std::move(cmdBuffer));

	// Issue #22: the read-done points recorded by *_WaitForRender() above are
	// only reachable now that this submission is on the queue. Promoting them
	// here (and only here) is what lets the overlays' next general-queue
	// render safely wait on them without any chance of waiting on a signal
	// that will never come.
	gamescope::SettingsOverlay_CommitReads();
	gamescope::FpsDisplay_CommitReads();
	gamescope::Notifications::CommitReads();

	if ( !GetBackend()->UsesVulkanSwapchain() && pOutputOverride == nullptr && increment )
	{
		g_output.nOutImage = ( g_output.nOutImage + 1 ) % 3;
	}

	return sequence;
}

void vulkan_wait( uint64_t ulSeqNo, bool bReset )
{
	return g_device.wait( ulSeqNo, bReset );
}

bool vulkan_has_drm_props()
{
	for (const auto& ext : g_device.supportedExtensions()) {
		if ( strcmp(ext.extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0 )
			return true;
	}

	return false;
}

bool vulkan_has_drm_modifiers_for_features(VkFormat format, VkFormatFeatureFlags features)
{
	if ( !g_device.supportsModifiers() )
		return false;

	VkDrmFormatModifierPropertiesListEXT modifierPropList = {
		.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
	};
	VkFormatProperties2 formatProps = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		.pNext = &modifierPropList,
	};

	g_device.vk.GetPhysicalDeviceFormatProperties2( g_device.physDev(), format, &formatProps );

	if ( modifierPropList.drmFormatModifierCount == 0 )
		return false;

	std::vector<VkDrmFormatModifierPropertiesEXT> modifierProps(modifierPropList.drmFormatModifierCount);
	modifierPropList.pDrmFormatModifierProperties = modifierProps.data();
	g_device.vk.GetPhysicalDeviceFormatProperties2( g_device.physDev(), format, &formatProps );

	for ( size_t j = 0; j < modifierProps.size(); j++ ) {
		if ( ( modifierProps[j].drmFormatModifierTilingFeatures & features ) == features )
			return true;
	}

	return false;
}

gamescope::Rc<CVulkanTexture> vulkan_get_last_output_image( bool partial, bool defer )
{
	// Get previous image ( +2 )
	// 1 2 3
	//   |
	// |
	uint32_t nRegularImage = ( g_output.nOutImage + 2 ) % 3;

	// Get previous previous image ( +1 )
	// 1 2 3
	//   |
	//     |
	uint32_t nDeferredImage = ( g_output.nOutImage + 1 ) % 3;

	uint32_t nOutImage = defer ? nDeferredImage : nRegularImage;

	if ( partial )
	{

		//vk_log.infof( "Partial overlay frame: %d", nDeferredImage );
		return g_output.outputImagesPartialOverlay[ nOutImage ];
	}


	return g_output.outputImages[ nOutImage ];
}

bool vulkan_primary_dev_id(dev_t *id)
{
	*id = g_device.primaryDevId();
	return g_device.hasDrmPrimaryDevId();
}

bool vulkan_supports_modifiers(void)
{
	return g_device.supportsModifiers();
}

static void texture_destroy( struct wlr_texture *wlr_texture )
{
	VulkanWlrTexture_t *tex = (VulkanWlrTexture_t *)wlr_texture;
	wlr_buffer_unlock( tex->buf );
	delete tex;
}

static const struct wlr_texture_impl texture_impl = {
	.destroy = texture_destroy,
};

static const struct wlr_drm_format_set *renderer_get_texture_formats( struct wlr_renderer *wlr_renderer, uint32_t buffer_caps )
{
	if (buffer_caps & WLR_BUFFER_CAP_DMABUF)
	{
		return &sampledDRMFormats;
	}
	else if (buffer_caps & WLR_BUFFER_CAP_DATA_PTR)
	{
		return &sampledShmFormats;
	}
	else
	{
		return nullptr;
	}
}

static int renderer_get_drm_fd( struct wlr_renderer *wlr_renderer )
{
	return g_device.drmRenderFd();
}

static struct wlr_texture *renderer_texture_from_buffer( struct wlr_renderer *wlr_renderer, struct wlr_buffer *buf )
{
	VulkanWlrTexture_t *tex = new VulkanWlrTexture_t();
	wlr_texture_init( &tex->base, wlr_renderer, &texture_impl, buf->width, buf->height );
	tex->buf = wlr_buffer_lock( buf );
	// TODO: check format/modifier
	// TODO: if DMA-BUF, try importing it into Vulkan
	return &tex->base;
}

static struct wlr_render_pass *renderer_begin_buffer_pass( struct wlr_renderer *renderer, struct wlr_buffer *buffer, const struct wlr_buffer_pass_options *options )
{
	abort(); // unreachable
}

static const struct wlr_renderer_impl renderer_impl = {
	.get_texture_formats = renderer_get_texture_formats,
	.get_drm_fd = renderer_get_drm_fd,
	.texture_from_buffer = renderer_texture_from_buffer,
	.begin_buffer_pass = renderer_begin_buffer_pass,
};

struct wlr_renderer *vulkan_renderer_create( void )
{
	VulkanRenderer_t *renderer = new VulkanRenderer_t();
	wlr_renderer_init(&renderer->base, &renderer_impl, WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_DATA_PTR);
	return &renderer->base;
}

gamescope::OwningRc<CVulkanTexture> vulkan_create_texture_from_wlr_buffer( struct wlr_buffer *buf, gamescope::OwningRc<gamescope::IBackendFb> pBackendFb )
{

	struct wlr_dmabuf_attributes dmabuf = {0};
	if ( wlr_buffer_get_dmabuf( buf, &dmabuf ) )
	{
		return vulkan_create_texture_from_dmabuf( &dmabuf, pBackendFb );
	}

	VkResult result;

	void *src;
	uint32_t drmFormat;
	size_t stride;
	if ( !wlr_buffer_begin_data_ptr_access( buf, WLR_BUFFER_DATA_PTR_ACCESS_READ, &src, &drmFormat, &stride ) )
	{
		return nullptr;
	}

	uint32_t width = buf->width;
	uint32_t height = buf->height;

	VkBufferCreateInfo bufferCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = stride * height,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	VkBuffer buffer;
	result = g_device.vk.CreateBuffer( g_device.device(), &bufferCreateInfo, nullptr, &buffer );
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return nullptr;
	}

	VkMemoryRequirements memRequirements;
	g_device.vk.GetBufferMemoryRequirements(g_device.device(), buffer, &memRequirements);

	uint32_t memTypeIndex =  g_device.findMemoryType(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, memRequirements.memoryTypeBits );
	if ( memTypeIndex == ~0u )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return nullptr;
	}

	VkMemoryAllocateInfo allocInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = memRequirements.size,
		.memoryTypeIndex = memTypeIndex,
	};

	VkDeviceMemory bufferMemory;
	result = g_device.vk.AllocateMemory( g_device.device(), &allocInfo, nullptr, &bufferMemory);
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return nullptr;
	}

	result = g_device.vk.BindBufferMemory( g_device.device(), buffer, bufferMemory, 0 );
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return nullptr;
	}

	void *dst;
	result = g_device.vk.MapMemory( g_device.device(), bufferMemory, 0, VK_WHOLE_SIZE, 0, &dst );
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return nullptr;
	}

	memcpy( dst, src, stride * height );

	g_device.vk.UnmapMemory( g_device.device(), bufferMemory );

	wlr_buffer_end_data_ptr_access( buf );

	gamescope::OwningRc<CVulkanTexture> pTex = new CVulkanTexture();
	CVulkanTexture::createFlags texCreateFlags;
	texCreateFlags.bSampled = true;
	texCreateFlags.bTransferDst = true;
	texCreateFlags.bFlippable = true;
	if ( pTex->BInit( width, height, 1u, drmFormat, texCreateFlags, nullptr, 0, 0, nullptr, pBackendFb ) == false )
		return nullptr;

	auto cmdBuffer = g_device.commandBuffer();

	cmdBuffer->copyBufferToImage( buffer, 0, stride / DRMFormatGetBPP(drmFormat), pTex);
	// TODO: Sync this copyBufferToImage

	uint64_t sequence = g_device.submit(std::move(cmdBuffer));

	g_device.wait(sequence);

	g_device.vk.DestroyBuffer(g_device.device(), buffer, nullptr);
	g_device.vk.FreeMemory(g_device.device(), bufferMemory, nullptr);

	return pTex;
}

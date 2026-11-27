// gamescopesteamoverlay -- the "dummy window" that carries the Steam overlay.
//
// A tiny see-through full-screen X window on gamescope's own Xwayland that presents with
// Vulkan and does nothing else. gamescope-ritz starts it with Steam's overlay preload and
// Vulkan layer in ITS environment and without them in the game's and its own, so the
// overlay draws here (above the game, as its own layer) instead of inside the game or the
// compositor. Everything about the why is in superdoc/features/steam-overlay.md.
//
// PLAIN XLIB ON PURPOSE: the overlay library interposes Xlib's window-creation and
// event functions (XOpenDisplay, XCreateWindow, XNextEvent, XPending, ...) and only
// xcb_create_window from xcb. Using xcb or SDL here would leave its hooks blind.
// VULKAN ON PURPOSE: the overlay's lag reports are about the GL path.
//
// Depends only on Xlib and Vulkan (no gamescope code) so it can be built and run alone.

#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <dlfcn.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------- logging

static FILE *g_pLog = nullptr;

static double NowSeconds()
{
	timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void Log( const char *pszFmt, ... )
{
	static const double s_flStart = NowSeconds();
	char szMsg[1024];
	va_list args;
	va_start( args, pszFmt );
	vsnprintf( szMsg, sizeof( szMsg ), pszFmt, args );
	va_end( args );
	fprintf( g_pLog, "[%9.3f] steam-overlay: %s\n", NowSeconds() - s_flStart, szMsg );
	fflush( g_pLog );
}

static const char *Env( const char *pszName )
{
	const char *psz = getenv( pszName );
	return psz && *psz ? psz : nullptr;
}

static bool EnvFlag( const char *pszName )
{
	const char *psz = Env( pszName );
	return psz && strcmp( psz, "0" ) != 0;
}

static volatile sig_atomic_t g_bQuit = 0;
static void OnSignal( int ) { g_bQuit = 1; }

// ---------------------------------------------------------------- hotkey

struct Hotkey_t
{
	unsigned int uMods = ShiftMask;
	KeySym sym = XK_Tab;
	std::string sText = "Shift+Tab";
};

static KeySym KeySymFromName( std::string s )
{
	if ( s.rfind( "KEY_", 0 ) == 0 )
		s = s.substr( 4 );
	if ( s.empty() )
		return NoSymbol;
	std::string lower = s, cap = s;
	for ( char &c : lower ) c = tolower( c );
	for ( size_t i = 0; i < cap.size(); i++ ) cap[i] = i == 0 ? toupper( cap[i] ) : tolower( cap[i] );
	static const std::map<std::string, KeySym> k_Special = {
		{ "enter", XK_Return }, { "esc", XK_Escape }, { "pageup", XK_Prior }, { "pagedown", XK_Next },
		{ "grave", XK_grave }, { "backquote", XK_grave }, { "minus", XK_minus }, { "equal", XK_equal },
		{ "capslock", XK_Caps_Lock }, { "backslash", XK_backslash }, { "slash", XK_slash },
	};
	if ( auto it = k_Special.find( lower ); it != k_Special.end() )
		return it->second;
	if ( KeySym sym = XStringToKeysym( lower.c_str() ); sym != NoSymbol )
		return sym; // letters, digits
	return XStringToKeysym( cap.c_str() ); // Tab, Space, Home, F1, ...
}

// "Ctrl+Shift+Tab", "Ctrl<TAB>Shift<TAB>KEY_TAB", "Shift Tab" ... false = could not map.
static bool ParseHotkey( const std::string &sIn, Hotkey_t *pOut )
{
	Hotkey_t hk;
	hk.uMods = 0;
	hk.sym = NoSymbol;
	std::string sTok;
	std::vector<std::string> vecTok;
	for ( char c : sIn + "+" )
	{
		if ( c == '+' || c == '\t' || c == ' ' || c == ',' )
		{
			if ( !sTok.empty() )
				vecTok.push_back( sTok );
			sTok.clear();
		}
		else
			sTok += c;
	}
	for ( const std::string &t : vecTok )
	{
		std::string l = t;
		for ( char &c : l ) c = tolower( c );
		if ( l == "ctrl" || l == "control" || l == "lctrl" || l == "rctrl" ) hk.uMods |= ControlMask;
		else if ( l == "shift" || l == "lshift" || l == "rshift" ) hk.uMods |= ShiftMask;
		else if ( l == "alt" || l == "lalt" || l == "ralt" ) hk.uMods |= Mod1Mask;
		else if ( l == "super" || l == "meta" || l == "win" || l == "lwin" || l == "rwin" ) hk.uMods |= Mod4Mask;
		else if ( hk.sym == NoSymbol )
		{
			hk.sym = KeySymFromName( t );
			if ( hk.sym == NoSymbol )
				return false;
		}
		else
			return false;
	}
	if ( hk.sym == NoSymbol )
		return false;
	hk.sText = sIn;
	*pOut = hk;
	return true;
}

static std::vector<std::string> SteamRoots()
{
	std::vector<std::string> v;
	if ( const char *p = Env( "STEAM_COMPAT_CLIENT_INSTALL_PATH" ) )
		v.push_back( p );
	if ( const char *pszHome = Env( "HOME" ) )
	{
		std::string h = pszHome;
		v.push_back( h + "/.steam/steam" );
		v.push_back( h + "/.steam/root" );
		v.push_back( h + "/.local/share/Steam" );
	}
	return v;
}

// Newest localconfig.vdf's InGameOverlayShortcutKey, e.g. line
//   "InGameOverlayShortcutKey"<TAB><TAB>"Ctrl<TAB>Shift<TAB>KEY_TAB"
static bool HotkeyFromSteamConfig( std::string *psOut )
{
	std::string sBest;
	time_t tBest = 0;
	for ( const std::string &sRoot : SteamRoots() )
	{
		glob_t g{};
		if ( glob( ( sRoot + "/userdata/*/config/localconfig.vdf" ).c_str(), 0, nullptr, &g ) == 0 )
		{
			for ( size_t i = 0; i < g.gl_pathc; i++ )
			{
				struct stat st;
				if ( stat( g.gl_pathv[i], &st ) == 0 && st.st_mtime > tBest )
				{
					tBest = st.st_mtime;
					sBest = g.gl_pathv[i];
				}
			}
		}
		globfree( &g );
		if ( !sBest.empty() )
			break;
	}
	if ( sBest.empty() )
		return false;

	FILE *f = fopen( sBest.c_str(), "r" );
	if ( !f )
		return false;
	char szLine[1024];
	bool bFound = false;
	static const char k_szKey[] = "\"InGameOverlayShortcutKey\"";
	while ( fgets( szLine, sizeof( szLine ), f ) )
	{
		const char *pKey = strstr( szLine, k_szKey );
		if ( !pKey )
			continue;
		// The second quoted string.
		const char *q1 = strchr( pKey + sizeof( k_szKey ) - 1, '"' );
		const char *q2 = q1 ? strchr( q1 + 1, '"' ) : nullptr;
		if ( q2 )
		{
			*psOut = std::string( q1 + 1, q2 );
			bFound = true;
		}
		break;
	}
	fclose( f );
	return bFound;
}

static Hotkey_t ChooseHotkey()
{
	Hotkey_t hk;
	std::string s;
	if ( const char *p = Env( "GS_RITZ_STEAM_OVERLAY_HOTKEY" ) )
	{
		if ( ParseHotkey( p, &hk ) )
		{
			Log( "hotkey from GS_RITZ_STEAM_OVERLAY_HOTKEY: %s", p );
			return hk;
		}
		Log( "cannot map GS_RITZ_STEAM_OVERLAY_HOTKEY=\"%s\", falling through", p );
	}
	if ( HotkeyFromSteamConfig( &s ) )
	{
		if ( ParseHotkey( s, &hk ) )
		{
			Log( "hotkey from Steam's localconfig.vdf: InGameOverlayShortcutKey=\"%s\"", s.c_str() );
			return hk;
		}
		Log( "cannot map InGameOverlayShortcutKey=\"%s\", falling back to Shift+Tab", s.c_str() );
	}
	else
		Log( "no InGameOverlayShortcutKey found, using Steam's default Shift+Tab" );
	return Hotkey_t();
}

// ---------------------------------------------------------------- Vulkan

#define VK_CHECK( x ) do { VkResult r_ = ( x ); if ( r_ != VK_SUCCESS ) { Log( "Vulkan error %d at line %d (%s)", (int)r_, __LINE__, #x ); return false; } } while ( 0 )

struct Vk_t
{
	VkInstance inst = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice dev = VK_NULL_HANDLE;
	uint32_t uQueueFamily = 0;
	VkQueue queue = VK_NULL_HANDLE;
	VkSurfaceKHR surf = VK_NULL_HANDLE;
	VkSwapchainKHR sc = VK_NULL_HANDLE;
	VkFormat fmt = VK_FORMAT_UNDEFINED;
	VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	VkExtent2D ext{};
	VkRenderPass rp = VK_NULL_HANDLE;
	std::vector<VkImage> images;
	std::vector<VkImageView> views;
	std::vector<VkFramebuffer> fbs;
	std::vector<VkSemaphore> semDone; // per image
	VkSemaphore semAcquire = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
};

static const char *AlphaName( VkCompositeAlphaFlagBitsKHR a )
{
	switch ( a )
	{
		case VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR: return "OPAQUE";
		case VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR: return "PRE_MULTIPLIED";
		case VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR: return "POST_MULTIPLIED";
		case VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR: return "INHERIT";
		default: return "?";
	}
}

static void DestroySwapchain( Vk_t &v )
{
	if ( v.dev )
		vkDeviceWaitIdle( v.dev );
	for ( VkFramebuffer f : v.fbs ) vkDestroyFramebuffer( v.dev, f, nullptr );
	for ( VkImageView i : v.views ) vkDestroyImageView( v.dev, i, nullptr );
	for ( VkSemaphore s : v.semDone ) vkDestroySemaphore( v.dev, s, nullptr );
	v.fbs.clear(); v.views.clear(); v.semDone.clear(); v.images.clear();
	if ( v.sc ) vkDestroySwapchainKHR( v.dev, v.sc, nullptr );
	v.sc = VK_NULL_HANDLE;
}

static bool CreateSwapchain( Vk_t &v, uint32_t w, uint32_t h )
{
	VkSurfaceCapabilitiesKHR caps;
	VK_CHECK( vkGetPhysicalDeviceSurfaceCapabilitiesKHR( v.phys, v.surf, &caps ) );

	// Only PRE_MULTIPLIED is verified to be see-through inside gamescope (Mesa's X11 WSI
	// offers it for a window with a 32-bit visual like ours). Anything else would paint the
	// whole game black, so without it we exit: no overlay is better than a black screen.
	const VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
	if ( !( caps.supportedCompositeAlpha & alpha ) )
	{
		Log( "ERROR: the surface offers no pre-multiplied alpha composite mode (supported mask 0x%x); "
			 "presenting would cover the game with an opaque window. Exiting, no overlay.", (unsigned)caps.supportedCompositeAlpha );
		exit( 1 );
	}

	uint32_t nFmt = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR( v.phys, v.surf, &nFmt, nullptr );
	std::vector<VkSurfaceFormatKHR> fmts( nFmt );
	vkGetPhysicalDeviceSurfaceFormatsKHR( v.phys, v.surf, &nFmt, fmts.data() );
	VkSurfaceFormatKHR chosen{};
	bool bHave = false;
	for ( VkFormat f : { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM } )
		for ( const VkSurfaceFormatKHR &sf : fmts )
			if ( !bHave && sf.format == f ) { chosen = sf; bHave = true; }
	if ( !bHave )
	{
		Log( "no 8-bit UNORM swapchain format offered (%u formats)", nFmt );
		return false;
	}

	VkExtent2D ext = caps.currentExtent;
	if ( ext.width == 0xFFFFFFFFu )
		ext = { w, h };
	ext.width = std::clamp( ext.width, caps.minImageExtent.width, std::max( caps.maxImageExtent.width, caps.minImageExtent.width ) );
	ext.height = std::clamp( ext.height, caps.minImageExtent.height, std::max( caps.maxImageExtent.height, caps.minImageExtent.height ) );

	uint32_t nImages = std::max( 2u, caps.minImageCount );
	if ( caps.maxImageCount && nImages > caps.maxImageCount )
		nImages = caps.maxImageCount;

	DestroySwapchain( v );
	VkSwapchainCreateInfoKHR sci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	sci.surface = v.surf;
	sci.minImageCount = nImages;
	sci.imageFormat = chosen.format;
	sci.imageColorSpace = chosen.colorSpace;
	sci.imageExtent = ext;
	sci.imageArrayLayers = 1;
	sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	sci.preTransform = caps.currentTransform;
	sci.compositeAlpha = alpha;
	sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	sci.clipped = VK_TRUE;
	VK_CHECK( vkCreateSwapchainKHR( v.dev, &sci, nullptr, &v.sc ) );

	if ( chosen.format != v.fmt && v.rp )
	{
		vkDestroyRenderPass( v.dev, v.rp, nullptr );
		v.rp = VK_NULL_HANDLE;
	}
	v.fmt = chosen.format;
	if ( !v.rp )
	{
		VkAttachmentDescription att{};
		att.format = v.fmt;
		att.samples = VK_SAMPLE_COUNT_1_BIT;
		att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
		VkSubpassDescription sub{};
		sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		sub.colorAttachmentCount = 1;
		sub.pColorAttachments = &ref;
		VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		rpci.attachmentCount = 1;
		rpci.pAttachments = &att;
		rpci.subpassCount = 1;
		rpci.pSubpasses = &sub;
		VK_CHECK( vkCreateRenderPass( v.dev, &rpci, nullptr, &v.rp ) );
	}

	uint32_t n = 0;
	vkGetSwapchainImagesKHR( v.dev, v.sc, &n, nullptr );
	v.images.resize( n );
	vkGetSwapchainImagesKHR( v.dev, v.sc, &n, v.images.data() );
	for ( VkImage img : v.images )
	{
		VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		ivci.image = img;
		ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ivci.format = v.fmt;
		ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageView view;
		VK_CHECK( vkCreateImageView( v.dev, &ivci, nullptr, &view ) );
		v.views.push_back( view );

		VkFramebufferCreateInfo fbci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		fbci.renderPass = v.rp;
		fbci.attachmentCount = 1;
		fbci.pAttachments = &view;
		fbci.width = ext.width;
		fbci.height = ext.height;
		fbci.layers = 1;
		VkFramebuffer fb;
		VK_CHECK( vkCreateFramebuffer( v.dev, &fbci, nullptr, &fb ) );
		v.fbs.push_back( fb );

		VkSemaphoreCreateInfo semci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		VkSemaphore sem;
		VK_CHECK( vkCreateSemaphore( v.dev, &semci, nullptr, &sem ) );
		v.semDone.push_back( sem );
	}
	v.ext = ext;
	v.alpha = alpha;
	Log( "swapchain %ux%u format %d alpha mode %s, %u images, FIFO", ext.width, ext.height, (int)v.fmt, AlphaName( alpha ), n );
	return true;
}

static bool InitVulkan( Vk_t &v, Display *dpy, Window win )
{
	const char *apszInstExt[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME };
	VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = "gamescopesteamoverlay";
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = 2;
	ici.ppEnabledExtensionNames = apszInstExt;
	VK_CHECK( vkCreateInstance( &ici, nullptr, &v.inst ) );

	VkXlibSurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
	sci.dpy = dpy;
	sci.window = win;
	VK_CHECK( vkCreateXlibSurfaceKHR( v.inst, &sci, nullptr, &v.surf ) );

	uint32_t nDev = 0;
	vkEnumeratePhysicalDevices( v.inst, &nDev, nullptr );
	std::vector<VkPhysicalDevice> devs( nDev );
	vkEnumeratePhysicalDevices( v.inst, &nDev, devs.data() );
	// ponytail: first device that can present to the window. A multi-GPU box whose
	// Xwayland renders on another GPU pays a copy; upgrade path: match gamescope's device.
	for ( VkPhysicalDevice pd : devs )
	{
		uint32_t nQ = 0;
		vkGetPhysicalDeviceQueueFamilyProperties( pd, &nQ, nullptr );
		std::vector<VkQueueFamilyProperties> qs( nQ );
		vkGetPhysicalDeviceQueueFamilyProperties( pd, &nQ, qs.data() );
		for ( uint32_t i = 0; i < nQ && !v.phys; i++ )
		{
			VkBool32 bPresent = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR( pd, i, v.surf, &bPresent );
			if ( bPresent && ( qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT ) )
			{
				v.phys = pd;
				v.uQueueFamily = i;
			}
		}
		if ( v.phys )
			break;
	}
	if ( !v.phys )
	{
		Log( "no Vulkan device can present to the window" );
		return false;
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties( v.phys, &props );
	Log( "Vulkan device: %s", props.deviceName );

	float flPrio = 1.0f;
	VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	qci.queueFamilyIndex = v.uQueueFamily;
	qci.queueCount = 1;
	qci.pQueuePriorities = &flPrio;
	const char *pszDevExt = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = 1;
	dci.ppEnabledExtensionNames = &pszDevExt;
	VK_CHECK( vkCreateDevice( v.phys, &dci, nullptr, &v.dev ) );
	vkGetDeviceQueue( v.dev, v.uQueueFamily, 0, &v.queue );

	VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = v.uQueueFamily;
	VK_CHECK( vkCreateCommandPool( v.dev, &pci, nullptr, &v.pool ) );
	VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	cbai.commandPool = v.pool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VK_CHECK( vkAllocateCommandBuffers( v.dev, &cbai, &v.cmd ) );
	VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	VK_CHECK( vkCreateFence( v.dev, &fci, nullptr, &v.fence ) );
	VkSemaphoreCreateInfo semci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VK_CHECK( vkCreateSemaphore( v.dev, &semci, nullptr, &v.semAcquire ) );
	return true;
}

// One frame: transparent clear (premultiplied 0,0,0,0), plus the test rectangle.
// *pbRecreate is set when the swapchain must be recreated.
static bool PresentFrame( Vk_t &v, bool bTestRect, bool *pbRecreate )
{
	*pbRecreate = false;
	if ( vkWaitForFences( v.dev, 1, &v.fence, VK_TRUE, 1000000000ull ) != VK_SUCCESS )
	{
		Log( "previous frame's fence timed out" );
		return false;
	}
	uint32_t uImage = 0;
	VkResult res = vkAcquireNextImageKHR( v.dev, v.sc, 1000000000ull, v.semAcquire, VK_NULL_HANDLE, &uImage );
	if ( res == VK_ERROR_OUT_OF_DATE_KHR ) { *pbRecreate = true; return false; }
	if ( res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR )
	{
		Log( "vkAcquireNextImageKHR: %d", (int)res );
		return false;
	}
	vkResetFences( v.dev, 1, &v.fence );
	vkResetCommandBuffer( v.cmd, 0 );
	VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	vkBeginCommandBuffer( v.cmd, &bi );
	VkClearValue clear{};
	clear.color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
	VkRenderPassBeginInfo rbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	rbi.renderPass = v.rp;
	rbi.framebuffer = v.fbs[uImage];
	rbi.renderArea = { { 0, 0 }, v.ext };
	rbi.clearValueCount = 1;
	rbi.pClearValues = &clear;
	vkCmdBeginRenderPass( v.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE );
	if ( bTestRect )
	{
		VkClearAttachment ca{};
		ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ca.colorAttachment = 0;
		ca.clearValue.color = { { 1.0f, 0.3f, 0.0f, 1.0f } }; // opaque, so premultiplied == straight
		VkClearRect cr{ { { 0, 0 }, { v.ext.width / 2, v.ext.height / 2 } }, 0, 1 };
		vkCmdClearAttachments( v.cmd, 1, &ca, 1, &cr );
	}
	vkCmdEndRenderPass( v.cmd );
	vkEndCommandBuffer( v.cmd );

	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	si.waitSemaphoreCount = 1;
	si.pWaitSemaphores = &v.semAcquire;
	si.pWaitDstStageMask = &stage;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &v.cmd;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &v.semDone[uImage];
	if ( vkQueueSubmit( v.queue, 1, &si, v.fence ) != VK_SUCCESS )
	{
		Log( "vkQueueSubmit failed" );
		return false;
	}
	VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores = &v.semDone[uImage];
	pi.swapchainCount = 1;
	pi.pSwapchains = &v.sc;
	pi.pImageIndices = &uImage;
	res = vkQueuePresentKHR( v.queue, &pi );
	if ( res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR )
		*pbRecreate = true;
	return res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR;
}

// ---------------------------------------------------------------- X helpers

static Atom g_atomInputFocus, g_atomOverlay;

static void SetCard( Display *dpy, Window w, Atom a, unsigned long v )
{
	XChangeProperty( dpy, w, a, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&v, 1 );
}

static bool g_bGrabFailed = false;

static int OnXError( Display *, XErrorEvent *e )
{
	if ( e->request_code == 33 /* X_GrabKey */ && e->error_code == BadAccess )
		g_bGrabFailed = true;
	Log( "X error: request %d.%d code %d", e->request_code, e->minor_code, e->error_code );
	return 0;
}

static int OnXIOError( Display * )
{
	// The connection is gone (gamescope exiting); X cleans up after us.
	fprintf( stderr, "steam-overlay: X connection lost, exiting\n" );
	_exit( 0 );
}

static const char *EventName( int t )
{
	switch ( t )
	{
		case KeyPress: return "KeyPress"; case KeyRelease: return "KeyRelease";
		case ButtonPress: return "ButtonPress"; case ButtonRelease: return "ButtonRelease";
		case MotionNotify: return "MotionNotify"; case EnterNotify: return "EnterNotify";
		case LeaveNotify: return "LeaveNotify"; case FocusIn: return "FocusIn"; case FocusOut: return "FocusOut";
		case Expose: return "Expose"; case ConfigureNotify: return "ConfigureNotify";
		case PropertyNotify: return "PropertyNotify"; case MapNotify: return "MapNotify";
		case GenericEvent: return "GenericEvent"; default: return "other";
	}
}

// ---------------------------------------------------------------- input hand-over

// GS_RITZ_STEAM_OVERLAY_INPUT: "grab" (default) or "focus".
//  focus: STEAM_INPUT_FOCUS=1 while open. gamescope then calls XSetInputFocus on THIS window, so
//         the game gets FocusOut/NotifyNormal and a game that pauses on focus loss pauses.
//  grab : STEAM_INPUT_FOCUS=2 while open (pointer to this window, X focus and the Wayland
//         keyboard stay on the game) plus an active XGrabKeyboard on this window, so X delivers
//         the keys here. The game sees at most FocusOut/NotifyGrab.
// The overlay library exports XGrabKeyboard/XUngrabKeyboard itself (it tracks and suspends the
// game's own grabs), so a plain call from this process would go through ITS hook and might be
// recorded and deferred while the overlay is active. We therefore resolve libX11's own
// functions from libX11's handle (dlsym on a handle does not see the preloaded interposer).
// The passive hotkey grab stays as it is; its activation produces FocusOut/NotifyGrab and
// FocusOut+FocusIn/NotifyUngrab events on this window (observed headless), which are not focus
// changes by gamescope.
static bool g_bGrabInput = true;
static bool g_bKbGrabbed = false;
static int ( *g_pfnGrabKeyboard )( Display *, Window, Bool, int, int, Time ) = XGrabKeyboard;
static int ( *g_pfnUngrabKeyboard )( Display *, Time ) = XUngrabKeyboard;
static const char *g_pszGrabPath = "plain Xlib call (through the overlay's hook if it is loaded)";

static void ResolveRealGrabFunctions()
{
	// ponytail: RTLD_NOLOAD only finds libX11 when it is already mapped (it is: we link it);
	// ceiling: a static libX11 would fall back to the plain call, which is logged.
	if ( void *pX11 = dlopen( "libX11.so.6", RTLD_LAZY | RTLD_NOLOAD ) )
	{
		auto pfnGrab = (int ( * )( Display *, Window, Bool, int, int, Time ))dlsym( pX11, "XGrabKeyboard" );
		auto pfnUngrab = (int ( * )( Display *, Time ))dlsym( pX11, "XUngrabKeyboard" );
		if ( pfnGrab && pfnUngrab )
		{
			g_pfnGrabKeyboard = pfnGrab;
			g_pfnUngrabKeyboard = pfnUngrab;
			g_pszGrabPath = "libX11's own function (past the overlay's hook)";
		}
	}
}

static const char *GrabStatusName( int n )
{
	switch ( n )
	{
		case GrabSuccess: return "GrabSuccess";
		case AlreadyGrabbed: return "AlreadyGrabbed";
		case GrabInvalidTime: return "GrabInvalidTime";
		case GrabNotViewable: return "GrabNotViewable";
		case GrabFrozen: return "GrabFrozen";
		default: return "?";
	}
}

static void ReleaseKeyboardGrab( Display *dpy )
{
	if ( !g_bKbGrabbed )
		return;
	g_pfnUngrabKeyboard( dpy, CurrentTime );
	XFlush( dpy );
	g_bKbGrabbed = false;
	Log( "XUngrabKeyboard done (%s)", g_pszGrabPath );
}

// The input hand-over for one open/close transition. A failed grab falls back to
// STEAM_INPUT_FOCUS=1 for that open, so the overlay is never left without keys.
static void ApplyInput( Display *dpy, Window win, bool bOpen )
{
	if ( !bOpen )
	{
		ReleaseKeyboardGrab( dpy ); // ungrab first, then give the input back
		SetCard( dpy, win, g_atomInputFocus, 0 );
		XFlush( dpy );
		Log( "STEAM_INPUT_FOCUS -> 0" );
		return;
	}
	unsigned long ulMode = 1;
	if ( g_bGrabInput )
	{
		const int nRes = g_pfnGrabKeyboard( dpy, win, False, GrabModeAsync, GrabModeAsync, CurrentTime );
		Log( "XGrabKeyboard (%s) -> %s (%d)", g_pszGrabPath, GrabStatusName( nRes ), nRes );
		if ( nRes == GrabSuccess )
		{
			g_bKbGrabbed = true;
			ulMode = 2;
		}
		else
			Log( "keyboard grab failed, falling back to STEAM_INPUT_FOCUS=1 for this open" );
	}
	SetCard( dpy, win, g_atomInputFocus, ulMode );
	XFlush( dpy );
	Log( "STEAM_INPUT_FOCUS -> %lu", ulMode );
}

// ---------------------------------------------------------------- Steam callbacks

// Steam posts GameOverlayActivated_t (callback id 331 = k_iSteamFriendsCallbacks 300 + 31,
// first payload byte m_bActive) whenever the overlay opens or closes, however that happened.
// We read it from our own pipe on steamclient.so, with the same flat functions and the same
// CallbackMsg_t layout that the probe tests/steam_friends_live_probe.cpp (namespace
// callbacks) uses. Callback queues are per pipe, so this cannot eat anybody else's.
struct CallbackMsg_t
{
	int32_t m_hSteamUser;
	int m_iCallback;
	uint8_t *m_pubParam;
	int m_cubParam;
};

struct SteamCb_t
{
	int32_t hPipe = 0, hUser = 0;
	bool ( *pfnGet )( int32_t, CallbackMsg_t * ) = nullptr;
	void ( *pfnFree )( int32_t ) = nullptr;
	void ( *pfnReleaseUser )( int32_t, int32_t ) = nullptr;
	bool ( *pfnReleasePipe )( int32_t ) = nullptr;
};

static const int k_iGameOverlayActivated = 331;

// ponytail: the library is never dlclose()d (steamclient.so starts threads and atexit
// handlers, same rule as src/SteamFriends.cpp); the process exits right after the release.
// Every failure is logged once and returns false: the hotkey toggle then stays in charge.
static bool ConnectSteamCallbacks( SteamCb_t &c )
{
	std::vector<std::string> vecPaths;
	if ( const char *p = Env( "GS_RITZ_STEAMCLIENT" ) )
		vecPaths.push_back( p );
	else
		for ( const std::string &sRoot : SteamRoots() )
			vecPaths.push_back( sRoot + "/linux64/steamclient.so" );

	void *pLib = nullptr;
	std::string sUsed;
	for ( const std::string &sPath : vecPaths )
	{
		pLib = dlopen( sPath.c_str(), RTLD_LAZY | RTLD_LOCAL );
		if ( pLib )
		{
			sUsed = sPath;
			break;
		}
	}
	if ( !pLib )
	{
		Log( "callbacks: steamclient.so not found or not loadable (%zu path(s) tried), hotkey toggle only", vecPaths.size() );
		return false;
	}

	auto pfnCreate = (int32_t ( * )())dlsym( pLib, "Steam_CreateSteamPipe" );
	auto pfnConnect = (int32_t ( * )( int32_t ))dlsym( pLib, "Steam_ConnectToGlobalUser" );
	c.pfnGet = (bool ( * )( int32_t, CallbackMsg_t * ))dlsym( pLib, "Steam_BGetCallback" );
	c.pfnFree = (void ( * )( int32_t ))dlsym( pLib, "Steam_FreeLastCallback" );
	c.pfnReleaseUser = (void ( * )( int32_t, int32_t ))dlsym( pLib, "Steam_ReleaseUser" );
	c.pfnReleasePipe = (bool ( * )( int32_t ))dlsym( pLib, "Steam_BReleaseSteamPipe" );
	if ( !pfnCreate || !pfnConnect || !c.pfnGet || !c.pfnFree || !c.pfnReleaseUser || !c.pfnReleasePipe )
	{
		Log( "callbacks: %s lacks a required Steam_* symbol, not used, hotkey toggle only", sUsed.c_str() );
		return false;
	}
	c.hPipe = pfnCreate();
	if ( !c.hPipe )
	{
		Log( "callbacks: Steam_CreateSteamPipe returned 0 (Steam not running?), hotkey toggle only" );
		return false;
	}
	c.hUser = pfnConnect( c.hPipe );
	if ( !c.hUser )
	{
		Log( "callbacks: Steam_ConnectToGlobalUser returned 0 (signed out?), hotkey toggle only" );
		c.pfnReleasePipe( c.hPipe );
		c.hPipe = 0;
		return false;
	}
	Log( "callbacks: connected (%s), pipe %d, user %d", sUsed.c_str(), c.hPipe, c.hUser );
	return true;
}

// ---------------------------------------------------------------- open / closed

// Authority rule: until the first GameOverlayActivated_t (331) of the session has arrived the
// hotkey toggles, exactly as before (so a failed connection degrades to that). From the first
// 331 on, callbacks are the ONLY thing that changes the state; hotkey presses are only logged.
// Why the hotkey cannot be trusted: while the overlay is open its hooks swallow the key
// presses, so the press that closes it never reaches us (measured on real Steam).
// Why BOverlayNeedsPresent() cannot be used: it is true almost always with the overlay closed.
struct OpenState_t
{
	bool bOpen = false;
	bool bAuthority = false; // true once a 331 has been seen
};

// Returns the new open state. Every transition and every ignored press is logged.
// iCallback: -1 = not a callback, else m_bActive.
static bool DecideOpen( OpenState_t &s, bool bHotkey, int iCallback, bool bHaveLib, bool bEnabled )
{
	if ( iCallback >= 0 )
	{
		if ( !s.bAuthority )
		{
			s.bAuthority = true;
			Log( "open/closed: first GameOverlayActivated_t, callbacks are now the only authority (hotkey presses only logged)" );
		}
		if ( s.bOpen != ( iCallback != 0 ) )
		{
			s.bOpen = iCallback != 0;
			Log( "open/closed: callback -> %s", s.bOpen ? "OPEN" : "CLOSED" );
		}
		return s.bOpen;
	}
	if ( !bHotkey )
		return s.bOpen;
	if ( s.bAuthority )
	{
		Log( "open/closed: hotkey press ignored, callbacks decide (state stays %s)", s.bOpen ? "OPEN" : "CLOSED" );
		return s.bOpen;
	}
	if ( !s.bOpen && !bHaveLib )
	{
		Log( "open/closed: hotkey ignored, no overlay library loaded (nothing to give input to)" );
		return s.bOpen;
	}
	if ( !s.bOpen && !bEnabled )
	{
		Log( "open/closed: hotkey ignored, IsOverlayEnabled() is false" );
		return s.bOpen;
	}
	s.bOpen = !s.bOpen;
	Log( "open/closed: hotkey -> %s", s.bOpen ? "OPEN" : "CLOSED" );
	return s.bOpen;
}

// ---------------------------------------------------------------- main

int main()
{
	// Hand-over: gamescope-ritz starts us under gamescopereaper, and nothing in between may
	// carry the overlay (it would register as a second hooked process of the game). So it
	// passes the overlay's values in private variables; we put them in place and re-exec
	// once, so the library loads into THIS process only.
	if ( Env( "GS_RITZ_STEAM_OVERLAY_HANDOVER" ) )
	{
		if ( const char *p = Env( "GS_RITZ_STEAM_OVERLAY_PRELOAD" ) )
			setenv( "LD_PRELOAD", p, 1 );
		else
			unsetenv( "LD_PRELOAD" );
		setenv( "ENABLE_VK_LAYER_VALVE_steam_overlay_1", "1", 1 );
		unsetenv( "DISABLE_VK_LAYER_VALVE_steam_overlay_1" );
		unsetenv( "GS_RITZ_STEAM_OVERLAY_PRELOAD" );
		unsetenv( "GS_RITZ_STEAM_OVERLAY_HANDOVER" );
		char *apszArgv[] = { (char *)"gamescopesteamoverlay", nullptr };
		execv( "/proc/self/exe", apszArgv );
		fprintf( stderr, "steam-overlay: re-exec for the overlay hand-over failed, exiting\n" );
		return 1;
	}

	const char *pszLogPath = Env( "GS_RITZ_STEAM_OVERLAY_LOG" );
	g_pLog = pszLogPath ? fopen( pszLogPath, "a" ) : nullptr;
	if ( !g_pLog )
		g_pLog = stderr;

	Log( "start. LD_PRELOAD=%s", Env( "LD_PRELOAD" ) ? Env( "LD_PRELOAD" ) : "(unset)" );
	Log( "ENABLE_VK_LAYER_VALVE_steam_overlay_1=%s DISABLE_VK_LAYER_VALVE_steam_overlay_1=%s",
		Env( "ENABLE_VK_LAYER_VALVE_steam_overlay_1" ) ? Env( "ENABLE_VK_LAYER_VALVE_steam_overlay_1" ) : "(unset)",
		Env( "DISABLE_VK_LAYER_VALVE_steam_overlay_1" ) ? Env( "DISABLE_VK_LAYER_VALVE_steam_overlay_1" ) : "(unset)" );
	for ( const char *p : { "SteamGameId", "SteamAppId", "SteamOverlayGameId", "DISPLAY", "ENABLE_GAMESCOPE_WSI", "ENABLE_GAMESCOPE_RITZ_WSI" } )
		if ( Env( p ) )
			Log( "%s=%s", p, Env( p ) );

	struct sigaction sa{};
	sa.sa_handler = OnSignal;
	sigaction( SIGTERM, &sa, nullptr );
	sigaction( SIGINT, &sa, nullptr );
	sigaction( SIGHUP, &sa, nullptr );

	// The overlay library exports these only when it is preloaded.
	using BoolFn = bool ( * )();
	BoolFn pfnNeeds = (BoolFn)dlsym( RTLD_DEFAULT, "BOverlayNeedsPresent" );
	BoolFn pfnEnabled = (BoolFn)dlsym( RTLD_DEFAULT, "IsOverlayEnabled" );
	Log( "BOverlayNeedsPresent %s, IsOverlayEnabled %s", pfnNeeds ? "resolved" : "NOT found", pfnEnabled ? "resolved" : "NOT found" );
	if ( !pfnNeeds )
		Log( "overlay library not loaded: heartbeat presents only, nothing is hosted" );

	double flIdleFps = 1.0;
	if ( const char *p = Env( "GS_RITZ_STEAM_OVERLAY_IDLE_FPS" ) )
		flIdleFps = std::max( 0.05, atof( p ) );
	const bool bTestRect = EnvFlag( "GS_RITZ_STEAM_OVERLAY_TEST_RECT" );
	if ( const char *p = Env( "GS_RITZ_STEAM_OVERLAY_INPUT" ) )
	{
		if ( strcmp( p, "focus" ) == 0 )
			g_bGrabInput = false;
		else if ( strcmp( p, "grab" ) != 0 )
			Log( "unknown GS_RITZ_STEAM_OVERLAY_INPUT=\"%s\", using grab", p );
	}
	ResolveRealGrabFunctions();
	Log( "input mode: %s (while open: %s)", g_bGrabInput ? "grab" : "focus",
		g_bGrabInput ? "STEAM_INPUT_FOCUS=2 + active keyboard grab" : "STEAM_INPUT_FOCUS=1, the game loses X focus" );
	if ( g_bGrabInput )
		Log( "keyboard grab will use: %s", g_pszGrabPath );

	XSetErrorHandler( OnXError );
	XSetIOErrorHandler( OnXIOError );
	Display *dpy = XOpenDisplay( nullptr );
	if ( !dpy )
	{
		Log( "cannot open the X display, exiting" );
		return 1;
	}
	const int nScreen = DefaultScreen( dpy );
	const Window root = RootWindow( dpy, nScreen );

	XVisualInfo vi;
	if ( !XMatchVisualInfo( dpy, nScreen, 32, TrueColor, &vi ) )
	{
		Log( "no 32-bit ARGB visual, exiting" );
		return 1;
	}
	XWindowAttributes rootAttr;
	XGetWindowAttributes( dpy, root, &rootAttr );
	unsigned int uW = rootAttr.width, uH = rootAttr.height;

	XSetWindowAttributes swa{};
	swa.colormap = XCreateColormap( dpy, root, vi.visual, AllocNone );
	swa.border_pixel = 0;
	swa.background_pixel = 0;
	swa.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
					 EnterWindowMask | LeaveWindowMask | FocusChangeMask | ExposureMask | StructureNotifyMask;
	Window win = XCreateWindow( dpy, root, 0, 0, uW, uH, 0, 32, InputOutput, vi.visual,
		CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &swa );

	g_atomOverlay = XInternAtom( dpy, "STEAM_OVERLAY", False );
	g_atomInputFocus = XInternAtom( dpy, "STEAM_INPUT_FOCUS", False );
	// STEAM_OVERLAY makes gamescope composite us above the game and never treat us as the
	// game. STEAM_INPUT_FOCUS stays 0 until the overlay opens: a window that keeps it set
	// swallows input even when invisible (superdoc/planning/steam-friends-window.md).
	SetCard( dpy, win, g_atomOverlay, 1 );
	SetCard( dpy, win, g_atomInputFocus, 0 );
	XStoreName( dpy, win, "gamescope steam overlay" );
	XSelectInput( dpy, root, StructureNotifyMask );
	XMapWindow( dpy, win );
	XFlush( dpy );
	Log( "window 0x%lx %ux%u, visual depth %d", win, uW, uH, vi.depth );

	Vk_t vk;
	bool bVk = InitVulkan( vk, dpy, win ) && CreateSwapchain( vk, uW, uH );
	if ( !bVk )
		Log( "Vulkan setup failed: the window stays empty and hosts nothing" );

	// Hotkey: a passive grab on the root, so the press reaches OUR event stream (where the
	// overlay's hooks look) even though the game has the keyboard. NumLock (Mod2) and
	// CapsLock (Lock) variants, or the grab would miss with either on.
	const Hotkey_t hk = ChooseHotkey();
	const KeyCode kc = XKeysymToKeycode( dpy, hk.sym );
	if ( kc )
	{
		for ( unsigned int uExtra : { 0u, (unsigned)Mod2Mask, (unsigned)LockMask, (unsigned)( Mod2Mask | LockMask ) } )
			XGrabKey( dpy, kc, hk.uMods | uExtra, root, False, GrabModeAsync, GrabModeAsync );
		XSync( dpy, False );
		if ( g_bGrabFailed )
			Log( "ERROR: XGrabKey failed with BadAccess (another client holds the grab on \"%s\"): the overlay CANNOT be opened by hotkey", hk.sText.c_str() );
		Log( "grabbed hotkey \"%s\" (keycode %u, modifiers 0x%x + NumLock/CapsLock variants)", hk.sText.c_str(), kc, hk.uMods );
	}
	else
		Log( "no keycode for hotkey \"%s\", nothing grabbed", hk.sText.c_str() );

	// Steam's open/closed callback. Only with an overlay library loaded: without one there is
	// nothing to give input to and the hotkey stays ignored, so no pipe is needed either.
	SteamCb_t cb;
	bool bCb = false;
	if ( EnvFlag( "GS_RITZ_STEAM_OVERLAY_NO_CALLBACK" ) )
		Log( "callbacks: disabled by GS_RITZ_STEAM_OVERLAY_NO_CALLBACK, pure hotkey toggle" );
	else if ( !pfnNeeds )
		Log( "callbacks: not connected, no overlay library loaded" );
	else
		bCb = ConnectSteamCallbacks( cb );

	OpenState_t open;
	bool bPrevOpen = false;
	bool bLastNeeds = false;
	double flNeedsChangedAt = 0;
	unsigned int uNeedsTrue = 0;
	std::map<int, int> mapCbCounts;
	double flHotkeyAt = 0; // a hotkey press waiting to see whether a 331 follows
	double flLast331 = 0;
	int nKeyLogsThisSecond = 0;
	int nLastEnabled = -1;
	double flNextHeartbeat = 0;
	bool bRedraw = true;
	double flSecondStart = NowSeconds();
	std::map<std::string, int> mapCounts;
	unsigned int uPresents = 0, uPresentsThisSecond = 0;
	// 16 ms is about a frame. Without the overlay library there is nothing to poll and
	// the heartbeat paces us.
	const int nPollMs = pfnNeeds ? 16 : (int)std::min( 1000.0, 1000.0 / flIdleFps );

	while ( !g_bQuit )
	{
		pollfd pfd{ ConnectionNumber( dpy ), POLLIN, 0 };
		if ( !XPending( dpy ) )
			poll( &pfd, 1, nPollMs );

		const double flNow = NowSeconds();
		bool bHotkeyFired = false;
		while ( XPending( dpy ) )
		{
			XEvent ev;
			XNextEvent( dpy, &ev );
			mapCounts[EventName( ev.type )]++;
			switch ( ev.type )
			{
				case FocusIn:
				case FocusOut:
				{
					static const char *const k_apszMode[] = { "NotifyNormal", "NotifyGrab", "NotifyUngrab", "NotifyWhileGrabbed" };
					static const char *const k_apszDetail[] = { "NotifyAncestor", "NotifyVirtual", "NotifyInferior", "NotifyNonlinear",
						"NotifyNonlinearVirtual", "NotifyPointer", "NotifyPointerRoot", "NotifyDetailNone" };
					const int m = ev.xfocus.mode, d = ev.xfocus.detail;
					Log( "%s window 0x%lx mode %s detail %s (own keyboard grab: %s)", EventName( ev.type ), ev.xfocus.window,
						m >= 0 && m < 4 ? k_apszMode[m] : "?", d >= 0 && d < 8 ? k_apszDetail[d] : "?", g_bKbGrabbed ? "yes" : "no" );
					break;
				}
				case KeyPress:
					if ( open.bOpen && nKeyLogsThisSecond < 20 )
					{
						nKeyLogsThisSecond++;
						Log( "KeyPress reached the helper while open: keycode %u state 0x%x", ev.xkey.keycode, ev.xkey.state );
					}
					if ( ev.xkey.keycode == kc && ( ev.xkey.state & ~( Mod2Mask | LockMask ) ) == hk.uMods )
					{
						bHotkeyFired = true;
						flHotkeyAt = flNow;
						Log( "hotkey press delivered (window 0x%lx, state 0x%x)", ev.xkey.window, ev.xkey.state );
					}
					break;
				case ConfigureNotify:
					if ( ev.xconfigure.window == root || ev.xconfigure.window == win )
					{
						XGetWindowAttributes( dpy, root, &rootAttr );
						if ( (unsigned)rootAttr.width != uW || (unsigned)rootAttr.height != uH )
						{
							uW = rootAttr.width;
							uH = rootAttr.height;
							Log( "root window is now %ux%u, resizing", uW, uH );
							XResizeWindow( dpy, win, uW, uH );
						}
						else if ( ev.xconfigure.window == win && bVk &&
								  ( (unsigned)ev.xconfigure.width != vk.ext.width || (unsigned)ev.xconfigure.height != vk.ext.height ) )
						{
							Log( "window size %dx%d", ev.xconfigure.width, ev.xconfigure.height );
							bVk = CreateSwapchain( vk, ev.xconfigure.width, ev.xconfigure.height );
							if ( !bVk )
								Log( "swapchain recreate failed, no more presents" );
							bRedraw = true;
						}
					}
					break;
				case DestroyNotify:
					if ( ev.xdestroywindow.window == win && g_bKbGrabbed )
					{
						g_bKbGrabbed = false; // X dropped the grab with the window
						Log( "window destroyed, keyboard grab is gone" );
					}
					break;
				case Expose:
					bRedraw = true;
					break;
			}
		}

		const bool bNeeds = pfnNeeds ? pfnNeeds() : false;
		if ( bNeeds )
			uNeedsTrue++;
		if ( pfnNeeds && bNeeds != bLastNeeds )
		{
			// Only a change after >= 0.5 s of stability is logged (it flickers constantly).
			if ( flNow - flNeedsChangedAt >= 0.5 )
				Log( "BOverlayNeedsPresent -> %s", bNeeds ? "true" : "false" );
			bLastNeeds = bNeeds;
			flNeedsChangedAt = flNow;
		}
		if ( pfnEnabled )
		{
			const int nEnabled = pfnEnabled() ? 1 : 0;
			if ( nEnabled != nLastEnabled )
			{
				Log( "IsOverlayEnabled -> %d", nEnabled );
				nLastEnabled = nEnabled;
			}
		}

		// Open / closed and the input hand-over.
		if ( bCb )
		{
			CallbackMsg_t msg{};
			while ( cb.pfnGet( cb.hPipe, &msg ) )
			{
				mapCbCounts[msg.m_iCallback]++;
				if ( msg.m_iCallback == k_iGameOverlayActivated )
				{
					char szBytes[3 * 16 + 1] = "";
					for ( int i = 0; i < msg.m_cubParam && i < 16 && msg.m_pubParam; i++ )
						snprintf( szBytes + strlen( szBytes ), 4, "%s%02x", i ? " " : "", msg.m_pubParam[i] );
					const int iActive = msg.m_cubParam >= 1 && msg.m_pubParam && msg.m_pubParam[0] ? 1 : 0;
					flLast331 = flNow;
					Log( "callback 331 GameOverlayActivated_t: %d bytes [%s] -> active=%d", msg.m_cubParam, szBytes, iActive );
					DecideOpen( open, false, iActive, pfnNeeds != nullptr, nLastEnabled != 0 );
				}
				cb.pfnFree( cb.hPipe );
				memset( &msg, 0, sizeof( msg ) );
			}
			if ( flHotkeyAt > 0 && flNow - flHotkeyAt > 1.0 )
			{
				if ( flLast331 < flHotkeyAt )
					Log( "a hotkey press was not followed by a 331 within 1 s (callback not delivered to this pipe?)" );
				flHotkeyAt = 0;
			}
		}
		const bool bWasOpen = bPrevOpen;
		if ( bHotkeyFired )
			DecideOpen( open, true, -1, pfnNeeds != nullptr, nLastEnabled != 0 );
		bPrevOpen = open.bOpen;
		if ( open.bOpen != bWasOpen )
		{
			ApplyInput( dpy, win, open.bOpen );
			bRedraw = true;
		}

		// Present when the overlay asks, on the idle heartbeat, or after a resize.
		if ( bVk && ( bNeeds || bRedraw || flNow >= flNextHeartbeat ) )
		{
			bool bRecreate = false;
			if ( PresentFrame( vk, bTestRect, &bRecreate ) )
			{
				uPresents++;
				uPresentsThisSecond++;
			}
			if ( bRecreate )
			{
				bVk = CreateSwapchain( vk, uW, uH );
				if ( !bVk )
					Log( "swapchain recreate failed, no more presents" );
			}
			bRedraw = false;
			flNextHeartbeat = NowSeconds() + 1.0 / flIdleFps;
		}

		if ( flNow - flSecondStart >= 1.0 )
		{
			std::string s, sCb;
			for ( auto &kv : mapCounts )
				s += " " + kv.first + "=" + std::to_string( kv.second );
			for ( auto &kv : mapCbCounts )
				sCb += " " + std::to_string( kv.first ) + "=" + std::to_string( kv.second );
			if ( !s.empty() || uPresentsThisSecond || !sCb.empty() )
				Log( "last %.1fs: presents=%u needsTrue=%u events:%s callbacks:%s", flNow - flSecondStart, uPresentsThisSecond,
					uNeedsTrue, s.empty() ? " none" : s.c_str(), sCb.empty() ? " none" : sCb.c_str() );
			mapCounts.clear();
			mapCbCounts.clear();
			uNeedsTrue = 0;
			nKeyLogsThisSecond = 0;
			uPresentsThisSecond = 0;
			flSecondStart = flNow;
		}
	}

	Log( "exiting (%u presents in total)", uPresents );
	if ( bCb )
	{
		cb.pfnReleaseUser( cb.hPipe, cb.hUser );
		cb.pfnReleasePipe( cb.hPipe );
	}
	if ( bVk )
		DestroySwapchain( vk );
	ReleaseKeyboardGrab( dpy );
	XCloseDisplay( dpy );
	return 0;
}

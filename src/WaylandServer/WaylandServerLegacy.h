#pragma once

#include <wayland-server-core.h>
#include "WaylandDecls.h"
#include <memory>
#include <optional>
#include <vector>
#include "vulkan_include.h"


#include "wlr_begin.hpp"
#include <wlr/types/wlr_compositor.h>
#include "wlr_end.hpp"


namespace gamescope
{
    class BackendBlob;
}

struct wlserver_x11_surface_info;
struct wlserver_xdg_surface_info;

struct wlserver_vk_swapchain_feedback
{
	uint32_t image_count;
	VkFormat vk_format;
	VkColorSpaceKHR vk_colorspace;
	VkCompositeAlphaFlagBitsKHR vk_composite_alpha;
	VkSurfaceTransformFlagBitsKHR vk_pre_transform;
	VkBool32 vk_clipped;
	std::shared_ptr<std::string> vk_engine_name;

	std::shared_ptr<gamescope::BackendBlob> hdr_metadata_blob;
};


struct wlserver_wl_surface_info
{
	wlserver_x11_surface_info *x11_surface = nullptr;
	wlserver_xdg_surface_info *xdg_surface = nullptr;

	gamescope::WaylandServer::CLinuxDrmSyncobjSurface *pSyncobjSurface = nullptr;

	struct wlr_surface *wlr = nullptr;
	struct wl_listener commit;
	struct wl_listener destroy;

	std::shared_ptr<wlserver_vk_swapchain_feedback> swapchain_feedback = {};
	std::optional<VkPresentModeKHR> oCurrentPresentMode;

	uint64_t sequence = 0;
	std::vector<struct wl_resource*> pending_presentation_feedbacks;

	std::vector<struct wl_resource *> gamescope_swapchains;
	std::optional<uint32_t> present_id = std::nullopt;
	uint64_t desired_present_time = 0;

	uint64_t last_refresh_cycle = 0;

	// Frame gate (gamescope_swapchain v2 frame_gate, "Hold game while generating"):
	// every commit of this surface is counted; gate_released is how many of them
	// the client has been told it may move past; gate_enabled_sent is the
	// `enabled` flag of the last event sent (clients only wait while it is true).
	uint32_t gate_commits = 0;
	uint32_t gate_released = 0;
	bool gate_enabled_sent = false;
};

wlserver_wl_surface_info *get_wl_surface_info(struct wlr_surface *wlr_surf);

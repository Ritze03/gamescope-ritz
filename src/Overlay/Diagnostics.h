// Diagnostics: checks of the setup around gamescope that cost the user frames
// without any error, run off the paint thread and published for the
// Diagnostics area (PanelDiagnostics.cpp). One check today: whether Hyprland has
// direct scanout on. See superdoc/features/diagnostics.md.
#pragma once

#include "DiagnosticsLogic.h"

namespace gamescope
{
	// First thing in main(), before gamescope changes its own environment: keeps
	// the environment gamescope was LAUNCHED with, which is the host session's
	// (HYPRLAND_INSTANCE_SIGNATURE, XDG_RUNTIME_DIR, the host's WAYLAND_DISPLAY and
	// DISPLAY). hyprctl and the browser are run with it, never with what gamescope
	// later sets for its own game.
	void Diagnostics_CaptureHostEnv();

	// Every paint_all(), on the steamcompmgr thread (SettingsOverlay_AddLayer()).
	// Starts the startup check once the output is sized, raises the one startup
	// toast when it finds Hyprland nested with direct scanout off, and reaps
	// finished browser launches. Cheap when idle.
	void Diagnostics_Tick();

	// Reruns every diagnostic on a worker thread. Any thread. A no-op while one
	// run is still going. Never toasts.
	void Diagnostics_Recheck();

	// Opens the Hyprland wiki's render options in the user's browser (xdg-open,
	// no shell, the host's environment). Returns false when xdg-open is missing.
	bool Diagnostics_OpenDocs();

	// Whether any diagnostic is in a warning state right now (today: direct
	// scanout Off). Drives the warning sign on the Shell's rail. Any thread;
	// reads the published results under the same short mutex Diagnostics_View()
	// uses (held only for a struct copy, never across a child process), starts
	// nothing. A recheck in flight keeps the previous answer until it lands.
	bool Diagnostics_HasWarning();

	// The "Silence warning" setting (global.json overlay.diagnostics_silence_warning,
	// default false): no startup toast and no rail dot. Any thread. The setter
	// persists to global.json and takes effect for the dot at once.
	bool Diagnostics_Silenced();
	void Diagnostics_SetSilenced( bool bSilenced );

	// ---- compatibility settings -------------------------------------------
	// The two switches below are global.json keys that may be ABSENT. An absent key
	// takes the recommendation for the HOST desktop (Recommend(), DiagnosticsLogic.h)
	// and is never written back by a save; a key the user set, or "Pick recommended
	// settings" wrote, is kept as is. Resolved on the first getter call (once, with
	// the host environment Diagnostics_CaptureHostEnv() kept): for Compress that is
	// rendervulkan.cpp's first output image, long after main() captured the env, so
	// the very first launch already runs with the recommendation.

	// The "Block direct scanout" setting (global.json overlay.diagnostics_block_scanout):
	// the Wayland connector keeps a transparent 1x1 subsurface on its
	// top plane so a nested host cannot scan the window out (WaylandBackend.cpp). Any
	// thread; the getter is an atomic read, safe on the present path. The setter
	// persists (making the key present), then repaints so the plane appears or goes
	// on the next frame.
	bool Diagnostics_BlockScanout();
	void Diagnostics_SetBlockScanout( bool bBlock );

	// The "Compress output images" setting (global.json overlay.compress_output_images):
	// lets the driver pick an AMD DCC modifier for the output images under a nested
	// Wayland host. rendervulkan.cpp latches the getter at the first output image, so
	// a change applies on the next launch. Any thread.
	bool Diagnostics_CompressOutputImages();
	void Diagnostics_SetCompressOutputImages( bool bCompress );

	// The Compress value this process STARTED with (what the first getter call
	// resolved, i.e. what rendervulkan.cpp latched). The UI shows "Restart to apply"
	// while Diagnostics_CompressOutputImages() differs from it. Any thread.
	bool Diagnostics_CompressOutputImagesAtLaunch();

	// The host desktop, detected once from the launch environment (not the
	// XDG_CURRENT_DESKTOP gamescope sets for itself), and what the D6 table
	// recommends for it. Any thread.
	diagnostics::Desktop        Diagnostics_HostDesktop();
	diagnostics::Recommendation Diagnostics_Recommended();
	// "Block scanout Off \xC2\xB7 Compress Off", for the Pick recommended settings row.
	std::string                 Diagnostics_RecommendationText();

	// Pick recommended settings: writes the recommended value of BOTH keys to
	// global.json (making them present). Block applies live; Compress on the next
	// launch. Any thread.
	void Diagnostics_ApplyRecommended();

	// Re-detect: refreshes the displayed facts (backend, GPU) and re-reads nothing
	// the launch fixed. Changes no setting. Any thread.
	void Diagnostics_Redetect();

	// Read-only environment facts for the Compatibility area. Desktop is the
	// DesktopName() of the host desktop ("Other" when unknown); Backend is e.g.
	// "Wayland (nested)", "SDL", "DRM", "Headless" ("unknown" before the backend
	// exists); Gpu is "unknown" until Diagnostics_SetGpuName() is called (the Vulkan
	// physical device name; nothing waits for it).
	struct CompatFacts
	{
		std::string sDesktop;
		std::string sBackend;
		std::string sGpu;
	};
	CompatFacts Diagnostics_Facts();
	void        Diagnostics_SetGpuName( const char *pszName );

	// Whether this backend can be scanned out by a host at all (Wayland): where the
	// Block switch is shown.
	bool Diagnostics_ScanoutApplicable();

	struct DiagnosticsView
	{
		bool                          bRunning = false;
		bool                          bEverChecked = false;
		diagnostics::ScanoutResult    scanout;
	};
	DiagnosticsView Diagnostics_View();
}

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

	// The "Block direct scanout" setting (global.json overlay.diagnostics_block_scanout,
	// default false): the Wayland connector keeps a transparent 1x1 subsurface on its
	// top plane so a nested host cannot scan the window out (WaylandBackend.cpp). Any
	// thread; the getter is an atomic read, safe on the present path. The setter
	// persists, then repaints so the plane appears or goes on the next frame.
	bool Diagnostics_BlockScanout();
	void Diagnostics_SetBlockScanout( bool bBlock );

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

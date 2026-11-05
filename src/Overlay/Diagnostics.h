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

	struct DiagnosticsView
	{
		bool                          bRunning = false;
		bool                          bEverChecked = false;
		diagnostics::ScanoutResult    scanout;
	};
	DiagnosticsView Diagnostics_View();
}

// The E2 shell -- SPEC.md §8.1's slab, and the three regions inside it.
//
// This is the whole of its public surface: one function. Everything else
// about the shell is private to Shell.cpp, deliberately, because SPEC §5.2
// clause 0 ("the Inspector has no authoring API") is only credible if there
// is no header a category file could include to reach into it.
//
// ---------------------------------------------------------------------------
// WHAT REPLACED WHAT
// ---------------------------------------------------------------------------
// This shell IS the settings overlay. P5 deleted the legacy dock and its five
// floating windows, and with them the whole window-management layer: no
// floating windows, no dragging, no resizing, no z-order, no tiling, no
// per-panel position memory.
//
// That is the point of the direction rather than a simplification of it --
// 39% of this project's overlay bug-fix commits were floating-window
// management, and nine of those sixteen were fixes to earlier fixes. A bug
// class is not fixed by fixing its instances; it is fixed by deleting the
// thing that produces them.
//
// The slab is centred, sized from the surface, and cannot be moved. There is
// exactly one of it.
//
// THERE IS NO `overlay_e2` ConVar ANY MORE. It was the flag both shells lived
// behind, and P5 removed it with the path it selected: a flag with one
// reachable value is not a flag, and a ConVar that accepts `0` while still
// drawing this shell would be a control that renders and does nothing --
// precisely the defect class (#25, #68, the computed-but-undrawn column
// count, the registered-nowhere Meter kind) this redesign spent itself
// removing. It was never a config key (AUTONOMOUS-DECISIONS.md D12 kept it
// runtime-only), so no config file mentions it and none can fail to load
// because it is gone. See D21.
#pragma once

namespace gamescope::ui::shell
{
	// One frame of the shell. Called from SettingsOverlay.cpp's draw loop
	// between NewFrame() and Render().
	//
	// Draws the slab, the rail, the sheet and the inspector; runs the
	// keyboard map (SPEC §8.2); and hosts whatever the selected area is.
	void Draw();
}

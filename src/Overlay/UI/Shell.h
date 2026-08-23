// The E2 shell -- SPEC.md §8.1's slab, and the three regions inside it.
//
// This is P2's deliverable and the whole of its public surface: two
// functions. Everything else about the shell is private to Shell.cpp,
// deliberately, because SPEC §5.2 clause 0 ("the Inspector has no authoring
// API") is only credible if there is no header a category file could include
// to reach into it.
//
// ---------------------------------------------------------------------------
// WHAT REPLACES WHAT
// ---------------------------------------------------------------------------
// With `overlay_e2 0` (the default, this phase) nothing here runs and the
// legacy dock plus its five floating windows behave exactly as before.
//
// With `overlay_e2 1` the shell replaces them entirely, and with them the
// whole window-management layer: no floating windows, no dragging, no
// resizing, no z-order, no tiling, no per-panel position memory. That is the
// point of the direction rather than a simplification of it -- 39% of this
// project's overlay bug-fix commits were floating-window management, and nine
// of those sixteen were fixes to earlier fixes. A bug class is not fixed by
// fixing its instances; it is fixed by deleting the thing that produces them.
//
// The slab is centred, sized from the surface, and cannot be moved. There is
// exactly one of it.
#pragma once

namespace gamescope::ui::shell
{
	// The `overlay_e2` ConVar. False by default for the whole of P2: no
	// behaviour change for anyone who does not set it.
	//
	// Read once per frame by SettingsOverlay.cpp, which draws EITHER this
	// shell or the legacy dock -- never both, and never a mixture. Toggling
	// it live is supported and takes effect on the next frame; the two paths
	// share no ImGui window ids, so neither can leave state behind that the
	// other trips over.
	bool Enabled();

	// One frame of the shell. Called from SettingsOverlay.cpp's draw loop
	// between NewFrame() and Render(), in place of the legacy
	// DrawFpsHudPanel()/Panel*_Draw()/DrawDock() sequence.
	//
	// Draws the slab, the rail, the sheet and the inspector; runs the
	// keyboard map (SPEC §8.2); and hosts whatever the selected area is --
	// which in P2 is a legacy panel body through ui::Area::Escape().
	void Draw();
}

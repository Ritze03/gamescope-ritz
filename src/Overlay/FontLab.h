// ============================================================================
// TEMPORARY DIAGNOSTIC -- NOT A FEATURE. DELETE ONCE THE ARTIFACT IS FOUND.
// ============================================================================
// Built to chase one specific user report: "'y' has a small half-transparent
// grey vertical line next to it" -- a glyph-neighbour-bleed artifact, not the
// integer-bake/resample blur issue #99 already fixed (see Fonts.h's
// RasterSize() comment -- that is a different, unrelated artifact that is
// NOT what this tool is for).
//
// Delete this file and FontLab.cpp, and the two lines marked "FontLab TEMP"
// in SettingsOverlay.cpp, once the cause is identified. No CHANGELOG entry
// for this per superdoc/claude-instructions/changelog.md -- internal
// diagnostic tooling with no shipped user-visible effect.
//
// See FontLab.cpp's own top-of-file comment for what each cell tests and why.
#pragma once

namespace gamescope::fontlab
{
	// True while the `overlay_e2_fontlab` ConVar is on. SettingsOverlay.cpp
	// reads this to keep rendering a frame even while the real shell is
	// fully closed (alpha == 0) -- the lab must be visible on demand
	// regardless of shell open/closed state, since that state is what a
	// user comparing glyph rendering has no reason to otherwise care about.
	bool Enabled();

	// Draws the comparison grid into the current ImGui context's foreground
	// draw list. Must be called from inside that context's own
	// NewFrame()/Render() bracket (SettingsOverlay.cpp calls it right before
	// Render(), same as the cursor art draw just above it), so it reuses the
	// exact same Vulkan submission path everything else in that frame uses
	// -- no separate context, texture or semaphore of its own. A no-op when
	// Enabled() is false.
	void Draw();
}

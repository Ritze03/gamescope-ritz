// M4 FPS display -- see superdoc/planning/SPEC.md's "Per-feature sections ->
// 3. FPS display" and superdoc/planning/DECISIONS.md #16/#17.
//
// Scope reduction (2026-09-03, the user's own call -- see CHANGELOG.md and
// superdoc/meta/TERMINOLOGY.md's "profiler" entry): this HUD used to be a
// small performance profiler (CPU/GPU load, a frametime graph, a percentile
// row, Now Playing) built on top of the FPS readout. All of that is gone.
// This file draws exactly one thing: the FPS integer, positioned by a
// 9-point anchor plus pixel margins. Phase 2 (2026-09-03, same day) rebuilt
// the `system.hud` settings area on top of what Phase 1 left -- update
// modes, hide-above-X, a plain backdrop, a two-way text-colour choice and a
// lag-spike reaction, a drop shadow -- see
// superdoc/features/fps-display.md for the whole feature as it stands now.
//
// Lifetime note (the subtlety that milestone is most likely to get wrong):
// this readout has its own visibility flag, entirely independent of the
// settings panel's `cv_settings_overlay_visible`. It must keep rendering
// every composited frame while the settings panel is closed. To keep that
// guarantee bulletproof against however SettingsOverlay.cpp evolves under
// other in-flight work, this owns a fully separate ImGui context, offscreen
// texture, general-queue submission and timeline semaphore from
// SettingsOverlay's -- structurally the same shape (see SettingsOverlay.h's
// own comments for why that shape looks the way it does), just a second,
// independent instance of it, so nothing here can accidentally get coupled
// to the settings panel's own toggle state.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

class CVulkanCmdBuffer;
struct FrameInfo_t;

namespace gamescope
{
	namespace ui { class Registry; }

	// -------------------------------------------------------------------
	// The readout's pure arithmetic (2026-09-05), kept free of ImGui, Vulkan
	// and the file-local state in FpsDisplay.cpp so tests/test_fps_counter.cpp
	// can pin it down on its own. Everything in here is a function of its
	// arguments only.
	// -------------------------------------------------------------------
	namespace fpsmath
	{
		// Smoothing samples the commit count once per second, then glides
		// the shown integer to the new value over 300 ms and holds it for
		// the remaining 700 ms -- the user's own spec, verbatim. Immediate
		// counts over a rolling 100 ms window.
		inline constexpr uint64_t kSmoothingWindowNs = 1000ull * 1000000ull;
		inline constexpr uint64_t kGlideNs           =  300ull * 1000000ull;
		inline constexpr uint64_t kImmediateWindowNs =  100ull * 1000000ull;

		// Frames per second from a commit-count delta over a wall-clock
		// delta. Zero time yields zero rather than a division by zero.
		inline float RateFromCounts( uint64_t ulDeltaCount, uint64_t ulDeltaNs )
		{
			if ( ulDeltaNs == 0 )
				return 0.0f;
			return (float)( (double)ulDeltaCount * 1e9 / (double)ulDeltaNs );
		}

		inline float Smoothstep( float t )
		{
			t = std::clamp( t, 0.0f, 1.0f );
			return t * t * ( 3.0f - 2.0f * t );
		}

		// The value shown `ulElapsedNs` after a glide from `flFrom` to `flTo`
		// began: smoothstep-eased through the 300 ms move, then pinned to
		// `flTo` for the rest of the second (the hold).
		inline float GlideValue( float flFrom, float flTo, uint64_t ulElapsedNs )
		{
			if ( ulElapsedNs >= kGlideNs )
				return flTo;
			const float t = (float)ulElapsedNs / (float)kGlideNs;
			return flFrom + ( flTo - flFrom ) * Smoothstep( t );
		}

		// True while the glide is still moving (the first 300 ms); false in
		// the hold phase. The repaint-timer thread ticks fast only while
		// this is true for the live glide.
		inline bool GlideMoving( uint64_t ulElapsedNs )
		{
			return ulElapsedNs < kGlideNs;
		}

		// How many digit cells the readout's box is sized for: the number's
		// own digit count, never fewer than 3. 0-999 sit in a 3-cell box
		// that never resizes; 1000 grows it to 4, 12345 to 5. Capped at 7
		// so a corrupt sample cannot ask for an absurd box (FpsDisplay.cpp's
		// buffers hold 7 digits).
		//
		// Why not a fixed 4- or 5-cell pin: the normal readout would then
		// sit in a box twice as wide as its digits. Why not the exact
		// count with no floor: a 2-digit reading would shrink the box every
		// time the game dipped below 100, which is the jitter the pin
		// exists to prevent.
		inline int PinnedDigitCount( int nFps )
		{
			nFps = std::max( nFps, 0 );
			int nDigits = 1;
			while ( nFps >= 10 )
			{
				nFps /= 10;
				++nDigits;
			}
			return std::clamp( nDigits, 3, 7 );
		}

		// FpsDisplaySettings::update_mode <-> the Choice row's int. Two
		// modes since 2026-09-05: 0 = Smoothing, 1 = Immediate. The removed
		// "per_second" (and anything unrecognised) maps to Smoothing, which
		// subsumed it, so an existing config loads unchanged.
		inline int UpdateModeToInt( std::string_view sMode )
		{
			return sMode == "immediate" ? 1 : 0;
		}
		inline const char *UpdateModeFromInt( int n )
		{
			return n == 1 ? "immediate" : "smoothing";
		}
	}

	// Called once per paint_all(), on the steamcompmgr thread. Reads
	// gamescope-ritz's fps_display config (loaded lazily on first call) and,
	// when enabled and not currently hidden by "hide above X", draws the
	// readout (game frame rate, backdrop, text-colour treatment per config)
	// into its own offscreen texture and appends a Layer_t for it to
	// *pFrameInfo. A no-op when disabled.
	void FpsDisplay_AddLayer( FrameInfo_t *pFrameInfo );

	// 2026-09-05: pre-pays the readout's one-time costs so its first visible
	// frame does not hitch -- the same lesson as the Shell's startup warm-up
	// in SettingsOverlay.cpp and Notifications::WarmUp(): ImGui 1.92 bakes
	// glyphs lazily, and ImGui_ImplVulkan_UpdateTexture() then does a
	// blocking vkQueueWaitIdle() on the first frame that shows a never-
	// before-drawn glyph. This creates the HUD's ImGui context and texture,
	// draws the digits 0-9 at the configured Hero size in one hidden frame
	// and submits it -- WITHOUT pushing a layer, so nothing reaches the
	// screen and the layer count is untouched. A no-op unless the HUD is
	// enabled, the output size is known, or once it has run. Safe to call
	// from paint_all() before or after FpsDisplay_AddLayer(); intended to be
	// called from the Shell's startup warm-up block.
	void FpsDisplay_WarmUp();

	// Called right after vulkan_composite()/vulkan_screenshot() obtain their
	// compute-queue command buffer, before recording any dispatches that
	// might sample the FPS display's texture -- same cross-queue
	// synchronization role as SettingsOverlay_WaitForRender(), against this
	// feature's own timeline semaphore. A no-op if nothing was rendered this
	// frame.
	void FpsDisplay_WaitForRender( CVulkanCmdBuffer *pComputeCmdBuffer );

	// Issue #22, same contract as SettingsOverlay_CommitReads() -- call once
	// the compute submission is on the queue. No-op when nothing is pending.
	void FpsDisplay_CommitReads();

	// Declares this feature's settings as the E2 `system.hud` area: the
	// master switch, placement (anchor + margins), font size, update mode,
	// hide-above-X, backdrop opacity, text colour and shadow strength -- see
	// this file's header comment and superdoc/features/fps-display.md.
	//
	// This REPLACED FpsDisplay_DrawSettingsPanel(), the six-tab panel issue
	// #59 built and P5 deleted. It is a declaration, not a draw call: it places no pixel,
	// runs at startup rather than per frame, and takes no ImGui context --
	// which is what let the last escape hatch for this area go.
	//
	// Only the SETTINGS half of FpsDisplay.cpp moved. The HUD drawn over the
	// game keeps its own ImGui context, its own offscreen texture and its
	// own submission path, all untouched by the redesign.
	void FpsDisplay_RegisterArea( ui::Registry &reg );
}

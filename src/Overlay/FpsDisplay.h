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
// modes, hide-above-X, a plain backdrop (removed again 2026-09-09), a
// two-way text-colour choice and a lag-spike reaction, a drop shadow -- see
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
#include <cstddef>
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

		// 2026-09-07 margin fix's pure arithmetic (FpsDisplay.cpp's
		// MeasureFpsModule(), "margin fix" comment, and fps-display.md's
		// "Margin" section carry the full reasoning): how far to shift the
		// digits, along one axis, so that the OUTERMOST drawn pixel on the
		// side facing the anchored edge -- glyph ink, or the outline's
		// ink-plus-radius when an outline is drawn -- lands exactly the
		// configured margin's own distance from the screen edge. Nothing
		// else pins it there: the readout's box is invisible (the backdrop
		// that used to fill it is gone, 2026-09-09) and a glyph's ink sits
		// inside its advance cell by its own side bearing.
		//
		// `nSide` is ParsePlacement's own axis numbering: 0 = the near
		// edge (left/top), 2 = the far edge (right/bottom), 1 = centred
		// (no edge to hug, so no shift). `flBearing` is the glyph's own
		// ink offset on this side (MeasureInkExtent()'s left/top, or the
		// numSize-relative right/bottom gap) -- rounded to a whole pixel
		// by the caller, see that comment for why; `flOutlineGeomRadius`
		// is the outline's actual geometric reach (0 when no outline is
		// drawn), which the ink must sit that far inside of so the ring
		// itself lands on the margin.
		//
		// Until 2026-09-09 this also took the drawn-backdrop flag (a
		// backdrop pinned the margin with its own crisp rect edge, so the
		// correction had to be 0 there) and backdrop_padding, which the
		// draw origin added and the box's size subtracted again -- it
		// cancelled out of this expression exactly, which is why dropping
		// it moved no pixel.
		inline float EdgeShift( int nSide, float flBearing, float flOutlineGeomRadius )
		{
			if ( nSide == 1 )
				return 0.0f;
			const float flInset = flBearing - flOutlineGeomRadius;
			return ( nSide == 0 ) ? -flInset : flInset;
		}

		// ---- the visibility floor (2026-09-14) ---------------------------
		//
		// The bearing EdgeShift() cancels used to be read straight off the
		// glyph's metric box (ImFontGlyph::X0/Y0/X1/Y1). That box is the
		// rasteriser's tight bitmap, and its edge row is whatever slice of
		// the outline's curve fell into the last pixel -- for a round '0'
		// (the pinned reference) that is the cap-height / baseline
		// OVERSHOOT, which at some sizes clips one pixel row by only a few
		// percent: at 36 px the top row peaks at 8/255 coverage, at 12 px
		// the bottom row at 15/255, at 18 px the top row at 2/255. Such a
		// row exists in the atlas and in the HUD's own texture, and yet
		// never reaches the screen in the default configuration: ImGui's
		// straight-alpha blend stores the texel as colour x coverage, and
		// the composite's COVERAGE blend then multiplies by the coverage
		// again, so a white row of coverage c lands on a dark game at
		// roughly c^2 -- 8/255 becomes +0.1 of a count and rounds back into
		// the background. The margin was being measured to a row nobody
		// could see, and the digits sat 1 px further from the edge than
		// configured at exactly the sizes whose overshoot phase produced
		// such a row (12, 14 at the bottom; 18, 36 at the top; and the same
		// on the horizontal axis at 26-28, where the '0' bakes an empty
		// left column). See fps-display.md's "Margin" section, 2026-09-14.
		//
		// The rule now: the margin is measured to the first row / column
		// that can actually SHOW -- one whose strongest pixel can change
		// what is on screen by at least 1/16 of full scale over its most
		// favourable background. That one criterion gives a different
		// coverage floor per blend path, because the three paths turn
		// coverage into on-screen change differently:
		//
		//   Fixed digits, no outline  -- white over black through the
		//     coverage blend: encode( decode(c) * c ) >= 16/255, first
		//     true at c = 47/255. (The premultiplied colour is decoded from
		//     sRGB by the sampler and multiplied by the raw alpha.)
		//   An outline (either mode)  -- the outermost drawn pixel is the
		//     black ring, which is straight alpha over the game: a white
		//     pixel darkens by 255 * c, so c >= 16/255.
		//   Inverted digits, no outline -- alphamode.h recovers the
		//     coverage and applies it in LINEAR light, so on black the row
		//     lands at encode( c ): 16/255 is reached at c = 2/255. Any
		//     real ink shows, exactly as the metric box always assumed.
		//
		// tests/test_fps_counter.cpp derives all three from the blend
		// formulae, so a change to either the floors or the shaders that
		// disagrees with the other fails a test rather than moving a pixel.
		inline constexpr int kInkFloorFixed    = 47;
		inline constexpr int kInkFloorOutline  = 16;
		inline constexpr int kInkFloorInverted = 2;

		inline int InkCoverageFloor( bool bInvertedMode, bool bOutline )
		{
			if ( bOutline )
				return kInkFloorOutline;
			return bInvertedMode ? kInkFloorInverted : kInkFloorFixed;
		}

		// The bounding box, in bitmap pixels and half-open, of every pixel
		// in an 8-bit coverage bitmap whose value reaches `nFloor`. `pAlpha`
		// is the top-left coverage byte, `nStride` the byte distance between
		// rows, and `nStep` the byte distance between neighbouring pixels
		// in a row (1 for an Alpha8 atlas, 4 for an RGBA32 one pointed at
		// its alpha byte). `bAny` is false when nothing reaches the floor,
		// and the box is then all zeros. This is the whole of the
		// measurement above; MeasureInkExtent() only supplies the atlas
		// rect of each pinned-reference glyph.
		struct InkBox
		{
			int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
			bool bAny = false;
		};

		inline InkBox ScanInk( const uint8_t *pAlpha, int nWidth, int nHeight, int nStride, int nStep, int nFloor )
		{
			InkBox box;
			if ( !pAlpha || nWidth <= 0 || nHeight <= 0 )
				return box;
			nFloor = std::max( nFloor, 1 ); // a floor of 0 would count the padding as ink
			int x0 = nWidth, y0 = nHeight, x1 = -1, y1 = -1;
			for ( int y = 0; y < nHeight; y++ )
			{
				const uint8_t *pRow = pAlpha + (ptrdiff_t)y * nStride;
				for ( int x = 0; x < nWidth; x++ )
				{
					if ( pRow[(ptrdiff_t)x * nStep] < nFloor )
						continue;
					x0 = std::min( x0, x );
					x1 = std::max( x1, x );
					y0 = std::min( y0, y );
					y1 = std::max( y1, y );
				}
			}
			if ( x1 < 0 )
				return box;
			box.x0 = x0;
			box.y0 = y0;
			box.x1 = x1 + 1;
			box.y1 = y1 + 1;
			box.bAny = true;
			return box;
		}
	}

	// Called once per paint_all(), on the steamcompmgr thread. Reads
	// gamescope-ritz's fps_display config (loaded lazily on first call) and,
	// when enabled and not currently hidden by "hide above X", draws the
	// readout (game frame rate, outline, text-colour treatment per config)
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
	// hide-above-X, text colour and outline size -- see
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

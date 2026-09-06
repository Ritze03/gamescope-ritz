// Compositor-drawn crosshair -- see Crosshair.h for what this is and why
// it draws into the FPS HUD's layer, and superdoc/features/crosshair.md
// for the feature as a whole.
//
// Threading: everything here except Crosshair_NotifyRightButton() runs on
// the steamcompmgr thread (paint_all() -> FpsDisplay_AddLayer() ->
// Crosshair_Draw(); the Shell's setters, which draw on that same thread).
// Crosshair_NotifyRightButton() runs on the wlserver thread and only ever
// touches one atomic plus force_repaint(), which is already safe from any
// thread (the console thread calls it) -- it never reads the config cache.
#include "Crosshair.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "CrosshairMath.h"
#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "log.hpp"
#include "Config/ConfigManager.h"
#include "Config/AppId.h"
#include "UI/Registry.h"

#include "imgui.h"

namespace gamescope
{
	namespace
	{
		// -----------------------------------------------------------------
		// Config: loaded lazily, reloaded whenever the config generation
		// moves (profile applied, override toggled) -- the same shape as
		// FpsDisplay.cpp's own cache, kept separate so neither file reaches
		// into the other's statics.
		// -----------------------------------------------------------------
		bool s_bConfigLoaded = false;
		uint64_t s_ulLoadedGeneration = 0;
		config::Settings s_Settings;

		void EnsureConfigLoaded()
		{
			const uint64_t ulGeneration = config::ConfigGeneration();
			if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
				return;
			s_Settings = config::ResolvedSettings();
			s_ulLoadedGeneration = ulGeneration;
			s_bConfigLoaded = true;
		}

		// Every setter goes through here: write the file (routed to the
		// per-game snapshot when one is active, global.json otherwise) and
		// ask for a frame, so an edit is visible on the very next paint
		// even when the game itself is idle. force_repaint(), not
		// hasRepaint, for the same reason FpsDisplay.cpp gives: it is the
		// one request that reliably reaches a possibly-idle main loop.
		void PersistAndRepaint()
		{
			config::EnqueueRoutedWrite( s_Settings );
			force_repaint();
		}

		// The right button's latest edge: bit 0 = held, the rest = the
		// edge's timestamp in ns with that bit cleared. One word so the
		// render side always sees a state and the time it began together.
		// Written on the wlserver thread, read on the steamcompmgr thread.
		std::atomic<uint64_t> s_ulRightEdge{ 0 };

		// The hide animation's integrator (crosshair::AdvanceHide) --
		// steamcompmgr thread only.
		crosshair::HideAnim s_HideAnim;

		LogScope s_CrosshairLog( "crosshair" );

		// -----------------------------------------------------------------
		// Apply Scaling's stretched raster (Crosshair.h, "two rendering
		// paths"). Everything below is steamcompmgr-thread only.
		// -----------------------------------------------------------------

		// Everything the output raster's PIXELS depend on. When it is
		// unchanged from the last frame the staging buffer is copied again
		// as it is; when it changes the raster is rebuilt on the CPU and
		// re-encoded into the staging buffer. Unlike the 2026-09-05 quad,
		// the hide fade IS in here: there is no tint any more, the fade is
		// baked into the texels (a few thousand of them, for <= 2 s).
		struct RasterKey
		{
			bool bLine = false, bDot = false, bOutline = false;
			int nLength = 0, nWidth = 0, nGap = 0, nDotSize = 0, nOutlineWidth = 0;
			int nLineColor = 0, nDotColor = 0, nOutlineColor = 0;
			float flLineOpacity = 0.0f, flDotOpacity = 0.0f, flOutlineOpacity = 0.0f;
			float flHideGap = 1.0f, flHideLength = 1.0f, flHideAlpha = 1.0f;
			uint32_t uGameW = 0, uGameH = 0;
			float flCenterX = 0.0f, flCenterY = 0.0f, flScaleX = 1.0f, flScaleY = 1.0f;
			bool bReserveInvertMarker = false; // the colours' G nudge (CrosshairFrame) changes the texels
			bool operator==( const RasterKey & ) const = default;
		};

		bool s_bRasterValid = false;       // s_Out matches s_RasterKey
		RasterKey s_RasterKey;
		crosshair::OutputRaster s_Out;     // this frame's stretched raster, output pixels, straight alpha
		bool s_bOutThisFrame = false;      // Crosshair_Draw() produced s_Out this frame; consumed by Crosshair_RecordUpload()
		uint64_t s_ulOutGeneration = 0;    // bumped whenever s_Out's pixels change
		uint64_t s_ulStagedGeneration = 0; // the generation encoded in the staging buffer
		VkFormat s_eStagedFormat = VK_FORMAT_UNDEFINED; // ... and the HUD format it was encoded for

		// The host-visible staging buffer the raster is copied from. Its
		// own, not g_device.uploadBufferData(): that bump allocator is only
		// reset by a device wait, and with the fade baked into the texels
		// an animation frame now re-uploads every frame -- a 4K-scale
		// crosshair at 144 Hz would walk through it. Rewritten only after
		// the caller has drained the previous HUD submission (the one that
		// last read it), so a single buffer is enough.
		struct Staging
		{
			VkBuffer buffer = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			uint8_t *pMapped = nullptr;
			VkDeviceSize ulSize = 0;
		};
		Staging s_Staging;

		void DestroyStaging()
		{
			if ( s_Staging.pMapped )
				g_device.vk.UnmapMemory( g_device.device(), s_Staging.memory );
			if ( s_Staging.buffer != VK_NULL_HANDLE )
				g_device.vk.DestroyBuffer( g_device.device(), s_Staging.buffer, nullptr );
			if ( s_Staging.memory != VK_NULL_HANDLE )
				g_device.vk.FreeMemory( g_device.device(), s_Staging.memory, nullptr );
			s_Staging = Staging{};
			s_ulStagedGeneration = 0;
			s_eStagedFormat = VK_FORMAT_UNDEFINED;
		}

		// Grows the staging buffer to at least ulBytes (never shrinks;
		// rounded up so an animation that wobbles the footprint does not
		// re-allocate every frame). Only valid after the previous HUD
		// submission has been drained -- see Crosshair_RecordUpload().
		bool EnsureStaging( VkDeviceSize ulBytes )
		{
			if ( s_Staging.buffer != VK_NULL_HANDLE && s_Staging.ulSize >= ulBytes )
				return true;
			DestroyStaging();

			constexpr VkDeviceSize kMinBytes = 64u * 1024u;
			VkDeviceSize ulSize = kMinBytes;
			while ( ulSize < ulBytes )
				ulSize *= 2;

			VkBufferCreateInfo bufferInfo = {
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = ulSize,
				.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			};
			VkResult res = g_device.vk.CreateBuffer( g_device.device(), &bufferInfo, nullptr, &s_Staging.buffer );
			if ( res != VK_SUCCESS )
			{
				s_CrosshairLog.errorf( "vkCreateBuffer failed for the scaled-crosshair staging buffer (%d)", (int)res );
				s_Staging = Staging{};
				return false;
			}
			VkMemoryRequirements memReq;
			g_device.vk.GetBufferMemoryRequirements( g_device.device(), s_Staging.buffer, &memReq );
			const int32_t nMemType = g_device.findMemoryType( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memReq.memoryTypeBits );
			if ( nMemType < 0 )
			{
				s_CrosshairLog.errorf( "no host-visible memory type for the scaled-crosshair staging buffer" );
				DestroyStaging();
				return false;
			}
			VkMemoryAllocateInfo allocInfo = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize = memReq.size,
				.memoryTypeIndex = (uint32_t)nMemType,
			};
			res = g_device.vk.AllocateMemory( g_device.device(), &allocInfo, nullptr, &s_Staging.memory );
			if ( res != VK_SUCCESS )
			{
				s_CrosshairLog.errorf( "vkAllocateMemory failed for the scaled-crosshair staging buffer (%d)", (int)res );
				DestroyStaging();
				return false;
			}
			g_device.vk.BindBufferMemory( g_device.device(), s_Staging.buffer, s_Staging.memory, 0 );
			void *pMapped = nullptr;
			res = g_device.vk.MapMemory( g_device.device(), s_Staging.memory, 0, VK_WHOLE_SIZE, 0, &pMapped );
			if ( res != VK_SUCCESS )
			{
				s_CrosshairLog.errorf( "vkMapMemory failed for the scaled-crosshair staging buffer (%d)", (int)res );
				DestroyStaging();
				return false;
			}
			s_Staging.pMapped = (uint8_t *)pMapped;
			s_Staging.ulSize = ulSize;
			return true;
		}

		// Bytes per texel of the HUD texture formats FpsDisplay.cpp's
		// ResolveTextureFormat() can hand us; 0 for anything else.
		uint32_t HudBytesPerTexel( VkFormat eFormat )
		{
			switch ( eFormat )
			{
				case VK_FORMAT_B8G8R8A8_UNORM:     return 4;
				case VK_FORMAT_R16G16B16A16_UNORM: return 8;
				default:                           return 0;
			}
		}

		// Encodes s_Out into the staging buffer in the HUD texture's
		// format. B8G8R8A8: an Argb IS the little-endian texel. R16G16B16A16:
		// each 8-bit channel spread to 16 (x 257), R first.
		void EncodeStaging( VkFormat eFormat )
		{
			const size_t n = s_Out.px.size();
			if ( eFormat == VK_FORMAT_B8G8R8A8_UNORM )
			{
				memcpy( s_Staging.pMapped, s_Out.px.data(), n * sizeof( crosshair::Argb ) );
			}
			else
			{
				uint16_t *pDst = (uint16_t *)s_Staging.pMapped;
				for ( size_t i = 0; i < n; i++ )
				{
					const crosshair::Argb v = s_Out.px[i];
					pDst[i * 4 + 0] = (uint16_t)( ( ( v >> 16 ) & 0xFF ) * 257u );
					pDst[i * 4 + 1] = (uint16_t)( ( ( v >> 8 ) & 0xFF ) * 257u );
					pDst[i * 4 + 2] = (uint16_t)( ( v & 0xFF ) * 257u );
					pDst[i * 4 + 3] = (uint16_t)( ( ( v >> 24 ) & 0xFF ) * 257u );
				}
			}
		}

		ImU32 PackColor( int nRgb, float flAlpha )
		{
			const int a = (int)std::lround( std::clamp( flAlpha, 0.0f, 1.0f ) * 255.0f );
			return IM_COL32( ( nRgb >> 16 ) & 0xFF, ( nRgb >> 8 ) & 0xFF, nRgb & 0xFF, a );
		}

		// CrosshairFrame::bReserveInvertMarker: a G of exactly 0 is the
		// Inverted HUD's "this texel is a digit" marker (alphamode.h), so a
		// crosshair colour with no green becomes G == 1 while sharing that
		// layer. One count, invisible; every other colour is untouched.
		int ReserveInvertMarker( int nRgb, bool bReserve )
		{
			if ( bReserve && ( ( nRgb >> 8 ) & 0xFF ) == 0 )
				return nRgb | 0x000100;
			return nRgb;
		}

		// Hide-mode Choice <-> stored string, same helper shape as
		// FpsDisplay.cpp's UpdateModeToInt()/UpdateModeFromInt().
		int HideModeToInt( const std::string &s )
		{
			switch ( crosshair::ParseHideMode( s ) )
			{
				case crosshair::HideMode::Focus:  return 1;
				case crosshair::HideMode::Shrink: return 2;
				default:                          return 0;
			}
		}
		const char *HideModeFromInt( int n )
		{
			switch ( n )
			{
				case 1: return crosshair::HideModeKey( crosshair::HideMode::Focus );
				case 2: return crosshair::HideModeKey( crosshair::HideMode::Shrink );
				default: return crosshair::HideModeKey( crosshair::HideMode::Fade );
			}
		}
		constexpr ui::Option kHideModeOptions[] = {
			{ 0, "Fade out" },
			{ 1, "Focus" },
			{ 2, "Shrink" },
		};
	}

	bool Crosshair_IsEnabled()
	{
		EnsureConfigLoaded();
		return s_Settings.crosshair.enabled;
	}

	void Crosshair_NotifyRightButton( bool bPressed )
	{
		const uint64_t ulNow = get_time_in_nanos() & ~1ull;
		if ( bPressed )
		{
			// Only the FIRST press starts the clock; a repeated press event
			// for a button already held (some backends re-report) must not
			// restart a hide that is already under way. wlserver is the
			// only writer, so a plain load/store pair is race-free.
			if ( ( s_ulRightEdge.load( std::memory_order_relaxed ) & 1ull ) == 0 )
				s_ulRightEdge.store( ulNow | 1ull );
		}
		else
		{
			// The render side decides what a release means: with "Animate
			// back" the hide runs backwards from where it is, without it
			// the crosshair is restored at once (crosshair::AdvanceHide).
			// Never read the config cache here -- wrong thread.
			s_ulRightEdge.store( ulNow );
		}
		// The press/release itself is a state change with no game frame
		// attached (an idle menu, a paused game): ask for one so the
		// animation starts / the crosshair comes back without waiting for
		// the game to commit. Crosshair_Draw() keeps the frames coming
		// while the animation is moving.
		force_repaint();
	}

	namespace
	{
		// Apply Scaling OFF (and the fallback for ON when the game's size
		// is unknown): every primitive is a whole-pixel rect at output
		// resolution, AA off. Unchanged from before the raster path existed
		// -- its output was measured pixel-for-pixel and must not move.
		void DrawPixelPath( ImDrawList *pDrawList, const config::CrosshairSettings &c, const crosshair::Style &st,
		                    const crosshair::Frame &fr, const crosshair::HideState &hs, bool bReserveMarker )
		{
			const crosshair::Shape shape = crosshair::Build( st, fr, hs );
			if ( shape.Empty() )
				return;

			// 1px mode: every primitive is an axis-aligned filled rect on whole
			// pixel coordinates (CrosshairMath.h), and anti-aliased fill is
			// switched off for exactly these draws, so a 1px line is one solid
			// pixel with no half-alpha neighbours. Restored afterwards -- the
			// FPS readout in the same draw list wants its glyphs antialiased.
			const ImDrawListFlags savedFlags = pDrawList->Flags;
			pDrawList->Flags &= ~( ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines );

			auto Fill = [&]( const std::vector<crosshair::IRect> &rects, ImU32 col )
			{
				if ( ( col & IM_COL32_A_MASK ) == 0 )
					return;
				for ( const crosshair::IRect &r : rects )
					pDrawList->AddRectFilled( ImVec2( (float)r.x0, (float)r.y0 ),
					                          ImVec2( (float)r.x1, (float)r.y1 ), col, 0.0f );
			};

			// Outline first (it is already computed as the ring OUTSIDE every
			// fill, so order only matters where the dot overlaps an arm), then
			// the arms, then the dot on top.
			if ( c.outline_enabled )
				Fill( shape.outline, PackColor( ReserveInvertMarker( c.outline_color, bReserveMarker ), c.outline_opacity * hs.flAlpha ) );
			Fill( shape.lines, PackColor( ReserveInvertMarker( c.line_color, bReserveMarker ), c.line_opacity * hs.flAlpha ) );
			Fill( shape.dot, PackColor( ReserveInvertMarker( c.dot_color, bReserveMarker ), c.dot_opacity * hs.flAlpha ) );

			pDrawList->Flags = savedFlags;
		}

		// Apply Scaling ON: the crosshair Build() at the GAME's resolution,
		// rasterised at that resolution (its own bounding box + 1 texel
		// margin), then stretched to the output by a CPU bilinear resample
		// at the game's per-axis scale (crosshair::ResampleToOutput). The
		// result is held in s_Out for Crosshair_RecordUpload() to copy into
		// the HUD texture; nothing is drawn through ImGui. Always succeeds
		// (the copy may still be skipped later if the staging buffer
		// cannot be made -- then the crosshair is simply absent for that
		// frame, and an error is logged once).
		void ComputeRasterPath( const config::CrosshairSettings &c, const crosshair::Style &st,
		                        const CrosshairFrame &frame, const crosshair::HideState &hs )
		{
			RasterKey key;
			key.bLine = c.line_enabled; key.bDot = c.dot_enabled; key.bOutline = c.outline_enabled;
			key.nLength = c.line_length; key.nWidth = c.line_width; key.nGap = c.line_gap;
			key.nDotSize = c.dot_size; key.nOutlineWidth = c.outline_width;
			key.nLineColor = c.line_color; key.nDotColor = c.dot_color; key.nOutlineColor = c.outline_color;
			key.flLineOpacity = c.line_opacity; key.flDotOpacity = c.dot_opacity; key.flOutlineOpacity = c.outline_opacity;
			key.flHideGap = hs.flGap; key.flHideLength = hs.flLength; key.flHideAlpha = hs.flAlpha;
			key.uGameW = frame.uGameWidth; key.uGameH = frame.uGameHeight;
			key.flCenterX = frame.flCenterX; key.flCenterY = frame.flCenterY;
			key.flScaleX = frame.flGamePixelScaleX; key.flScaleY = frame.flGamePixelScaleY;
			key.bReserveInvertMarker = frame.bReserveInvertMarker;

			if ( !s_bRasterValid || !( key == s_RasterKey ) )
			{
				// Rebuild: the exact drawing code, in game pixels, at the
				// configured opacity times the hide fade -- exactly the
				// alpha the pixel path would give PackColor().
				const crosshair::Frame gf = crosshair::GameFrame( frame.uGameWidth, frame.uGameHeight );
				crosshair::HideState hsRaster = hs;
				hsRaster.flAlpha = 1.0f;
				const crosshair::Shape shape = crosshair::Build( st, gf, hsRaster );
				const crosshair::IRect texRect = crosshair::RasterRect( shape );
				const float flFade = std::clamp( hs.flAlpha, 0.0f, 1.0f );
				const std::vector<crosshair::Argb> gamePx = crosshair::Rasterize( shape, texRect,
					crosshair::PackArgb( ReserveInvertMarker( c.outline_color, frame.bReserveInvertMarker ), ( c.outline_enabled ? c.outline_opacity : 0.0f ) * flFade ),
					crosshair::PackArgb( ReserveInvertMarker( c.line_color, frame.bReserveInvertMarker ), c.line_opacity * flFade ),
					crosshair::PackArgb( ReserveInvertMarker( c.dot_color, frame.bReserveInvertMarker ), c.dot_opacity * flFade ) );

				crosshair::Frame fr;
				fr.flCenterX = frame.flCenterX;
				fr.flCenterY = frame.flCenterY;
				fr.flScaleX = frame.flGamePixelScaleX;
				fr.flScaleY = frame.flGamePixelScaleY;
				s_Out = texRect.Empty() ? crosshair::OutputRaster{}
				                        : crosshair::ResampleToOutput( gamePx, texRect, frame.uGameWidth, frame.uGameHeight, fr );
				s_RasterKey = key;
				s_bRasterValid = true;
				s_ulOutGeneration++;
			}
			s_bOutThisFrame = !s_Out.Empty();
		}
	}

	bool Crosshair_Draw( ImDrawList *pDrawList, const CrosshairFrame &frame, uint64_t ulNowNs )
	{
		EnsureConfigLoaded();
		const config::CrosshairSettings &c = s_Settings.crosshair;
		if ( !c.enabled || !pDrawList )
			return false;

		s_bOutThisFrame = false;

		crosshair::HideState hs;
		bool bAnimating = false;
		if ( c.hide_on_right_click )
		{
			const uint64_t ulEdge = s_ulRightEdge.load( std::memory_order_relaxed );
			const bool bHeld = ( ulEdge & 1ull ) != 0;
			const float f = crosshair::AdvanceHide( s_HideAnim, bHeld, ulEdge & ~1ull, ulNowNs,
			                                        c.hide_time_ms, c.hide_animate_back );
			hs = crosshair::EvaluateHide( crosshair::ParseHideMode( c.hide_mode ), f,
			                              crosshair::ShrinkSplit( (float)c.line_gap, (float)c.line_length ) );
			// Fully hidden, or fully back, is static again: no more forced
			// frames until the next edge.
			bAnimating = crosshair::HideAnimating( s_HideAnim );
		}
		else
		{
			s_HideAnim = crosshair::HideAnim{};
		}
		if ( hs.flAlpha <= 0.0f )
			return bAnimating;

		crosshair::Style st;
		st.bLine = c.line_enabled;
		st.flLength = (float)c.line_length;
		st.flWidth = (float)c.line_width;
		st.flGap = (float)c.line_gap;
		st.bDot = c.dot_enabled;
		st.flDotSize = (float)c.dot_size;
		st.bOutline = c.outline_enabled;
		st.flOutlineWidth = (float)c.outline_width;

		// Apply Scaling on, and the game's size known: the raster path --
		// crisp at game resolution, stretched linearly with the game, and
		// copied into the HUD texture by Crosshair_RecordUpload().
		if ( c.apply_scaling && frame.uGameWidth > 0 && frame.uGameHeight > 0 )
		{
			ComputeRasterPath( c, st, frame, hs );
			return bAnimating;
		}

		// Apply Scaling off: every size is an output pixel and the crosshair
		// is square. (Fallback for ON: sizes are game pixels stretched per
		// axis and snapped -- the pre-raster behaviour.)
		crosshair::Frame fr;
		fr.flCenterX = frame.flCenterX;
		fr.flCenterY = frame.flCenterY;
		fr.flScaleX = c.apply_scaling ? frame.flGamePixelScaleX : 1.0f;
		fr.flScaleY = c.apply_scaling ? frame.flGamePixelScaleY : 1.0f;
		DrawPixelPath( pDrawList, c, st, fr, hs, frame.bReserveInvertMarker );
		return bAnimating;
	}

	bool Crosshair_RecordUpload( CVulkanCmdBuffer *pCmdBuffer, CVulkanTexture *pHudTexture )
	{
		// One frame's worth: Crosshair_Draw() sets this every frame it has
		// a stretched raster, so a frame without one (crosshair off, Apply
		// Scaling off, hidden) records nothing and the pass clears.
		if ( !s_bOutThisFrame || !pCmdBuffer || !pHudTexture || s_Out.Empty() )
			return false;
		s_bOutThisFrame = false;

		const VkFormat eFormat = pHudTexture->format();
		const uint32_t uBpp = HudBytesPerTexel( eFormat );
		if ( uBpp == 0 )
		{
			static bool s_bWarned = false;
			if ( !s_bWarned )
				s_CrosshairLog.errorf( "HUD texture format %d is not one the scaled crosshair can encode; scaled crosshair off", (int)eFormat );
			s_bWarned = true;
			return false;
		}

		// Clip the footprint to the texture; the buffer keeps the full
		// footprint's row length and the copy starts at the clipped corner.
		const int ow = s_Out.rect.x1 - s_Out.rect.x0, oh = s_Out.rect.y1 - s_Out.rect.y0;
		const int cx0 = std::max( s_Out.rect.x0, 0 ), cy0 = std::max( s_Out.rect.y0, 0 );
		const int cx1 = std::min( s_Out.rect.x1, (int)pHudTexture->width() );
		const int cy1 = std::min( s_Out.rect.y1, (int)pHudTexture->height() );
		if ( cx1 <= cx0 || cy1 <= cy0 )
			return false;

		// The previous HUD submission -- the last reader of the staging
		// buffer -- has been drained by the caller, so it can be rewritten
		// (or replaced) here.
		const VkDeviceSize ulBytes = (VkDeviceSize)ow * (VkDeviceSize)oh * uBpp;
		if ( !EnsureStaging( ulBytes ) )
			return false;
		if ( s_ulStagedGeneration != s_ulOutGeneration || s_eStagedFormat != eFormat )
		{
			EncodeStaging( eFormat );
			s_ulStagedGeneration = s_ulOutGeneration;
			s_eStagedFormat = eFormat;
		}

		// Record: clear the whole texture, copy the raster's clipped
		// footprint into it, and hand it to the render pass. The first
		// barrier's source stage is COLOR_ATTACHMENT_OUTPUT -- the stage
		// the caller's semaphore wait (Issue #22, the previous composite's
		// read of this texture) is attached to -- so the clear cannot start
		// before that read has finished; TRANSFER on its own is not in that
		// wait mask. Layout stays GENERAL throughout (the caller's initial
		// barrier put it there). The clear and the copy both write the
		// image, hence the transfer->transfer barrier between them.
		VkCommandBuffer raw = pCmdBuffer->rawBuffer();
		const VkImageSubresourceRange range = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		};
		auto ImageBarrier = [&]( VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess )
		{
			VkImageMemoryBarrier barrier = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.srcAccessMask = srcAccess,
				.dstAccessMask = dstAccess,
				.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
				.newLayout = VK_IMAGE_LAYOUT_GENERAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = pHudTexture->vkImage(),
				.subresourceRange = range,
			};
			g_device.vk.CmdPipelineBarrier( raw, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier );
		};

		ImageBarrier( VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
		              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
		const VkClearColorValue clear = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
		g_device.vk.CmdClearColorImage( raw, pHudTexture->vkImage(), VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range );
		ImageBarrier( VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
		const VkBufferImageCopy region = {
			.bufferOffset = ( (VkDeviceSize)( cy0 - s_Out.rect.y0 ) * (VkDeviceSize)ow + (VkDeviceSize)( cx0 - s_Out.rect.x0 ) ) * uBpp,
			.bufferRowLength = (uint32_t)ow,
			.bufferImageHeight = (uint32_t)oh,
			.imageSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.layerCount = 1,
			},
			.imageOffset = { cx0, cy0, 0 },
			.imageExtent = { (uint32_t)( cx1 - cx0 ), (uint32_t)( cy1 - cy0 ), 1 },
		};
		g_device.vk.CmdCopyBufferToImage( raw, s_Staging.buffer, pHudTexture->vkImage(), VK_IMAGE_LAYOUT_GENERAL, 1, &region );
		ImageBarrier( VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );
		return true;
	}

	// -------------------------------------------------------------------
	// The settings area: system.crosshair, right after the HUD's own area
	// in the rail. Groups in the user's own order -- Dot, Line, Outline,
	// Auto-hide, Scaling (Dot ahead of Line since 2026-09-06, request #15)
	// -- under a master switch. Every dependent row is greyed with a
	// reason while its element (or the whole crosshair) is off; the master
	// switch itself is never gated (SPEC §3.13).
	// -------------------------------------------------------------------
	void Crosshair_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "system.crosshair", "Crosshair", ui::Section::System );

		a.Keywords( "crosshair reticle aim dot sight overlay" );
		a.Summary( []
		{
			EnsureConfigLoaded();
			return s_Settings.crosshair.enabled ? std::string( "on" ) : std::string( "off" );
		} );

		auto On = []{ EnsureConfigLoaded(); return s_Settings.crosshair.enabled; };
		auto LineOn = []{ EnsureConfigLoaded(); return s_Settings.crosshair.enabled && s_Settings.crosshair.line_enabled; };
		auto DotOn = []{ EnsureConfigLoaded(); return s_Settings.crosshair.enabled && s_Settings.crosshair.dot_enabled; };
		auto OutlineOn = []{ EnsureConfigLoaded(); return s_Settings.crosshair.enabled && s_Settings.crosshair.outline_enabled; };
		auto HideOn = []{ EnsureConfigLoaded(); return s_Settings.crosshair.enabled && s_Settings.crosshair.hide_on_right_click; };
		constexpr const char *kOffReason = "the crosshair is off";
		constexpr const char *kLineOffReason = "the line is off";
		constexpr const char *kDotOffReason = "the dot is off";
		constexpr const char *kOutlineOffReason = "the outline is off";
		constexpr const char *kHideOffReason = "auto-hide is off";

		using S = config::CrosshairSettings;
		#define CROSSHAIR_BIND( type, field ) \
			ui::AnyBind::Of<type>( \
				[]{ EnsureConfigLoaded(); return (type)s_Settings.crosshair.field; }, \
				[]( type v ) { EnsureConfigLoaded(); s_Settings.crosshair.field = v; PersistAndRepaint(); } )

		// GROUP DECLARATION ORDER IS NOT THE READING ORDER (2026-09-06,
		// request #2, requests-2026-09-07.md). The user asked for an exact
		// two-column split -- left Crosshair/Line/Dot, right
		// Outline/Auto-hide/Scaling -- but Shell.cpp's sheet has no per-group
		// column API: DrawSheetBody() packs whole groups into columns
		// greedily by running weight (shortest column gets the next group),
		// in the order they were registered, and a group's ROWS always
		// render in that same registration order within whichever column it
		// lands in. So the six groups below are declared Crosshair, Outline,
		// Line, Auto-hide, Dot, Scaling -- an order chosen by hand-simulating
		// the packer against each group's row count (Crosshair 1, Outline 4,
		// Line 6, Auto-hide 4, Dot 4, Scaling 1 rows; a Composite colour row
		// counts as 2) so it lands Crosshair/Line/Dot in column 0 and
		// Outline/Auto-hide/Scaling in column 1, each in the requested
		// top-to-bottom order. This is fragile to future row-count changes
		// in any one group (adding or removing a row can tip the greedy
		// balance and silently move a group to the other column) -- verify
		// the two-column split with a screenshot after touching any group's
		// row count, don't assume this order still holds.
		// =================================================================
		//  Crosshair
		// =================================================================
		a.Group( "Crosshair" );

		a.Switch( "crosshair.enabled", "Show crosshair", CROSSHAIR_BIND( bool, enabled ) )
			.Help( "Draws a crosshair over the middle of the game. It is drawn by gamescope, not "
			       "the game, so it stays sharp even when frame generation is smearing the "
			       "game's own crosshair, and it stays up after you close this menu." )
			.Default( S{}.enabled )
			.Keywords( "crosshair show enable reticle aim" );

		// =================================================================
		//  Outline
		// =================================================================
		a.Group( "Outline" );

		a.Switch( "crosshair.outline", "Show outline", CROSSHAIR_BIND( bool, outline_enabled ) )
			.Key( "crosshair.outline_enabled" )
			.Help( "A border drawn around the arms and the dot, just outside them, so the "
			       "crosshair stays visible over bright or busy scenes." )
			.Default( S{}.outline_enabled )
			.Keywords( "outline border stroke edge enable show" )
			.DisabledUnless( On, kOffReason );

		a.Slider( "crosshair.outline_width", "Width", CROSSHAIR_BIND( int, outline_width ) )
			.Help( "How thick the outline is, in pixels." )
			.Range( 1.0f, 8.0f ).Step( 1.0f ).Unit( "px" )
			.Default( S{}.outline_width )
			.Keywords( "outline width thickness" )
			.DisabledUnless( OutlineOn, kOutlineOffReason );

		a.Slider( "crosshair.outline_opacity", "Opacity", CROSSHAIR_BIND( float, outline_opacity ) )
			.Help( "How solid the outline is. All the way down makes it fully transparent." )
			.Range( 0.0f, 1.0f ).Step( 0.05f )
			.Default( S{}.outline_opacity )
			.Keywords( "outline opacity transparency alpha" )
			.DisabledUnless( OutlineOn, kOutlineOffReason );

		a.Composite( "crosshair.outline_color", "Colour", ui::CompositeKind::Color,
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.crosshair.outline_color; },
				[]( int nPacked ) { EnsureConfigLoaded(); s_Settings.crosshair.outline_color = nPacked & 0xFFFFFF; PersistAndRepaint(); } ) )
			.Help( "Colour of the outline." )
			.Default( S{}.outline_color )
			.Keywords( "outline colour color tint rgb" )
			.DisabledUnless( OutlineOn, kOutlineOffReason );

		// =================================================================
		//  Line
		// =================================================================
		a.Group( "Line" );

		a.Switch( "crosshair.line", "Show lines", CROSSHAIR_BIND( bool, line_enabled ) )
			.Key( "crosshair.line_enabled" )
			.Help( "The four arms of the crosshair. Turn them off for a dot-only crosshair." )
			.Default( S{}.line_enabled )
			.Keywords( "line arms enable show" )
			.DisabledUnless( On, kOffReason );

		a.Slider( "crosshair.line_length", "Length", CROSSHAIR_BIND( int, line_length ) )
			.Help( "How long each arm is, in pixels. 1 is a single pixel." )
			.Range( 1.0f, 64.0f ).Step( 1.0f ).Unit( "px" )
			.Default( S{}.line_length )
			.Keywords( "line length size long short" )
			.DisabledUnless( LineOn, kLineOffReason );

		a.Slider( "crosshair.line_width", "Width", CROSSHAIR_BIND( int, line_width ) )
			.Help( "How thick each arm is, in pixels. 1 gives an exactly one-pixel line with no "
			       "soft edges." )
			.Range( 1.0f, 16.0f ).Step( 1.0f ).Unit( "px" )
			.Default( S{}.line_width )
			.Keywords( "line width thickness thin thick 1px" )
			.DisabledUnless( LineOn, kLineOffReason );

		a.Slider( "crosshair.line_gap", "Gap", CROSSHAIR_BIND( int, line_gap ) )
			.Help( "How far each arm starts from the centre, in pixels. 0 joins the arms into a "
			       "solid plus." )
			.Range( 0.0f, 64.0f ).Step( 1.0f ).Unit( "px" )
			.ZeroMeans( "None" )
			.Default( S{}.line_gap )
			.Keywords( "line gap spacing centre center distance" )
			.DisabledUnless( LineOn, kLineOffReason );

		a.Composite( "crosshair.line_color", "Colour", ui::CompositeKind::Color,
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.crosshair.line_color; },
				[]( int nPacked ) { EnsureConfigLoaded(); s_Settings.crosshair.line_color = nPacked & 0xFFFFFF; PersistAndRepaint(); } ) )
			.Help( "Colour of the arms." )
			.Default( S{}.line_color )
			.Keywords( "line colour color tint rgb" )
			.DisabledUnless( LineOn, kLineOffReason );

		a.Slider( "crosshair.line_opacity", "Opacity", CROSSHAIR_BIND( float, line_opacity ) )
			.Help( "How solid the arms are. All the way down makes them fully transparent." )
			.Range( 0.0f, 1.0f ).Step( 0.05f )
			.Default( S{}.line_opacity )
			.Keywords( "line opacity transparency alpha see-through" )
			.DisabledUnless( LineOn, kLineOffReason );

		// =================================================================
		//  Auto-hide
		// =================================================================
		a.Group( "Auto-hide" );

		a.Switch( "crosshair.hide", "Hide while holding right-click", CROSSHAIR_BIND( bool, hide_on_right_click ) )
			.Key( "crosshair.hide_on_right_click" )
			.Help( "Hides the crosshair while the right mouse button is held -- aiming down "
			       "sights, in most games -- and brings it back the instant you let go. Only a "
			       "right-click that reaches the game counts; clicks inside this menu never do." )
			.Default( S{}.hide_on_right_click )
			.Keywords( "hide auto autohide right click mouse button aim ads scope" )
			.DisabledUnless( On, kOffReason );

		a.Choice( "crosshair.hide_mode", "Hide mode",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return HideModeToInt( s_Settings.crosshair.hide_mode ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.crosshair.hide_mode = HideModeFromInt( n ); PersistAndRepaint(); } ),
			kHideModeOptions, std::size( kHideModeOptions ) )
			.Help( "How the crosshair goes away. Fade out simply turns transparent. Focus closes "
			       "the gap first, then fades. Shrink closes the gap first, then shrinks the arms "
			       "and the dot to nothing." )
			.Default( 0 )
			.Keywords( "hide mode fade focus shrink animation" )
			.DisabledUnless( HideOn, kHideOffReason );

		a.Slider( "crosshair.hide_time", "Time to hide", CROSSHAIR_BIND( int, hide_time_ms ) )
			.Key( "crosshair.hide_time_ms" )
			.Help( "How long the hide takes from the moment you press, in milliseconds. 0 hides "
			       "at once. With Animate back on, coming back takes the same time." )
			.Range( 0.0f, 2000.0f ).Step( 10.0f ).Unit( "ms" )
			.ZeroMeans( "Instant" )
			.Default( S{}.hide_time_ms )
			.Keywords( "hide time duration milliseconds speed" )
			.DisabledUnless( HideOn, kHideOffReason );

		a.Switch( "crosshair.hide_animate_back", "Animate back", CROSSHAIR_BIND( bool, hide_animate_back ) )
			.Help( "When you let go, plays the hide animation backwards from wherever it was, at "
			       "the same speed, instead of the crosshair popping straight back. Off brings it "
			       "back instantly." )
			.Default( S{}.hide_animate_back )
			.Keywords( "hide animate back reverse release restore pop" )
			.DisabledUnless( HideOn, kHideOffReason );

		// =================================================================
		//  Dot
		// =================================================================
		a.Group( "Dot" );

		a.Switch( "crosshair.dot", "Show dot", CROSSHAIR_BIND( bool, dot_enabled ) )
			.Key( "crosshair.dot_enabled" )
			.Help( "A small square in the exact centre, on its own or inside the arms' gap." )
			.Default( S{}.dot_enabled )
			.Keywords( "dot centre center point enable show" )
			.DisabledUnless( On, kOffReason );

		a.Slider( "crosshair.dot_size", "Size", CROSSHAIR_BIND( int, dot_size ) )
			.Help( "The dot's width and height, in pixels. 1 is a single pixel." )
			.Range( 1.0f, 16.0f ).Step( 1.0f ).Unit( "px" )
			.Default( S{}.dot_size )
			.Keywords( "dot size big small" )
			.DisabledUnless( DotOn, kDotOffReason );

		a.Composite( "crosshair.dot_color", "Colour", ui::CompositeKind::Color,
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.crosshair.dot_color; },
				[]( int nPacked ) { EnsureConfigLoaded(); s_Settings.crosshair.dot_color = nPacked & 0xFFFFFF; PersistAndRepaint(); } ) )
			.Help( "Colour of the dot." )
			.Default( S{}.dot_color )
			.Keywords( "dot colour color tint rgb" )
			.DisabledUnless( DotOn, kDotOffReason );

		a.Slider( "crosshair.dot_opacity", "Opacity", CROSSHAIR_BIND( float, dot_opacity ) )
			.Help( "How solid the dot is. All the way down makes it fully transparent." )
			.Range( 0.0f, 1.0f ).Step( 0.05f )
			.Default( S{}.dot_opacity )
			.Keywords( "dot opacity transparency alpha" )
			.DisabledUnless( DotOn, kDotOffReason );

		// =================================================================
		//  Scaling
		// =================================================================
		a.Group( "Scaling" );

		a.Switch( "crosshair.apply_scaling", "Apply scaling", CROSSHAIR_BIND( bool, apply_scaling ) )
			.Help( "Off: sizes are screen pixels and the crosshair is always square, drawn "
			       "pixel-sharp. On: sizes are game pixels and the crosshair is stretched exactly "
			       "like the game is, with the same softly filtered edges -- a 4:3 game stretched "
			       "to a 16:9 screen gets a wider crosshair, the way a stretched in-game one looks." )
			.Default( S{}.apply_scaling )
			.Keywords( "scaling stretch aspect ratio 4:3 game pixels resolution" )
			.DisabledUnless( On, kOffReason );

		#undef CROSSHAIR_BIND
	}
}

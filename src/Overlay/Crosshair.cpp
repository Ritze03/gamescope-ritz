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
#include "backends/imgui_impl_vulkan.h"

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
			s_Settings = config::ResolveEffective( config::SessionAppId() );
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

		// "Right button held since <ns>", 0 while not held. Written on the
		// wlserver thread, read on the steamcompmgr thread.
		std::atomic<uint64_t> s_ulRightPressNs{ 0 };

		LogScope s_CrosshairLog( "crosshair" );

		// -----------------------------------------------------------------
		// Apply Scaling's game-resolution raster (Crosshair.h, "two
		// rendering paths"). Everything below is steamcompmgr-thread only.
		// -----------------------------------------------------------------

		// Everything the raster's PIXELS depend on. When it is unchanged
		// from the last frame the texture is simply drawn again; when it
		// changes the raster is rebuilt on the CPU and re-uploaded. Fade
		// (HideState::flAlpha) is deliberately NOT in here -- it is applied
		// as the quad's tint, so a fade re-uploads nothing. Focus/Shrink
		// move the gap/length, which change the pixels, so those do rebuild
		// per animation frame (a few hundred texels, for <= 2 s).
		struct RasterKey
		{
			bool bLine = false, bDot = false, bOutline = false;
			int nLength = 0, nWidth = 0, nGap = 0, nDotSize = 0, nOutlineWidth = 0;
			int nLineColor = 0, nDotColor = 0, nOutlineColor = 0;
			float flLineOpacity = 0.0f, flDotOpacity = 0.0f, flOutlineOpacity = 0.0f;
			float flHideGap = 1.0f, flHideLength = 1.0f;
			uint32_t uGameW = 0, uGameH = 0;
			bool operator==( const RasterKey & ) const = default;
		};

		struct RasterTexture
		{
			OwningRc<CVulkanTexture> pTex;
			VkDescriptorSet descriptorSet = VK_NULL_HANDLE; // ImGui's handle onto pTex, in the HUD context
			uint32_t uWidth = 0, uHeight = 0;
		};

		bool s_bRasterValid = false;
		RasterKey s_RasterKey;
		crosshair::IRect s_RasterRect;           // game px footprint of the texture
		std::vector<crosshair::Argb> s_RasterPixels;
		bool s_bRasterUploadPending = false;
		// Which texture object the current s_RasterPixels were last copied
		// into; a re-created texture with an unchanged key still needs them.
		const CVulkanTexture *s_RasterUploadedTex = nullptr;
		RasterTexture s_Raster;
		// Textures whose descriptor set the previous HUD submission may
		// still be reading; freed by Crosshair_RecordUpload() once that
		// submission has been drained. In practice 0 or 1 entries.
		std::vector<RasterTexture> s_RetiredRasters;

		void RetireRaster()
		{
			if ( s_Raster.pTex || s_Raster.descriptorSet != VK_NULL_HANDLE )
				s_RetiredRasters.push_back( std::move( s_Raster ) );
			s_Raster = RasterTexture{};
			// The replacement may be allocated at the retired object's
			// address once that is freed; never let it inherit "uploaded".
			s_RasterUploadedTex = nullptr;
		}

		// (Re)creates the raster texture + ImGui descriptor for a footprint
		// of w x h texels, reusing the current one when the size matches.
		// Must run with the HUD's ImGui context current (AddTexture reads
		// the backend data off the current context's IO).
		bool EnsureRasterTexture( uint32_t w, uint32_t h )
		{
			if ( s_Raster.pTex && s_Raster.uWidth == w && s_Raster.uHeight == h && s_Raster.descriptorSet != VK_NULL_HANDLE )
				return true;

			RetireRaster();

			OwningRc<CVulkanTexture> pTex = new CVulkanTexture();
			CVulkanTexture::createFlags flags;
			flags.bSampled = true;      // read by ImGui's fragment shader on the general queue
			flags.bTransferDst = true;  // written by the buffer->image copy in Crosshair_RecordUpload()
			// Only ever touched on the general queue (the HUD's own
			// submission both uploads and samples it), so no cross-queue
			// sharing: it is the HUD texture, not this one, that the
			// compute composite reads.
			if ( !pTex->BInit( w, h, 1u, VulkanFormatToDRM( VK_FORMAT_B8G8R8A8_UNORM ), flags ) )
			{
				s_CrosshairLog.errorf( "failed to create the %ux%u scaled-crosshair texture", w, h );
				return false;
			}

			// srgbView() is, despite the name, the UNORM-format view (the
			// bytes as stored, no sRGB decode on read) -- the same view the
			// HUD renders INTO, so a texel of value v is written to the HUD
			// texture as v, exactly as a rect of vertex colour v would be.
			// Layout GENERAL: that is where CVulkanCmdBuffer's barriers
			// leave every image.
			VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture( pTex->srgbView(), VK_IMAGE_LAYOUT_GENERAL );
			if ( ds == VK_NULL_HANDLE )
			{
				s_CrosshairLog.errorf( "ImGui_ImplVulkan_AddTexture failed for the scaled-crosshair texture" );
				return false;
			}

			s_Raster.pTex = std::move( pTex );
			s_Raster.descriptorSet = ds;
			s_Raster.uWidth = w;
			s_Raster.uHeight = h;
			return true;
		}

		ImU32 PackColor( int nRgb, float flAlpha )
		{
			const int a = (int)std::lround( std::clamp( flAlpha, 0.0f, 1.0f ) * 255.0f );
			return IM_COL32( ( nRgb >> 16 ) & 0xFF, ( nRgb >> 8 ) & 0xFF, nRgb & 0xFF, a );
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
		if ( bPressed )
		{
			// Only the FIRST press starts the clock; a repeated press event
			// for a button already held (some backends re-report) must not
			// restart a hide that is already under way.
			uint64_t ulExpected = 0;
			s_ulRightPressNs.compare_exchange_strong( ulExpected, get_time_in_nanos() );
		}
		else
		{
			// Instant restore on release -- deliberately no reverse
			// animation. The moment the player comes off the sights they
			// want the crosshair back to re-acquire; an ease-in there would
			// be the one place a delay is actually felt.
			s_ulRightPressNs.store( 0 );
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
		                    const crosshair::Frame &fr, const crosshair::HideState &hs, float flOffY )
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
					pDrawList->AddRectFilled( ImVec2( (float)r.x0, (float)r.y0 + flOffY ),
					                          ImVec2( (float)r.x1, (float)r.y1 + flOffY ), col, 0.0f );
			};

			// Outline first (it is already computed as the ring OUTSIDE every
			// fill, so order only matters where the dot overlaps an arm), then
			// the arms, then the dot on top.
			if ( c.outline_enabled )
				Fill( shape.outline, PackColor( c.outline_color, c.outline_opacity * hs.flAlpha ) );
			Fill( shape.lines, PackColor( c.line_color, c.line_opacity * hs.flAlpha ) );
			Fill( shape.dot, PackColor( c.dot_color, c.dot_opacity * hs.flAlpha ) );

			pDrawList->Flags = savedFlags;
		}

		// Apply Scaling ON: the crosshair Build() at the GAME's resolution,
		// rasterised into a texture the size of its own bounding box (+1
		// texel margin), drawn as one quad stretched by the game's per-axis
		// scale and sampled linearly -- ImGui's Vulkan backend binds its
		// LINEAR / CLAMP_TO_EDGE sampler for every image draw
		// (imgui_impl_vulkan.cpp, SamplerLinear). Returns false when the
		// texture could not be made, so the caller can fall back.
		bool DrawRasterPath( ImDrawList *pDrawList, const config::CrosshairSettings &c, const crosshair::Style &st,
		                     const CrosshairFrame &frame, const crosshair::HideState &hs )
		{
			RasterKey key;
			key.bLine = c.line_enabled; key.bDot = c.dot_enabled; key.bOutline = c.outline_enabled;
			key.nLength = c.line_length; key.nWidth = c.line_width; key.nGap = c.line_gap;
			key.nDotSize = c.dot_size; key.nOutlineWidth = c.outline_width;
			key.nLineColor = c.line_color; key.nDotColor = c.dot_color; key.nOutlineColor = c.outline_color;
			key.flLineOpacity = c.line_opacity; key.flDotOpacity = c.dot_opacity; key.flOutlineOpacity = c.outline_opacity;
			key.flHideGap = hs.flGap; key.flHideLength = hs.flLength;
			key.uGameW = frame.uGameWidth; key.uGameH = frame.uGameHeight;

			if ( !s_bRasterValid || !( key == s_RasterKey ) )
			{
				// Rebuild: the exact drawing code, in game pixels. Alpha
				// stays at the configured opacity; the hide fade is the
				// quad's tint below.
				const crosshair::Frame gf = crosshair::GameFrame( frame.uGameWidth, frame.uGameHeight );
				crosshair::HideState hsRaster = hs;
				hsRaster.flAlpha = 1.0f;
				const crosshair::Shape shape = crosshair::Build( st, gf, hsRaster );
				s_RasterRect = crosshair::RasterRect( shape );
				s_RasterPixels = crosshair::Rasterize( shape, s_RasterRect,
					crosshair::PackArgb( c.outline_color, c.outline_enabled ? c.outline_opacity : 0.0f ),
					crosshair::PackArgb( c.line_color, c.line_opacity ),
					crosshair::PackArgb( c.dot_color, c.dot_opacity ) );
				s_RasterKey = key;
				s_bRasterValid = true;
				s_bRasterUploadPending = !s_RasterRect.Empty();
			}

			if ( s_RasterRect.Empty() )
				return true; // nothing to draw this frame (e.g. Shrink at 100 %) -- handled, not a failure

			const uint32_t w = (uint32_t)( s_RasterRect.x1 - s_RasterRect.x0 );
			const uint32_t h = (uint32_t)( s_RasterRect.y1 - s_RasterRect.y0 );
			if ( !EnsureRasterTexture( w, h ) )
				return false;
			// A fresh texture needs the pixels even if the key did not
			// change (e.g. the first frame after a size change).
			if ( s_Raster.pTex && s_RasterUploadedTex != s_Raster.pTex.get() )
				s_bRasterUploadPending = true;

			crosshair::Frame fr;
			fr.flCenterX = frame.flCenterX;
			fr.flCenterY = frame.flCenterY;
			fr.flScaleX = frame.flGamePixelScaleX;
			fr.flScaleY = frame.flGamePixelScaleY;
			const crosshair::FRect q = crosshair::ScaledQuad( s_RasterRect, frame.uGameWidth, frame.uGameHeight, fr );

			const float flOffY = frame.flDrawOffsetY;
			const ImU32 tint = IM_COL32( 255, 255, 255, (int)std::lround( std::clamp( hs.flAlpha, 0.0f, 1.0f ) * 255.0f ) );
			pDrawList->AddImage( ImTextureRef( (ImTextureID)(uintptr_t)s_Raster.descriptorSet ),
			                     ImVec2( q.x0, q.y0 + flOffY ), ImVec2( q.x1, q.y1 + flOffY ),
			                     ImVec2( 0.0f, 0.0f ), ImVec2( 1.0f, 1.0f ), tint );
			return true;
		}
	}

	bool Crosshair_Draw( ImDrawList *pDrawList, const CrosshairFrame &frame, uint64_t ulNowNs )
	{
		EnsureConfigLoaded();
		const config::CrosshairSettings &c = s_Settings.crosshair;
		if ( !c.enabled || !pDrawList )
			return false;

		crosshair::HideState hs;
		bool bAnimating = false;
		if ( c.hide_on_right_click )
		{
			const uint64_t ulPress = s_ulRightPressNs.load( std::memory_order_relaxed );
			if ( ulPress != 0 )
			{
				const float f = crosshair::HideProgress( ulPress, ulNowNs, c.hide_time_ms );
				hs = crosshair::EvaluateHide( crosshair::ParseHideMode( c.hide_mode ), f );
				bAnimating = f < 1.0f; // fully hidden is static again: no more forced frames
			}
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
		// crisp at game resolution, stretched linearly with the game.
		if ( c.apply_scaling && frame.uGameWidth > 0 && frame.uGameHeight > 0 )
		{
			if ( DrawRasterPath( pDrawList, c, st, frame, hs ) )
				return bAnimating;
			// Texture creation failed: fall through to the vector path at
			// the same per-axis scale, so the crosshair is at least there.
		}

		// Apply Scaling off: every size is an output pixel and the crosshair
		// is square. (Fallback for ON: sizes are game pixels stretched per
		// axis and snapped -- the pre-raster behaviour.)
		crosshair::Frame fr;
		fr.flCenterX = frame.flCenterX;
		fr.flCenterY = frame.flCenterY;
		fr.flScaleX = c.apply_scaling ? frame.flGamePixelScaleX : 1.0f;
		fr.flScaleY = c.apply_scaling ? frame.flGamePixelScaleY : 1.0f;
		DrawPixelPath( pDrawList, c, st, fr, hs, frame.flDrawOffsetY );
		return bAnimating;
	}

	void Crosshair_RecordUpload( CVulkanCmdBuffer *pCmdBuffer )
	{
		// The previous HUD submission has been drained by the caller, so
		// nothing on the GPU still reads these descriptors / images.
		for ( RasterTexture &rt : s_RetiredRasters )
		{
			if ( rt.descriptorSet != VK_NULL_HANDLE )
				ImGui_ImplVulkan_RemoveTexture( rt.descriptorSet );
			rt.pTex = nullptr;
		}
		s_RetiredRasters.clear();

		if ( !s_bRasterUploadPending || !pCmdBuffer || !s_Raster.pTex )
			return;
		const size_t nExpected = (size_t)s_Raster.uWidth * (size_t)s_Raster.uHeight;
		if ( s_RasterPixels.size() != nExpected )
			return; // raster and texture disagree on size; the next Draw() re-syncs them

		// Same staging path vulkan_create_texture_from_bits() uses -- the
		// device's bump-allocated upload buffer, only reset on a device
		// idle, so the bytes stay put until this submission has consumed
		// them -- minus its vkQueueWaitIdle: the copy is recorded into the
		// HUD's own command buffer ahead of the render pass that samples
		// the texture. copyBufferToImage() transitions the image to
		// GENERAL (discarding old contents); the explicit insertBarrier()
		// after it turns the copy's TRANSFER_WRITE into a SHADER_READ
		// dependency for the fragment shader.
		const uint32_t uBytes = (uint32_t)( nExpected * sizeof( crosshair::Argb ) );
		auto [ pDst, uOffset ] = g_device.uploadBufferData( uBytes );
		memcpy( pDst, s_RasterPixels.data(), uBytes );
		pCmdBuffer->copyBufferToImage( g_device.uploadBuffer(), uOffset, 0, s_Raster.pTex.get() );
		pCmdBuffer->insertBarrier();

		s_RasterUploadedTex = s_Raster.pTex.get();
		s_bRasterUploadPending = false;
	}

	// -------------------------------------------------------------------
	// The settings area: system.crosshair, right after the HUD's own area
	// in the rail. Groups in the user's own order -- Line, Dot, Outline,
	// Auto-hide, Scaling -- under a master switch. Every dependent row is
	// greyed with a reason while its element (or the whole crosshair) is
	// off; the master switch itself is never gated (SPEC §3.13).
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
		//  Line
		// =================================================================
		a.Group( "Line" );

		a.Switch( "crosshair.line", "Show lines", CROSSHAIR_BIND( bool, line_enabled ) )
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
		//  Dot
		// =================================================================
		a.Group( "Dot" );

		a.Switch( "crosshair.dot", "Show dot", CROSSHAIR_BIND( bool, dot_enabled ) )
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
		//  Outline
		// =================================================================
		a.Group( "Outline" );

		a.Switch( "crosshair.outline", "Show outline", CROSSHAIR_BIND( bool, outline_enabled ) )
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
		//  Auto-hide
		// =================================================================
		a.Group( "Auto-hide" );

		a.Switch( "crosshair.hide", "Hide while holding right-click", CROSSHAIR_BIND( bool, hide_on_right_click ) )
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
			.Help( "How long the hide takes from the moment you press, in milliseconds. 0 hides "
			       "at once. Coming back is always instant." )
			.Range( 0.0f, 2000.0f ).Step( 10.0f ).Unit( "ms" )
			.ZeroMeans( "Instant" )
			.Default( S{}.hide_time_ms )
			.Keywords( "hide time duration milliseconds speed" )
			.DisabledUnless( HideOn, kHideOffReason );

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

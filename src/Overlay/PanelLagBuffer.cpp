// The "Lag spike buffer" settings area -- see PanelLagBuffer.h.
//
// SHAPE. Four per-profile rows -- Lag spike buffer (the on/off switch), Look-back
// (1..10 min), Max buffer (0..250 ms), Test mode (Off / Force minimum / Force
// maximum) -- plus one live Status line. Every row's id IS its config key
// (`lag_buffer.enabled`, `lag_buffer.lookback_min`, ...), which is how the Shell's
// per-profile inherited/overridden dot and "Reset to inherited" find it. Every edit
// calls fghost::SetLagBufferConfig() at once; the pacer reads it on the next frame.
//
// `Why this panel is thin:` the user's architecture rule -- "you're basically only
// building the GUI in this chat and most of the stuff should go into the frame gen
// itself". Which spikes count, how big the buffer is, how it ramps, how deep the
// history is: all the library's. This file maps four choices onto
// fghost::LagBufferConfig and prints the pacer's Report.
// `Why a tab of its own, with no "1x fill gaps" value in Frame generation's
// multiplier:` the user rejected that magic value. The buffer is independent of the
// multiplier: with frame generation off it shows real frames and generates only inside
// gaps; with it on it fills gaps as part of normal pacing; it combines with blur.
// `Why no keybind:` a standing mode of the picture, like frame generation.
#include "PanelLagBuffer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "Config/ConfigManager.h"
#include "FrameGen/FrameGenHost.h"

namespace gamescope
{
	namespace
	{
		bool s_bConfigLoaded = false;
		uint64_t s_ulLoadedGeneration = 0;
		config::Settings s_Settings;

		// The config stores readable strings; the Choice row binds ints; fghost wants
		// its own enum. An unknown string falls to "off".
		constexpr ui::Option kTestOptions[] = { { 0, "Off" }, { 1, "Force minimum" }, { 2, "Force maximum" } };
		constexpr const char *kTestKeys[] = { "off", "min", "max" };

		int TestIndex( const std::string &s )
		{
			for ( int i = 0; i < 3; ++i )
				if ( s == kTestKeys[ i ] )
					return i;
			return 0;
		}

		int ClampIdx( int n, int nMax ) { return n < 0 ? 0 : ( n > nMax ? nMax : n ); }

		int ClampLookback( int n )
		{
			return n < fghost::kMinLagLookbackMin ? fghost::kMinLagLookbackMin
			     : ( n > fghost::kMaxLagLookbackMin ? fghost::kMaxLagLookbackMin : n );
		}

		int ClampMax( int n ) { return n < 0 ? 0 : ( n > fghost::kMaxLagBufferMs ? fghost::kMaxLagBufferMs : n ); }

		fghost::LagBufferConfig ToHostConfig( const config::LagBufferSettings &l )
		{
			fghost::LagBufferConfig c;
			c.enabled = l.enabled;
			c.lookbackMin = ClampLookback( l.lookback_min );
			c.maxBufferMs = ClampMax( l.max_ms );
			c.testMode = fghost::LagTest( TestIndex( l.test_mode ) );
			return c;
		}

		// The one place config state becomes pacer state: after every edit, after
		// every (re)load, and at startup / profile switch via
		// PanelLagBuffer_ApplyStartupConfig().
		void PushToPacer( const config::LagBufferSettings &l )
		{
			fghost::SetLagBufferConfig( ToHostConfig( l ) );
		}

		void EnsureConfigLoaded()
		{
			const uint64_t ulGeneration = config::ConfigGeneration();
			if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
				return;
			s_Settings = config::ResolvedSettings();
			s_ulLoadedGeneration = ulGeneration;
			s_bConfigLoaded = true;
			// A (re)load -- first draw, or a profile switch -- applies now, not
			// when the user next touches a row.
			PushToPacer( s_Settings.lag_buffer );
		}

		void PersistAndPush()
		{
			PushToPacer( s_Settings.lag_buffer );
			config::EnqueueRoutedWrite( s_Settings );
		}

		bool On()
		{
			EnsureConfigLoaded();
			return s_Settings.lag_buffer.enabled;
		}

		// ---- the Status line ----------------------------------------------------
		// One compact line, always. Pure over its inputs. The font is Basic Latin +
		// Latin-1, so the middle dot renders.
		//   Off                    -> "Off"
		//   renderer refuses       -> its reason (HDR, 10-bit, YCbCr, ...)
		//   nothing published yet  -> "Waiting for frames"
		//   otherwise              -> "buffer 32 ms · 4 frames · last spike 28 ms"
		//                             (or "no spikes yet", and "· ignored 1 outlier (>max)"
		//                             when spikes above Max buffer were left out; "· test:
		//                             max" while a test mode pins the target)
		// "frames" is the real frames the host keeps (the library's history depth, which
		// follows the buffer): it is what the memory scales with.
		std::string StatusLine( const config::LagBufferSettings &l, const fghost::PacingStatus &ps, const fghost::RenderStatus &rs )
		{
			if ( !l.enabled )
				return "Off";

			if ( rs.reason != fghost::Unavailable::Ok )
				return fghost::UnavailableText( rs.reason );

			if ( !ps.valid )
				return "Waiting for frames";

			char sz[ 160 ];
			int n = std::snprintf( sz, sizeof( sz ), "buffer %d ms · %d frames", int( std::lround( ps.bufferDelayMs ) ), ps.historyFrames );
			if ( ps.lastSpikeMs > 0.0f )
				n += std::snprintf( sz + n, sizeof( sz ) - n, " · last spike %d ms", int( std::lround( ps.lastSpikeMs ) ) );
			else
				n += std::snprintf( sz + n, sizeof( sz ) - n, " · no spikes yet" );
			if ( ps.outliersIgnored > 0 )
				n += std::snprintf( sz + n, sizeof( sz ) - n, " · ignored %d outlier%s (>max)", ps.outliersIgnored, ps.outliersIgnored == 1 ? "" : "s" );
			const int nTest = TestIndex( l.test_mode );
			if ( nTest != 0 )
				std::snprintf( sz + n, sizeof( sz ) - n, " · test: %s", nTest == 1 ? "min" : "max" );
			return sz;
		}
	}

	void PanelLagBuffer_ApplyStartupConfig( const config::Settings &config )
	{
		PushToPacer( config.lag_buffer );
	}

	void PanelLagBuffer_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "image.lagbuffer", "Lag spike buffer", ui::Section::Display );

		a.Keywords( "lag spike buffer stutter hitch freeze stall frame time frametime gap fill "
		            "generated frames delay latency smooth" );
		a.Summary( []
		{
			EnsureConfigLoaded();
			const config::LagBufferSettings &l = s_Settings.lag_buffer;
			if ( !l.enabled )
				return std::string( "off" );
			return "max " + std::to_string( l.max_ms ) + " ms";
		} );

		a.Group( "Lag spike buffer" );

		a.Switch( "lag_buffer.enabled", "Lag spike buffer",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.lag_buffer.enabled; },
				[]( bool b ) { EnsureConfigLoaded(); s_Settings.lag_buffer.enabled = b; PersistAndPush(); } ) )
			.Help( "Detects the game's frame-time spikes and runs the picture slightly behind, so "
			       "a repeat of a recent spike is bridged with generated frames instead of "
			       "freezing. The newest big spike in the look-back window sizes the buffer; "
			       "older ones fade. Spikes above Max buffer (for example shader compiles) are "
			       "ignored. The first spike of a session still freezes, because it reacts. "
			       "It adds its buffer as input delay, so it is meant for controller and slower "
			       "games. Audio is not delayed, so above about 50-80 ms lip-sync drifts. "
			       "It works with or without Frame generation, and with Motion blur. Memory: "
			       "about one frame copy per 16 ms of buffer at 60 fps (4 bytes per pixel each, "
			       "plus the same again with UI protection on)." )
			.Default( false )
			.Keywords( "lag spike buffer stutter hitch freeze enable on off switch toggle" );

		a.Slider( "lag_buffer.lookback_min", "Look-back",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return ClampLookback( s_Settings.lag_buffer.lookback_min ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.lag_buffer.lookback_min = ClampLookback( n ); PersistAndPush(); } ) )
			.Help( "How far back spikes count, 1 to 10 minutes. Only the newest big spike inside "
			       "this window really sizes the buffer; older ones fade out smoothly instead of "
			       "dropping off. A longer window suits a game that hitches only every few "
			       "minutes." )
			.Range( float( fghost::kMinLagLookbackMin ), float( fghost::kMaxLagLookbackMin ) ).Step( 1.0f ).Unit( "min" )
			.Default( 5 )
			.Keywords( "lag spike buffer look-back lookback window minutes history memory" )
			.DisabledUnless( On, "the lag spike buffer is off" );

		a.Slider( "lag_buffer.max_ms", "Max buffer",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return ClampMax( s_Settings.lag_buffer.max_ms ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.lag_buffer.max_ms = ClampMax( n ); PersistAndPush(); } ) )
			.Help( "The most delay the buffer may add, 0 to 250 ms, and the line between a spike "
			       "and an outlier: a spike bigger than this (a shader compile, a disk hitch) is "
			       "ignored, not sized for. 50 ms covers about 3 missing frames at 60 fps. "
			       "More is only for games that really need it, and costs input delay and "
			       "memory (about one frame copy per 16 ms at 60 fps)." )
			.Range( 0.0f, float( fghost::kMaxLagBufferMs ) ).Step( 5.0f ).Unit( "ms" )
			.Default( 50 )
			.Keywords( "lag spike buffer max maximum cap delay latency milliseconds outlier ignore" )
			.DisabledUnless( On, "the lag spike buffer is off" );

		a.Choice( "lag_buffer.test_mode", "Test mode",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return TestIndex( s_Settings.lag_buffer.test_mode ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.lag_buffer.test_mode = kTestKeys[ ClampIdx( n, 2 ) ]; PersistAndPush(); } ),
			kTestOptions, std::size( kTestOptions ) )
			.Help( "Forces the minimum (no buffer) or the maximum (Max buffer) so you can feel "
			       "both ends. The change is smooth, not a jump. Leave it Off to size the buffer "
			       "from the game's own spikes." )
			.Default( 0 )
			.Keywords( "lag spike buffer test mode force minimum maximum preview feel" )
			.DisabledUnless( On, "the lag spike buffer is off" );

		a.Group( "Status" );

		a.Facts( "lag_buffer.status", "Status", []
		{
			EnsureConfigLoaded();
			return StatusLine( s_Settings.lag_buffer, fghost::GetPacingStatus(), fghost::GetRenderStatus() );
		} )
			.Help( "What the buffer is doing right now: the delay it adds, the real frames it "
			       "keeps (memory follows this number), the size of the newest spike in the "
			       "window, and how many spikes above Max buffer were ignored. Spikes need a few "
			       "frames of history to be detected." )
			.Keywords( "lag spike buffer status delay frames last spike outlier ignored" );
	}
}

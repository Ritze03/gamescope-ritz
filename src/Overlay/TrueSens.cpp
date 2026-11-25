// True Sens' glue -- see TrueSens.h for the maths and the pure recorder, and
// superdoc/features/true-sens.md for the feature, the hook order and the
// threading rules.
#include "TrueSens.h"

#include <atomic>
#include <cassert>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "convar.h"
#include "log.hpp"
#include "steamcompmgr.hpp"
#include "wlserver.hpp"
#include "Config/ConfigManager.h"
#include "Zoom.h"

namespace gamescope
{
	namespace
	{
		using truesens::Target;

		// ---- what the input thread reads and writes --------------------------------
		// The hook (wlserver thread, wlserver_lock() held) touches ONLY these and
		// s_Carry. ConfigManager's mirror has no lock and belongs to the compositor
		// thread, so none of it is ever read from the hook (NullBinds.cpp's file
		// comment says the same for the same reason).

		// The running recording's target (a truesens::Target as int; 0 = none) and
		// its accumulators. The hook is the single writer of the sums, under the
		// wlserver lock. Start/Stop take that lock too (always: they run on the
		// compositor thread, which never holds it, see TrueSens_StartRecording), so
		// a reset or a read never interleaves with a half-done +=. The compositor
		// thread reads the sums without the lock only for the live status.
		std::atomic<int>      s_eTarget{ 0 };
		std::atomic<double>   s_dSumX{ 0.0 };
		std::atomic<double>   s_dSumY{ 0.0 };
		std::atomic<uint64_t> s_ulSamples{ 0 };
		std::atomic<bool>     s_bZoomSpoiled{ false }; // Zoom was on during the recording (D15)

		// The multiplier the hook applies, and whether it applies one at all.
		std::atomic<double> s_dMultiplier{ 1.0 };
		std::atomic<bool>   s_bScale{ false };

		// The fraction carry (D19). Touched only by the hook, i.e. under the
		// wlserver lock on the input thread.
		truesens::FractionCarry s_Carry;

		// ---- compositor-thread state -----------------------------------------------
		bool s_bLoaded = false;   // a key has been resolved (possibly provisionally)
		bool s_bKeyFinal = false; // the first tick has resolved it for good
		config::TrueSensKey s_Key;
		std::string s_sLastOutcome;
		// Bumped whenever the stored in-game value changes or a recording replaces
		// the game's counts, so the area can drop a stale entry error.
		uint64_t s_ulIngameGeneration = 0;

		LogScope log_truesens( "truesens" );

		// The wlserver lock, taken unconditionally. wlserver_is_lock_held() is a
		// global trylock probe ("somebody holds it"), not "this thread holds it", so
		// it cannot be used to skip locking: while the input thread is inside
		// wlserver_mousemotion() it answers "held" and the caller would then read
		// the sums unlocked. This is only correct because every caller runs on the
		// compositor thread, which never holds the lock there (see
		// TrueSens_StartRecording).
		struct ScopedWlserverLock
		{
			ScopedWlserverLock() { wlserver_lock(); }
			~ScopedWlserverLock() { wlserver_unlock(); }
		};

		double ChosenInches( const config::TrueSensGlobal &g, const config::GameTrueSens &gm )
		{
			return gm.profile == "high" ? g.high_inches : g.low_inches;
		}

		// Recomputes the hook's atomics from the config. Called after every setter,
		// after a recording and from the tick.
		void Refresh()
		{
			const config::TrueSensGlobal g = config::GetTrueSensGlobal();
			const config::GameTrueSens gm = config::GetGameTrueSens( s_Key.key );
			double dMult = 1.0;
			if ( gm.enabled )
				dMult = truesens::Multiplier( gm.counts_360, ChosenInches( g, gm ), g.dpi );
			s_dMultiplier.store( dMult, std::memory_order_relaxed );
			s_bScale.store( dMult != 1.0, std::memory_order_relaxed );
		}

		// Resolves the session's key (app id from the environment, else the launch
		// command's basename) and refreshes the hook's atomics when it changed.
		// Builds strings and reads the config, so it is NOT called per getter.
		void ResolveKey()
		{
			config::TrueSensKey k = config::SessionTrueSensKey();
			if ( !s_bLoaded || k.key != s_Key.key || k.shared != s_Key.shared )
			{
				s_Key = std::move( k );
				s_bLoaded = true;
				Refresh();
				log_truesens.debugf( "game entry '%s'%s", s_Key.key.c_str(), s_Key.shared ? " (shared unknown-game entry)" : "" );
			}
		}

		// The getters use the cached key; only a call before the first tick (an
		// early getter) resolves one, provisionally. What can still change: the
		// app id is read from the environment once and cached by the config, the
		// launch command is noted once by LaunchNestedChildren() before the paint
		// loop starts. So TrueSens_Tick() resolves once more on its first call
		// (inside the loop, after both are known) and never again.
		void EnsureLoaded()
		{
			if ( !s_bLoaded )
				ResolveKey();
		}

		config::GameTrueSens Game()
		{
			EnsureLoaded();
			return config::GetGameTrueSens( s_Key.key );
		}

		void SetGame( const config::GameTrueSens &gm )
		{
			EnsureLoaded();
			config::SetGameTrueSens( s_Key.key, gm );
			Refresh();
		}

		// ---- debug commands --------------------------------------------------------
		// gamescopectl runs a ConCommand on the wlserver thread, which must not
		// touch ConfigManager's mirror. So the commands only queue their text and
		// TrueSens_Tick() runs them on the compositor thread.
		std::mutex s_CmdMutex;
		std::vector<std::string> s_PendingCmds;

		void QueueCmd( std::string sVerb, std::span<std::string_view> args )
		{
			std::string s = std::move( sVerb );
			for ( size_t i = 1; i < args.size(); i++ )
			{
				s += ' ';
				s += std::string( args[ i ] );
			}
			{
				std::lock_guard<std::mutex> lock( s_CmdMutex );
				s_PendingCmds.push_back( std::move( s ) );
			}
			force_repaint();
		}

		std::vector<std::string> Split( const std::string &s )
		{
			std::vector<std::string> v;
			size_t i = 0;
			while ( i < s.size() )
			{
				while ( i < s.size() && s[ i ] == ' ' )
					i++;
				size_t j = i;
				while ( j < s.size() && s[ j ] != ' ' )
					j++;
				if ( j > i )
					v.push_back( s.substr( i, j - i ) );
				i = j;
			}
			return v;
		}

		void PrintStatus()
		{
			EnsureLoaded();
			const config::TrueSensGlobal g = config::GetTrueSensGlobal();
			const config::GameTrueSens gm = Game();
			const std::optional<double> oCalc = TrueSens_CalcIngameSens();
			console_log.infof( "true_sens: key='%s' shared=%d enabled=%d profile=%s dpi=%d low_inches=%.4f high_inches=%.4f",
				s_Key.key.c_str(), s_Key.shared ? 1 : 0, gm.enabled ? 1 : 0, gm.profile.c_str(), g.dpi,
				(double)g.low_inches, (double)g.high_inches );
			console_log.infof( "true_sens: counts_360=%.2f ingame_sens=%.4f multiplier=%.6f (%s) calc_ingame=%s low='%s' high='%s'",
				(double)gm.counts_360, (double)gm.ingame_sens, TrueSens_Multiplier(), TrueSens_MultiplierText().c_str(),
				oCalc ? truesens::FormatFixed( *oCalc, 4 ).c_str() : "n/a",
				TrueSens_LowText().c_str(), TrueSens_HighText().c_str() );
			console_log.infof( "true_sens: recording=%s net=%.2f sum_y=%.2f samples=%llu zoom_spoiled=%d last_outcome='%s'",
				truesens::TargetName( TrueSens_RecordingTarget() ), TrueSens_RecordingNet(),
				s_dSumY.load( std::memory_order_relaxed ), (unsigned long long)s_ulSamples.load( std::memory_order_relaxed ),
				s_bZoomSpoiled.load( std::memory_order_relaxed ) ? 1 : 0, s_sLastOutcome.c_str() );
		}

		void RunCommand( const std::string &sLine )
		{
			const std::vector<std::string> a = Split( sLine );
			if ( a.empty() )
				return;
			if ( a[ 0 ] == "record" && a.size() >= 2 )
			{
				Target t = a[ 1 ] == "low" ? Target::Low : a[ 1 ] == "high" ? Target::High : a[ 1 ] == "game" ? Target::Game : Target::Idle;
				const truesens::OpResult st = TrueSens_StartRecording( t );
				console_log.infof( "true_sens_record %s: %s", a[ 1 ].c_str(), st.ok ? "recording" : st.reason.c_str() );
			}
			else if ( a[ 0 ] == "stop" )
			{
				const truesens::RecordResult r = TrueSens_StopRecording();
				console_log.infof( "true_sens_stop: ok=%d counts=%.2f sum_y=%.2f samples=%llu: %s", r.ok ? 1 : 0, r.counts, r.sum_y,
					(unsigned long long)r.samples, TrueSens_LastOutcome().c_str() );
			}
			else if ( a[ 0 ] == "set" && a.size() >= 3 )
			{
				const std::string &sVal = a[ 2 ];
				if ( a[ 1 ] == "enabled" )
					TrueSens_SetEnabled( sVal == "1" || sVal == "true" || sVal == "on" );
				else if ( a[ 1 ] == "profile" )
					TrueSens_SetUseHigh( sVal == "high" );
				else if ( a[ 1 ] == "dpi" )
					TrueSens_SetDpi( gamescope::Parse<int>( sVal ).value_or( truesens::kDefaultDpi ) );
				else if ( a[ 1 ] == "ingame" )
					TrueSens_SetIngameSens( (float)gamescope::Parse<double>( sVal ).value_or( 0.0 ) );
				else
					console_log.errorf( "true_sens_set: unknown field '%s' (enabled|profile|dpi|ingame)", a[ 1 ].c_str() );
			}
			else if ( a[ 0 ] != "status" )
				console_log.errorf( "true_sens: bad command '%s'", sLine.c_str() );
			PrintStatus();
		}

		static ConCommand cc_true_sens_record(
			"true_sens_record",
			"Start a True Sens recording: true_sens_record <low|high|game>. Counts only sideways mouse "
			"movement while the cursor is hidden, with the multiplier off. Run true_sens_stop to end it.",
			[]( std::span<std::string_view> args ) { QueueCmd( "record", args ); } );

		static ConCommand cc_true_sens_stop(
			"true_sens_stop",
			"Stop the True Sens recording and store its net movement (a failed one stores nothing and "
			"logs why).",
			[]( std::span<std::string_view> args ) { QueueCmd( "stop", args ); } );

		static ConCommand cc_true_sens_set(
			"true_sens_set",
			"Set a True Sens value: true_sens_set <enabled 0|1 | profile low|high | dpi N | ingame X>. "
			"Through gamescopectl the arguments must be ONE quoted argument: gamescopectl true_sens_set \"dpi 800\".",
			[]( std::span<std::string_view> args ) { QueueCmd( "set", args ); } );

		static ConCommand cc_true_sens_status(
			"true_sens_status",
			"Print every True Sens value on three log lines: this game's key and settings, the "
			"derived multiplier and calculator result, and the recording state.",
			[]( std::span<std::string_view> args ) { QueueCmd( "status", args ); } );
	}

	// ---- the hook --------------------------------------------------------------------

	void TrueSens_OnMotion( double &dx, double &dy, bool bCursorHidden )
	{
		if ( s_eTarget.load( std::memory_order_relaxed ) != 0 )
		{
			// A recording runs with the multiplier OFF and counts only movement
			// while the cursor is hidden (D11); the carry is dropped so nothing
			// stale leaks into play afterwards.
			s_Carry.Reset();
			if ( bCursorHidden )
			{
				if ( Zoom_IsActive() )
					s_bZoomSpoiled.store( true, std::memory_order_relaxed ); // D15
				// Single writer under the wlserver lock, so load+store is exact.
				s_dSumX.store( s_dSumX.load( std::memory_order_relaxed ) + dx, std::memory_order_relaxed );
				s_dSumY.store( s_dSumY.load( std::memory_order_relaxed ) + dy, std::memory_order_relaxed );
				s_ulSamples.store( s_ulSamples.load( std::memory_order_relaxed ) + 1, std::memory_order_relaxed );
			}
			return;
		}

		// D5: only while the cursor is not visible. Anything else is left alone.
		if ( !bCursorHidden || !s_bScale.load( std::memory_order_relaxed ) )
		{
			s_Carry.Reset();
			return;
		}
		s_Carry.Apply( dx, dy, s_dMultiplier.load( std::memory_order_relaxed ) );
	}

	// ---- the tick --------------------------------------------------------------------

	void TrueSens_Tick()
	{
		if ( !s_bKeyFinal )
		{
			ResolveKey();
			s_bKeyFinal = true;
		}

		std::vector<std::string> cmds;
		{
			std::lock_guard<std::mutex> lock( s_CmdMutex );
			cmds.swap( s_PendingCmds );
		}
		for ( const std::string &c : cmds )
			RunCommand( c );

		// D15: Zoom on during a recording spoils it (the hook also notes this
		// per motion event; this covers a chord pressed between two events).
		if ( s_eTarget.load( std::memory_order_relaxed ) != 0 && Zoom_IsActive() )
			s_bZoomSpoiled.store( true, std::memory_order_relaxed );
	}

	// ---- recording -------------------------------------------------------------------

	LogScope &TrueSens_LogScope()
	{
		return log_truesens;
	}

	bool TrueSens_RecordingActive()
	{
		return s_eTarget.load( std::memory_order_relaxed ) != 0;
	}

	truesens::Target TrueSens_RecordingTarget()
	{
		return (Target)s_eTarget.load( std::memory_order_relaxed );
	}

	double TrueSens_RecordingNet()
	{
		const double d = s_dSumX.load( std::memory_order_relaxed );
		return d < 0.0 ? -d : d;
	}

	std::string TrueSens_LastOutcome()
	{
		return s_sLastOutcome;
	}

	truesens::OpResult TrueSens_StartRecording( truesens::Target t )
	{
		truesens::OpResult st;
		if ( t != Target::Low && t != Target::High && t != Target::Game )
		{
			st.ok = false;
			st.reason = "Unknown recording target.";
		}
		else if ( TrueSens_RecordingActive() )
		{
			st.ok = false;
			st.reason = "A recording is already running.";
		}
		else if ( Zoom_IsActive() )
		{
			st.ok = false;
			st.reason = "Zoom is active. Turn Zoom off before recording.";
		}
		if ( !st.ok )
		{
			s_sLastOutcome = st.reason;
			return st;
		}

		EnsureLoaded();
		{
			ScopedWlserverLock lock;
			s_dSumX.store( 0.0, std::memory_order_relaxed );
			s_dSumY.store( 0.0, std::memory_order_relaxed );
			s_ulSamples.store( 0, std::memory_order_relaxed );
			s_bZoomSpoiled.store( false, std::memory_order_relaxed );
			s_eTarget.store( (int)t, std::memory_order_relaxed );
		}
		s_sLastOutcome.clear();
		TrueSens_ReferenceRequest();
		force_repaint(); // the hint (and the picture) must appear on an idle client too
		return st;
	}

	truesens::RecordResult TrueSens_StopRecording()
	{
		Target target;
		double dSumX, dSumY;
		uint64_t ulSamples;
		bool bSpoiled;
		{
			ScopedWlserverLock lock; // no motion event is half-summed while we read
			target = (Target)s_eTarget.load( std::memory_order_relaxed );
			dSumX = s_dSumX.load( std::memory_order_relaxed );
			dSumY = s_dSumY.load( std::memory_order_relaxed );
			ulSamples = s_ulSamples.load( std::memory_order_relaxed );
			bSpoiled = s_bZoomSpoiled.load( std::memory_order_relaxed );
			s_eTarget.store( 0, std::memory_order_relaxed );
		}
		TrueSens_ReferenceClear();

		if ( target == Target::Idle )
		{
			truesens::RecordResult r;
			r.reason = "No recording is running.";
			return r; // leaves the last outcome alone
		}

		EnsureLoaded();
		truesens::RecordResult r = truesens::Evaluate( target, dSumX, dSumY, ulSamples, bSpoiled ? truesens::kReasonZoom : "" );
		if ( !r.ok )
		{
			s_sLastOutcome = r.reason;
			return r; // a failed recording changes nothing
		}

		// D12: Low/High also store the game's own value, it is the same measurement.
		const config::TrueSensGlobal g = config::GetTrueSensGlobal();
		config::GameTrueSens gm = Game();
		gm.counts_360 = (float)r.counts;
		// The typed in-game value belongs to the recording it was typed for: with a
		// new counts_360 the old number would give a wrong result, so the calculator
		// asks for it again (a failed recording returned above and changes nothing).
		gm.ingame_sens = 0.0f;
		s_ulIngameGeneration++;
		if ( target == Target::Low || target == Target::High )
		{
			config::TrueSensGlobal ng = g;
			const float flInches = (float)truesens::InchesFromCounts( r.counts, g.dpi );
			( target == Target::Low ? ng.low_inches : ng.high_inches ) = flInches;
			config::SetTrueSensGlobal( ng );
			s_sLastOutcome = std::string( target == Target::Low ? "Low" : "High" ) + " stored: " +
				truesens::FormatCmPer360( flInches ) + " (" + truesens::FormatFixed( r.counts, 0 ) + " counts at " +
				std::to_string( g.dpi ) + " DPI). The game's value was stored too.";
		}
		else
		{
			s_sLastOutcome = "Game stored: " + truesens::FormatCounts360( r.counts ) + ".";
		}
		SetGame( gm ); // also refreshes the hook's atomics
		return r;
	}

	// ---- this game's values ----------------------------------------------------------

	bool TrueSens_GetEnabled()
	{
		return Game().enabled;
	}

	void TrueSens_SetEnabled( bool bEnabled )
	{
		config::GameTrueSens gm = Game();
		gm.enabled = bEnabled;
		SetGame( gm );
	}

	bool TrueSens_GetUseHigh()
	{
		return Game().profile == "high";
	}

	void TrueSens_SetUseHigh( bool bHigh )
	{
		config::GameTrueSens gm = Game();
		gm.profile = bHigh ? "high" : "low";
		SetGame( gm );
	}

	float TrueSens_GetIngameSens()
	{
		return Game().ingame_sens;
	}

	void TrueSens_SetIngameSens( float flValue )
	{
		config::GameTrueSens gm = Game();
		gm.ingame_sens = truesens::IsPositive( flValue ) ? flValue : 0.0f;
		s_ulIngameGeneration++;
		SetGame( gm );
	}

	uint64_t TrueSens_IngameGeneration()
	{
		return s_ulIngameGeneration;
	}

	double TrueSens_GameCounts360()
	{
		return Game().counts_360;
	}

	// ---- global values ---------------------------------------------------------------

	int TrueSens_GetDpi()
	{
		EnsureLoaded();
		return config::GetTrueSensGlobal().dpi;
	}

	void TrueSens_SetDpi( int dpi )
	{
		EnsureLoaded();
		config::TrueSensGlobal g = config::GetTrueSensGlobal();
		g.dpi = truesens::ClampDpi( dpi );
		config::SetTrueSensGlobal( g );
		Refresh();
	}

	double TrueSens_LowInches()
	{
		EnsureLoaded();
		return config::GetTrueSensGlobal().low_inches;
	}

	double TrueSens_HighInches()
	{
		EnsureLoaded();
		return config::GetTrueSensGlobal().high_inches;
	}

	// ---- derived ---------------------------------------------------------------------

	double TrueSens_Multiplier()
	{
		return s_dMultiplier.load( std::memory_order_relaxed );
	}

	std::optional<double> TrueSens_CalcIngameSens()
	{
		EnsureLoaded();
		const config::TrueSensGlobal g = config::GetTrueSensGlobal();
		const config::GameTrueSens gm = config::GetGameTrueSens( s_Key.key );
		return truesens::IngameSensForTarget( gm.ingame_sens, gm.counts_360, ChosenInches( g, gm ), g.dpi );
	}

	std::string TrueSens_LowText()
	{
		return truesens::FormatCmPer360( TrueSens_LowInches() );
	}

	std::string TrueSens_HighText()
	{
		return truesens::FormatCmPer360( TrueSens_HighInches() );
	}

	std::string TrueSens_GameText()
	{
		return truesens::FormatCounts360( TrueSens_GameCounts360() );
	}

	std::string TrueSens_MultiplierText()
	{
		return s_bScale.load( std::memory_order_relaxed )
			? truesens::FormatMultiplier( TrueSens_Multiplier() )
			: std::string( "not recorded" );
	}

	const std::string &TrueSens_SessionKey()
	{
		EnsureLoaded();
		return s_Key.key;
	}

	bool TrueSens_SessionKeyShared()
	{
		EnsureLoaded();
		return s_Key.shared;
	}
}

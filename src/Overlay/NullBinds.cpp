// The WASD null-binds / SOCD engine's glue -- see NullBinds.h for the engine
// itself, the hook contract, and why the feature exists at all.
#include "NullBinds.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "log.hpp"
#include "steamcompmgr.hpp"
#include "wlserver.hpp"
#include "Config/ConfigManager.h"
#include "SettingsOverlay.h"
#include "UI/Registry.h"

namespace gamescope
{
	namespace
	{
		// Opt-in trace (log_nullbinds=debug, or `gamescopectl log_nullbinds
		// debug`): one line per physical event this module consumes and one
		// per synthetic emit it sends, from whichever of the three producers
		// (NullBinds_OnKey()'s own immediate emits, the worker thread's
		// delayed press, OnCaptureStart()'s reconcile) -- EmitOne() is the
		// one place all three funnel through, so logging there covers all
		// of them. Off by default (LOG_INFO), matching log_binding's own
		// convention (Keybinds.cpp) for the same kind of per-keystroke trace
		// that would otherwise be noise during ordinary play.
		LogScope log_nullbinds( "nullbinds" );

		using Clock = std::chrono::steady_clock;

		uint64_t NowMs()
		{
			return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
				Clock::now().time_since_epoch() ).count();
		}

		Clock::time_point DeadlineFromMs( uint64_t ulMs )
		{
			return Clock::time_point( std::chrono::milliseconds( ulMs ) );
		}

		// Config: the same lazy, generation-keyed cache Autoclicker.cpp and
		// Zoom.cpp keep. Read from the settings-area's own callbacks
		// (getters/setters/Summary) AND from NullBinds_Tick() (2026-09-27,
		// see NullBinds.h) -- both run on the steamcompmgr thread (the
		// settings-overlay ImGui pass and Zoom_FillRequest()/NullBinds_Tick()
		// are all called from the same paint_all(), never concurrently with
		// each other), so this needs no lock of its own. NEVER from
		// NullBinds_OnKey(), which runs on the wlserver thread and must
		// touch atomics only, exactly as Zoom_OnChord()/Autoclicker_OnChord()
		// do (see those files' threading notes): ConfigManager's own state
		// is not safe to read from that thread, and a profile load is not
		// something a real-time input path should ever be able to block on.
		bool s_bConfigLoaded = false;
		uint64_t s_ulLoadedGeneration = 0;
		config::Settings s_Settings;

		// THE ONE LOCK ORDER, copied from Autoclicker.cpp:43-49. The
		// wlserver thread takes wlserver_lock() (already held by
		// wlserver_key()'s caller before NullBinds_OnKey() ever runs) and
		// THEN s_Mutex, to touch the engine. Every other caller of the
		// engine (the worker thread, and a settings row's setter on the
		// UI/render thread) takes s_Mutex and wlserver_lock() at DISJOINT
		// times, never nested: release s_Mutex before ever calling
		// wlserver_lock()/wlserver_key(). So wlserver_lock -> s_Mutex is the
		// only nesting that ever happens, from either direction.
		std::mutex s_Mutex;
		std::condition_variable s_Cv;
		nullbinds::Engine s_Engine;           // guarded by s_Mutex
		bool s_bHasDeadline = false;          // guarded by s_Mutex
		uint64_t s_ulDeadlineMs = 0;           // guarded by s_Mutex

		// True only inside NullBinds's own synthetic wlserver_key() call --
		// see NullBinds_IsInjecting() in NullBinds.h.
		std::atomic<bool> s_bInjecting{ false };

		// Mirrored atomics for NullBinds_OnKey()'s hot path -- NOT read
		// there; the hot path only needs to know whether to bother calling
		// into the engine at all, so it can skip s_Mutex for the ~100 keys
		// on the keyboard NullBinds does not touch. IsPairKey() alone
		// already answers "is this key relevant"; s_bEnabled additionally
		// lets the very-common case of "feature off" skip the mutex too.
		std::atomic<bool> s_bEnabled{ false };

		void EmitOne( const nullbinds::Emit &e, uint32_t uTime )
		{
			log_nullbinds.debugf( "emit key=%u press=%d time=%u", e.key, (int)e.press, uTime );
			s_bInjecting.store( true, std::memory_order_relaxed );
			wlserver_key( e.key, e.press, uTime );
			s_bInjecting.store( false, std::memory_order_relaxed );
		}

		// For the WORKER THREAD and the UI/render-thread settings path only
		// -- both are on a thread that does NOT already hold wlserver_lock,
		// so both take and release it themselves around the whole burst,
		// exactly as Autoclicker.cpp's Emit() does. NEVER call this while
		// holding s_Mutex (see the lock-order comment above).
		void EmitAllLocking( const std::vector<nullbinds::Emit> &emits )
		{
			if ( emits.empty() )
				return;
			wlserver_lock();
			for ( const nullbinds::Emit &e : emits )
				EmitOne( e, get_time_in_milliseconds() );
			wlserver_unlock();
		}

		void UpdateDeadline( const nullbinds::Result &r )
		{
			// Called with s_Mutex already held.
			s_bHasDeadline = r.has_deadline;
			s_ulDeadlineMs = r.deadline_ms;
		}

		// One worker for the process, parked on s_Cv whenever nothing is
		// pending, created on first use and never joined -- the same
		// "why not per-activation" reasoning as Autoclicker.cpp's
		// EnsureThread() (Autoclicker_OnChord() and NullBinds_OnKey() alike
		// run with wlserver_lock() held, so a join here could deadlock
		// against a worker waiting for that same lock).
		void Worker()
		{
			std::unique_lock<std::mutex> lock( s_Mutex );
			for ( ;; )
			{
				if ( !s_bHasDeadline )
				{
					s_Cv.wait( lock );
					continue;
				}

				const Clock::time_point tpDeadline = DeadlineFromMs( s_ulDeadlineMs );
				if ( s_Cv.wait_until( lock, tpDeadline ) == std::cv_status::timeout )
				{
					nullbinds::Result r = s_Engine.OnTimer( NowMs() );
					UpdateDeadline( r );
					if ( !r.emits.empty() )
					{
						lock.unlock();
						EmitAllLocking( r.emits );
						lock.lock();
					}
				}
				// Otherwise: notified early (settings or a physical event
				// changed the pending press) -- state is already fresh
				// under s_Mutex; loop back and re-read it.
			}
		}

		void EnsureWorker()
		{
			static std::once_flag s_Once;
			std::call_once( s_Once, []{ std::thread( Worker ).detach(); } );
		}

		nullbinds::Settings ToEngineSettings( const config::NullBindsSettings &n )
		{
			nullbinds::Settings s;
			s.enabled   = n.enabled;
			s.pair_ad   = n.pair_ad;
			s.pair_ws   = n.pair_ws;
			s.delay_ms  = n.delay_ms;
			s.jitter_ms = n.jitter_ms;
			return s;
		}

		// Applies settings to the engine and sends any immediate reconcile
		// emits (e.g. the master switch going off while a key is down) right
		// now. Called from the UI/render thread (a settings row's setter),
		// never from NullBinds_OnKey() -- so it is free to take s_Mutex and
		// wlserver_lock() at disjoint times exactly like the worker thread.
		void ApplyEngineSettings( const config::NullBindsSettings &n )
		{
			s_bEnabled.store( n.enabled, std::memory_order_relaxed );
			nullbinds::Result r;
			{
				std::lock_guard<std::mutex> lock( s_Mutex );
				r = s_Engine.SetSettings( ToEngineSettings( n ) );
				UpdateDeadline( r );
			}
			EmitAllLocking( r.emits );
			if ( r.has_deadline )
			{
				EnsureWorker();
				s_Cv.notify_all();
			}
		}

		void Mirror()
		{
			ApplyEngineSettings( s_Settings.null_binds );
		}

		void EnsureConfigLoaded()
		{
			const uint64_t ulGeneration = config::ConfigGeneration();
			if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
				return;
			s_Settings = config::ResolvedSettings();
			s_ulLoadedGeneration = ulGeneration;
			s_bConfigLoaded = true;
			Mirror();
		}

		void PersistAndRepaint()
		{
			Mirror();
			config::EnqueueRoutedWrite( s_Settings );
			force_repaint();
		}

		// steamcompmgr thread only (NullBinds_Tick() is only ever called
		// from there) -- the edge NullBinds_Tick() watches for. Starts
		// false so a Shell already open at process start (not possible
		// today, but cheap to be correct about) is treated as "already
		// capturing", not as a fresh edge.
		bool s_bWasCapturingKeyboard = false;

	}

	bool NullBinds_OnKey( uint32_t key, bool press, uint32_t time )
	{
		assert( wlserver_is_lock_held() );

		if ( !s_bEnabled.load( std::memory_order_relaxed ) )
			return false;
		if ( !nullbinds::Engine::IsPairKey( key ) )
			return false;

		nullbinds::Result r;
		{
			std::lock_guard<std::mutex> lock( s_Mutex );
			r = s_Engine.OnPhysical( key, press, NowMs() );
			UpdateDeadline( r );
		}

		if ( !r.consumed )
			return false;

		log_nullbinds.debugf( "physical key=%u press=%d consumed, %zu emit(s), %s",
			key, (int)press, r.emits.size(), r.has_deadline ? "press scheduled" : "no pending press" );

		// wlserver_lock() is ALREADY held by wlserver_key()'s own caller --
		// do NOT lock it again here. `time` is the original event's own
		// timestamp, reused for every immediate emit this same physical
		// event produces (they all happen at this same instant).
		for ( const nullbinds::Emit &e : r.emits )
			EmitOne( e, time );

		if ( r.has_deadline )
		{
			EnsureWorker();
			s_Cv.notify_all();
		}

		return true;
	}

	bool NullBinds_IsInjecting()
	{
		return s_bInjecting.load( std::memory_order_relaxed );
	}

	// Called once a frame from steamcompmgr.cpp -- see NullBinds.h's own
	// comment for why the two jobs below share this one call. Runs on the
	// steamcompmgr thread, so it is free to take s_Mutex and wlserver_lock()
	// at disjoint times exactly like a settings row's own setter (the SAME
	// lock-order rule NullBinds_OnKey()'s file comment states).
	void NullBinds_Tick()
	{
		EnsureConfigLoaded();

		const bool bCapturingNow = SettingsOverlay_IsCapturingKeyboard();
		if ( bCapturingNow && !s_bWasCapturingKeyboard )
		{
			// The edge: capturing just started. Reconcile the engine (see
			// Engine::OnCaptureStart()'s own comment) and send any release
			// it asks for right now, before the overlay has a chance to
			// steal any more key events out from under an in-flight
			// scheduled press.
			nullbinds::Result r;
			{
				std::lock_guard<std::mutex> lock( s_Mutex );
				r = s_Engine.OnCaptureStart();
				UpdateDeadline( r );
			}
			EmitAllLocking( r.emits );
			// No has_deadline check needed: OnCaptureStart() never leaves
			// one pending (it cancels any in-flight press outright), so the
			// worker has nothing new to wait for.
		}
		s_bWasCapturingKeyboard = bCapturingNow;
	}

	void NullBinds_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "system.null_binds", "Null binds", ui::Section::System );

		a.Keywords( "null binds snap tap socd wasd strafe counter strafe anti cheat "
		            "free snap tap key priority" );
		a.Summary( []
		{
			EnsureConfigLoaded();
			return s_Settings.null_binds.enabled ? std::string( "on" ) : std::string( "off" );
		} );

		auto On = []{ EnsureConfigLoaded(); return s_Settings.null_binds.enabled; };
		constexpr const char *kOffReason = "null binds are off";

		using S = config::NullBindsSettings;
		#define NULL_BINDS_BIND( type, field ) \
			ui::AnyBind::Of<type>( \
				[]{ EnsureConfigLoaded(); return (type)s_Settings.null_binds.field; }, \
				[]( type v ) { EnsureConfigLoaded(); s_Settings.null_binds.field = v; PersistAndRepaint(); } )

		a.Group( "Null binds" );

		a.Switch( "null_binds.enabled", "Enable null binds", NULL_BINDS_BIND( bool, enabled ) )
			.Help( "Makes sure only one of each pair of movement keys is ever held at once: "
			       "pressing the new one instantly lets go of the old one, so you never hold "
			       "both left+right (or forward+back) strafe keys together. Some games' "
			       "anti-cheat -- Valve has said this about CS2 on VAC-secured servers -- can kick "
			       "players whose input looks like this, so if a game you play enforces that, keep "
			       "this off on that game's own profile rather than everywhere." )
			.Default( S{}.enabled )
			.Keywords( "null binds enable snap tap socd" );

		a.Switch( "null_binds.pair_ad", "A / D", NULL_BINDS_BIND( bool, pair_ad ) )
			.Help( "Apply null binds to the A and D keys." )
			.Default( S{}.pair_ad )
			.Keywords( "null binds a d strafe left right" )
			.DisabledUnless( On, kOffReason );

		a.Switch( "null_binds.pair_ws", "W / S", NULL_BINDS_BIND( bool, pair_ws ) )
			.Help( "Apply null binds to the W and S keys." )
			.Default( S{}.pair_ws )
			.Keywords( "null binds w s forward back" )
			.DisabledUnless( On, kOffReason );

		a.Slider( "null_binds.delay_ms", "Delay", NULL_BINDS_BIND( int, delay_ms ) )
			.Help( "How long to wait, after letting go of the old key, before pressing the new "
			       "one. 0 switches instantly." )
			.Range( (float)nullbinds::kMinDelayMs, (float)nullbinds::kMaxDelayMs ).Step( 1.0f ).Unit( "ms" )
			.Default( S{}.delay_ms )
			.Keywords( "null binds delay debounce timing" )
			.DisabledUnless( On, kOffReason );

		a.Slider( "null_binds.jitter_ms", "Randomize +/-", NULL_BINDS_BIND( int, jitter_ms ) )
			.Help( "Adds a small random amount, plus or minus this much, to the delay above so "
			       "every switch takes a slightly different amount of time -- a touch more "
			       "natural than a perfectly fixed delay." )
			.Range( (float)nullbinds::kMinJitterMs, (float)nullbinds::kMaxJitterMs ).Step( 1.0f ).Unit( "ms" )
			.Default( S{}.jitter_ms )
			.Keywords( "null binds jitter random variance" )
			.DisabledUnless( On, kOffReason );

		#undef NULL_BINDS_BIND
	}
}

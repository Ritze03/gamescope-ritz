// The autoclicker -- see Autoclicker.h for why the tick is its own thread and
// for the threading contract this file has to hold up.
#include "Autoclicker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <linux/input-event-codes.h>

#include "SettingsOverlay.h"
#include "steamcompmgr.hpp"
#include "wlserver.hpp"
#include "Config/ConfigManager.h"
#include "Keybinds.h"
#include "UI/Registry.h"

namespace gamescope
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// Config: the same lazy, generation-keyed cache Zoom.cpp keeps, plus
		// a mirror of the fields the wlserver and worker threads read.
		bool s_bConfigLoaded = false;
		uint64_t s_ulLoadedGeneration = 0;
		config::Settings s_Settings;

		std::atomic<bool>     s_bEnabled{ false };
		std::atomic<bool>     s_bToggle{ false };
		std::atomic<int>      s_nCps{ 10 };
		std::atomic<uint32_t> s_uButton{ BTN_LEFT };

		// True only inside the worker's own wlserver_mousebutton() call. See
		// Autoclicker_IsInjecting() and wlserver_ritz_mouse_hotkey().
		std::atomic<bool> s_bInjecting{ false };

		// THE ONE LOCK ORDER, and the reason this cannot deadlock: the
		// wlserver thread takes wlserver_lock() and THEN s_Mutex (a chord
		// arrives inside wlserver_mousebutton(), which asserts that lock);
		// the worker takes s_Mutex and wlserver_lock() at disjoint times,
		// never nested -- Emit() is called with s_Mutex released. So
		// wlserver_lock -> s_Mutex is the only nesting that ever happens.
		std::mutex s_Mutex;
		std::condition_variable s_Cv;
		bool s_bWanted = false;   // guarded by s_Mutex

		void Mirror()
		{
			const config::AutoclickerSettings &a = s_Settings.autoclicker;
			s_bEnabled.store( a.enabled, std::memory_order_relaxed );
			s_bToggle.store( a.mode == "toggle", std::memory_order_relaxed );
			s_nCps.store( std::clamp( a.cps, kAutoclickerMinCps, kAutoclickerMaxCps ), std::memory_order_relaxed );
			s_uButton.store( a.button == "right" ? BTN_RIGHT
			               : a.button == "middle" ? BTN_MIDDLE
			               : BTN_LEFT, std::memory_order_relaxed );
		}

		void SetWanted( bool bWant );

		void EnsureConfigLoaded()
		{
			const uint64_t ulGeneration = config::ConfigGeneration();
			if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
				return;
			s_Settings = config::ResolvedSettings();
			s_ulLoadedGeneration = ulGeneration;
			s_bConfigLoaded = true;
			Mirror();
			// Switching the master switch off -- here or through a profile
			// that has it off -- stops a run in progress rather than leaving
			// it clicking with its own settings page saying "off".
			if ( !s_Settings.autoclicker.enabled )
				SetWanted( false );
		}

		void PersistAndRepaint()
		{
			Mirror();
			if ( !s_Settings.autoclicker.enabled )
				SetWanted( false );
			config::EnqueueRoutedWrite( s_Settings );
			force_repaint();
		}

		// ---- the click itself -------------------------------------------
		// wlserver_mousebutton() is the exact function every real backend
		// calls (OpenVRBackend.cpp:1358-1367 does this same lock/click/unlock
		// bracket for its own synthetic click), so the event goes wherever a
		// real one would -- including, without the flag below, back into the
		// keybind engine. See Autoclicker_IsInjecting().
		void Emit( uint32_t uButton, bool bPressed )
		{
			wlserver_lock();
			s_bInjecting.store( true, std::memory_order_relaxed );
			wlserver_mousebutton( (int)uButton, bPressed, get_time_in_milliseconds() );
			s_bInjecting.store( false, std::memory_order_relaxed );
			wlserver_unlock();
		}

		// THE STUCK-BUTTON GUARANTEE. The button is down between Press() and
		// Release(); anything that leaves the scope holding this -- the loop
		// ending, the chord letting go mid-press, an exception -- emits the
		// matching release on the way out. There is no path out of RunBurst()
		// that skips it, which is the point: a game left holding a mouse
		// button down is the one failure here the user cannot undo from
		// inside the game.
		class ButtonHolder
		{
		public:
			ButtonHolder() = default;
			ButtonHolder( const ButtonHolder & ) = delete;
			ButtonHolder &operator=( const ButtonHolder & ) = delete;
			~ButtonHolder() { Release(); }

			void Press( uint32_t uButton )
			{
				Emit( uButton, true );
				m_uDown = uButton;
			}

			void Release()
			{
				if ( m_uDown == 0 )
					return;
				// Cleared BEFORE the emit so a throw out of Emit() cannot
				// make the destructor release the same button twice.
				const uint32_t uButton = m_uDown;
				m_uDown = 0;
				Emit( uButton, false );
			}

		private:
			uint32_t m_uDown = 0;
		};

		// Sleeps until an ABSOLUTE deadline -- so a late wake-up costs that
		// one half-period and not every one after it -- waking early the
		// moment the chord lets go. Returns false when the run is over.
		//
		// The condition variable IS the sleep, rather than
		// sleep_until_nanos(): an uninterruptible sleep would leave the
		// button down for up to half a period after release (half a SECOND at
		// 1 CPS), and that is exactly the stuck button the holder above
		// exists to prevent. Same absolute-deadline pacing either way.
		bool WaitUntil( Clock::time_point tpDeadline )
		{
			std::unique_lock<std::mutex> lock( s_Mutex );
			s_Cv.wait_until( lock, tpDeadline, []{ return !s_bWanted; } );
			return s_bWanted;
		}

		void RunBurst()
		{
			ButtonHolder holder;
			Clock::time_point tpDeadline = Clock::now();

			for ( ;; )
			{
				// Re-read every cycle: the sliders are live, so a change
				// takes effect on the next click rather than the next press.
				const std::chrono::nanoseconds halfPeriod(
					Autoclicker_HalfPeriodNs( s_nCps.load( std::memory_order_relaxed ) ) );

				// THE OVERLAY GATE. While the settings overlay is capturing,
				// wlserver_dispatch_mouse_button() routes a click into the
				// overlay's own queue instead of the game (wlserver.cpp) --
				// so an ungated autoclicker would sit there clicking the
				// user's own settings UI. The pacing keeps running through
				// it, so letting the overlay go resumes in rhythm rather
				// than with a burst.
				if ( !SettingsOverlay_IsCapturingInput() )
					holder.Press( s_uButton.load( std::memory_order_relaxed ) );

				tpDeadline += halfPeriod;
				if ( !WaitUntil( tpDeadline ) )
					return;               // ~ButtonHolder emits the release

				holder.Release();

				tpDeadline += halfPeriod;
				if ( !WaitUntil( tpDeadline ) )
					return;
			}
		}

		void Worker()
		{
			for ( ;; )
			{
				{
					std::unique_lock<std::mutex> lock( s_Mutex );
					s_Cv.wait( lock, []{ return s_bWanted; } );
				}
				RunBurst();
			}
		}

		// ONE worker for the process, parked on the condition variable
		// whenever the autoclicker is idle, created on the first activation
		// and never joined.
		//
		// `Why not start and join per activation:` Autoclicker_OnChord() runs
		// with wlserver_lock() HELD (Autoclicker.h's threading note), and the
		// worker takes that same lock for every click -- so a join there
		// would deadlock against a worker waiting for the lock the joiner is
		// holding. A parked thread costs one blocked futex and no wakeups.
		//
		// ponytail: the worker is detached and outlives every deactivation,
		// so a teardown mid-click cannot emit the release the holder would
		// (the compositor and the game are both going down at that point
		// anyway). Upgrade path if that ever matters: a stop flag the worker
		// also waits on, signalled from wlserver_shutdown() -- which needs a
		// join site that is not under wlserver_lock().
		void EnsureThread()
		{
			static std::once_flag s_Once;
			std::call_once( s_Once, []{ std::thread( Worker ).detach(); } );
		}

		void SetWanted( bool bWant )
		{
			{
				std::lock_guard<std::mutex> lock( s_Mutex );
				if ( s_bWanted == bWant )
					return;
				s_bWanted = bWant;
			}
			if ( bWant )
				EnsureThread();
			s_Cv.notify_all();
		}

		bool Wanted()
		{
			std::lock_guard<std::mutex> lock( s_Mutex );
			return s_bWanted;
		}

		// Choice rows are int-backed (Registry.h); the file keeps a word, as
		// zoom.mode and crosshair.hide_mode do, so a config stays readable.
		constexpr ui::Option kModeOptions[] = { { 0, "Hold" }, { 1, "Toggle" } };
		constexpr ui::Option kButtonOptions[] = { { 0, "Left" }, { 1, "Right" }, { 2, "Middle" } };
		constexpr const char *kButtonKeys[] = { "left", "right", "middle" };

		int ButtonToInt( const std::string &s )
		{
			for ( int i = 0; i < 3; i++ )
				if ( s == kButtonKeys[ i ] ) return i;
			return 0;
		}
	}

	void Autoclicker_OnChord( bool bPressed )
	{
		// The master switch: with it off the chord does nothing at all, and
		// anything already running stops.
		if ( !s_bEnabled.load( std::memory_order_relaxed ) )
		{
			SetWanted( false );
			return;
		}

		if ( s_bToggle.load( std::memory_order_relaxed ) )
		{
			if ( !bPressed )
				return;              // toggle: the release is a no-op
			SetWanted( !Wanted() );
		}
		else
			SetWanted( bPressed );   // hold: down starts it, up stops it
	}

	bool Autoclicker_IsInjecting()
	{
		return s_bInjecting.load( std::memory_order_relaxed );
	}

	void Autoclicker_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "system.autoclicker", "Autoclicker", ui::Section::System );

		// HIDDEN FROM THE UI (2026-09-22), until the click train has been
		// tested against a real game: no rail entry, no palette rows, no way
		// in from the shell -- the same one-predicate hiding the friends list
		// uses when there is no Steam app id (PanelFriends.cpp). The area is
		// still REGISTERED, so its rows, icon and config keys keep existing
		// and the settings audit still sees them.
		// ponytail: a constant false rather than a debug ConVar or a build
		// flag. Delete this line to ship it; that is the whole re-enable.
		a.AvailableWhen( []{ return false; } );

		a.Keywords( "autoclicker auto click clicker turbo rapid fire spam macro cps" );
		a.Summary( []
		{
			EnsureConfigLoaded();
			return s_Settings.autoclicker.enabled ? std::string( "on" ) : std::string( "off" );
		} );

		auto On = []{ EnsureConfigLoaded(); return s_Settings.autoclicker.enabled; };
		constexpr const char *kOffReason = "the autoclicker is off";

		using S = config::AutoclickerSettings;
		#define AUTOCLICKER_BIND( type, field ) \
			ui::AnyBind::Of<type>( \
				[]{ EnsureConfigLoaded(); return (type)s_Settings.autoclicker.field; }, \
				[]( type v ) { EnsureConfigLoaded(); s_Settings.autoclicker.field = v; PersistAndRepaint(); } )

		a.Group( "Autoclicker" );

		a.Switch( "autoclicker.enabled", "Enable autoclicker", AUTOCLICKER_BIND( bool, enabled ) )
			.Help( "Holds down a mouse button for you, over and over, while the autoclicker key "
			       "is held (or toggled). The clicks go to the game exactly as real ones do. The "
			       "key itself is set under Keybinds." )
			.Default( S{}.enabled )
			.Keywords( "autoclicker enable click auto" );

		a.Facts( "autoclicker.bind", "Autoclicker key",
			[]{ return keybinds::ChordTextFor( keybinds::Action::Autoclicker ); } )
			.Help( "The key or mouse button that clicks. Change it under Settings > Keybinds > "
			       "Autoclicker; mouse buttons are LMB, RMB, MMB, Mouse4 and Mouse5." )
			.Keywords( "autoclicker key bind keybind chord button mouse4" );

		a.Slider( "autoclicker.cps", "Clicks per second", AUTOCLICKER_BIND( int, cps ) )
			.Help( "How many clicks a second. Each click holds the button down for half that "
			       "time, so a game that reads the button rather than the click still sees it. "
			       "Very high rates are paced as closely as the system allows." )
			.Range( (float)kAutoclickerMinCps, (float)kAutoclickerMaxCps ).Step( 1.0f ).Unit( "/s" )
			.Default( S{}.cps )
			.Keywords( "autoclicker cps rate speed clicks per second frequency" )
			.DisabledUnless( On, kOffReason );

		a.Choice( "autoclicker.mode", "Activation",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.autoclicker.mode == "toggle" ? 1 : 0; },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.autoclicker.mode = n == 1 ? "toggle" : "hold"; PersistAndRepaint(); } ),
			kModeOptions, std::size( kModeOptions ) )
			.Help( "Hold: clicking for as long as the key is down. Toggle: one press starts, "
			       "the next stops." )
			.Default( 0 )
			.Keywords( "autoclicker hold toggle mode activation press" )
			.DisabledUnless( On, kOffReason );

		a.Choice( "autoclicker.button", "Button",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return ButtonToInt( s_Settings.autoclicker.button ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.autoclicker.button = kButtonKeys[ std::clamp( n, 0, 2 ) ]; PersistAndRepaint(); } ),
			kButtonOptions, std::size( kButtonOptions ) )
			.Help( "Which mouse button is clicked. This is the button the GAME receives; it does "
			       "not have to be the key that starts the autoclicker." )
			.Default( 0 )
			.Keywords( "autoclicker button left right middle mouse lmb rmb mmb" )
			.DisabledUnless( On, kOffReason );

		#undef AUTOCLICKER_BIND
	}
}

#pragma once

// Autoclicker.h -- a paced synthetic click train on a held or toggled chord
// (2026-09-18). Mouse4 by default, because a side button is the one button an
// autoclicker can own without taking a button the game already wants.
//
// WHAT THIS FILE OWNS: the config (`autoclicker`, AutoclickerSettings), the
// settings area (`system.autoclicker`, MISC), the wanted flag, and the worker
// thread that emits the clicks.
// WHAT IT DOES NOT: the click itself. That is wlserver_mousebutton(), the
// same entry point every real backend uses -- see the threading note below.
//
// WHY A DEDICATED THREAD and not the two tick sources already here:
//   - the frame/paint loop caps at the display's refresh rate, so it
//     structurally cannot reach the 1000 CPS the slider offers;
//   - wl_event_loop_add_timer() has 1 ms granularity, which at 1000 CPS is
//     the entire half-period -- zero headroom.
// So the pacing is one std::thread sleeping to an ABSOLUTE deadline, which is
// also what keeps the rate from drifting: each half-period is scheduled from
// the previous deadline, never from "now", so a late wake-up does not push
// every later click out with it.
//
// Threading: Autoclicker_OnChord() runs on the wlserver thread WITH
// wlserver_lock() HELD (a mouse chord reaches it through
// wlserver_mousebutton(), which asserts that lock) and touches atomics and
// one short-lived mutex only. The worker thread takes wlserver_lock() itself
// around each event, exactly as OpenVRBackend.cpp's synthetic click does.
// The settings the two of them read are mirrored into atomics by the
// steamcompmgr thread whenever the config is (re)loaded, as Zoom.cpp does.

#include <algorithm>
#include <cstdint>

namespace gamescope
{
	namespace ui { class Registry; }

	void Autoclicker_RegisterArea( ui::Registry &reg );

	// The chord went down (true) or came apart (false). Hold or toggle is
	// this module's own setting; a release in toggle mode is a no-op.
	void Autoclicker_OnChord( bool bPressed );

	// True only while the worker thread is inside its own
	// wlserver_mousebutton() call. THE FEEDBACK-LOOP GUARD: the fork's
	// wlserver_ritz_mouse_hotkey() (wlserver.cpp) returns early on this, so a
	// synthetic click never re-enters the keybind engine that produced it.
	// Read on the wlserver thread, under the same wlserver_lock() the worker
	// holds while it is set -- see wlserver.cpp's own comment at the check.
	bool Autoclicker_IsInjecting();

	// The slider's ends. 1000 CPS is where the pacing stops being honest on
	// a normal kernel (a 500 us half-period is already inside the scheduler's
	// own jitter), so it is the ceiling rather than an arbitrary round number.
	inline constexpr int kAutoclickerMinCps = 1;
	inline constexpr int kAutoclickerMaxCps = 1000;

	// Half of one click period, in nanoseconds: press, sleep this long,
	// release, sleep this long -- a 50% duty cycle, because a press and
	// release with no measurable gap between them is missed by any game that
	// polls button state rather than reading the event stream.
	//
	// Pure, with no compositor deps, so it can be unit-tested with no
	// Autoclicker.cpp/link-time dependency -- the same arrangement
	// Zoom_StepFactor() uses and for the same reason.
	inline uint64_t Autoclicker_HalfPeriodNs( int nCps )
	{
		return 500'000'000ull / (uint64_t)std::clamp( nCps, kAutoclickerMinCps, kAutoclickerMaxCps );
	}
}

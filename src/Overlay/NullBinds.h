#pragma once

// NullBinds.h -- WASD null binds / SOCD cleaning (2026-09-27). The user's own
// request, quoting the tool that inspired it (wasiejen/Free-Snap-Tap):
// *"I'll need you to add null binds for WASD. The user should be able to set
// a delay between activating the other key, so between the deactivation of
// the previously held key and the activation of the new key, plus the small
// randomizer just to make it feel a little more natural."*
//
// WHAT THIS IS. Two fixed pairs, A/D and W/S, each independently switchable.
// Within an enabled pair, at most one key is ever let through to the game at
// a time -- "last-input priority": of the pair's physically-held keys, the
// one most recently pressed wins, exactly as upstream Free-Snap-Tap defines
// it. Releasing the winner while the other is still physically held
// re-presses the other. A `Delay` (0..50ms) plus a `Randomize +/-` jitter
// (0..20ms) sit between the synthetic release of the old winner and the
// synthetic press of the new one, so the two edges are not back-to-back --
// see the `Why:` below.
//
// WHY A DELAY AND JITTER AT ALL, RATHER THAN AN INSTANT SWITCH. Free-Snap-Tap
// (https://github.com/wasiejen/Free-Snap-Tap) documents these existing
// specifically so the input still passes anti-cheat "if delays are not set
// too short" -- an instant, sub-millisecond release+press on every direction
// change is a signature a heuristic can key on. Separately, in August 2024
// Valve said it kicks players from Valve (VAC-secured) CS2 servers whose
// input looks like snap-tap/SOCD cleaning. This module does not (cannot)
// know whether a given server enforces that, so its settings-area help text
// says so plainly and points at this fork's own per-game profiles: put a
// game where it is enforced on a profile with this switched off.
//
// WHAT THIS FILE OWNS: a pure, dependency-free SOCD engine (`Engine` below),
// testable with no wlserver/ImGui/Config link at all -- fed by injected time
// and an injected jitter source, exactly as Zoom_StepFactor() and
// Autoclicker_HalfPeriodNs() are pure for the same reason (see those
// headers). NullBinds.cpp is the glue: the config (`null_binds`,
// config::NullBindsSettings, per-profile), the settings area
// (`system.null_binds`, MISC), and the wlserver_key() hook contract below.
//
// THE HOOK CONTRACT (for whichever step wires it into wlserver.cpp):
//   bool NullBinds_OnKey( uint32_t key, bool press, uint32_t time );
// Call this from the very TOP of wlserver_key() (src/wlserver.cpp), on the
// wlserver thread, with wlserver_lock() ALREADY held (wlserver_key() itself
// asserts that), and ONLY when `!NullBinds_IsInjecting()`:
//
//     if ( !gamescope::NullBinds_IsInjecting()
//       && gamescope::NullBinds_OnKey( key, press, time ) )
//         return;
//
// Returning true means the physical event was fully handled here (consumed)
// and wlserver_key() must do nothing further with it -- NullBinds_OnKey()
// has already, from inside the SAME call and under the SAME held lock,
// called wlserver_key() itself (recursively, NOT re-locking -- the lock is
// already held) for every immediate synthetic key event the engine produced
// (e.g. releasing the old winner the instant a new one wins). Because the
// hook sits before wlserver_key()'s own xkb/pressed-array/hotkey/dispatch
// work and returns early when consumed, each synthetic call runs that whole
// normal path on its own, so the client's keyboard state stays consistent
// (the game sees clean, individually-dispatched press/release events, never
// a physical event that silently did two things at once). Returning false
// means either the key is not part of an enabled pair (or the feature is
// off) -- wlserver_key() must process the physical event exactly as if this
// hook did not exist.
//
// Threading: NullBinds_OnKey() runs on the wlserver thread with
// wlserver_lock() held, exactly as Autoclicker_OnChord()/Zoom_OnChord() do,
// and touches the engine under one short-lived module mutex. A DELAYED
// emit (the press that lands after `Delay` + jitter) is produced by a
// dedicated worker thread paced against absolute steady_clock deadlines,
// copying Autoclicker.cpp's WaitUntil()/Worker() shape exactly: the worker
// takes the module mutex and wlserver_lock() at DISJOINT times, never
// nested, so the one lock order that ever nests is wlserver thread's
// wlserver_lock() -> module mutex (see NullBinds.cpp's own comment for the
// full statement, copied from Autoclicker.cpp:43-49).

#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include <linux/input-event-codes.h>

namespace gamescope
{
	namespace ui { class Registry; }

	void NullBinds_RegisterArea( ui::Registry &reg );

	// See "THE HOOK CONTRACT" above.
	bool NullBinds_OnKey( uint32_t key, bool press, uint32_t time );

	// True only while a NullBinds-produced synthetic key event is inside its
	// own recursive/worker-thread wlserver_key() call -- the same
	// feedback-loop guard as Autoclicker_IsInjecting(): without it, a
	// synthetic A or D would re-enter this very hook and be reinterpreted as
	// a second physical press.
	bool NullBinds_IsInjecting();

	namespace nullbinds
	{
		// The slider ends (Delay, Randomize +/-) and the resulting cap on
		// the computed gap between the old key's release and the new key's
		// press: clamp(delay_ms + jitter_offset, 0, kMaxDelayMs + kMaxJitterMs).
		inline constexpr int kMinDelayMs  = 0;
		inline constexpr int kMaxDelayMs  = 50;
		inline constexpr int kMinJitterMs = 0;
		inline constexpr int kMaxJitterMs = 20;

		// Deliberately its OWN struct, not config::NullBindsSettings: this
		// header has no Config/ dependency (the "pure, testable" contract
		// above), and ConfigSchema.h is a large, unrelated header a unit
		// test of the engine alone should not need to pull in. NullBinds.cpp
		// is the one place that translates between the two.
		struct Settings
		{
			bool enabled  = false;
			bool pair_ad  = true;
			bool pair_ws  = true;
			int  delay_ms  = 5;
			int  jitter_ms = 3;
		};

		// One synthetic key event the caller must send (immediately, in the
		// order given) to wlserver_key().
		struct Emit
		{
			uint32_t key;
			bool     press;
		};

		// The outcome of any one call into the engine.
		struct Result
		{
			// OnPhysical() only: whether the triggering physical event was
			// part of an enabled pair and is therefore fully handled here --
			// the caller's own return value. Always false from OnTimer()/
			// SetSettings(), which have no "the physical event" to consume.
			bool consumed = false;

			// Emit these, in order, right now.
			std::vector<Emit> emits;

			// The engine's CURRENT earliest pending deadline across both
			// pairs, recomputed fresh after every call -- not just whether
			// this call scheduled one. A caller (the worker thread) reads
			// this after every OnPhysical()/OnTimer()/SetSettings() call to
			// know what to wait for next.
			bool     has_deadline = false;
			uint64_t deadline_ms  = 0;
		};

		// uniform_int(-jitter_ms, +jitter_ms). The engine's default is a
		// real std::mt19937 draw (seeded once, on first use, from
		// std::random_device) so production code needs to wire up nothing
		// -- tests call SetJitterSource() to make the draw deterministic.
		// Only ever invoked from inside a call the caller has already
		// serialized (OnPhysical()/SetSettings(), both single-threaded by
		// contract -- see NullBinds.cpp's lock-order comment), so the
		// shared RNG the default uses needs no lock of its own.
		using JitterFn = std::function<int( int jitter_ms )>;

		// A fixed pair of raw evdev keycodes, index 0 vs index 1 -- which
		// index is "first" only matters for reading the code (A before D,
		// W before S); the last-input-priority rule below treats both
		// indices identically.
		enum class PairId { AD = 0, WS = 1 };

		// ------------------------------------------------------------------
		// The engine. Pure: no wlserver, no ImGui, no Config/ include, no
		// I/O, no threads of its own. Every side effect is returned in a
		// Result for the caller to actually perform. Driven entirely by
		// injected time (milliseconds, monotonic but otherwise domain-free
		// -- the caller picks one clock and stays with it across every
		// OnPhysical()/OnTimer() call) and an injected jitter source.
		//
		// THE RULE, PRECISELY (last-input priority, Free-Snap-Tap's own
		// definition). Each pair tracks which of its two keys is physically
		// held and, of the ones currently held, which was pressed most
		// recently ("the desired winner"). Whenever the desired winner
		// changes:
		//   - if a key is currently actually sent (down in-game) for this
		//     pair, it is released immediately -- the two pair keys are
		//     never both down together in-game;
		//   - if the pair was genuinely idle before this event (neither key
		//     held), the new winner is pressed immediately too -- nothing to
		//     debounce, there was no other key to switch away from;
		//   - otherwise (a real switch: either a press while the other was
		//     already held, or a release that hands priority to the key
		//     still held) the new winner's press is scheduled `Delay` +/-
		//     jitter ms later instead, and any earlier scheduled press for
		//     this pair that hasn't fired yet is cancelled first -- rapid
		//     back-and-forth restarts the debounce rather than stacking
		//     presses.
		// A key that is not the winner produces no output at all: releasing
		// the currently-losing key (still physically held, never sent)
		// changes nothing about what is sent.
		class Engine
		{
		public:
			// Fast, allocation-free check for the caller (NullBinds.cpp) to
			// skip taking its module mutex at all for the other ~100 keys
			// on the keyboard.
			static bool IsPairKey( uint32_t key )
			{
				int pairIdx, keyIdx;
				Locate( key, pairIdx, keyIdx );
				return pairIdx >= 0;
			}

			void SetJitterSource( JitterFn fn )
			{
				m_Jitter = std::move( fn );
			}

			// Applies new settings. Any pair that goes from active
			// (feature AND that pair both on) to inactive is reconciled
			// IMMEDIATELY, in this same call, to whatever is physically
			// held right now: release any sent key no longer held, press
			// any held key not yet sent -- so turning the feature (or one
			// pair) off while keys are down leaves nothing stuck, and both
			// keys simply pass through like a normal keyboard from then on.
			// A pair that goes from inactive to active resets to a clean
			// slate (see the comment at ReconcileActivate below) -- this is
			// a deliberate, documented simplification: the engine cannot
			// know true press recency for events it never saw while off.
			Result SetSettings( const Settings &s )
			{
				Settings ns = s;
				ns.delay_ms  = std::clamp( ns.delay_ms,  kMinDelayMs,  kMaxDelayMs );
				ns.jitter_ms = std::clamp( ns.jitter_ms, kMinJitterMs, kMaxJitterMs );

				const bool bOldActiveAD = PairActive( 0 );
				const bool bOldActiveWS = PairActive( 1 );
				m_Settings = ns;
				const bool bNewActiveAD = PairActive( 0 );
				const bool bNewActiveWS = PairActive( 1 );

				Result result;
				ReconcileTransition( m_AD, 0, bOldActiveAD, bNewActiveAD, result );
				ReconcileTransition( m_WS, 1, bOldActiveWS, bNewActiveWS, result );
				FillDeadline( result );
				return result;
			}

			// The physical key event. `now_ms` and every later `now_ms`
			// this engine is called with must come from the SAME monotonic
			// clock -- deadlines are `now_ms + d`, compared against a later
			// OnTimer()'s own `now_ms`.
			Result OnPhysical( uint32_t key, bool press, uint64_t now_ms )
			{
				Result result;
				int pairIdx, keyIdx;
				Locate( key, pairIdx, keyIdx );
				if ( pairIdx < 0 || !PairActive( pairIdx ) )
				{
					result.consumed = false;
					FillDeadline( result );
					return result;
				}
				result.consumed = true;

				PairState &p = PairAt( pairIdx );
				const int nOldDesired = p.desired;
				if ( press )
				{
					p.held[ keyIdx ] = true;
					p.seq[ keyIdx ] = ++p.seq_counter;
				}
				else
				{
					p.held[ keyIdx ] = false;
				}
				const int nNewDesired = ComputeDesired( p );

				if ( nNewDesired != nOldDesired )
				{
					// Any earlier scheduled press for this pair is stale
					// the instant the desired winner changes again --
					// cancel it before deciding what (if anything) to do
					// now. It was never sent, so cancelling it alone needs
					// no release.
					p.pending_active = false;

					// The pair's own sent-state invariant during normal
					// (active) operation is "at most one of sent[0]/
					// sent[1] is true" -- but this loop is written to be
					// correct even if that ever doesn't hold (e.g. right
					// after ReconcileActivate has to break a tie), which
					// is why it checks both rather than assuming one.
					for ( int i = 0; i < 2; i++ )
					{
						if ( p.sent[ i ] )
						{
							result.emits.push_back( { KeyOf( pairIdx, i ), false } );
							p.sent[ i ] = false;
						}
					}

					if ( nNewDesired != -1 )
					{
						if ( nOldDesired == -1 )
						{
							// The pair was genuinely idle -- nothing to
							// debounce against.
							result.emits.push_back( { KeyOf( pairIdx, nNewDesired ), true } );
							p.sent[ nNewDesired ] = true;
						}
						else
						{
							const int d = ComputeDelayMs();
							if ( d <= 0 )
							{
								// delay=0, jitter=0 (or a jitter draw that
								// clamped to 0): no reason to schedule a
								// wakeup for zero time -- send it now, right
								// after the release above, so the two go out
								// back-to-back with the release first.
								result.emits.push_back( { KeyOf( pairIdx, nNewDesired ), true } );
								p.sent[ nNewDesired ] = true;
							}
							else
							{
								p.pending_active = true;
								p.pending_target = nNewDesired;
								p.pending_deadline_ms = now_ms + (uint64_t)d;
							}
						}
					}
					p.desired = nNewDesired;
				}

				FillDeadline( result );
				return result;
			}

			// Fires any pending press whose deadline has passed. The
			// caller (the worker thread) is expected to call this at or
			// after the deadline the previous Result reported.
			Result OnTimer( uint64_t now_ms )
			{
				Result result;
				FirePendingIfDue( m_AD, 0, now_ms, result );
				FirePendingIfDue( m_WS, 1, now_ms, result );
				FillDeadline( result );
				return result;
			}

		private:
			struct PairState
			{
				bool     held[ 2 ]  = { false, false };
				uint64_t seq[ 2 ]   = { 0, 0 };
				uint64_t seq_counter = 0;
				bool     sent[ 2 ]  = { false, false };
				int      desired    = -1; // -1 = neither key held
				bool     pending_active = false;
				int      pending_target = -1;
				uint64_t pending_deadline_ms = 0;
			};

			static void Locate( uint32_t key, int &pairIdx, int &keyIdx )
			{
				if ( key == KEY_A ) { pairIdx = 0; keyIdx = 0; return; }
				if ( key == KEY_D ) { pairIdx = 0; keyIdx = 1; return; }
				if ( key == KEY_W ) { pairIdx = 1; keyIdx = 0; return; }
				if ( key == KEY_S ) { pairIdx = 1; keyIdx = 1; return; }
				pairIdx = -1; keyIdx = -1;
			}

			static uint32_t KeyOf( int pairIdx, int keyIdx )
			{
				static constexpr uint32_t kKeys[ 2 ][ 2 ] = { { KEY_A, KEY_D }, { KEY_W, KEY_S } };
				return kKeys[ pairIdx ][ keyIdx ];
			}

			PairState &PairAt( int i ) { return i == 0 ? m_AD : m_WS; }

			bool PairActive( int i ) const
			{
				return m_Settings.enabled && ( i == 0 ? m_Settings.pair_ad : m_Settings.pair_ws );
			}

			static int ComputeDesired( const PairState &p )
			{
				if ( p.held[ 0 ] && p.held[ 1 ] )
					return p.seq[ 0 ] > p.seq[ 1 ] ? 0 : 1;
				if ( p.held[ 0 ] ) return 0;
				if ( p.held[ 1 ] ) return 1;
				return -1;
			}

			int ComputeDelayMs() const
			{
				const int nJitter = m_Settings.jitter_ms > 0 ? m_Jitter( m_Settings.jitter_ms ) : 0;
				return std::clamp( m_Settings.delay_ms + nJitter, kMinDelayMs, kMaxDelayMs + kMaxJitterMs );
			}

			static void FirePendingIfDue( PairState &p, int pairIdx, uint64_t now_ms, Result &result )
			{
				if ( p.pending_active && now_ms >= p.pending_deadline_ms )
				{
					result.emits.push_back( { KeyOf( pairIdx, p.pending_target ), true } );
					p.sent[ p.pending_target ] = true;
					p.pending_active = false;
				}
			}

			// bWasActive -> bWillBeActive for one pair. Only the two
			// "changed" directions do anything.
			static void ReconcileTransition( PairState &p, int pairIdx, bool bWasActive, bool bWillBeActive, Result &result )
			{
				if ( bWasActive == bWillBeActive )
					return;

				if ( !bWillBeActive )
					ReconcileDeactivate( p, pairIdx, result );
				else
					ReconcileActivate( p, pairIdx, result );
			}

			// Going inactive (feature or this pair switched off): from this
			// instant the two physical keys pass straight through, so make
			// "sent" match "held" exactly, one key at a time -- this is the
			// whole of the "no stuck keys" guarantee on the way out.
			static void ReconcileDeactivate( PairState &p, int pairIdx, Result &result )
			{
				p.pending_active = false;
				for ( int i = 0; i < 2; i++ )
				{
					if ( p.held[ i ] && !p.sent[ i ] )
					{
						result.emits.push_back( { KeyOf( pairIdx, i ), true } );
						p.sent[ i ] = true;
					}
					else if ( !p.held[ i ] && p.sent[ i ] )
					{
						result.emits.push_back( { KeyOf( pairIdx, i ), false } );
						p.sent[ i ] = false;
					}
				}
				p.desired = -1;
			}

			// Going active (feature and this pair switched on): the engine
			// was not watching physical events while inactive, so it has no
			// real press-recency to resume from. If a stale reconcile left
			// both keys "sent" (both were physically held while off, which
			// ReconcileDeactivate lets through), the at-most-one-sent
			// invariant is restored by RELEASING index 1 (D or S) --
			// emitted, not just cleared internally, since the game already
			// thinks that key is down -- and keeping index 0 (A or W): an
			// arbitrary but deterministic tie-break, not a correctness claim
			// about which was pressed more recently. No emit is needed for
			// the ordinary case (0 or 1 keys held): "desired" just picks up
			// whichever is (or isn't) already sent.
			static void ReconcileActivate( PairState &p, int pairIdx, Result &result )
			{
				if ( p.sent[ 0 ] && p.sent[ 1 ] )
				{
					result.emits.push_back( { KeyOf( pairIdx, 1 ), false } );
					p.sent[ 1 ] = false;
				}
				p.pending_active = false;
				p.desired = p.sent[ 0 ] ? 0 : ( p.sent[ 1 ] ? 1 : -1 );
			}

			void FillDeadline( Result &result ) const
			{
				result.has_deadline = false;
				if ( m_AD.pending_active )
				{
					result.has_deadline = true;
					result.deadline_ms = m_AD.pending_deadline_ms;
				}
				if ( m_WS.pending_active &&
				     ( !result.has_deadline || m_WS.pending_deadline_ms < result.deadline_ms ) )
				{
					result.has_deadline = true;
					result.deadline_ms = m_WS.pending_deadline_ms;
				}
			}

			Settings m_Settings;
			PairState m_AD;
			PairState m_WS;
			JitterFn m_Jitter = []( int nJitterMs ) -> int
			{
				static std::mt19937 s_Rng{ std::random_device{}() };
				std::uniform_int_distribution<int> dist( -nJitterMs, nJitterMs );
				return dist( s_Rng );
			};
		};
	}
}

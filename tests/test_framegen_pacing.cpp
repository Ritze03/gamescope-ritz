// Unit tests for src/FrameGen/Pacing.h -- the pure half of frame-generation
// pacing (decisions D5, D9, D10, D12, D14, D16, D21 of the FG plan). No
// compositor, no Vulkan, no clock: every test drives the pacer with times it
// picks itself.
//
// `Driver` below stands in for steamcompmgr's main loop on a fixed-refresh
// display: real frames arrive at their own times, a vblank paints iff a frame
// arrived since the last paint or the last decision asked for a repaint (that
// is hasRepaint + force_repaint()), and what each paint decided is logged.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "FrameGen/Pacing.h"

using namespace fgpacing;

namespace
{
	constexpr Ns MS = kNsPerMs;

	struct Painted
	{
		Ns t;
		Pacer::Decision d;
	};

	struct Driver
	{
		Pacer pacer;
		double refreshHz = 120.0;
		int chosenN = 2;
		// Generation cost per unit of N, ms; < 0 = the GPU cannot time it. The renderer
		// reports the cost of the last pair that GENERATED, i.e. costPerN x the N it ran
		// at, and keeps reporting that value (stale) while nothing generates.
		double costPerNMs = -1.0;
		bool rendererOk = true;
		uint64_t focusKey = 1;
		LayerKey layer{ 1920, 1080, 87 };

		Ns now = 0;
		Ns nextVblank = 0;
		uint64_t newestId = 0;
		bool wantPaint = false;
		bool forced = false;
		float lastCostMs = -1.0f;
		Ns lastArrival = 0;
		std::vector<Painted> log;

		Ns VblankNs() const { return Ns( 1e9 / refreshHz ); }

		Pacer::Inputs Inputs( Ns t ) const
		{
			Pacer::Inputs in;
			in.now = t;
			in.chosenN = chosenN;
			in.refreshHz = refreshHz;
			in.costMs = lastCostMs;
			in.rendererOk = rendererOk;
			return in;
		}

		void Arrive( Ns t )
		{
			newestId++;
			lastArrival = t;
			pacer.OnArrival( newestId, t, focusKey, layer );
			wantPaint = true;
		}

		Pacer::Decision Paint( Ns t, bool bVblank = true )
		{
			const Pacer::Decision d = pacer.OnPaint( Inputs( t ), newestId, bVblank );
			log.push_back( { t, d } );
			forced = d.repaintNext;
			if ( d.k >= 1 )
				lastCostMs = costPerNMs >= 0.0 ? float( costPerNMs * d.n ) : -1.0f;
			return d;
		}

		void Vblank()
		{
			if ( wantPaint || forced )
			{
				wantPaint = false;
				Paint( nextVblank );
			}
			nextVblank += VblankNs();
		}

		// Vblanks only (no new frames) for dur: lets the last pair play out.
		void Settle( Ns dur )
		{
			const Ns tEnd = now + dur;
			while ( nextVblank < tEnd )
			{
				now = nextVblank;
				Vblank();
			}
			now = tEnd;
		}

		// A real frame arrives at t (the vblanks before it run first), and the next
		// vblank paints. Returns what that paint decided.
		Pacer::Decision ArriveAndPaint( Ns t )
		{
			while ( nextVblank < t )
			{
				now = nextVblank;
				Vblank();
			}
			now = t;
			Arrive( t );
			now = nextVblank;
			Vblank();
			return log.back().d;
		}

		// Advance to tEnd: game frames every flIntervalMs starting at tFirstArrival,
		// vblanks on the fixed grid. An arrival at the same instant as a vblank goes
		// first (it was ready before the loop looked).
		void Run( Ns tEnd, double flIntervalMs, Ns tFirstArrival )
		{
			Ns nextArrival = tFirstArrival;
			uint64_t nArrival = 0;
			while ( nextVblank < tEnd || nextArrival < tEnd )
			{
				if ( nextArrival <= nextVblank && nextArrival < tEnd )
				{
					now = nextArrival;
					Arrive( nextArrival );
					nArrival++;
					nextArrival = tFirstArrival + Ns( double( nArrival ) * flIntervalMs * double( MS ) );
				}
				else if ( nextVblank < tEnd )
				{
					now = nextVblank;
					Vblank();
				}
				else
					break;
			}
			now = tEnd;
		}

		// The last full pair in the log: the paints that share the final pairId.
		std::vector<Pacer::Decision> LastPair() const
		{
			std::vector<Pacer::Decision> out;
			if ( log.empty() )
				return out;
			const uint64_t id = log.back().d.pairId;
			for ( size_t i = log.size(); i-- > 0 && log[ i ].d.pairId == id; )
				out.insert( out.begin(), log[ i ].d );
			return out;
		}
	};
}

// ---------------------------------------------------------------------------
//  D5 / slot sequence
// ---------------------------------------------------------------------------

TEST_CASE( "framegen pacing: fixed-refresh slot sequence for N = 2, 3, 4", "[framegen_pacing]" )
{
	// 30 fps on 120 Hz: every N in 2..4 fits (30 x 4 = 120 <= 144).
	for ( int n = 2; n <= 4; n++ )
	{
		Driver drv;
		drv.chosenN = n;
		drv.refreshHz = 120.0;
		// Arrivals 1 ms before a vblank (vblanks are every 8.33 ms, the game every 33.33 ms).
		drv.Run( 2000 * MS, 1000.0 / 30.0, Ns( 7.3 * double( MS ) ) );
		drv.Settle( 40 * MS );

		INFO( "N = " << n );
		REQUIRE( drv.pacer.ActiveN() == n );

		// The paints of the last COMPLETE pair: slots 1..n-1 then the real frame, on
		// consecutive vblanks. (The log ends right after the real frame of the last
		// arrival, since nothing repaints after it.)
		const auto pair = drv.LastPair();
		REQUIRE( int( pair.size() ) == n );
		for ( int i = 0; i < n - 1; i++ )
		{
			CHECK( pair[ i ].k == i + 1 );
			CHECK( pair[ i ].n == n );
		}
		CHECK( pair[ n - 1 ].k == 0 );   // the real frame
		CHECK( pair[ n - 1 ].n == n );
		// only the last generated slot is NOT followed by another generated one
		for ( int i = 0; i < n - 1; i++ )
			CHECK( pair[ i ].repaintNext );
		CHECK_FALSE( pair[ n - 1 ].repaintNext );

		// consecutive vblanks
		const size_t nLog = drv.log.size();
		for ( int i = 1; i < n; i++ )
			CHECK( drv.log[ nLog - n + i ].t - drv.log[ nLog - n + i - 1 ].t == drv.VblankNs() );

		// and no reset in steady state
		for ( size_t i = 1; i < drv.log.size(); i++ )
			CHECK_FALSE( drv.log[ i ].d.reset );
	}
}

TEST_CASE( "framegen pacing: the first slot is the first vblank after the real frame arrives", "[framegen_pacing]" )
{
	Driver drv;
	drv.chosenN = 2;
	drv.refreshHz = 120.0;
	drv.Run( 1000 * MS, 1000.0 / 30.0, Ns( 7.3 * double( MS ) ) );
	drv.Settle( 40 * MS );

	// Arrival at t = 7.3 + i * 33.33 ms; the vblank grid is every 8.33 ms.
	// The paint carrying slot 1 of each pair is the first vblank at or after it.
	const size_t nLog = drv.log.size();
	REQUIRE( nLog >= 4 );
	const Painted &slot1 = drv.log[ nLog - 2 ];
	const Painted &real = drv.log[ nLog - 1 ];
	REQUIRE( slot1.d.k == 1 );
	REQUIRE( real.d.k == 0 );
	CHECK( real.t - slot1.t == drv.VblankNs() );
	// no vblank between arrival and slot 1 was skipped: slot 1 lands within one
	// refresh of the arrival that made it
	const Ns tArrival = Ns( 7.3 * double( MS ) ) + Ns( double( drv.newestId - 1 ) * ( 1000.0 / 30.0 ) * double( MS ) );
	CHECK( slot1.t >= tArrival );
	CHECK( slot1.t - tArrival < drv.VblankNs() );
}

TEST_CASE( "framegen pacing: a UI repaint with nothing new shows the real frame again", "[framegen_pacing]" )
{
	Driver drv;
	drv.chosenN = 2;
	drv.refreshHz = 120.0;
	drv.Run( 1000 * MS, 1000.0 / 30.0, Ns( 7.3 * double( MS ) ) );
	drv.Settle( 40 * MS );
	REQUIRE( drv.log.back().d.k == 0 );
	const uint64_t id = drv.log.back().d.pairId;

	const Pacer::Decision a = drv.Paint( drv.log.back().t + 20 * MS );
	CHECK( a.pairId == id );
	CHECK( a.k == 0 );
	CHECK( a.n == 2 );
	CHECK_FALSE( a.repaintNext );
	CHECK_FALSE( a.reset );
}

TEST_CASE( "framegen pacing: a paint that is not a vblank repeats the current slot", "[framegen_pacing]" )
{
	Driver drv;
	drv.chosenN = 3;
	drv.refreshHz = 120.0;
	drv.Run( 500 * MS, 1000.0 / 30.0, Ns( 7.3 * double( MS ) ) );
	drv.Settle( 30 * MS );
	// Stop right after slot 1 of a pair has been shown.
	const Pacer::Decision s1 = drv.ArriveAndPaint( drv.nextVblank - 2 * MS );
	REQUIRE( s1.k == 1 );

	// a cursor-move repaint between vblanks
	const Pacer::Decision rep = drv.Paint( drv.nextVblank - 3 * MS, false );
	CHECK( rep.pairId == s1.pairId );
	CHECK( rep.k == 1 );
	CHECK( rep.n == 3 );
	CHECK( rep.repaintNext );   // the pair is not finished

	// the next real vblank is slot 2, not 3
	const Pacer::Decision s2 = drv.Paint( drv.nextVblank );
	CHECK( s2.k == 2 );
}

// ---------------------------------------------------------------------------
//  D9
// ---------------------------------------------------------------------------

TEST_CASE( "framegen pacing: an early arrival drops the rest of the pair and starts the next at once", "[framegen_pacing]" )
{
	Driver drv;
	drv.chosenN = 4;
	drv.refreshHz = 120.0;
	drv.Run( 500 * MS, 1000.0 / 30.0, Ns( 7.3 * double( MS ) ) );
	drv.Settle( 30 * MS );
	REQUIRE( drv.pacer.ActiveN() == 4 );

	// A new real frame; show its slot 1 and slot 2 ...
	const Pacer::Decision a1 = drv.ArriveAndPaint( drv.nextVblank - 1 * MS );
	const uint64_t idA = drv.newestId;
	drv.Vblank();                       // the pair asked for this repaint
	const Pacer::Decision a2 = drv.log.back().d;
	REQUIRE( a1.pairId == idA );
	REQUIRE( a1.k == 1 );
	REQUIRE( a2.k == 2 );

	// ... and the next real frame arrives before slot 3 and the held real frame.
	const Pacer::Decision b1 = drv.ArriveAndPaint( drv.nextVblank - 1 * MS );
	const uint64_t idB = drv.newestId;

	// The very next vblank is slot 1 of the NEW pair: no slot 3 of A, no real A.
	CHECK( b1.pairId == idB );
	CHECK( b1.k == 1 );
	CHECK( b1.n == 4 );
	CHECK( b1.repaintNext );
	CHECK_FALSE( b1.reset );

	// A never reached the screen as a real frame and its slot 3 never played.
	for ( const Painted &p : drv.log )
		if ( p.d.pairId == idA )
			CHECK( ( p.d.k == 1 || p.d.k == 2 ) );
}

TEST_CASE( "framegen pacing: delay does not pile up when the game runs a little above refresh/N", "[framegen_pacing]" )
{
	// 70 fps x 2 = 140 <= 1.2 x 120: fits, but a pair (2 refreshes = 16.7 ms) is
	// longer than the game interval (14.3 ms), so pairs are dropped. Each paint's
	// pair must be the NEWEST frame at that time (never an older one).
	Driver drv;
	drv.chosenN = 2;
	drv.refreshHz = 120.0;
	drv.Run( 3000 * MS, 1000.0 / 70.0, Ns( 3.1 * double( MS ) ) );
	REQUIRE( drv.pacer.ActiveN() == 2 );

	for ( size_t i = 1; i < drv.log.size(); i++ )
		CHECK( drv.log[ i ].d.pairId >= drv.log[ i - 1 ].d.pairId );

	// and each real frame's slot 1 is shown within one refresh of its arrival
	// (the pair is dropped to the next one, never queued behind it)
	size_t nSlot1 = 0;
	for ( const Painted &p : drv.log )
		if ( p.d.k == 1 )
			nSlot1++;
	CHECK( nSlot1 > 100 );
}

// ---------------------------------------------------------------------------
//  D14
// ---------------------------------------------------------------------------

TEST_CASE( "framegen pacing: interval is the median of the last 8, clamped to 4..50 ms", "[framegen_pacing]" )
{
	IntervalEstimator e;
	CHECK_FALSE( e.Valid() );
	CHECK( e.IntervalMs() == 0.0 );

	for ( int i = 0; i < 3; i++ )
		e.AddInterval( 10 * MS );
	CHECK_FALSE( e.Valid() );          // 3 < kMinSamples
	e.AddInterval( 10 * MS );
	CHECK( e.Valid() );
	CHECK( e.IntervalMs() == 10.0 );

	// An outlier does not move a median.
	e.AddInterval( 90 * MS );
	CHECK( e.IntervalMs() == 10.0 );

	// Even count: mean of the two middle values (10, 10, 10, 10, 10, 90 -> 10).
	// 2 more outliers: 10 x4, 90 x3 -> sorted 10 10 10 10 90 90 90 -> 10.
	e.AddInterval( 90 * MS );
	e.AddInterval( 90 * MS );
	CHECK( e.IntervalMs() == 10.0 );
	// Eighth: 10 x4, 90 x4 -> mean(10, 90) = 50.
	e.AddInterval( 90 * MS );
	CHECK( e.Count() == 8 );
	CHECK( e.IntervalMs() == 50.0 );

	// Only the LAST 8 count: nine more 20 ms frames push everything else out.
	for ( int i = 0; i < 9; i++ )
		e.AddInterval( 20 * MS );
	CHECK( e.Count() == 8 );
	CHECK( e.IntervalMs() == 20.0 );
}

TEST_CASE( "framegen pacing: the interval estimate is clamped", "[framegen_pacing]" )
{
	IntervalEstimator fast;
	for ( int i = 0; i < 8; i++ )
		fast.AddInterval( 1 * MS );       // a batch of commits: ~1 ms apart
	CHECK( fast.IntervalMs() == kMinIntervalMs );

	IntervalEstimator slow;
	for ( int i = 0; i < 8; i++ )
		slow.AddInterval( 80 * MS );      // 12.5 fps (still under the 100 ms gap reset)
	CHECK( slow.IntervalMs() == kMaxIntervalMs );

	CHECK( ClampIntervalMs( 3.99 ) == 4.0 );
	CHECK( ClampIntervalMs( 50.01 ) == 50.0 );
	CHECK( ClampIntervalMs( 16.6 ) == 16.6 );

	CHECK( MedianOf( nullptr, 0 ) == 0.0 );
	const double v[ 3 ] = { 3.0, 1.0, 2.0 };
	CHECK( MedianOf( v, 3 ) == 2.0 );
}

TEST_CASE( "framegen pacing: the interval comes from arrival times, not from when vblanks latch them", "[framegen_pacing]" )
{
	// 60 fps on 144 Hz: latch times are quantised to 6.94 ms, arrivals are not.
	Driver drv;
	drv.chosenN = 2;
	drv.refreshHz = 144.0;
	drv.Run( 1000 * MS, 1000.0 / 60.0, Ns( 2.0 * double( MS ) ) );
	REQUIRE( drv.pacer.EstimateValid() );
	CHECK( std::fabs( drv.pacer.IntervalMs() - 1000.0 / 60.0 ) < 0.001 );

	const Pacer::Report s = drv.pacer.TakeStatus( drv.Inputs( drv.now ), drv.now );
	CHECK( std::fabs( s.gameFps - 60.0f ) < 0.01f );
}

// ---------------------------------------------------------------------------
//  D5 / D21
// ---------------------------------------------------------------------------

TEST_CASE( "framegen pacing: fits thresholds are 0.6 / 0.4 / 0.3 x refresh", "[framegen_pacing]" )
{
	// 144 Hz: 2x up to 86.4 fps, 3x up to 57.6, 4x up to 43.2.
	CHECK( Fits( 1000.0 / 86.0, 2, 144.0 ) );
	CHECK_FALSE( Fits( 1000.0 / 87.0, 2, 144.0 ) );
	CHECK( Fits( 1000.0 / 57.0, 3, 144.0 ) );
	CHECK_FALSE( Fits( 1000.0 / 58.0, 3, 144.0 ) );
	CHECK( Fits( 1000.0 / 43.0, 4, 144.0 ) );
	CHECK_FALSE( Fits( 1000.0 / 44.0, 4, 144.0 ) );

	// exactly on the line fits (60 fps x 2 on a 100 Hz display is 120 = 1.2 x 100)
	CHECK( Fits( 1000.0 / 60.0, 2, 100.0 ) );
	CHECK( Fits( 1000.0 / 30.0, 4, 100.0 ) );
	CHECK( Fits( 1000.0 / 40.0, 3, 100.0 ) );
	CHECK_FALSE( Fits( 1000.0 / 31.0, 4, 100.0 ) );

	// nonsense in, "no"
	CHECK_FALSE( Fits( 0.0, 2, 144.0 ) );
	CHECK_FALSE( Fits( 16.0, 2, 0.0 ) );
}

TEST_CASE( "framegen pacing: effective N is the highest that fits, below 2 is pass-through", "[framegen_pacing]" )
{
	const double hz = 144.0;

	// Everything fits: the chosen N.
	NChoice c = ChooseN( 4, 1000.0 / 40.0, hz, -1.0 );
	CHECK( c.n == 4 );
	CHECK( c.reason == Reason::Normal );

	// 50 fps: 4x needs 200 > 172.8, 3x needs 150: steps down to 3.
	c = ChooseN( 4, 1000.0 / 50.0, hz, -1.0 );
	CHECK( c.n == 3 );
	CHECK( c.reason == Reason::RefreshLimit );

	// 70 fps: only 2x (140).
	c = ChooseN( 4, 1000.0 / 70.0, hz, -1.0 );
	CHECK( c.n == 2 );
	CHECK( c.reason == Reason::RefreshLimit );

	// 90 fps: not even 2x (180 > 172.8): pass-through, because the game's rate
	// already fills the display.
	c = ChooseN( 4, 1000.0 / 90.0, hz, -1.0 );
	CHECK( c.n == 0 );
	CHECK( c.reason == Reason::GameTooFast );

	// A chosen N below what fits is just honoured.
	c = ChooseN( 2, 1000.0 / 30.0, hz, -1.0 );
	CHECK( c.n == 2 );
	CHECK( c.reason == Reason::Normal );

	// Off.
	c = ChooseN( 0, 1000.0 / 30.0, hz, -1.0 );
	CHECK( c.n == 0 );
	CHECK( c.reason == Reason::Off );

	// The 60 Hz case from the plan: 2x of a 30 fps game fits (60 <= 72), 45 fps
	// does not (90 > 72).
	CHECK( ChooseN( 2, 1000.0 / 30.0, 60.0, -1.0 ).n == 2 );
	CHECK( ChooseN( 2, 1000.0 / 45.0, 60.0, -1.0 ).n == 0 );
}

TEST_CASE( "framegen pacing: a step down only happens at a pair boundary", "[framegen_pacing]" )
{
	// Game speeds up so that 3x stops fitting; the held N changes only after the
	// hysteresis, and then only takes effect at the next pair's slot 1 (it is
	// latched there), never mid-pair.
	Driver drv;
	drv.chosenN = 3;
	drv.refreshHz = 144.0;
	drv.Run( 1000 * MS, 1000.0 / 40.0, Ns( 1.0 * double( MS ) ) );   // 40 fps: 3x fits (120 <= 172.8)
	REQUIRE( drv.pacer.ActiveN() == 3 );

	// 62 fps: 3x = 186 > 172.8, 2x fits.
	const Ns t0 = drv.now;
	drv.Run( t0 + 3000 * MS, 1000.0 / 62.0, t0 + 1 * MS );
	REQUIRE( drv.pacer.ActiveN() == 2 );

	// Every pair in the log is internally consistent: all paints of one pairId that
	// generated share ONE n.
	uint64_t id = 0;
	int n = 0;
	for ( const Painted &p : drv.log )
	{
		if ( p.d.pairId != id )
		{
			id = p.d.pairId;
			n = 0;
		}
		if ( p.d.k >= 1 )
		{
			if ( n == 0 )
				n = p.d.n;
			CHECK( p.d.n == n );
		}
	}
}

// ---------------------------------------------------------------------------
//  D10
// ---------------------------------------------------------------------------

TEST_CASE( "framegen pacing: hysteresis holds a change for 0.5 s", "[framegen_pacing]" )
{
	NHysteresis h;
	CHECK_FALSE( h.Initialised() );

	// The first target is adopted at once.
	NChoice four; four.n = 4;
	NChoice three; three.n = 3; three.reason = Reason::RefreshLimit;
	NChoice off; off.n = 0; off.reason = Reason::GameTooFast;

	Ns t = 1000 * MS;
	CHECK( h.Update( t, four ).n == 4 );
	CHECK( h.Changed() );

	// A different target does not take effect before 0.5 s ...
	CHECK( h.Update( t + 10 * MS, three ).n == 4 );
	CHECK_FALSE( h.Changed() );
	CHECK( h.Update( t + 300 * MS, three ).n == 4 );
	CHECK( h.Update( t + 509 * MS, three ).n == 4 );
	// ... and does after (measured from when the target first changed, t + 10 ms).
	CHECK( h.Update( t + 510 * MS, three ).n == 3 );
	CHECK( h.Changed() );
	CHECK( h.Held().reason == Reason::RefreshLimit );
	CHECK( h.Update( t + 520 * MS, three ).n == 3 );
	CHECK_FALSE( h.Changed() );
}

TEST_CASE( "framegen pacing: a target that flaps never gets through", "[framegen_pacing]" )
{
	NHysteresis h;
	NChoice two; two.n = 2;
	NChoice off; off.n = 0; off.reason = Reason::GameTooFast;

	Ns t = 0;
	h.Update( t, two );
	// Alternating every 100 ms for 10 s: the timer restarts every time, so pass-through
	// is never entered.
	for ( int i = 0; i < 100; i++ )
	{
		t += 100 * MS;
		const NChoice &held = h.Update( t, ( i & 1 ) ? two : off );
		CHECK( held.n == 2 );
	}

	// Steady at pass-through: switches after 0.5 s.
	t += 100 * MS;
	h.Update( t, off );
	CHECK( h.Held().n == 2 );
	h.Update( t + 499 * MS, off );
	CHECK( h.Held().n == 2 );
	h.Update( t + 500 * MS, off );
	CHECK( h.Held().n == 0 );
	CHECK( h.Held().reason == Reason::GameTooFast );

	// and the way back is held just as long
	t += 600 * MS;
	h.Update( t, two );
	CHECK( h.Held().n == 0 );
	h.Update( t + 500 * MS, two );
	CHECK( h.Held().n == 2 );
}

TEST_CASE( "framegen pacing: pass-through is entered and left only after 0.5 s", "[framegen_pacing]" )
{
	// 40 fps, 2x on 144 Hz: generating.
	Driver drv;
	drv.chosenN = 2;
	drv.refreshHz = 144.0;
	drv.Run( 1000 * MS, 1000.0 / 40.0, Ns( 1.0 * double( MS ) ) );
	REQUIRE( drv.pacer.ActiveN() == 2 );

	// The game jumps to 120 fps (2x no longer fits: 240 > 172.8).
	const Ns t0 = drv.now;
	drv.Run( t0 + 250 * MS, 1000.0 / 120.0, t0 + 1 * MS );
	// 250 ms in, the median has long caught up but the hysteresis has not elapsed.
	CHECK( drv.pacer.ActiveN() == 2 );

	drv.Run( t0 + 1000 * MS, 1000.0 / 120.0, drv.now + 1 * MS );
	CHECK( drv.pacer.ActiveN() == 0 );

	// In pass-through the renderer is left inert and every frame goes out at once.
	const Painted &last = drv.log.back();
	CHECK( last.d.k == 0 );
	CHECK( last.d.n == 0 );
	CHECK_FALSE( last.d.repaintNext );
	const Pacer::Report s = drv.pacer.TakeStatus( drv.Inputs( drv.now ), drv.now );
	CHECK( s.reason == Reason::GameTooFast );
	CHECK( s.activeN == 0 );
	CHECK( s.delayMs == 0.0f );
}

// ---------------------------------------------------------------------------
//  D12
// ---------------------------------------------------------------------------

namespace
{
	// A running 30 fps / 120 Hz / 2x session, ready to be disturbed.
	void WarmUp( Driver &drv )
	{
		drv.chosenN = 2;
		drv.refreshHz = 120.0;
		drv.Run( 1000 * MS, 1000.0 / 30.0, Ns( 7.3 * double( MS ) ) );
		drv.Settle( 40 * MS );
		REQUIRE( drv.pacer.ActiveN() == 2 );
		REQUIRE( drv.pacer.HavePrev() );
		REQUIRE( drv.pacer.EstimateValid() );
	}
}

TEST_CASE( "framegen pacing: a gap longer than 100 ms resets", "[framegen_pacing]" )
{
	Driver drv;
	WarmUp( drv );

	// 99 ms between real frames is a slow frame, not a gap.
	Pacer::Decision d = drv.ArriveAndPaint( drv.lastArrival + 99 * MS );
	CHECK_FALSE( d.reset );
	CHECK( drv.pacer.EstimateValid() );

	// 101 ms is.
	d = drv.ArriveAndPaint( drv.lastArrival + 101 * MS );
	CHECK( d.reset );                      // fghost::Reset() before this composite
	CHECK( d.k == 0 );                     // the first frame after the gap goes out directly
	CHECK_FALSE( d.repaintNext );
	CHECK_FALSE( drv.pacer.EstimateValid() );   // history gone

	// The status says why, until the estimate has settled again.
	Pacer::Report s = drv.pacer.TakeStatus( drv.Inputs( drv.now ), drv.now );
	CHECK( s.reason == Reason::GameStalled );
	CHECK( s.activeN == 0 );

	// Frames keep coming at 30 fps: real frames only until the interval is known
	// again (kMinSamples intervals), then generation resumes.
	drv.Run( drv.now + 1500 * MS, 1000.0 / 30.0, drv.now + 3 * MS );
	CHECK( drv.pacer.EstimateValid() );
	CHECK( drv.pacer.ActiveN() == 2 );
	CHECK( drv.log.back().d.n == 2 );
	s = drv.pacer.TakeStatus( drv.Inputs( drv.now ), drv.now );
	CHECK( s.reason == Reason::Normal );
	CHECK( s.activeN == 2 );

	// Exactly one reset in the whole run since the gap.
	int nReset = 0;
	for ( const Painted &p : drv.log )
		nReset += p.d.reset ? 1 : 0;
	CHECK( nReset == 1 );
}

TEST_CASE( "framegen pacing: a size or format change of layer 0 resets", "[framegen_pacing]" )
{
	for ( int what = 0; what < 3; what++ )
	{
		Driver drv;
		WarmUp( drv );
		INFO( "what = " << what );

		if ( what == 0 )
			drv.layer.width = 2560;
		else if ( what == 1 )
			drv.layer.height = 1440;
		else
			drv.layer.format = 88;

		const Pacer::Decision d = drv.ArriveAndPaint( drv.lastArrival + 33 * MS );
		CHECK( d.reset );
		CHECK( d.k == 0 );
		CHECK_FALSE( drv.pacer.EstimateValid() );
		CHECK_FALSE( d.repaintNext );
	}
}

TEST_CASE( "framegen pacing: a focus change resets", "[framegen_pacing]" )
{
	// with an arrival of its own
	{
		Driver drv;
		WarmUp( drv );
		drv.focusKey = 2;
		const Pacer::Decision d = drv.ArriveAndPaint( drv.lastArrival + 33 * MS );
		CHECK( d.reset );
		CHECK( d.k == 0 );
		CHECK_FALSE( drv.pacer.EstimateValid() );
	}
	// layer 0 swapped for another window's commit with no arrival at all
	// (determine_and_apply_focus() picks that window's last done commit)
	{
		Driver drv;
		WarmUp( drv );
		drv.newestId += 1000;
		const Pacer::Decision d = drv.Paint( drv.nextVblank );
		CHECK( d.reset );
		CHECK( d.k == 0 );
		CHECK( d.pairId == drv.newestId );
		CHECK( d.n >= 2 );                 // warm-up frame: the renderer still copies it
	}
	// layer 0 not a game frame (fade, Steam UI): the glue reports a discontinuity
	{
		Driver drv;
		WarmUp( drv );
		drv.pacer.Discontinuity();
		CHECK_FALSE( drv.pacer.HavePrev() );
		CHECK_FALSE( drv.pacer.PairActive() );
		// the game's next frame is the first of a fresh sequence
		const Pacer::Decision d = drv.ArriveAndPaint( drv.lastArrival + 33 * MS );
		CHECK( d.reset );
		CHECK( d.k == 0 );
	}
}

TEST_CASE( "framegen pacing: after a reset the next pair is a real frame and the one after generates", "[framegen_pacing]" )
{
	Driver drv;
	WarmUp( drv );
	drv.focusKey = 7;
	drv.layer.width = 1280;
	drv.layer.height = 720;
	// 30 fps, long enough for the estimate and one pair
	drv.Run( drv.now + 1000 * MS, 1000.0 / 30.0, drv.now + 3 * MS );
	drv.Settle( 40 * MS );
	CHECK( drv.pacer.ActiveN() == 2 );
	const auto pair = drv.LastPair();
	REQUIRE( pair.size() == 2 );
	CHECK( pair[ 0 ].k == 1 );
	CHECK( pair[ 1 ].k == 0 );
}

// ---------------------------------------------------------------------------
//  D16
// ---------------------------------------------------------------------------

TEST_CASE( "framegen pacing: cost guard step-down arithmetic", "[framegen_pacing]" )
{
	// 60 fps: the budget is 25 % of 16.67 ms = 4.17 ms. Cost per unit of N is the
	// measured pair cost divided by the N it ran at.
	const double iv = 1000.0 / 60.0;
	const double hz = 240.0;

	// cheap: no guard
	NChoice c = ChooseN( 4, iv, hz, 1.0 );    // 4 x 1.0 = 4.0 <= 4.17
	CHECK( c.n == 4 );
	CHECK( c.reason == Reason::Normal );

	// 4 x 1.2 = 4.8 over, 3 x 1.2 = 3.6 fits
	c = ChooseN( 4, iv, hz, 1.2 );
	CHECK( c.n == 3 );
	CHECK( c.reason == Reason::CostGuard );

	// 1.5: 6.0, 4.5 over, 3.0 fits at 2x
	c = ChooseN( 4, iv, hz, 1.5 );
	CHECK( c.n == 2 );
	CHECK( c.reason == Reason::CostGuard );

	// 2.2: even 2x costs 4.4 > 4.17: pass-through
	c = ChooseN( 4, iv, hz, 2.2 );
	CHECK( c.n == 0 );
	CHECK( c.reason == Reason::CostGuard );
	c = ChooseN( 2, iv, hz, 2.2 );
	CHECK( c.n == 0 );
	CHECK( c.reason == Reason::CostGuard );

	// The guard never changes the user's chosen N upward, and a fit limit that is
	// already lower wins the reason.
	c = ChooseN( 2, iv, hz, 0.1 );
	CHECK( c.n == 2 );
	CHECK( c.reason == Reason::Normal );
	c = ChooseN( 4, 1000.0 / 100.0, 144.0, 0.1 );   // 100 fps on 144 Hz: too fast
	CHECK( c.n == 0 );
	CHECK( c.reason == Reason::GameTooFast );

	// No timings (n/a): no guard at all, whatever the numbers.
	c = ChooseN( 4, iv, hz, -1.0 );
	CHECK( c.n == 4 );
	CHECK( c.reason == Reason::Normal );
}

TEST_CASE( "framegen pacing: the cost guard steps N down as measured cost rises", "[framegen_pacing]" )
{
	Driver drv;
	drv.chosenN = 4;
	drv.refreshHz = 240.0;
	drv.costPerNMs = 1.0;      // cheap: 4 x 1.0 = 4.0 ms <= 4.17
	drv.Run( 3000 * MS, 1000.0 / 60.0, 2 * MS );
	CHECK( drv.pacer.ActiveN() == 4 );
	CHECK( drv.pacer.TakeStatus( drv.Inputs( drv.now ), drv.now ).reason == Reason::Normal );

	// the GPU gets busier: 1.25 per N
	drv.costPerNMs = 1.25;
	const Ns t0 = drv.now;
	drv.Run( t0 + 300 * MS, 1000.0 / 60.0, t0 + 2 * MS );
	CHECK( drv.pacer.ActiveN() == 4 );     // not yet: hysteresis
	drv.Run( t0 + 4000 * MS, 1000.0 / 60.0, drv.now + 2 * MS );
	CHECK( drv.pacer.ActiveN() == 3 );
	Pacer::Report s = drv.pacer.TakeStatus( drv.Inputs( drv.now ), drv.now );
	CHECK( s.reason == Reason::CostGuard );
	CHECK( s.activeN == 3 );
	CHECK( s.chosenN == 4 );

	// busier still
	drv.costPerNMs = 1.8;
	const Ns t1 = drv.now;
	drv.Run( t1 + 4000 * MS, 1000.0 / 60.0, t1 + 2 * MS );
	CHECK( drv.pacer.ActiveN() == 2 );
	CHECK( drv.log.back().d.n == 2 );
}

TEST_CASE( "framegen pacing: when even 2x is over budget the cost guard passes through", "[framegen_pacing]" )
{
	Driver drv;
	drv.chosenN = 2;
	drv.refreshHz = 120.0;
	drv.costPerNMs = 0.5;
	drv.Run( 2000 * MS, 1000.0 / 30.0, 2 * MS );
	REQUIRE( drv.pacer.ActiveN() == 2 );

	drv.costPerNMs = 3.0;      // 2 x 3.0 = 6 ms > 25 % of 33.3 ms? No: budget is 8.3 ms.
	const Ns t0 = drv.now;
	drv.Run( t0 + 3000 * MS, 1000.0 / 30.0, t0 + 2 * MS );
	CHECK( drv.pacer.ActiveN() == 2 );   // 6 ms fits the 8.3 ms budget of a 30 fps game

	drv.costPerNMs = 5.0;      // 2 x 5.0 = 10 ms > 8.3 ms
	const Ns t1 = drv.now;
	drv.Run( t1 + 3000 * MS, 1000.0 / 30.0, t1 + 2 * MS );
	CHECK( drv.pacer.ActiveN() == 0 );
	const Pacer::Report s = drv.pacer.TakeStatus( drv.Inputs( drv.now ), drv.now );
	CHECK( s.reason == Reason::CostGuard );
	CHECK( s.activeN == 0 );
	CHECK( s.delayMs == 0.0f );
	CHECK( drv.log.back().d.k == 0 );
	CHECK( drv.log.back().d.n == 0 );    // inert renderer
}

TEST_CASE( "framegen pacing: a cost-guard pass-through is retried after a while", "[framegen_pacing]" )
{
	Driver drv;
	drv.chosenN = 4;
	drv.refreshHz = 240.0;
	drv.costPerNMs = 2.5;      // over budget even at 2x (5 ms > 4.17 ms)
	drv.Run( 3000 * MS, 1000.0 / 60.0, 2 * MS );
	REQUIRE( drv.pacer.ActiveN() == 0 );

	// The load goes away. Nothing is generated, so nothing is measured: only the
	// periodic probe can find out.
	drv.costPerNMs = 0.5;
	const Ns t0 = drv.now;
	drv.Run( t0 + 5000 * MS, 1000.0 / 60.0, t0 + 2 * MS );
	CHECK( drv.pacer.ActiveN() == 0 );     // 5 s: not yet
	drv.Run( t0 + 20000 * MS, 1000.0 / 60.0, drv.now + 2 * MS );
	CHECK( drv.pacer.ActiveN() == 4 );     // probed with 2x, measured, stepped back up
}

TEST_CASE( "framegen pacing: no timings means no cost guard", "[framegen_pacing]" )
{
	Driver drv;
	drv.chosenN = 4;
	drv.refreshHz = 240.0;
	drv.costPerNMs = -1.0;     // timestamps unsupported: lastPairGpuMs < 0
	drv.Run( 5000 * MS, 1000.0 / 60.0, 2 * MS );
	CHECK( drv.pacer.ActiveN() == 4 );
	const Pacer::Report s = drv.pacer.TakeStatus( drv.Inputs( drv.now ), drv.now );
	CHECK( s.reason == Reason::Normal );
}

// ---------------------------------------------------------------------------
//  Renderer unavailable, status, chosen N
// ---------------------------------------------------------------------------

TEST_CASE( "framegen pacing: an unavailable renderer passes through but keeps driving it", "[framegen_pacing]" )
{
	Driver drv;
	drv.chosenN = 3;
	drv.refreshHz = 120.0;
	drv.rendererOk = false;
	drv.Run( 1500 * MS, 1000.0 / 30.0, 2 * MS );

	for ( const Painted &p : drv.log )
	{
		CHECK( p.d.k == 0 );                  // real frames only
		CHECK( p.d.n >= 2 );                  // but the renderer is still told, so it can notice the cause went away
		CHECK_FALSE( p.d.repaintNext );
	}
	const Pacer::Report s = drv.pacer.TakeStatus( drv.Inputs( drv.now ), drv.now );
	CHECK( s.reason == Reason::RendererUnavailable );
	CHECK( s.activeN == 0 );

	// It comes back: generation resumes after one warm-up frame.
	drv.rendererOk = true;
	const Ns t0 = drv.now;
	drv.Run( t0 + 1000 * MS, 1000.0 / 30.0, t0 + 2 * MS );
	drv.Settle( 40 * MS );
	CHECK( drv.pacer.ActiveN() == 3 );
	CHECK( drv.LastPair().size() == 3 );
}

TEST_CASE( "framegen pacing: status numbers", "[framegen_pacing]" )
{
	Driver drv;
	drv.chosenN = 2;
	drv.refreshHz = 120.0;
	drv.Run( 1000 * MS, 1000.0 / 60.0, 2 * MS );   // 60 fps, 2x on 120 Hz: every refresh is a paint
	REQUIRE( drv.pacer.ActiveN() == 2 );

	// A fresh window, then exactly one second of play.
	drv.pacer.TakeStatus( drv.Inputs( drv.now ), drv.now );
	const Ns t0 = drv.now;
	drv.Run( t0 + 1000 * MS, 1000.0 / 60.0, t0 + 2 * MS );
	const Pacer::Report s = drv.pacer.TakeStatus( drv.Inputs( drv.now ), drv.now );
	CHECK( std::fabs( s.gameFps - 60.0f ) < 0.1f );
	CHECK( std::fabs( s.presentedFps - 120.0f ) < 3.0f );
	CHECK( s.chosenN == 2 );
	CHECK( s.activeN == 2 );
	CHECK( std::fabs( s.delayMs - 1000.0f / 60.0f / 2.0f ) < 0.01f );   // (N-1)/N x interval
	CHECK( s.reason == Reason::Normal );

	// published at a modest rate
	CHECK_FALSE( drv.pacer.StatusDue( drv.now + 100 * MS ) );
	CHECK( drv.pacer.StatusDue( drv.now + 250 * MS ) );
}

TEST_CASE( "framegen pacing: changing the chosen multiplier takes effect at once", "[framegen_pacing]" )
{
	Driver drv;
	drv.chosenN = 2;
	drv.refreshHz = 240.0;
	drv.Run( 1000 * MS, 1000.0 / 60.0, 2 * MS );
	REQUIRE( drv.pacer.ActiveN() == 2 );

	// the user picks 4x: adopted at the next paint, not after half a second
	drv.chosenN = 4;
	const Ns t0 = drv.now;
	drv.Run( t0 + 100 * MS, 1000.0 / 60.0, t0 + 2 * MS );
	CHECK( drv.pacer.ActiveN() == 4 );
}

TEST_CASE( "framegen pacing: Reset forgets everything", "[framegen_pacing]" )
{
	Driver drv;
	drv.chosenN = 2;
	drv.refreshHz = 120.0;
	drv.Run( 1000 * MS, 1000.0 / 30.0, 7 * MS );
	REQUIRE( drv.pacer.EstimateValid() );
	drv.pacer.Reset();
	CHECK_FALSE( drv.pacer.EstimateValid() );
	CHECK_FALSE( drv.pacer.HavePrev() );
	CHECK_FALSE( drv.pacer.PairActive() );
	CHECK( drv.pacer.ActiveN() == 0 );
}

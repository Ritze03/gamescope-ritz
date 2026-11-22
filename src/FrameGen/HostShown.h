// How many of the commits gamescope sent to a nested host compositor the host actually
// showed. The output rate ("presented 280 fps") counts what gamescope sent; the host
// answers every commit with wp_presentation feedback, `presented` (it reached the screen)
// or `discarded` (a newer commit replaced it before the screen was updated). `zero-copy`
// is how many of the shown frames the host scanned out directly, without drawing them
// itself. `torn` is how many shown frames came without the VSYNC kind flag (the host
// presented them without waiting for a refresh), counted for every `presented` whatever the
// tearing hint. `no free image` is how often a paint was dropped because the host still held
// all the output images. Only the nested Wayland backend feeds this; everywhere else the
// counters stay at zero and the status line leaves the numbers out.
#pragma once

#include <atomic>
#include <cstdint>

namespace fghostshown
{
	struct Counters
	{
		std::atomic<uint64_t> ulCommits{ 0 };
		std::atomic<uint64_t> ulShown{ 0 };
		std::atomic<uint64_t> ulDiscarded{ 0 };
		std::atomic<uint64_t> ulZeroCopy{ 0 };
		std::atomic<uint64_t> ulTorn{ 0 };
		std::atomic<uint64_t> ulNoFreeImage{ 0 };
		// The host showed our async-hinted commits on its vblank (VsyncVerdict below).
		std::atomic<bool> bHostVsync{ false };
	};

	// Does the host tear for us, or show at most one buffer per refresh? Fed with the
	// `presented` feedback of commits sent with the async tearing hint only: a host that
	// honours the hint reports them without WP_PRESENTATION_FEEDBACK_KIND_VSYNC, a host
	// that does not (window not fullscreen, tearing not allowed there) with it. Feedback
	// of vsync-hinted commits says nothing (the host was asked to wait) and is not fed.
	// Of the last 32 samples, 28 or more with VSYNC -> vsync host; 16 or fewer -> tearing
	// host; in between the verdict stays. Starts as "tearing" (the behaviour before the
	// verdict existed). Pure: one writer, no clock, unit-tested.
	class VsyncVerdict
	{
	public:
		static constexpr int kWindow = 32, kVsyncAt = 28, kTearAt = 16;

		bool Note( bool bVsync )
		{
			m_uBits = ( m_uBits << 1 ) | ( bVsync ? 1u : 0u );
			if ( m_nCount < kWindow )
				m_nCount++;
			if ( m_nCount == kWindow )
			{
				const int nVsync = __builtin_popcount( m_uBits );
				if ( nVsync >= kVsyncAt )
					m_bVsync = true;
				else if ( nVsync <= kTearAt )
					m_bVsync = false;
			}
			return m_bVsync;
		}

		bool Vsync() const { return m_bVsync; }

	private:
		uint32_t m_uBits = 0;
		int m_nCount = 0;
		bool m_bVsync = false;
	};

	inline Counters &Get()
	{
		static Counters s_Counters;
		return s_Counters;
	}

	// Rates since the previous call of this function (the status line's own period).
	struct Rates
	{
		bool bValid = false;
		double flCommits = 0, flShown = 0, flDiscarded = 0, flZeroCopy = 0, flTorn = 0, flNoFreeImage = 0;
	};

	inline Rates TakeRates( uint64_t ulNowNs )
	{
		static uint64_t s_ulLastNs = 0, s_ulC = 0, s_ulS = 0, s_ulD = 0, s_ulZ = 0, s_ulT = 0, s_ulN = 0;
		Counters &C = Get();
		const uint64_t c = C.ulCommits.load( std::memory_order_relaxed );
		const uint64_t s = C.ulShown.load( std::memory_order_relaxed );
		const uint64_t d = C.ulDiscarded.load( std::memory_order_relaxed );
		const uint64_t z = C.ulZeroCopy.load( std::memory_order_relaxed );
		const uint64_t t = C.ulTorn.load( std::memory_order_relaxed );
		const uint64_t n = C.ulNoFreeImage.load( std::memory_order_relaxed );
		Rates r;
		if ( s_ulLastNs && ulNowNs > s_ulLastNs && c > s_ulC )
		{
			const double flSec = double( ulNowNs - s_ulLastNs ) / 1e9;
			r.bValid = true;
			r.flCommits = double( c - s_ulC ) / flSec;
			r.flShown = double( s - s_ulS ) / flSec;
			r.flDiscarded = double( d - s_ulD ) / flSec;
			r.flZeroCopy = double( z - s_ulZ ) / flSec;
			r.flTorn = double( t - s_ulT ) / flSec;
			r.flNoFreeImage = double( n - s_ulN ) / flSec;
		}
		s_ulLastNs = ulNowNs;
		s_ulC = c;
		s_ulS = s;
		s_ulD = d;
		s_ulZ = z;
		s_ulT = t;
		s_ulN = n;
		return r;
	}
}

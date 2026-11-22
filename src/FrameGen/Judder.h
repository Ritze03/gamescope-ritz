// Motion evenness ("judder") of what the nested host showed: for every `presented` feedback of
// the toplevel, the presented step (time since the previous presented frame) against the
// content step (game time between the frame shown now and the one shown before). A perfectly
// smooth stream has both equal: step err ~0, uneven 0, gap 0. A stream that beats against the
// host's refresh shows periodic uneven steps. Content time 0 = unknown (counted, not judged).
// Pure (no clock, no lock), unit-tested in tests/test_judder.cpp; the `status:` line
// (ConVar ritz_status) prints it. See superdoc/features/status-line.md.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace fgjudder
{
	struct Stats
	{
		uint64_t shown = 0;      // presented frames
		uint64_t repeat = 0;     // judged frames whose content step is 0 (the same game frame again)
		uint64_t unknown = 0;    // presented frames without a content time (or after one without)
		uint64_t judged = 0;     // frames with a step error
		uint64_t uneven = 0;     // |content step - presented step| > half a refresh
		uint64_t gap = 0;        // presented step > 1.5 refreshes (the host skipped a refresh)
		double errMeanMs = 0, errP99Ms = 0, errMaxMs = 0;
	};

	class Meter
	{
	public:
		// A step longer than this is an idle stretch (nothing painted), not judder: not judged.
		static constexpr uint64_t kIdleNs = 250'000'000ull;

		// One presented frame: when it was shown, its content time (0 = unknown), and the
		// refresh period at that moment (0 = unknown: no uneven / gap verdict).
		void Note( uint64_t ulPresentedNs, uint64_t ulContentNs, uint64_t ulRefreshNs )
		{
			m_Stats.shown++;
			if ( ulContentNs == 0 )
				m_Stats.unknown++;
			if ( m_ulPrevPresented && ulPresentedNs > m_ulPrevPresented )
			{
				const uint64_t ulStep = ulPresentedNs - m_ulPrevPresented;
				if ( ulRefreshNs && ulStep * 2 > ulRefreshNs * 3 && ulStep < kIdleNs )
					m_Stats.gap++;
				if ( ulContentNs && m_ulPrevContent && ulStep < kIdleNs )
				{
					const int64_t nContentStep = int64_t( ulContentNs ) - int64_t( m_ulPrevContent );
					if ( nContentStep == 0 )
						m_Stats.repeat++;
					const uint64_t ulErr = uint64_t( std::llabs( nContentStep - int64_t( ulStep ) ) );
					m_Errors.push_back( ulErr );
					m_Stats.judged++;
					if ( ulRefreshNs && ulErr * 2 > ulRefreshNs )
						m_Stats.uneven++;
				}
			}
			m_ulPrevPresented = ulPresentedNs;
			m_ulPrevContent = ulContentNs;
		}

		// The interval's numbers; starts the next interval (the step chain itself continues).
		Stats Take()
		{
			Stats s = m_Stats;
			if ( !m_Errors.empty() )
			{
				uint64_t ulSum = 0, ulMax = 0;
				for ( uint64_t e : m_Errors )
				{
					ulSum += e;
					ulMax = std::max( ulMax, e );
				}
				const size_t nIdx = ( m_Errors.size() * 99 ) / 100;
				std::nth_element( m_Errors.begin(), m_Errors.begin() + nIdx, m_Errors.end() );
				s.errMeanMs = double( ulSum ) / double( m_Errors.size() ) / 1e6;
				s.errP99Ms = double( m_Errors[ nIdx ] ) / 1e6;
				s.errMaxMs = double( ulMax ) / 1e6;
			}
			m_Stats = Stats{};
			m_Errors.clear();
			return s;
		}

	private:
		Stats m_Stats;
		std::vector<uint64_t> m_Errors;
		uint64_t m_ulPrevPresented = 0, m_ulPrevContent = 0;
	};
}

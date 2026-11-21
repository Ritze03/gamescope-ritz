// FrameGenCapture.cpp -- the part of `ritz_fg_capture` without Vulkan: see FrameGenCapture.h
// and superdoc/features/frame-generation-capture.md (the file formats are written down there).
//
// Everything here is plain C++ so the unit tests can run it without a GPU.

#include "FrameGenCapture.h"

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

#include "../../subprojects/FrameGen/gpu/framegen.h"

// capture.txt is read by a replay tool, so the library's Settings struct must not grow a field the
// dump does not know. If this fires after a library bump: add the new field to FormatSettingLines()
// below (and to the doc page and the test), then update the size.
#if defined( __x86_64__ ) || defined( __aarch64__ )
static_assert( sizeof( framegen::Settings ) == 164, "framegen::Settings changed: FormatSettingLines() must dump the new field" );
#endif

namespace fghost::capture
{
	// ------------------------------------------------------------------
	//  Decisions
	// ------------------------------------------------------------------

	bool DecisionGenerates( const DecisionRec &r )
	{
		return !r.inert && !r.skip && !r.discontinuity && r.prevId != 0 && r.t < 1.0f;
	}

	const char *ReasonName( uint8_t uReason )
	{
		// Mirrors framegen::pacing::Reason (pacing.h), in its order; it is not linked into the unit tests.
		switch ( uReason )
		{
			case 0: return "Normal";
			case 1: return "Off";
			case 2: return "WarmingUp";
			case 3: return "CostGuard";
			case 4: return "GameTooFast";
			case 5: return "GameStalled";
			case 6: return "RendererUnavailable";
			case 7: return "WaitingForDecoder";
			default: return "Unknown";
		}
	}

	void DecisionRing::Push( DecisionRec r )
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		r.counter = ++m_ulCount;
		m_Buf[ ( m_ulCount - 1 ) % kDecisionRing ] = r;
	}

	std::vector<DecisionRec> DecisionRing::Snapshot( size_t nMax ) const
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		const size_t nHave = size_t( std::min<uint64_t>( m_ulCount, kDecisionRing ) );
		const size_t nTake = std::min( nMax, nHave );
		std::vector<DecisionRec> out;
		out.reserve( nTake );
		for ( size_t i = 0; i < nTake; i++ )
		{
			const uint64_t ulIndex = m_ulCount - nTake + i;   // 0-based number of the record
			out.push_back( m_Buf[ ulIndex % kDecisionRing ] );
		}
		return out;
	}

	uint64_t DecisionRing::Count() const
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		return m_ulCount;
	}

	void ArrivalTable::Note( uint64_t ulId, uint64_t ulNs )
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		// A repeated id (the same commit noted again) updates in place.
		for ( int i = 0; i < kArrivalSlots; i++ )
		{
			if ( m_ulId[ i ] == ulId && ulId != 0 )
			{
				m_ulNs[ i ] = ulNs;
				return;
			}
		}
		m_ulId[ m_uNext ] = ulId;
		m_ulNs[ m_uNext ] = ulNs;
		m_uNext = ( m_uNext + 1 ) % kArrivalSlots;
	}

	int64_t ArrivalTable::Find( uint64_t ulId ) const
	{
		if ( ulId == 0 )
			return -1;
		std::lock_guard<std::mutex> lock( m_Mutex );
		for ( int i = 0; i < kArrivalSlots; i++ )
		{
			if ( m_ulId[ i ] == ulId )
				return int64_t( m_ulNs[ i ] );
		}
		return -1;
	}

	DecisionRing &Decisions()
	{
		static DecisionRing s_Ring;
		return s_Ring;
	}

	ArrivalTable &Arrivals()
	{
		static ArrivalTable s_Table;
		return s_Table;
	}

	// ------------------------------------------------------------------
	//  Arguments, space
	// ------------------------------------------------------------------

	ArgResult ParseArgs( std::span<std::string_view> args )
	{
		ArgResult r;
		if ( args.size() <= 1 )
		{
			r.ok = true;
			return r;
		}
		if ( args.size() > 2 )
		{
			r.error = "usage: ritz_fg_capture [seconds]  (0..600; no argument starts now)";
			return r;
		}
		const std::string sArg( args[ 1 ] );
		bool bDigits = !sArg.empty() && sArg.size() <= 6;
		for ( char c : sArg )
			bDigits = bDigits && c >= '0' && c <= '9';
		if ( !bDigits )
		{
			r.error = "usage: ritz_fg_capture [seconds]  (a whole number of seconds, 0..600)";
			return r;
		}
		const long nSeconds = strtol( sArg.c_str(), nullptr, 10 );
		if ( nSeconds < 0 || nSeconds > kMaxArmSeconds )
		{
			r.error = "ritz_fg_capture: the delay must be 0..600 seconds";
			return r;
		}
		r.ok = true;
		r.seconds = int( nSeconds );
		return r;
	}

	uint64_t RequiredBytes( uint32_t uWidth, uint32_t uHeight, uint32_t uBytesPerPixel, int nFrames )
	{
		const uint64_t ulImage = uint64_t( uWidth ) * uHeight * uBytesPerPixel;
		return ( uint64_t( nFrames ) + uint64_t( nFrames ) + 2ull ) * ulImage + kReserveBytes;
	}

	bool HasEnoughSpace( uint64_t ulAvailableBytes, uint64_t ulRequiredBytes )
	{
		return ulAvailableBytes >= ulRequiredBytes;
	}

	// ------------------------------------------------------------------
	//  Formats
	// ------------------------------------------------------------------

	const char *VkFormatName( int nVkFormat )
	{
		switch ( nVkFormat )
		{
			case 37: return "R8G8B8A8_UNORM";
			case 44: return "B8G8R8A8_UNORM";
			case 58: return "A2R10G10B10_UNORM_PACK32";
			case 64: return "A2B10G10R10_UNORM_PACK32";
			case 97: return "R16G16B16A16_SFLOAT";
			default: return "UNKNOWN";
		}
	}

	uint32_t VkFormatBytes( int nVkFormat )
	{
		switch ( nVkFormat )
		{
			case 37: case 44: case 58: case 64: return 4;
			case 97: return 8;
			default: return 0;
		}
	}

	std::string OutFileName( int nSeq )
	{
		char sz[ 32 ];
		snprintf( sz, sizeof( sz ), "%03d_out.bgra", nSeq );
		return sz;
	}

	std::string RealFileName( uint64_t ulId )
	{
		char sz[ 48 ];
		snprintf( sz, sizeof( sz ), "id%llu_real.bgra", (unsigned long long)ulId );
		return sz;
	}

	namespace
	{
		std::string Fmt( const char *pszFormat, ... ) __attribute__( ( format( printf, 1, 2 ) ) );
		std::string Fmt( const char *pszFormat, ... )
		{
			char sz[ 1024 ];
			va_list ap;
			va_start( ap, pszFormat );
			vsnprintf( sz, sizeof( sz ), pszFormat, ap );
			va_end( ap );
			return sz;
		}

		// A token value must not hold a space (the files are split on spaces).
		std::string Token( std::string s )
		{
			if ( s.empty() )
				return "unknown";
			for ( char &c : s )
			{
				if ( c == ' ' || c == '\n' || c == '\t' )
					c = '_';
			}
			return s;
		}

		const char *LearnedNetText( uint32_t u )
		{
			static const char *const k[] = { "E2v3", "E1v3", "E2v4", "E1v4", "E2v5", "E1v5", "G2v2", "G2v3", "M2v2", "M2v3" };
			return u < 10 ? k[ u ] : "Unknown";
		}
	}

	std::string FormatHeaderLine( const HeaderInfo &h )
	{
		return Fmt( "capture v=%d start_wall=%s start_mono_ns=%llu width=%u height=%u vk_format=%d format_name=%s bytes_per_pixel=%u "
			"pitch=tight library_commit=%s ritz_version=%s ritz_commit=%s ritz_dirty=%d motion_wanted=%s motion_effective=%s "
			"fallback_v1=%d flowScale=%u n=%d prepare_ms=%.3f prepared_early=%d",
			kVersion, Token( h.sWallStart ).c_str(), (unsigned long long)h.ulStartNs, h.uWidth, h.uHeight, h.nVkFormat,
			VkFormatName( h.nVkFormat ), h.uBytesPerPixel, Token( h.sLibraryCommit ).c_str(), Token( h.sRitzVersion ).c_str(),
			Token( h.sRitzCommit ).c_str(), h.bRitzDirty ? 1 : 0, Token( h.sMotionWanted ).c_str(), Token( h.sMotionEffective ).c_str(),
			h.sMotionWanted != h.sMotionEffective ? 1 : 0, h.uFlowScale, h.nFrames, double( h.flPrepareMs ), h.bPreparedEarly ? 1 : 0 );
	}

	uint64_t MaxDecisionGapNs( const std::vector<DecisionRec> &v, uint64_t *pulAfterCounter )
	{
		uint64_t ulMax = 0, ulAfter = 0;
		for ( size_t i = 1; i < v.size(); i++ )
		{
			const uint64_t ulGap = v[ i ].ns > v[ i - 1 ].ns ? v[ i ].ns - v[ i - 1 ].ns : 0;
			if ( ulGap > ulMax )
			{
				ulMax = ulGap;
				ulAfter = v[ i ].counter;
			}
		}
		if ( pulAfterCounter )
			*pulAfterCounter = ulAfter;
		return ulMax;
	}

	std::string FormatTimingLine( uint64_t ulFirstNs, uint64_t ulLastNs, uint64_t ulMaxGapNs, uint64_t ulGapAfterCounter, uint64_t ulLastDecisionToStartNs )
	{
		return Fmt( "timing first_frame_ns=%llu last_frame_ns=%llu burst_ns=%llu max_decision_gap_ns=%llu gap_after_n=%llu last_decision_to_start_ns=%llu",
			(unsigned long long)ulFirstNs, (unsigned long long)ulLastNs, (unsigned long long)( ulLastNs > ulFirstNs ? ulLastNs - ulFirstNs : 0 ),
			(unsigned long long)ulMaxGapNs, (unsigned long long)ulGapAfterCounter, (unsigned long long)ulLastDecisionToStartNs );
	}

	std::vector<std::string> FormatSettingLines( const framegen::Settings &s )
	{
		std::vector<std::string> v;
		auto F = [ & ]( const char *pszName, float fl ) { v.push_back( Fmt( "setting %s=%.9g", pszName, double( fl ) ) ); };
		auto U = [ & ]( const char *pszName, uint32_t u ) { v.push_back( Fmt( "setting %s=%u", pszName, u ) ); };
		auto I = [ & ]( const char *pszName, int32_t n ) { v.push_back( Fmt( "setting %s=%d", pszName, n ) ); };
		auto B = [ & ]( const char *pszName, bool b ) { v.push_back( Fmt( "setting %s=%d", pszName, b ? 1 : 0 ) ); };
		auto E = [ & ]( const char *pszName, uint32_t u, const char *pszText ) { v.push_back( Fmt( "setting %s=%u text=%s", pszName, u, pszText ) ); };

		// In the order of the struct in framegen.h.
		F( "trustLow", s.trustLow );
		F( "trustHigh", s.trustHigh );
		F( "globalFallback", s.globalFallback );
		F( "sceneCutSad", s.sceneCutSad );
		F( "hudBonus", s.hudBonus );
		F( "smoothBonus", s.smoothBonus );
		F( "searchPenalty", s.searchPenalty );
		U( "searchRadius", s.searchRadius );
		U( "searchRadiusTop", s.searchRadiusTop );
		B( "subpixel", s.subpixel );
		{
			const uint32_t u = uint32_t( s.uiProtection );
			static const char *const k[] = { "Off", "Crosshair", "WholeScreen", "CrosshairV2" };
			E( "uiProtection", u, u < 4 ? k[ u ] : "Unknown" );
		}
		U( "uiHistory", s.uiHistory );
		F( "uiBoxHeightFrac", s.uiBoxHeightFrac );
		U( "uiBoxWidth", s.uiBoxWidth );
		U( "uiBoxHeight", s.uiBoxHeight );
		F( "uiCrosshairArea", s.uiCrosshairArea );
		I( "uiBoxOffsetX", s.uiBoxOffsetX );
		I( "uiBoxOffsetY", s.uiBoxOffsetY );
		U( "uiStillFrames", s.uiStillFrames );
		F( "uiStillThreshold", s.uiStillThreshold );
		U( "uiInpaintReach", s.uiInpaintReach );
		F( "uiMaxStillFraction", s.uiMaxStillFraction );
		F( "uiContrastThreshold", s.uiContrastThreshold );
		U( "uiLeaveFrames", s.uiLeaveFrames );
		U( "uiReentryFrames", s.uiReentryFrames );
		B( "uiEstimateFill", s.uiEstimateFill );
		U( "blurSamples", s.blurSamples );
		{
			const uint32_t u = uint32_t( s.blurWeights );
			E( "blurWeights", u, u == 0 ? "Even" : u == 1 ? "Gaussian" : "Unknown" );
		}
		{
			const uint32_t u = uint32_t( s.motionVersion );
			static const char *const k[] = { "V1", "V2", "Learned", "V3" };
			E( "motionVersion", u, u < 4 ? k[ u ] : "Unknown" );
		}
		{
			const uint32_t u = uint32_t( s.v2Mode );
			E( "v2Mode", u, u == 0 ? "Quality" : u == 1 ? "Performance" : "Unknown" );
		}
		B( "v2StaticMask", s.v2StaticMask );
		F( "v3Smear", s.v3Smear );
		F( "v3OcclSoft", s.v3OcclSoft );
		F( "v3Unsure", s.v3Unsure );
		F( "v3SceneCut", s.v3SceneCut );
		B( "v3ColourWindow", s.v3ColourWindow );
		B( "v3DetailSplit", s.v3DetailSplit );
		{
			const uint32_t u = uint32_t( s.v3Guard );
			E( "v3Guard", u, u == 0 ? "Off" : u == 1 ? "Guard" : u == 2 ? "V2Motion" : "Unknown" );
		}
		{
			const uint32_t u = uint32_t( s.learnedNet );
			E( "learnedNet", u, LearnedNetText( u ) );
		}
		B( "learnedHalf", s.learnedHalf );
		B( "learnedZeroTest", s.learnedZeroTest );
		B( "learnedStaticGuard", s.learnedStaticGuard );
		U( "flowScale", s.flowScale );
		U( "pyramidMinSide", s.pyramidMinSide );
		return v;
	}

	std::string FormatDecisionLine( const DecisionRec &r )
	{
		return Fmt( "decision n=%llu ns=%llu newestId=%llu prevId=%llu currId=%llu outId=%llu showId=%llu t=%.9g t0=%.9g t1=%.9g "
			"inert=%d skip=%d reset=%d repaintNext=%d newOutput=%d discontinuity=%d generate=%d reason=%u reason_text=%s",
			(unsigned long long)r.counter, (unsigned long long)r.ns, (unsigned long long)r.newestId, (unsigned long long)r.prevId,
			(unsigned long long)r.currId, (unsigned long long)r.outId, (unsigned long long)r.showId, double( r.t ), double( r.t0 ),
			double( r.t1 ), r.inert ? 1 : 0, r.skip ? 1 : 0, r.reset ? 1 : 0, r.repaintNext ? 1 : 0, r.newOutput ? 1 : 0,
			r.discontinuity ? 1 : 0, DecisionGenerates( r ) ? 1 : 0, unsigned( r.reason ), ReasonName( r.reason ) );
	}

	std::string FormatFrameLine( const FrameInfo &f )
	{
		return Fmt( "frame seq=%d outId=%llu prevId=%llu currId=%llu newestId=%llu t0=%.9g t1=%.9g arrival_prev_ns=%lld "
			"arrival_curr_ns=%lld now_ns=%llu width=%u height=%u estimate=%s estimate_ret=%s synth_ret=%d cached=%d out_slot=%d "
			"prev_slot=%d curr_slot=%d out_file=%s prev_file=%s curr_file=%s",
			f.nSeq, (unsigned long long)f.ulOutId, (unsigned long long)f.ulPrevId, (unsigned long long)f.ulCurrId,
			(unsigned long long)f.ulNewestId, double( f.flT0 ), double( f.flT1 ), (long long)f.nArrivalPrevNs,
			(long long)f.nArrivalCurrNs, (unsigned long long)f.ulNowNs, f.uWidth, f.uHeight,
			f.bEstimateRecorded ? "recorded" : "reused", f.bEstimateRecorded && f.nEstimateRet >= 0 ? ( f.nEstimateRet ? "1" : "0" ) : "na",
			f.nSynthRet, f.bCached ? 1 : 0, f.nOutSlot, f.nPrevSlot, f.nCurrSlot, Token( f.sOutFile ).c_str(), Token( f.sPrevFile ).c_str(),
			Token( f.sCurrFile ).c_str() );
	}

	std::string FormatEndLine( int nFrames, int nReals, int nDropped, const char *pszReason )
	{
		return Fmt( "end frames=%d reals=%d dropped=%d reason=%s", nFrames, nReals, nDropped, Token( pszReason ? pszReason : "unknown" ).c_str() );
	}

	bool WriteTightRows( FILE *f, const uint8_t *pPixels, uint32_t uPitch, uint32_t uRowBytes, uint32_t uHeight )
	{
		if ( !f || !pPixels || uPitch < uRowBytes )
			return false;
		for ( uint32_t y = 0; y < uHeight; y++ )
		{
			if ( fwrite( pPixels + size_t( y ) * uPitch, 1, uRowBytes, f ) != uRowBytes )
				return false;
		}
		return true;
	}
}

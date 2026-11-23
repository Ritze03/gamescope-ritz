// FrameGenCapture.cpp -- the part of `ritz_fg_capture` without Vulkan: see FrameGenCapture.h
// and superdoc/features/frame-generation-capture.md (the file formats are written down there).
//
// Everything here is plain C++ so the unit tests can run it without a GPU.

#include "FrameGenCapture.h"

#include <algorithm>
#include <cmath>
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
		if ( args[ 1 ] == "watch" )
		{
			const char *pszUsage = "usage: ritz_fg_capture watch [seconds 1..600, default 60] [sensitivity >= 0, default 1; 0 = every frame triggers]";
			if ( args.size() > 4 )
			{
				r.error = pszUsage;
				return r;
			}
			r.watch = true;
			r.seconds = kWatchDefaultSeconds;
			if ( args.size() >= 3 )
			{
				const std::string s( args[ 2 ] );
				bool bDigits = !s.empty() && s.size() <= 6;
				for ( char c : s )
					bDigits = bDigits && c >= '0' && c <= '9';
				const long n = bDigits ? strtol( s.c_str(), nullptr, 10 ) : -1;
				if ( n < 1 || n > kWatchMaxSeconds )
				{
					r.error = pszUsage;
					return r;
				}
				r.seconds = int( n );
			}
			if ( args.size() == 4 )
			{
				const std::string s( args[ 3 ] );
				bool bNum = !s.empty() && s.size() <= 12;
				int nDots = 0;
				for ( char c : s )
				{
					nDots += c == '.';
					bNum = bNum && ( ( c >= '0' && c <= '9' ) || c == '.' );
				}
				char *pEnd = nullptr;
				const double fl = bNum && nDots <= 1 ? strtod( s.c_str(), &pEnd ) : -1.0;
				if ( !bNum || nDots > 1 || !pEnd || *pEnd || !( fl >= 0.0 ) || fl > 1000.0 )
				{
					r.error = pszUsage;
					return r;
				}
				r.sensitivity = float( fl );
			}
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

		// " blend=A" for V3 Hybrid's variants (1..3), "" otherwise (plain V3 and every other version say nothing).
		// " blend=A(off)" when the variant is selected but the library runs V3's rule (HeaderInfo::bBlendOff).
		std::string BlendToken( int nBlend, bool bOff )
		{
			return nBlend >= 1 && nBlend <= 3 ? std::string( " blend=" ) + char( 'A' + nBlend - 1 ) + ( bOff ? "(off)" : "" ) : std::string();
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
			"fallback_v1=%d%s flowScale=%u n=%d prepare_ms=%.3f prepared_early=%d",
			kVersion, Token( h.sWallStart ).c_str(), (unsigned long long)h.ulStartNs, h.uWidth, h.uHeight, h.nVkFormat,
			VkFormatName( h.nVkFormat ), h.uBytesPerPixel, Token( h.sLibraryCommit ).c_str(), Token( h.sRitzVersion ).c_str(),
			Token( h.sRitzCommit ).c_str(), h.bRitzDirty ? 1 : 0, Token( h.sMotionWanted ).c_str(), Token( h.sMotionEffective ).c_str(),
			h.sMotionWanted != h.sMotionEffective ? 1 : 0, BlendToken( h.nBlend, h.bBlendOff ).c_str(), h.uFlowScale, h.nFrames, double( h.flPrepareMs ), h.bPreparedEarly ? 1 : 0 );
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
		U( "v3Blend", uint32_t( s.v3Blend ) );
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
			"arrival_curr_ns=%lld now_ns=%llu width=%u height=%u estimate=%s estimate_ret=%s synth_ret=%d cached=%d near=%d out_slot=%d "
			"prev_slot=%d curr_slot=%d out_file=%s prev_file=%s curr_file=%s",
			f.nSeq, (unsigned long long)f.ulOutId, (unsigned long long)f.ulPrevId, (unsigned long long)f.ulCurrId,
			(unsigned long long)f.ulNewestId, double( f.flT0 ), double( f.flT1 ), (long long)f.nArrivalPrevNs,
			(long long)f.nArrivalCurrNs, (unsigned long long)f.ulNowNs, f.uWidth, f.uHeight,
			f.bEstimateRecorded ? "recorded" : "reused", f.bEstimateRecorded && f.nEstimateRet >= 0 ? ( f.nEstimateRet ? "1" : "0" ) : "na",
			f.nSynthRet, f.bCached ? 1 : 0, f.bNear ? 1 : 0, f.nOutSlot, f.nPrevSlot, f.nCurrSlot, Token( f.sOutFile ).c_str(), Token( f.sPrevFile ).c_str(),
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

	// ------------------------------------------------------------------
	//  Watch mode
	// ------------------------------------------------------------------

	const char *FlagName( int nBit )
	{
		static const char *const k[ kFlagKinds ] = { "gap", "back", "stale", "refused", "near", "pairmismatch", "repeat", "static" };
		return nBit >= 0 && nBit < kFlagKinds ? k[ nBit ] : "?";
	}

	std::string FormatFlags( uint32_t uFlags )
	{
		std::string s;
		for ( int i = 0; i < kFlagKinds; i++ )
		{
			if ( !( uFlags & ( 1u << i ) ) )
				continue;
			if ( !s.empty() )
				s += ',';
			s += FlagName( i );
		}
		return s.empty() ? "-" : s;
	}

	uint32_t EvalFlags( const FlagInput &in, uint64_t *pulLastCurr, uint64_t *pulLastOut )
	{
		uint32_t u = 0;
		if ( in.ulCurrId != in.ulPrevId + 1 )
			u |= kFlagGap;
		if ( pulLastCurr && pulLastOut && ( ( *pulLastCurr && in.ulCurrId < *pulLastCurr ) || ( *pulLastOut && in.ulOutId < *pulLastOut ) ) )
			u |= kFlagBack;
		if ( !in.bEstimateRecorded && in.bStaleHost )
			u |= kFlagStale;
		if ( in.nEstimateRet == 0 || in.nSynthRet == 0 )
			u |= kFlagRefused;
		if ( in.bNear )
			u |= kFlagNear;
		if ( in.bMismatchHost )
			u |= kFlagPairMismatch;
		if ( pulLastCurr )
			*pulLastCurr = in.ulCurrId;
		if ( pulLastOut )
			*pulLastOut = in.ulOutId;
		return u;
	}

	namespace
	{
		float HalfToFloat( uint16_t h )
		{
			const uint32_t uSign = ( h >> 15 ) & 1, uExp = ( h >> 10 ) & 0x1f, uMant = h & 0x3ff;
			float fl;
			if ( uExp == 0 )
				fl = std::ldexp( float( uMant ), -24 );
			else if ( uExp == 31 )
				fl = uMant ? 0.0f : 65504.0f;   // inf -> the largest finite, NaN -> 0 (a metric, not a decoder)
			else
				fl = std::ldexp( float( uMant | 0x400 ), int( uExp ) - 25 );
			return uSign ? -fl : fl;
		}
	}

	bool ComputeBlockMeans( const uint8_t *pPixels, uint32_t uPitch, uint32_t uWidth, uint32_t uHeight, int nVkFormat, uint32_t uStep,
		BlockMeans *pOut )
	{
		const uint32_t uBpp = VkFormatBytes( nVkFormat );
		if ( !pPixels || !pOut || uBpp == 0 || uStep == 0 || uStep > kMetricBlock || kMetricBlock % uStep != 0 )
			return false;
		const uint32_t uBw = uWidth / kMetricBlock, uBh = uHeight / kMetricBlock;
		if ( uBw == 0 || uBh == 0 || uPitch < uWidth * uBpp )
			return false;
		pOut->uBw = uBw;
		pOut->uBh = uBh;
		pOut->v.assign( size_t( uBw ) * uBh * 3, 0.0f );
		const uint32_t uOff = uStep / 2;   // sample near the centre of each step x step cell
		const uint32_t uPer = kMetricBlock / uStep;
		const float flInv = 1.0f / float( uPer * uPer );
		for ( uint32_t by = 0; by < uBh; by++ )
		{
			float *pRow = &pOut->v[ size_t( by ) * uBw * 3 ];
			for ( uint32_t sy = 0; sy < uPer; sy++ )
			{
				const uint8_t *pLine = pPixels + size_t( by * kMetricBlock + sy * uStep + uOff ) * uPitch;
				for ( uint32_t bx = 0; bx < uBw; bx++ )
				{
					float a = 0.0f, b = 0.0f, c = 0.0f;
					for ( uint32_t sx = 0; sx < uPer; sx++ )
					{
						const uint8_t *p = pLine + size_t( bx * kMetricBlock + sx * uStep + uOff ) * uBpp;
						if ( uBpp == 4 && ( nVkFormat == 37 || nVkFormat == 44 ) )
						{
							a += p[ 0 ];
							b += p[ 1 ];
							c += p[ 2 ];
						}
						else if ( uBpp == 4 )
						{
							uint32_t w;
							memcpy( &w, p, 4 );
							a += float( w & 0x3ff ) * 0.25f;
							b += float( ( w >> 10 ) & 0x3ff ) * 0.25f;
							c += float( ( w >> 20 ) & 0x3ff ) * 0.25f;
						}
						else
						{
							uint16_t h[ 3 ];
							memcpy( h, p, 6 );
							a += HalfToFloat( h[ 0 ] ) * 255.0f;
							b += HalfToFloat( h[ 1 ] ) * 255.0f;
							c += HalfToFloat( h[ 2 ] ) * 255.0f;
						}
					}
					pRow[ bx * 3 + 0 ] += a;
					pRow[ bx * 3 + 1 ] += b;
					pRow[ bx * 3 + 2 ] += c;
				}
			}
			for ( uint32_t i = 0; i < uBw * 3; i++ )
				pRow[ i ] *= flInv;
		}
		return true;
	}

	float BlockDiff( const BlockMeans &a, const BlockMeans &b )
	{
		if ( a.v.empty() || a.v.size() != b.v.size() )
			return -1.0f;
		double d = 0.0;
		for ( size_t i = 0; i < a.v.size(); i++ )
			d += std::fabs( double( a.v[ i ] ) - double( b.v[ i ] ) );
		return float( d / double( a.v.size() ) );
	}

	float BlendDiff( const BlockMeans &out, const BlockMeans &prev, const BlockMeans &curr, float t )
	{
		if ( out.v.empty() || out.v.size() != prev.v.size() || out.v.size() != curr.v.size() )
			return -1.0f;
		double d = 0.0;
		for ( size_t i = 0; i < out.v.size(); i++ )
			d += std::fabs( double( out.v[ i ] ) - ( ( 1.0 - t ) * double( prev.v[ i ] ) + double( t ) * double( curr.v[ i ] ) ) );
		return float( d / double( out.v.size() ) );
	}

	float Threshold( float flPair, float flSensitivity )
	{
		if ( !( flSensitivity > 0.0f ) )
			return 0.0f;
		return ( kMetricBase + kMetricPairGain * flPair ) / flSensitivity;
	}

	bool PixelTriggers( float flCoarse, float flPair, float flSensitivity )
	{
		if ( !( flSensitivity > 0.0f ) )
			return true;
		return flCoarse > Threshold( flPair, flSensitivity );
	}

	std::string FormatWatchLine( const WatchRec &r )
	{
		char szCoarse[ 24 ] = "na", szPair[ 24 ] = "na";
		if ( r.flCoarse >= 0.0f && r.flPair >= 0.0f )
		{
			snprintf( szCoarse, sizeof( szCoarse ), "%.3f", double( r.flCoarse ) );
			snprintf( szPair, sizeof( szPair ), "%.3f", double( r.flPair ) );
		}
		char szSpan[ 32 ] = "na";
		if ( PairSpanNs( r ) >= 0 )
			snprintf( szSpan, sizeof( szSpan ), "%.3f", double( PairSpanNs( r ) ) / 1e6 );
		return Fmt( "rec i=%llu ns=%llu outId=%llu prevId=%llu currId=%llu newestId=%llu t0=%.9g t1=%.9g arrival_prev_ns=%lld "
			"arrival_curr_ns=%lld span_ms=%s estimate=%s estimate_ret=%s synth_ret=%d cached=%d near=%d out_slot=%d prev_slot=%d curr_slot=%d "
			"coarse=%s pair=%s flags=%s",
			(unsigned long long)r.ulIndex, (unsigned long long)r.ulNs, (unsigned long long)r.ulOutId, (unsigned long long)r.ulPrevId,
			(unsigned long long)r.ulCurrId, (unsigned long long)r.ulNewestId, double( r.flT0 ), double( r.flT1 ),
			(long long)r.nArrivalPrevNs, (long long)r.nArrivalCurrNs, szSpan, r.bEstimateRecorded ? "recorded" : "reused",
			r.bEstimateRecorded && r.nEstimateRet >= 0 ? ( r.nEstimateRet ? "1" : "0" ) : "na", int( r.nSynthRet ), r.bCached ? 1 : 0,
			r.bNear ? 1 : 0, int( r.nOutSlot ), int( r.nPrevSlot ), int( r.nCurrSlot ), szCoarse, szPair, FormatFlags( r.uFlags ).c_str() );
	}

	std::string FormatWatchHeader( const HeaderInfo &h, int nSeconds, float flSensitivity )
	{
		return Fmt( "watch v=%d start_wall=%s start_mono_ns=%llu width=%u height=%u vk_format=%d format_name=%s bytes_per_pixel=%u "
			"library_commit=%s ritz_version=%s ritz_commit=%s ritz_dirty=%d motion_wanted=%s motion_effective=%s fallback_v1=%d%s "
			"flowScale=%u seconds=%d sensitivity=%.6g threshold=(%g+%g*pair)/sensitivity block=%u sample_step=%u max_dumps=%d "
			"window=%d after_trigger=%d",
			kVersion, Token( h.sWallStart ).c_str(), (unsigned long long)h.ulStartNs, h.uWidth, h.uHeight, h.nVkFormat,
			VkFormatName( h.nVkFormat ), h.uBytesPerPixel, Token( h.sLibraryCommit ).c_str(), Token( h.sRitzVersion ).c_str(),
			Token( h.sRitzCommit ).c_str(), h.bRitzDirty ? 1 : 0, Token( h.sMotionWanted ).c_str(), Token( h.sMotionEffective ).c_str(),
			h.sMotionWanted != h.sMotionEffective ? 1 : 0, BlendToken( h.nBlend, h.bBlendOff ).c_str(), h.uFlowScale, nSeconds, double( flSensitivity ), double( kMetricBase ),
			double( kMetricPairGain ), kMetricBlock, kMetricStep, kWatchMaxDumps, kFrames, kWatchAfterTrigger );
	}

	void SummaryAdd( WatchSummary *pS, const WatchRec &r )
	{
		pS->ulRecords++;
		if ( r.flCoarse >= 0.0f && r.flPair >= 0.0f )
		{
			pS->ulMeasured++;
			const float flRatio = r.flCoarse / Threshold( r.flPair, 1.0f );
			if ( flRatio > pS->flMaxRatio )
			{
				pS->flMaxRatio = flRatio;
				pS->maxRec = r;
			}
		}
		else
			pS->ulNotMeasured++;
		for ( int i = 0; i < kFlagKinds; i++ )
		{
			if ( r.uFlags & ( 1u << i ) )
				pS->ulFlagCount[ i ]++;
		}
	}

	int64_t PairSpanNs( const WatchRec &r )
	{
		if ( r.nArrivalPrevNs < 0 || r.nArrivalCurrNs < 0 )
			return -1;
		return r.nArrivalCurrNs - r.nArrivalPrevNs;
	}

	SpanStats ComputeSpanStats( const std::vector<WatchRec> &recs )
	{
		SpanStats st;
		std::vector<int64_t> v;
		v.reserve( recs.size() );
		for ( const WatchRec &r : recs )
		{
			const int64_t n = PairSpanNs( r );
			if ( n < 0 )
				continue;
			v.push_back( n );
			const double flMs = double( n ) / 1e6;
			if ( flMs > st.flMaxMs )
			{
				st.flMaxMs = flMs;
				st.ulMaxRec = r.ulIndex;
				st.ulMaxPrevId = r.ulPrevId;
				st.ulMaxCurrId = r.ulCurrId;
			}
		}
		st.ulKnown = v.size();
		if ( v.empty() )
			return st;
		std::vector<int64_t> sorted = v;
		std::sort( sorted.begin(), sorted.end() );
		const size_t n = sorted.size();
		st.flMedianMs = ( n % 2 ? double( sorted[ n / 2 ] ) : 0.5 * ( double( sorted[ n / 2 - 1 ] ) + double( sorted[ n / 2 ] ) ) ) / 1e6;
		// Distinct pairs, counted once.
		std::vector<std::pair<uint64_t, uint64_t>> seen;
		static const double k[ 4 ] = { 2.0, 3.0, 5.0, 8.0 };
		for ( const WatchRec &r : recs )
		{
			const int64_t ns = PairSpanNs( r );
			if ( ns < 0 )
				continue;
			const std::pair<uint64_t, uint64_t> p( r.ulPrevId, r.ulCurrId );
			if ( std::find( seen.begin(), seen.end(), p ) != seen.end() )
				continue;
			seen.push_back( p );
			if ( seen.size() > 64 )
				seen.erase( seen.begin() );   // pairs come in runs: the recent ones are enough to count each once
			for ( int i = 0; i < 4; i++ )
			{
				if ( double( ns ) / 1e6 > k[ i ] * st.flMedianMs )
					st.ulPairsOver[ i ]++;
			}
		}
		return st;
	}

	std::string FormatSpanTokens( const SpanStats &s )
	{
		const bool b = s.ulKnown > 0;
		return Fmt( "span_known=%llu span_median_ms=%s span_max_ms=%s span_max_record=%lld span_max_prevId=%llu span_max_currId=%llu "
			"span_gt2=%llu span_gt3=%llu span_gt5=%llu span_gt8=%llu",
			(unsigned long long)s.ulKnown, b ? Fmt( "%.3f", s.flMedianMs ).c_str() : "na", b ? Fmt( "%.3f", s.flMaxMs ).c_str() : "na",
			b ? (long long)s.ulMaxRec : -1ll, (unsigned long long)s.ulMaxPrevId, (unsigned long long)s.ulMaxCurrId,
			(unsigned long long)s.ulPairsOver[ 0 ], (unsigned long long)s.ulPairsOver[ 1 ], (unsigned long long)s.ulPairsOver[ 2 ],
			(unsigned long long)s.ulPairsOver[ 3 ] );
	}

	std::string FormatWatchSummary( const WatchSummary &s, const char *pszReason, const std::string &sExtra )
	{
		std::string sFlags;
		for ( int i = 0; i < kFlagKinds; i++ )
			sFlags += Fmt( " flag_%s=%llu", FlagName( i ), (unsigned long long)s.ulFlagCount[ i ] );
		sFlags += Fmt( " repeat=%llu static=%llu", (unsigned long long)s.ulFlagCount[ 6 ], (unsigned long long)s.ulFlagCount[ 7 ] );
		const bool bMax = s.flMaxRatio >= 0.0f;
		return Fmt( "summary records=%llu measured=%llu not_measured=%llu not_measured_dumping=%llu dropped_records=%llu dumps=%d "
			"triggers=%llu max_ratio=%s max_record=%lld max_outId=%llu max_prevId=%llu max_currId=%llu max_coarse=%s max_pair=%s%s%s%s reason=%s",
			(unsigned long long)s.ulRecords, (unsigned long long)s.ulMeasured, (unsigned long long)s.ulNotMeasured,
			(unsigned long long)s.ulNotMeasuredDumping, (unsigned long long)s.ulDroppedRecords, s.nDumps, (unsigned long long)s.ulTriggers,
			bMax ? Fmt( "%.4f", double( s.flMaxRatio ) ).c_str() : "na", bMax ? (long long)s.maxRec.ulIndex : -1ll,
			(unsigned long long)( bMax ? s.maxRec.ulOutId : 0 ), (unsigned long long)( bMax ? s.maxRec.ulPrevId : 0 ),
			(unsigned long long)( bMax ? s.maxRec.ulCurrId : 0 ), bMax ? Fmt( "%.3f", double( s.maxRec.flCoarse ) ).c_str() : "na",
			bMax ? Fmt( "%.3f", double( s.maxRec.flPair ) ).c_str() : "na", sFlags.c_str(), sExtra.empty() ? "" : " ", sExtra.c_str(),
			Token( pszReason ? pszReason : "unknown" ).c_str() );
	}

	std::string FormatTriggerLine( const char *pszKind, uint64_t ulRecord, int nSeq, float flCoarse, float flPair, float flThreshold,
		uint64_t ulId, uint64_t ulSameAs )
	{
		const bool bPix = flCoarse >= 0.0f && flPair >= 0.0f;
		std::string s = Fmt( "trigger kind=%s record=%llu seq=%d coarse=%s pair=%s threshold=%s", Token( pszKind ).c_str(), (unsigned long long)ulRecord, nSeq,
			bPix ? Fmt( "%.3f", double( flCoarse ) ).c_str() : "na", bPix ? Fmt( "%.3f", double( flPair ) ).c_str() : "na",
			bPix ? Fmt( "%.3f", double( flThreshold ) ).c_str() : "na" );
		if ( ulId )
			s += Fmt( " id=%llu same_as=%llu", (unsigned long long)ulId, (unsigned long long)ulSameAs );
		return s;
	}

	uint64_t RowHash( const uint8_t *pPixels, uint32_t uPitch, uint32_t uWidth, uint32_t uHeight, uint32_t uBytesPerPixel )
	{
		uint64_t h = 1469598103934665603ull;
		if ( !pPixels || uHeight == 0 )
			return h;
		for ( uint32_t y : { uHeight / 4, uHeight / 2, ( 3 * uHeight ) / 4 } )
		{
			const uint8_t *p = pPixels + size_t( y ) * uPitch;
			for ( size_t i = 0; i < size_t( uWidth ) * uBytesPerPixel; i++ )
				h = ( h ^ p[ i ] ) * 1099511628211ull;
		}
		return h;
	}

	RepeatDetector::Result RepeatDetector::Add( uint64_t ulId, const BlockMeans &m, uint64_t ulRowHash )
	{
		Result r;
		for ( const Sig &s : m_Hist )
		{
			if ( s.ulId == ulId )
				return r;
		}
		auto Same = [ & ]( const Sig &s ) { return s.ulHash == ulRowHash && s.m.uBw == m.uBw && s.m.uBh == m.uBh && s.m.v == m.v; };
		// The newest identical earlier frame; a repeat when some frame after it (between the two) differs from it.
		for ( size_t i = m_Hist.size(); i-- > 0; )
		{
			if ( !Same( m_Hist[ i ] ) )
				continue;
			const Sig &old = m_Hist[ i ];
			bool bDiffBetween = false;
			for ( size_t k = i + 1; k < m_Hist.size(); k++ )
			{
				const Sig &b = m_Hist[ k ];
				bDiffBetween = bDiffBetween || !( b.ulHash == old.ulHash && b.m.v == old.m.v );
			}
			r.eKind = bDiffBetween ? kRepeat : kStatic;
			r.ulSameAs = old.ulId;
			break;
		}
		m_Hist.push_back( { ulId, m, ulRowHash } );
		if ( m_Hist.size() > kHistory )
			m_Hist.pop_front();
		return r;
	}

	uint64_t WatchRequiredBytes( uint32_t uWidth, uint32_t uHeight, uint32_t uBytesPerPixel, int nSeconds )
	{
		const uint64_t ulText = uint64_t( nSeconds ) * kWatchRecordsPerSecond * 400ull;
		return uint64_t( kWatchMaxDumps ) * RequiredBytes( uWidth, uHeight, uBytesPerPixel, kFrames ) + ulText;
	}

	RollingWindow::RollingWindow( int nOuts, int nReals )
		: m_nOuts( nOuts ), m_nRef( size_t( nReals ), 0 ), m_ulRealId( size_t( nReals ), 0 ), m_bOutUsed( size_t( nOuts ), false )
	{
	}

	void RollingWindow::Clear()
	{
		std::fill( m_nRef.begin(), m_nRef.end(), 0 );
		std::fill( m_ulRealId.begin(), m_ulRealId.end(), 0 );
		std::fill( m_bOutUsed.begin(), m_bOutUsed.end(), false );
		m_Entries.clear();
		m_nRealCursor = 0;
	}

	void RollingWindow::EvictFront()
	{
		const Entry e = m_Entries.front();
		m_Entries.erase( m_Entries.begin() );
		m_bOutUsed[ size_t( e.nOut ) ] = false;
		for ( int i = 0; i < 2; i++ )
		{
			if ( e.nReal[ i ] >= 0 )
				m_nRef[ size_t( e.nReal[ i ] ) ]--;
		}
	}

	RollingWindow::AddResult RollingWindow::Add( uint64_t ulRec, uint64_t ulPrevId, uint64_t ulCurrId, const std::function<bool( uint64_t )> &canEvict )
	{
		AddResult r;
		if ( !ulPrevId || !ulCurrId || ulPrevId == ulCurrId || m_nOuts <= 0 || m_nRef.size() < 2 )
			return r;
		const uint64_t ulId[ 2 ] = { ulPrevId, ulCurrId };
		int nHave[ 2 ] = { -1, -1 };
		int nNeedNew = 0;
		for ( int i = 0; i < 2; i++ )
		{
			for ( size_t j = 0; j < m_ulRealId.size(); j++ )
			{
				if ( m_ulRealId[ j ] == ulId[ i ] )
					nHave[ i ] = int( j );
			}
			if ( nHave[ i ] < 0 )
				nNeedNew++;
			else
				m_nRef[ size_t( nHave[ i ] ) ]++;   // pinned: an eviction below must not free it
		}
		auto FreeReals = [ & ]()
		{
			int n = 0;
			for ( size_t j = 0; j < m_nRef.size(); j++ )
				n += m_nRef[ j ] == 0;
			return n;
		};
		while ( int( m_Entries.size() ) >= m_nOuts || FreeReals() < nNeedNew )
		{
			if ( m_Entries.empty() || !canEvict( m_Entries.front().ulRec ) )
			{
				r.bBlocked = !m_Entries.empty();
				for ( int i = 0; i < 2; i++ )
				{
					if ( nHave[ i ] >= 0 )
						m_nRef[ size_t( nHave[ i ] ) ]--;
				}
				return r;
			}
			EvictFront();
		}
		Entry e;
		e.ulRec = ulRec;
		for ( size_t j = 0; j < m_bOutUsed.size(); j++ )
		{
			if ( !m_bOutUsed[ j ] )
			{
				e.nOut = int( j );
				break;
			}
		}
		m_bOutUsed[ size_t( e.nOut ) ] = true;
		const int nReals = int( m_nRef.size() );
		for ( int i = 0; i < 2; i++ )
		{
			e.ulId[ i ] = ulId[ i ];
			if ( nHave[ i ] >= 0 )
			{
				e.nReal[ i ] = nHave[ i ];
				continue;
			}
			for ( int k = 0; k < nReals; k++ )
			{
				const int j = ( m_nRealCursor + k ) % nReals;
				if ( m_nRef[ size_t( j ) ] == 0 )
				{
					e.nReal[ i ] = j;
					m_nRealCursor = ( j + 1 ) % nReals;
					break;
				}
			}
			m_ulRealId[ size_t( e.nReal[ i ] ) ] = ulId[ i ];
			m_nRef[ size_t( e.nReal[ i ] ) ]++;
			r.bRealNew[ i ] = true;
		}
		m_Entries.push_back( e );
		r.ok = true;
		r.nOut = e.nOut;
		r.nReal[ 0 ] = e.nReal[ 0 ];
		r.nReal[ 1 ] = e.nReal[ 1 ];
		return r;
	}
}

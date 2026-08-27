#include "CommandPalette.h"

#include <algorithm>
#include <cctype>

namespace gamescope::ui
{
	std::string ToLowerAscii( std::string_view s )
	{
		std::string out;
		out.reserve( s.size() );
		for ( char c : s )
			out.push_back( (char)std::tolower( (unsigned char)c ) );
		return out;
	}

	void AppendUtf8( std::string &sOut, unsigned int cp )
	{
		// The four-range encoder, written out rather than pulled from a
		// library: ImWchar is at most 0x10FFFF and this is the whole of what
		// the query field needs.
		if ( cp < 0x80 )
		{
			sOut.push_back( (char)cp );
		}
		else if ( cp < 0x800 )
		{
			sOut.push_back( (char)( 0xC0 | ( cp >> 6 ) ) );
			sOut.push_back( (char)( 0x80 | ( cp & 0x3F ) ) );
		}
		else if ( cp < 0x10000 )
		{
			sOut.push_back( (char)( 0xE0 | ( cp >> 12 ) ) );
			sOut.push_back( (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) ) );
			sOut.push_back( (char)( 0x80 | ( cp & 0x3F ) ) );
		}
		else if ( cp <= 0x10FFFF )
		{
			sOut.push_back( (char)( 0xF0 | ( cp >> 18 ) ) );
			sOut.push_back( (char)( 0x80 | ( ( cp >> 12 ) & 0x3F ) ) );
			sOut.push_back( (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) ) );
			sOut.push_back( (char)( 0x80 | ( cp & 0x3F ) ) );
		}
	}

	void PopUtf8( std::string &s )
	{
		// Walk back over continuation bytes (10xxxxxx) so one Backspace
		// deletes one CHARACTER, not one byte -- otherwise backspacing over a
		// non-ASCII character leaves a partial sequence that renders as a
		// replacement glyph and never matches anything.
		while ( !s.empty() )
		{
			const unsigned char c = (unsigned char)s.back();
			s.pop_back();
			if ( ( c & 0xC0 ) != 0x80 )
				break;
		}
	}

	void PopWord( std::string &s )
	{
		while ( !s.empty() && s.back() == ' ' )
			s.pop_back();
		while ( !s.empty() && s.back() != ' ' )
			PopUtf8( s );
	}

	int Score( std::string_view sTitle, std::string_view sId,
	           std::string_view sHaystack, std::string_view sQueryLower )
	{
		// BROWSING IS SEARCH WITH AN EMPTY QUERY -- see CommandPalette.h.
		// Every item scores the same, the stable sort preserves registration
		// order, and the caller needs no separate "list all" branch.
		if ( sQueryLower.empty() )
			return kScoreExact;

		if ( sTitle.rfind( sQueryLower, 0 ) == 0 )        return kScoreExact;
		if ( sTitle.find( sQueryLower ) != std::string_view::npos )    return kScoreTitle;
		if ( sId.find( sQueryLower ) != std::string_view::npos )       return kScoreId;
		if ( sHaystack.find( sQueryLower ) != std::string_view::npos ) return kScoreKeyword;

		// Subsequence: every query character appears in order somewhere in
		// the blob. This is what makes "dsp" find "display.filter.sharpness"
		// without a real fuzzy scorer's tuning burden.
		size_t i = 0;
		for ( char c : sQueryLower )
		{
			i = sHaystack.find( c, i );
			if ( i == std::string_view::npos )
				return kScoreNoMatch;
			i++;
		}
		return kScoreFuzzy;
	}

	namespace
	{
		// The three lowercased strings Score() wants, built once per item per
		// rebuild rather than per comparison.
		struct Hay
		{
			std::string sTitle, sId, sBlob;
		};

		Hay HayForEntry( const Entry &e )
		{
			Hay h;
			h.sTitle = ToLowerAscii( e.Title() );
			h.sId    = ToLowerAscii( e.Id() );
			h.sBlob  = h.sTitle + " " + h.sId + " " + ToLowerAscii( e.KeywordText() );
			return h;
		}

		Hay HayForParam( const Parameter &p, const Entry &parent )
		{
			Hay h;
			h.sTitle = ToLowerAscii( p.Title() );
			h.sId    = ToLowerAscii( p.Id() );
			// The parent's title and keywords are in a Param's blob on
			// purpose: "hdr denoise" should find a param called "Denoise"
			// that lives under an entry called "HDR", which is how a user who
			// remembers where a setting lives -- rather than its exact name --
			// actually searches.
			h.sBlob  = h.sTitle + " " + h.sId + " " + ToLowerAscii( p.KeywordText() )
			         + " " + ToLowerAscii( parent.Title() ) + " " + ToLowerAscii( parent.KeywordText() );
			return h;
		}
	}

	std::vector<PaletteItem> Build( const Registry &reg, std::string_view sQuery )
	{
		const std::string sQ = ToLowerAscii( sQuery );

		std::vector<PaletteItem> out;
		for ( size_t a = 0; a < reg.AreaCount(); a++ )
		{
			const Area &area = reg.AreaAt( a );

			// An area the machine cannot offer is not searchable either --
			// otherwise the palette is a route to a row the rail deliberately
			// hides, which is the "reachable but not present" state SPEC's
			// rail rule exists to avoid.
			if ( !area.Available() )
				continue;

			for ( size_t i = 0; i < area.EntryCount(); i++ )
			{
				const Entry &e = area.EntryAt( i );

				// Issue #91: a read-only row (Meter/Facts/Graph -- see
				// Entry::ReadOnly()) has nothing a launcher jump could act
				// on, and HideFromPalette() covers the rest -- entries with
				// a setter that still make no sense to jump to. Either way
				// the entry AND its params are invisible to search, not
				// merely deprioritised.
				if ( e.ReadOnly() || e.ExcludedFromPalette() )
					continue;

				const Hay h = HayForEntry( e );
				const int nScore = Score( h.sTitle, h.sId, h.sBlob, sQ );
				if ( nScore != kScoreNoMatch )
					out.push_back( PaletteItem{ e.Id(), e.Id(), e.Title(), false, nScore } );

				// PARAMS ARE SEARCHABLE EXACTLY LIKE ROWS. This loop is the
				// whole of SPEC §5.2's "a setting in the Inspector is one
				// keystroke from anywhere", and it is the reason the
				// anti-junk-drawer law is credible rather than merely strict.
				for ( size_t j = 0; j < e.ParamCount(); j++ )
				{
					const Parameter &p = e.ParamAt( j );
					const Hay ph = HayForParam( p, e );
					const int nPScore = Score( ph.sTitle, ph.sId, ph.sBlob, sQ );
					if ( nPScore != kScoreNoMatch )
					{
						// U+00BB, not the mockup's U+25B8: Fonts.cpp bakes
						// GetGlyphRangesDefault() (Basic Latin + Latin-1
						// Supplement) because this UI is English-only, and a
						// glyph outside that range draws as the atlas's
						// fallback box. Same meaning, one range down.
						out.push_back( PaletteItem{
							p.Id(), p.Id(),
							e.Title() + " \xC2\xBB " + p.Title(),
							true, nPScore } );
					}
				}
			}
		}

		// STABLE, so equal scores keep registration order. This is what makes
		// the empty query render the registry in rail/sheet order instead of
		// an arbitrary permutation that changes between builds.
		std::stable_sort( out.begin(), out.end(),
			[]( const PaletteItem &l, const PaletteItem &r ) { return l.nScore < r.nScore; } );
		return out;
	}

	int WorstCharsToReach( const Registry &reg, int nTopN, std::string *psWorst )
	{
		int nWorst = 0;
		if ( psWorst )
			psWorst->clear();

		// Every id the registry holds, with the title a user would type
		// toward. Collected first so the inner loop rebuilds the palette per
		// PREFIX, not per id per prefix.
		struct Target { std::string sId, sTitle; };
		std::vector<Target> targets;
		for ( size_t a = 0; a < reg.AreaCount(); a++ )
		{
			const Area &area = reg.AreaAt( a );
			if ( !area.Available() )
				continue;
			for ( size_t i = 0; i < area.EntryCount(); i++ )
			{
				const Entry &e = area.EntryAt( i );

				// Issue #91: Build() itself skips read-only/excluded entries
				// (and everything under them -- their params included) as
				// unreachable by design, not by search-ranking accident. A
				// target this function can never find would just report the
				// entry's full title length as "worst", which is a false
				// positive on discoverability, not a real one.
				if ( e.ReadOnly() || e.ExcludedFromPalette() )
					continue;

				targets.push_back( { e.Id(), e.Title() } );
				for ( size_t j = 0; j < e.ParamCount(); j++ )
					targets.push_back( { e.ParamAt( j ).Id(), e.ParamAt( j ).Title() } );
			}
		}

		for ( const Target &t : targets )
		{
			int nNeeded = (int)t.sTitle.size();
			for ( int n = 1; n <= (int)t.sTitle.size(); n++ )
			{
				const std::vector<PaletteItem> hits = Build( reg, t.sTitle.substr( 0, (size_t)n ) );
				const int nLimit = std::min( nTopN, (int)hits.size() );
				bool bFound = false;
				for ( int k = 0; k < nLimit; k++ )
				{
					if ( hits[ (size_t)k ].sId == t.sId )
					{
						bFound = true;
						break;
					}
				}
				if ( bFound )
				{
					nNeeded = n;
					break;
				}
			}
			if ( nNeeded > nWorst )
			{
				nWorst = nNeeded;
				if ( psWorst )
					*psWorst = t.sId;
			}
		}
		return nWorst;
	}
}

// The changelog parser -- see ChangelogParse.h for what it is and is not.
#include "ChangelogParse.h"

#include <string>

namespace gamescope
{
	namespace
	{
		using ui::ContentKind;
		using ui::ContentLine;

		// Emphasis markers are removed EVERYWHERE, not only where a lead-in is
		// recognised. The screen has no bold face to switch to, so a surviving
		// `**` would be a marker rendered as content -- the one outcome the
		// brief rules out. Removing the marker and keeping the words is the
		// lossless half of that choice.
		std::string StripEmphasis( std::string_view sv )
		{
			std::string s;
			s.reserve( sv.size() );
			for ( size_t i = 0; i < sv.size(); )
			{
				if ( i + 1 < sv.size() && sv[ i ] == '*' && sv[ i + 1 ] == '*' )
					i += 2;
				else
					s += sv[ i++ ];
			}
			return s;
		}

		std::string_view TrimRight( std::string_view sv )
		{
			while ( !sv.empty() && ( sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' ) )
				sv.remove_suffix( 1 );
			return sv;
		}

		size_t IndentOf( std::string_view sv )
		{
			size_t n = 0;
			while ( n < sv.size() && sv[ n ] == ' ' )
				n++;
			return n;
		}

		// `- **Lead**: rest` -> lead "Lead", text "rest".
		// `- **Lead** rest`  -> lead "Lead", text "rest".
		// `- **Lead**`       -> lead "Lead", text "".
		// `- plain`          -> lead "",     text "plain".
		// An UNCLOSED `**` is not a lead-in: the whole remainder becomes text
		// (with markers stripped), because guessing where the author meant the
		// bold to end would invent a title they did not write.
		void SplitBullet( std::string_view svBody, std::string &sLead, std::string &sText )
		{
			sLead.clear();

			if ( svBody.size() > 2 && svBody[ 0 ] == '*' && svBody[ 1 ] == '*' )
			{
				const size_t nClose = svBody.find( "**", 2 );
				if ( nClose != std::string_view::npos )
				{
					sLead = StripEmphasis( svBody.substr( 2, nClose - 2 ) );

					std::string_view svRest = svBody.substr( nClose + 2 );
					// The colon belongs to the format, not to the sentence, so
					// it is dropped rather than shown hanging off the lead-in.
					if ( !svRest.empty() && svRest.front() == ':' )
						svRest.remove_prefix( 1 );
					while ( !svRest.empty() && svRest.front() == ' ' )
						svRest.remove_prefix( 1 );

					sText = StripEmphasis( svRest );
					return;
				}
			}

			sText = StripEmphasis( svBody );
		}

		ContentLine ParseLine( std::string_view svRaw, bool bAfterBullet )
		{
			ContentLine line;

			const std::string_view sv = TrimRight( svRaw );
			const size_t nIndent = IndentOf( sv );
			std::string_view svBody = sv.substr( nIndent );

			// Blank line: Plain and empty. It is KEPT rather than collapsed --
			// the blank lines are the document's paragraph spacing, and the
			// body draws one row per line, so dropping them would close up the
			// gaps the author put between blocks.
			if ( svBody.empty() )
				return line;

			// ---- headings ---------------------------------------------------
			// `#`/`##` are one class and `###` another, because the document
			// only ever nests two deep: the title and the dates read as peers
			// on screen (each opens a block), and the categories sit inside
			// them. A third level would be a format change, and would arrive
			// here as Plain until this line is updated -- deliberately.
			if ( nIndent == 0 && svBody.front() == '#' )
			{
				size_t nHashes = 0;
				while ( nHashes < svBody.size() && svBody[ nHashes ] == '#' )
					nHashes++;

				// A `#` with no space after it is not a heading (`#5`, a hash
				// tag), so it falls through to Plain below.
				if ( nHashes < svBody.size() && svBody[ nHashes ] == ' ' )
				{
					std::string_view svTitle = svBody.substr( nHashes + 1 );
					while ( !svTitle.empty() && svTitle.front() == ' ' )
						svTitle.remove_prefix( 1 );

					line.eKind = ( nHashes >= 3 ) ? ContentKind::Subheading
					                              : ContentKind::Heading;
					line.sText = StripEmphasis( svTitle );
					return line;
				}
			}

			// ---- bullets ----------------------------------------------------
			if ( nIndent == 0 && svBody.size() > 1 && svBody[ 0 ] == '-' && svBody[ 1 ] == ' ' )
			{
				line.eKind = ContentKind::Bullet;
				SplitBullet( svBody.substr( 2 ), line.sLead, line.sText );
				return line;
			}

			// ---- a wrapped bullet's continuation -----------------------------
			// Only indented text DIRECTLY under a bullet counts. Indentation on
			// its own means nothing here (an indented line in the preamble is
			// just prose), and this keeps the rule to one bit of state instead
			// of a nesting stack.
			if ( nIndent > 0 && bAfterBullet )
			{
				line.eKind = ContentKind::BulletCont;
				line.sText = StripEmphasis( svBody );
				return line;
			}

			// ---- everything else --------------------------------------------
			// The preamble, a horizontal rule, a stray line: kept verbatim
			// apart from emphasis markers. This is the fallback the header
			// promises, and it is why a malformed file still reads.
			line.sText = StripEmphasis( sv );
			return line;
		}
	}

	std::vector<ui::ContentLine> ParseChangelog( std::string_view svText )
	{
		std::vector<ui::ContentLine> vec;

		bool bAfterBullet = false;
		size_t nStart = 0;
		while ( nStart <= svText.size() )
		{
			size_t nEnd = svText.find( '\n', nStart );
			const bool bLast = ( nEnd == std::string_view::npos );
			if ( bLast )
				nEnd = svText.size();

			ContentLine line = ParseLine( svText.substr( nStart, nEnd - nStart ), bAfterBullet );

			// A continuation extends the bullet, so the run survives it; a
			// blank line ends the run, which is what stops the next block's
			// indented prose from being glued onto the last bullet.
			bAfterBullet = ( line.eKind == ContentKind::Bullet )
			            || ( line.eKind == ContentKind::BulletCont );

			vec.push_back( std::move( line ) );

			if ( bLast )
				break;
			nStart = nEnd + 1;
		}

		// A file ending in a newline yields one empty final line; drop it so
		// the body does not always end on blank space.
		if ( !vec.empty() && vec.back().eKind == ContentKind::Plain
			&& vec.back().sText.empty() )
			vec.pop_back();

		return vec;
	}
}

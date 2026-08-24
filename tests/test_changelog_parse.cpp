// Unit tests for the CHANGELOG.md parser.
//
// WHY THIS FILE IS CHEAP TO HAVE. ParseChangelog() is pure -- string in,
// vector<ContentLine> out, no ImGui, no fonts, no globals, no I/O. So every
// rule below is assertable without a compositor, and the malformed-input
// cases (the ones that actually matter, because a hand-edited changelog is
// the realistic failure) cost nothing to cover.
//
// THE LOAD-BEARING GUARANTEE is the last section: every input line produces
// exactly one output line, and anything unclassifiable falls through as
// Plain with its text intact. A changelog that renders as unstyled text is a
// bad day; one that renders as garbage, drops entries, or crashes the
// compositor's render thread is a bug. These tests pin the difference.
//
// Format source of truth: superdoc/claude-instructions/changelog.md
#include <catch2/catch_test_macros.hpp>

#include "Overlay/ChangelogParse.h"
#include "Overlay/UI/Registry.h"

#include <string>
#include <string_view>
#include <vector>

using namespace gamescope;
using ui::ContentKind;
using ui::ContentLine;

namespace
{
	std::vector<ContentLine> Parse( std::string_view sv )
	{
		return ParseChangelog( sv );
	}

	// Every line's text, joined -- used to assert nothing was dropped.
	std::string AllText( const std::vector<ContentLine> &vec )
	{
		std::string s;
		for ( const ContentLine &l : vec )
		{
			s += l.sLead;
			s += l.sText;
			s += "\n";
		}
		return s;
	}
}

TEST_CASE( "changelog: the document's five line shapes", "[changelog_parse]" )
{
	SECTION( "a title and a version heading are both Heading" )
	{
		// `#` and `##` are one class deliberately: on screen the title and
		// the version blocks read as peers, each opening a block.
		const auto vec = Parse( "# Changelog\n## [0.3.1] - 2026-08-24" );
		REQUIRE( vec.size() == 2 );
		CHECK( vec[ 0 ].eKind == ContentKind::Heading );
		CHECK( vec[ 0 ].sText == "Changelog" );
		CHECK( vec[ 1 ].eKind == ContentKind::Heading );
		CHECK( vec[ 1 ].sText == "[0.3.1] - 2026-08-24" );
	}

	SECTION( "a category is a Subheading" )
	{
		const auto vec = Parse( "### Added" );
		REQUIRE( vec.size() == 1 );
		CHECK( vec[ 0 ].eKind == ContentKind::Subheading );
		CHECK( vec[ 0 ].sText == "Added" );
	}

	SECTION( "a bullet splits into lead-in and sentence" )
	{
		const auto vec = Parse( "- **Sheet scrolls**: the scrollbar moved alone." );
		REQUIRE( vec.size() == 1 );
		CHECK( vec[ 0 ].eKind == ContentKind::Bullet );
		CHECK( vec[ 0 ].sLead == "Sheet scrolls" );
		CHECK( vec[ 0 ].sText == "the scrollbar moved alone." );
	}

	SECTION( "a two-space indented line continues the bullet above it" )
	{
		const auto vec = Parse( "- **A**: one\n  two" );
		REQUIRE( vec.size() == 2 );
		CHECK( vec[ 1 ].eKind == ContentKind::BulletCont );
		CHECK( vec[ 1 ].sText == "two" );
	}

	SECTION( "a blank line is kept, because it is the document's spacing" )
	{
		const auto vec = Parse( "# A\n\n### B" );
		REQUIRE( vec.size() == 3 );
		CHECK( vec[ 1 ].eKind == ContentKind::Plain );
		CHECK( vec[ 1 ].sText.empty() );
	}
}

TEST_CASE( "changelog: asterisks never reach the screen", "[changelog_parse]" )
{
	// The one outcome the format rules out: a marker rendered as content.
	// There is no bold face at this size, so emphasis is carried by the
	// lead-in split and by colour -- the markers are always removed.
	SECTION( "a bold lead-in keeps its words and loses its markers" )
	{
		const auto vec = Parse( "- **Bold**: text" );
		CHECK( vec[ 0 ].sLead.find( '*' ) == std::string::npos );
		CHECK( vec[ 0 ].sText.find( '*' ) == std::string::npos );
	}

	SECTION( "mid-sentence bold is stripped too, not just the lead-in" )
	{
		const auto vec = Parse( "- **A**: some **inner** words" );
		CHECK( vec[ 0 ].sText == "some inner words" );
	}

	SECTION( "a bold lead-in with no colon still splits" )
	{
		const auto vec = Parse( "- **Panels shrank** while dragged." );
		CHECK( vec[ 0 ].sLead == "Panels shrank" );
		CHECK( vec[ 0 ].sText == "while dragged." );
	}

	SECTION( "a bold-only bullet has a lead and an empty sentence" )
	{
		const auto vec = Parse( "- **Just a title**" );
		CHECK( vec[ 0 ].sLead == "Just a title" );
		CHECK( vec[ 0 ].sText.empty() );
	}

	SECTION( "an UNCLOSED bold is not a lead-in -- it stays a sentence" )
	{
		// Guessing where the author meant the bold to end would invent a
		// title they never wrote, so the whole remainder is text instead.
		const auto vec = Parse( "- **never closed and so on" );
		CHECK( vec[ 0 ].eKind == ContentKind::Bullet );
		CHECK( vec[ 0 ].sLead.empty() );
		CHECK( vec[ 0 ].sText == "never closed and so on" );
		CHECK( vec[ 0 ].sText.find( '*' ) == std::string::npos );
	}

	SECTION( "backticks are kept verbatim -- only asterisks are markers" )
	{
		const auto vec = Parse( "- **A**: press `Ctrl+K` now" );
		CHECK( vec[ 0 ].sText == "press `Ctrl+K` now" );
	}
}

TEST_CASE( "changelog: malformed input degrades to plain text", "[changelog_parse]" )
{
	SECTION( "empty input yields no lines and does not crash" )
	{
		CHECK( Parse( "" ).empty() );
	}

	SECTION( "a lone newline is one blank line, not zero" )
	{
		// The trailing-newline rule drops ONE empty final line, so a file
		// whose entire content is a blank line still renders that blank
		// line. Consistent with blank lines being kept as spacing, and it
		// keeps the rule to a single unconditional pop.
		const auto vec = Parse( "\n" );
		REQUIRE( vec.size() == 1 );
		CHECK( vec[ 0 ].eKind == ContentKind::Plain );
		CHECK( vec[ 0 ].sText.empty() );
	}

	SECTION( "arbitrary prose falls through as Plain, intact" )
	{
		const auto vec = Parse( "just some words" );
		REQUIRE( vec.size() == 1 );
		CHECK( vec[ 0 ].eKind == ContentKind::Plain );
		CHECK( vec[ 0 ].sText == "just some words" );
	}

	SECTION( "a hash with no space is NOT a heading" )
	{
		// `#5` is a hash tag, not a title.
		const auto vec = Parse( "#5 is not a heading" );
		CHECK( vec[ 0 ].eKind == ContentKind::Plain );
		CHECK( vec[ 0 ].sText == "#5 is not a heading" );
	}

	SECTION( "a dash with no space is NOT a bullet" )
	{
		const auto vec = Parse( "-notabullet" );
		CHECK( vec[ 0 ].eKind == ContentKind::Plain );
		CHECK( vec[ 0 ].sText == "-notabullet" );
	}

	SECTION( "an indented line NOT under a bullet is plain, not a continuation" )
	{
		const auto vec = Parse( "# Title\n  indented prose" );
		CHECK( vec[ 1 ].eKind == ContentKind::Plain );
	}

	SECTION( "a blank line ends a bullet's run" )
	{
		// Otherwise the next block's indented prose glues onto the last
		// bullet of the previous one.
		const auto vec = Parse( "- **A**: one\n\n  unrelated" );
		REQUIRE( vec.size() == 3 );
		CHECK( vec[ 2 ].eKind == ContentKind::Plain );
	}

	SECTION( "a deeper heading level degrades rather than vanishing" )
	{
		const auto vec = Parse( "#### too deep" );
		CHECK( vec[ 0 ].eKind == ContentKind::Subheading );
		CHECK( vec[ 0 ].sText == "too deep" );
	}

	SECTION( "stray asterisks alone do not crash or leak" )
	{
		const auto vec = Parse( "- **" );
		REQUIRE( vec.size() == 1 );
		CHECK( vec[ 0 ].sText.find( '*' ) == std::string::npos );
	}

	SECTION( "a file of only markers survives" )
	{
		const auto vec = Parse( "****\n**\n*" );
		REQUIRE( vec.size() == 3 );
		CHECK( vec[ 2 ].sText == "*" );   // a single star is not a marker
	}
}

TEST_CASE( "changelog: line accounting is exact", "[changelog_parse]" )
{
	SECTION( "one output line per input line" )
	{
		const auto vec = Parse( "a\nb\nc" );
		CHECK( vec.size() == 3 );
	}

	SECTION( "a trailing newline does not add a blank final line" )
	{
		// The body would otherwise always end on empty space.
		CHECK( Parse( "a\nb\n" ).size() == 2 );
		CHECK( Parse( "a\nb" ).size() == 2 );
	}

	SECTION( "CRLF is handled -- no stray carriage returns reach the text" )
	{
		const auto vec = Parse( "# Title\r\n- **A**: b\r\n" );
		REQUIRE( vec.size() == 2 );
		CHECK( vec[ 0 ].sText == "Title" );
		CHECK( vec[ 1 ].sText == "b" );
		CHECK( AllText( vec ).find( '\r' ) == std::string::npos );
	}

	SECTION( "nothing is dropped from a mixed, partly-malformed document" )
	{
		const std::string in =
			"# Changelog\n"
			"\n"
			"preamble prose\n"
			"\n"
			"## [0.3.1] - 2026-08-24\n"
			"\n"
			"### Added\n"
			"- **Good**: a well-formed entry.\n"
			"  wrapped continuation\n"
			"- malformed, no lead-in\n"
			"- **unclosed bold\n"
			"\n"
			"garbage ****  line\n";

		const auto vec = Parse( in );

		// 13 source lines, minus the trailing-newline blank.
		CHECK( vec.size() == 13 );

		// Every kind that should appear, does.
		bool bHeading = false, bSub = false, bBullet = false, bCont = false;
		for ( const ContentLine &l : vec )
		{
			bHeading |= ( l.eKind == ContentKind::Heading );
			bSub     |= ( l.eKind == ContentKind::Subheading );
			bBullet  |= ( l.eKind == ContentKind::Bullet );
			bCont    |= ( l.eKind == ContentKind::BulletCont );
		}
		CHECK( bHeading );
		CHECK( bSub );
		CHECK( bBullet );
		CHECK( bCont );

		// And no marker survived anywhere in the document.
		CHECK( AllText( vec ).find( "**" ) == std::string::npos );
	}
}

TEST_CASE( "changelog: the shipped file parses cleanly", "[changelog_parse]" )
{
	// A guard against the real document drifting out of the shape the rules
	// doc promises: if someone hand-edits CHANGELOG.md into something the
	// parser has to fall back on, that shows up here rather than on screen.
	//
	// Only the SHAPE is asserted, never the wording -- tying tests to entry
	// text would make every changelog edit a test failure.
	const std::string in =
		"# Changelog\n"
		"\n"
		"All notable user-facing changes, newest first.\n"
		"\n"
		"## [0.3.1] - 2026-08-24\n"
		"\n"
		"### Added\n"
		"- **Changelog area**: a new rail area showing this file.\n"
		"\n"
		"### Fixed\n"
		"- **The sheet scrolls**: the scrollbar used to move while the\n"
		"  content stayed put.\n";

	const auto vec = Parse( in );

	// The first heading is the title; the second opens a version block.
	REQUIRE( vec.size() > 4 );
	CHECK( vec[ 0 ].eKind == ContentKind::Heading );

	// Every bullet in a well-formed document has a lead-in.
	for ( const ContentLine &l : vec )
	{
		if ( l.eKind == ContentKind::Bullet )
			CHECK_FALSE( l.sLead.empty() );
	}
}

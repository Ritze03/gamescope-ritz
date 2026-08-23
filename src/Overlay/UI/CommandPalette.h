// CommandPalette.h -- Ctrl+K over every registered Entry AND every Param.
//
// SPEC §5.2's searchability guarantee, §8.2's key table, and API.md §10.
// Direction B contributed the idea and the scorer; this is that scorer over
// E2's registry.
//
// TWO PROPERTIES THIS HEADER EXISTS TO KEEP TESTABLE WITHOUT A WINDOW:
//
//   1. MATCHING AND RANKING ARE PURE. Score() takes a haystack bundle and a
//      query and returns an integer; Build() walks a Registry and returns a
//      vector. Neither touches ImGui, a font, a frame or a pixel, so
//      tests/test_overlay_ui.cpp exercises them with no compositor at all.
//      The drawing half lives in Shell.cpp, where every other pixel does.
//
//   2. BROWSING IS SEARCH WITH AN EMPTY QUERY. There is deliberately no
//      second "list everything" path: Score() returns kScoreExact for an
//      empty query, so an empty query matches every item at equal rank and
//      the registry's own declaration order survives the stable sort. One
//      code path, which is what stops the browse list and the search list
//      drifting apart the way two would.
//
// WHY THE QUERY TEXT IS NOT ImGui::InputText -- see Shell.cpp's
// PaletteConsumeInput(). Summary: InputText owns Left/Right with no escape
// hatch (the palette needs them to adjust a value in place), its Esc reverts
// the buffer rather than dismissing, and it buys no IME here because this
// ImGui context has no platform backend. Direction B's FEASIBILITY.md §2
// reached the same conclusion and hand-rolled the field on
// io.InputQueueCharacters, which wlserver already fills with layout-correct
// UTF-8. This does the same.
#pragma once

#include "Registry.h"

#include <string>
#include <vector>

namespace gamescope::ui
{
	// One row of the palette. Points into the Registry, which outlives it --
	// a PaletteItem is rebuilt from scratch on every query change and never
	// held across a frame (a dynamic area's rebuild frees Entries, so holding
	// one would be exactly the dangling pointer Area::Rebuilds() warns about).
	struct PaletteItem
	{
		// The id the palette addresses -- an Entry id, or a Param's
		// Prefix-Law-synthesised `<parent>.<leaf>`. This is what Jump()
		// and the in-place adjust resolve through.
		std::string sId;

		// D5: the PATH COLUMN SHOWS THE CONFIG KEY, not the area id.
		// SPEC §2.6 -- an area id is a UI grouping label and carries no
		// promise that the settings inside share it as a key prefix (four
		// of eleven areas don't). The key is what a reviewer checks against
		// the on-disk JSON, so it is the more useful thing to show.
		std::string sPath;

		// What the name column draws. For a Param this is
		// "<parent title> > <param title>", so the row says what it belongs
		// to without a second column.
		std::string sLabel;

		bool bParam = false;

		int nScore = 0;
	};

	// Score bands, low is better. Named so a test asserts intent rather than
	// a magic number, and so the ordering between bands is stated once.
	inline constexpr int kScoreExact    = 0;   // title starts with the query (and the empty query)
	inline constexpr int kScoreTitle    = 1;   // title contains it
	inline constexpr int kScoreId       = 2;   // config key contains it
	inline constexpr int kScoreKeyword  = 3;   // title/key/keywords blob contains it
	inline constexpr int kScoreFuzzy    = 6;   // characters appear in order
	inline constexpr int kScoreNoMatch  = -1;  // filtered out

	// The scorer, isolated so it is testable on plain strings.
	//
	// `sTitle` and `sId` are matched on their own because a hit in either is
	// worth more than a hit in the keyword blob; `sHaystack` is the
	// lowercased title + id + keywords concatenation the last two bands use.
	// All three are expected LOWERCASE -- Build() lowercases once per item
	// rather than per keystroke.
	int Score( std::string_view sTitle, std::string_view sId,
	           std::string_view sHaystack, std::string_view sQueryLower );

	// Every Entry and every Param in the registry, filtered by `sQuery` and
	// ranked. The sort is STABLE, so items sharing a score keep registration
	// order -- which is what makes an empty query render the registry in the
	// order the rail and the sheets already show it.
	std::vector<PaletteItem> Build( const Registry &reg, std::string_view sQuery );

	// Case-fold ASCII. The registry's ids and keywords are ASCII by
	// construction (the Prefix Law's ids, and keyword blobs authors type),
	// and a title that isn't still matches through its id -- so a full
	// Unicode fold would buy nothing here and cost a dependency.
	std::string ToLowerAscii( std::string_view s );

	// Append one code point as UTF-8. The query field's own accumulator --
	// io.InputQueueCharacters hands out ImWchar, and the registry is matched
	// as bytes.
	void AppendUtf8( std::string &sOut, unsigned int cp );

	// Remove the last UTF-8 code point (Backspace), and the last word
	// (Ctrl+W). Both no-op on an empty string.
	void PopUtf8( std::string &s );
	void PopWord( std::string &s );

	// ---- discoverability ------------------------------------------------
	// SPEC §5.2's promise is that everything is one keystroke away; this is
	// the mechanical form of it. For each registered id, the fewest leading
	// characters of its own title that bring it into the top `nTopN` results.
	// Returns the worst (largest) such prefix length over the whole registry,
	// and writes the id that needed it to `psWorst` when non-null.
	//
	// Direction B gated its BUILD on this (<= 3 characters). See
	// AUTONOMOUS-DECISIONS.md D16 for why this ships as a TEST rather than a
	// registration abort.
	int WorstCharsToReach( const Registry &reg, int nTopN, std::string *psWorst );
}

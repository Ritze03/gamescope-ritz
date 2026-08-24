// Turns CHANGELOG.md's text into classified ContentLines.
//
// WHAT THIS IS NOT: a Markdown renderer. It reads exactly one document whose
// shape is fixed by superdoc/claude-instructions/changelog.md -- a `#` title,
// `##` date headings, `### Added/Fixed/Removed/Info` categories, and
// `- **Lead**: sentence` bullets hard-wrapped with two-space continuations.
// Five line shapes, nothing else. Supporting general Markdown would be a far
// larger thing than the one file it would be used on.
//
// WHAT IT GUARANTEES. Every input line produces exactly one output line, in
// order. A line it cannot classify comes back as ContentKind::Plain with its
// text intact -- never dropped, never truncated, never a crash. That is what
// keeps a malformed or hand-edited changelog readable instead of garbage.
//
// It is PURE: no ImGui, no fonts, no globals, no I/O. That is what makes it
// unit-testable (tests/test_changelog_parse.cpp) and what lets it run once at
// load time rather than every frame.
#pragma once

#include "UI/Registry.h"

#include <string_view>
#include <vector>

namespace gamescope
{
	// Parses `svText` (the whole file) into one ContentLine per source line.
	// Handles LF and CRLF. A trailing newline does not produce a final blank
	// line.
	std::vector<ui::ContentLine> ParseChangelog( std::string_view svText );
}

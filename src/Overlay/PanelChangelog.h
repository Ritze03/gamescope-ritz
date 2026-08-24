// The CHANGELOG area: this build's version identity, and the fork's
// changelog as a scrolling content body.
//
// It is the Log's shape reused rather than a new kind of screen -- version
// facts as ordinary rows, prose beneath them via Area::Content(), which is
// exactly "a scrolling text view the shell draws from data the area
// supplies". Nothing here places a pixel.
#pragma once

namespace gamescope
{
	namespace ui { class Registry; }

	// Declares the Changelog area: the base-gamescope and gamescope-ritz
	// version rows, and the embedded CHANGELOG.md as content lines.
	void PanelChangelog_RegisterArea( ui::Registry &reg );
}

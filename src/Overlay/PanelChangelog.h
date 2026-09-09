// The ABOUT area (rail label "About"; its id is still "system.changelog",
// see PanelChangelog.cpp for why): this build's version identity, the fork's
// changelog, and the licences of everything compiled into the binary, all as
// one scrolling content body.
//
// It is the Log's shape reused rather than a new kind of screen -- version
// facts as ordinary rows, prose beneath them via Area::Content(), which is
// exactly "a scrolling text view the shell draws from data the area
// supplies". Nothing here places a pixel.
#pragma once

namespace gamescope
{
	namespace ui { class Registry; }

	// Declares the About area: the base-gamescope and gamescope-ritz version
	// rows, a Licences row, and the embedded CHANGELOG.md plus the embedded
	// licence texts as content lines.
	void PanelChangelog_RegisterArea( ui::Registry &reg );
}

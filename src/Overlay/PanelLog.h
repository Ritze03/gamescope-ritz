// Issue #39: the "LOG" panel -- two tabs, gamescope's own log (via
// LogCapture's global LogScope listener) and the launched game's captured
// stdout/stderr (via LogCapture's pipe-and-reader-thread plumbing). All the
// actual capture/buffering lives in LogCapture.h -- this file only ever
// reads LogCapture::Get*Log()/Get*LogGeneration() and draws.
#pragma once

namespace gamescope
{
	// Draws the LOG panel's ImGui window. Called every frame from the same
	// place every other panel is (SettingsOverlay's draw loop), same
	// contract as PanelAudio_Draw() etc.
	void PanelLog_Draw();

	// E2 MIGRATION SEAM (P2), temporary -- see PanelDisplay.h's
	// PanelDisplay_DrawBody() for the full note. The same body with no
	// window around it, for ui::Area::Escape() to host in the E2 sheet.
	void PanelLog_DrawBody();
}

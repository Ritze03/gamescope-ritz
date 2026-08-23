// Issue #39 LOG panel.
//
// P3 part C: the legacy two-tab panel became the E2 `system.log` AREA. The
// tab bar is gone -- "Gamescope" and "Game" are now two members of the
// `log.sources` chip bank, which is what the two tabs always were: one
// decision about which subsystems to look at, expressed as a set (D7).
#pragma once

namespace gamescope
{
	namespace ui { class Registry; }

	// Declares the Log area: the source and severity banks, the text
	// filter, the auto-scroll switch, the buffer facts and the captured
	// text itself (Area::Content -- the shell draws it; this file only
	// supplies lines).
	void PanelLog_RegisterArea( ui::Registry &reg );

	// The legacy floating window. Still reachable under `overlay_e2 0`,
	// which must stay byte-identical, so it keeps its own body.
	void PanelLog_Draw();
}

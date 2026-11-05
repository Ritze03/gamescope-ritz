// The Diagnostics area (OTHER rail group, id "system.diagnostics"): checks of
// the setup around gamescope that cost frames silently, with what to change.
// See Diagnostics.h and superdoc/features/diagnostics.md.
#pragma once

namespace gamescope
{
	namespace ui { class Registry; }

	void PanelDiagnostics_RegisterArea( ui::Registry &reg );
}

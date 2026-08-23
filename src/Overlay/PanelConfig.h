// Milestone M7 -- the "Config" panel of the settings overlay: the
// global-vs-per-game "Override Global Config" control, the profile manager
// (apply/copy/save), and a small General settings section for
// gamescope-ritz itself. See superdoc/planning/SPEC.md's Feature 6 ("Config
// system") and "UI structure" -> "Config/Profiles panel", and
// superdoc/planning/DECISIONS.md #19-#21.
//
// This is the only panel that ever changes *which file* the other panels
// (PanelDisplay, PanelShaders, FpsDisplay) persist their own live edits
// into -- see Config/ConfigManager.h's session-routing section
// (SessionAppId/IsSessionOverrideActive/ConfigGeneration), which this panel
// is the sole writer of.
#pragma once

namespace gamescope
{
	namespace ui { class Registry; }

	// P3 part B: the E2 registration, replacing the Escape() hatch below.
	//
	// The panel's three tabs become THREE AREAS -- setup.profiles,
	// setup.pergame and setup.appearance -- following D13.1: the rail is
	// the product's only navigation, and index.html lists exactly these
	// three as rail items. Profiles and Per-game are DYNAMIC (the profile
	// list and the set of other games with overrides both change while the
	// overlay is open); Appearance is not.
	void PanelConfig_RegisterAreas( ui::Registry &reg );

}

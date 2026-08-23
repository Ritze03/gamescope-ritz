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
	// Draws the Config panel's ImGui window. Must be called from the same
	// place/thread SettingsOverlay draws its own window (steamcompmgr
	// thread, between ImGui::NewFrame() and ImGui::Render()) -- same
	// requirement as PanelDisplay_Draw()/PanelShaders_Draw(), since this
	// panel's actions (SetSessionOverrideActive, BumpConfigGeneration) are
	// read by those panels' own EnsureConfigLoaded() on that same thread
	// with no lock of its own.
	void PanelConfig_Draw();

	// E2 MIGRATION SEAM (P2), temporary -- see PanelDisplay.h's
	// PanelDisplay_DrawBody() for the full note. The same body with no
	// window around it, for ui::Area::Escape() to host in the E2 sheet.
	void PanelConfig_DrawBody();
}

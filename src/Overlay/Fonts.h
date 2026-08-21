// M8 part 1 (issue #13): the settings overlay's typography system --
// IBM Plex Sans for prose, IBM Plex Mono for every number/unit/state-word,
// per superdoc/planning/ui-design-guide.md's "Typography" section.
//
// A small named-style API, not a general typography engine (per the task
// brief): the design guide's "Scale observed" list names a finite set of
// roles, and Style below is exactly that list, one enumerator per role --
// nothing here is meant to grow arbitrary new sizes/weights on request.
#pragma once

struct ImFont;

namespace gamescope::fonts
{
	// One enumerator per role the design guide's Typography section names
	// under "Scale observed" (superdoc/planning/ui-design-guide.md). Sizes
	// and weights are the guide's own values (its ranges' midpoints where a
	// range is given); see Fonts.cpp's kSpecs table for the exact numbers.
	enum class Style
	{
		Title,   // Mono 600, 11px -- window/section title-bar text.
		         // Uppercase the string yourself (Fonts.cpp doesn't); the
		         // guide's ~.15-.16em letter-spacing/tracking on top of that
		         // is NOT applied -- ImGui has no letter-spacing primitive,
		         // and the task brief calls faking it badly worse than
		         // saying so. Real tracking would need per-glyph manual
		         // AddText() advance-offset code, which is out of scope for
		         // a typography-only pass; left for whichever of #14/#15
		         // ends up custom-drawing title chrome.
		Section, // Sans 500, 12.5px -- group/section names.
		Label,   // Sans 400, 12px -- parameter labels, body/prose text.
		         // This is also the atlas's default font (ImGuiIO::FontDefault),
		         // so every pre-existing ImGui::Text/Checkbox/Slider/etc.
		         // call that never explicitly pushes a Style gets Sans for
		         // free, with no per-callsite change needed.
		Value,   // Mono 500, 12.5px -- numeric/unit/state-word readouts
		         // drawn as plain text (ImGui::Text/AddText), tabular by
		         // construction since Plex Mono is genuinely monospaced.
		         // Does NOT reach text baked into a stock ImGui widget's own
		         // internal draw (e.g. SliderInt's live value overlay) --
		         // ImGui draws that with whatever font is active for the
		         // whole widget, not a separately stylable run; giving those
		         // their own custom draw code is issue #14's job (widget
		         // rendering), not this one.
		Meta,    // Mono 400, 10px -- units, hints, disabled/meta text.
		Hero,    // Mono 600, 18px -- large standalone numeric readouts
		         // (the FPS display's number).
	};

	// Builds the IBM Plex atlas for the *currently current* ImGui context
	// and must be called after ImGui::CreateContext() but before
	// ImGui_ImplVulkan_Init() -- the Vulkan backend uploads the font atlas
	// texture from whatever ImGui::GetIO().Fonts holds at Init() time, so
	// the atlas has to be finished building before that call, not after.
	//
	// ImGui font atlases are per-IO/per-context, not shared across contexts
	// -- SettingsOverlay.cpp and FpsDisplay.cpp each own a separate ImGui
	// context (see FpsDisplay.h's file comment for why), so each calls this
	// once, from its own EnsureImguiInit().
	//
	// Fallback (required, not optional): if the bundled IBM Plex data fails
	// to build into the atlas for any reason, this leaves the atlas on
	// ImGui's built-in default font and every Style resolves to that same
	// default font instead -- text still renders, just not in Plex. Never
	// leaves the atlas empty and never crashes.
	void Load();

	// Returns the ImFont* for a role on the currently-current context. Once
	// Load() has run for that context (as SettingsOverlay.cpp/FpsDisplay.cpp's
	// EnsureImguiInit() always does before any drawing) this is never null:
	// a real Plex face after a successful Load(), or ImGui's own built-in
	// default font for every Style after a failed/fallback Load(). Calling
	// this before Load() has ever run for the current context is a misuse
	// this API doesn't promise to handle gracefully beyond best-effort
	// (falls back to ImGui::GetIO().FontDefault, which can itself be null).
	//
	// How a new panel (e.g. M5's Audio panel, M7's Config panel) should use
	// this: call gamescope::fonts::Get(Style::Value) (or whichever role
	// fits) and wrap just the text/number in ImGui::PushFont(...)/PopFont()
	// -- see FpsDisplay.cpp's DrawReadout() and SettingsOverlay.cpp's
	// "Layer alpha" line for the pattern. Everything else needs no change:
	// Style::Label is already the atlas's default font, so ordinary
	// ImGui::Text/Checkbox/SliderFloat/etc. calls pick it up automatically.
	ImFont *Get( Style style );
}

// The E2 shell. See Shell.h for what this replaces and why.
//
// ---------------------------------------------------------------------------
// HOW THE THREE-REGION SPLIT IS BUILT, AND WHY THIS WAY
// ---------------------------------------------------------------------------
// Dear ImGui here is 1.92.9b, stock, NON-DOCKING branch -- so there is no
// dock-space, no splitter and no host-window machinery to lean on. The split
// is therefore made by hand, and made in the one way that keeps it from ever
// becoming a window-management problem again:
//
//   * ONE top-level ImGui window -- the slab. NoMove, NoResize, NoCollapse,
//     NoSavedSettings, NoBringToFrontOnFocus, no title bar. It is placed by
//     SetNextWindowPos/Size every frame from Layout.h's arithmetic, so its
//     position is a pure function of the surface size and display_scale and
//     there is no state anywhere that a user could drag out of alignment.
//   * THE REGIONS ARE CHILD WINDOWS INSIDE IT, at rects Layout.h computes.
//     A child window gives clipping and independent scrolling, which is all
//     the split actually needs; it gives no drag, no resize and no z-order,
//     which is all the split must never acquire.
//
// The reason to hand-roll rather than reach for splitters is not that the
// docking branch is unavailable -- it is that a splitter is a stored, mutable,
// per-user geometry, i.e. exactly the category of state whose bug history
// motivated this whole redesign. Region widths here come from SPEC §8.3's
// ladder and from nothing else. There is no persisted layout to corrupt,
// migrate or reset.
//
// The one piece of layout state the user does own is the Inspector's HOST
// (column / drawer / hidden, Ctrl+I). It is a single enum, it is not a
// geometry, and the ladder may override it downward -- see Layout.cpp.
//
#include "Shell.h"

#include "Band.h"
#include "Colors.h"
#include "CommandPalette.h"
#include "Controls.h"
#include "Layout.h"
#include "Lane.h"
#include "Registry.h"
#include "Row.h"
#include "Tokens.h"

#include "Overlay/Fonts.h"
#include "Overlay/Palette.h"
#include "Overlay/PanelAudio.h"
#include "Overlay/PanelConfig.h"
#include "Overlay/PanelDisplay.h"
#include "Overlay/PanelChangelog.h"
#include "Overlay/PanelLog.h"
#include "Overlay/PanelShaders.h"
#include "Overlay/FpsDisplay.h"

#include "convar.h"

// D18: overlay_e2_key pushes onto the overlay's OWN input queue, which is
// what makes a real key event reachable from a script. See cc_overlay_e2_key.
#include "SettingsOverlay.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <string>

namespace gamescope::ui::shell
{
	namespace
	{
		// =================================================================
		//  The gate
		// =================================================================
		// Spelled with an underscore, not the dot the design docs use for
		// it ("overlay.e2"), because every one of gamescope's ~200 ConVars
		// is snake_case and a lone dotted name would be the only one. The
		// concept is unchanged; see AUTONOMOUS-DECISIONS.md D12.
		//
		// =================================================================
		//  Shell state -- ALL of it. This list is meant to stay short.
		// =================================================================
		// Note what is absent: no window positions, no window sizes, no
		// z-order, no open/closed set, no per-panel anything. The shell has
		// one selected area, one selected row, one host preference and one
		// mode. That is the entire difference between this and the thing it
		// replaces.
		Registry     *s_pRegistry       = nullptr;
		std::string   s_sSelectedArea;
		std::string   s_sSelectedEntry;   // empty => Overview (SPEC §5.5)
		// The Inspector host preference lives in cv_overlay_e2_host below --
		// ONE storage, so the console, Ctrl+I, the spine, the close glyph and
		// the setup.shell row cannot disagree about which host is current.
		// Host() / SetHost() are the only accessors.
		bool          s_bModeOverridden  = false;
		InspectorMode s_eMode            = InspectorMode::Configure;

		// P6. The selected CONTENT line, by ContentLine::ulSeq, and 0 for
		// "none". One number, alongside the one selected area and the one
		// selected row -- it is the same kind of state as s_sSelectedEntry
		// and is kept here for the same reason: so there is exactly one place
		// a selection can live.
		//
		// It is a SEQ and not an index deliberately. The Log's buffer evicts
		// old lines and its filters hide others, so the line at index 12
		// changes identity between frames; selecting a line only to have the
		// Inspector describe a different one is the bug this avoids.
		uint64_t      s_ulSelectedLine   = 0;

		// Which of the two content hosts the Inspector is showing. Only ever
		// consulted for an area whose rows live in the Inspector.
		bool          s_bContentFilterHost = false;

		// P3: the id of the row whose Choice has downgraded to a dropdown AND
		// is currently open. One string, because exactly one dropdown can be
		// open at a time -- the alternative (a bool per row) is per-row state,
		// which is the category this shell exists to not have.
		//
		// It is needed at all because controls::Choice() auto-downgrades when
		// the segmented group does not fit the lane (API.md §12.6 -- "a caller
		// cannot pick wrong because a caller does not pick"). P2 had one Choice
		// with three short options and never saw the downgrade; a five-option
		// filter row in a narrow sheet does, and without a popup the control
		// would render correctly while doing nothing -- the exact failure mode
		// issues #25 and #68 were.
		std::string   s_sOpenDropdown;

		// D18: which Choice ids actually DREW as a dropdown, last frame.
		//
		// The keyboard needs this and cannot derive it. controls::Choice
		// decides segmented-vs-dropdown by measuring its own cells against
		// the lane it got (API.md §12.6), so the answer depends on the font,
		// the scale and the drawer -- and the shell must not re-derive it,
		// because a second copy of that decision is exactly the drawn-vs-
		// hit-tested divergence Controls.h exists to prevent.
		//
		// Without it, Enter on a SEGMENTED Choice would set s_sOpenDropdown,
		// the popup would never be drawn (the segmented branch draws no
		// popup), and the popup key handler would then swallow every key
		// with nothing on screen -- a keyboard trap, which is worse than the
		// gap it was meant to close.
		//
		// Two vectors and a swap rather than one set: RunKeyboard() runs
		// before any drawing, so it reads the COMPLETED record of the
		// previous frame while the current frame refills the other.
		std::vector<std::string> s_DropdownRows;      // last frame, complete
		std::vector<std::string> s_DropdownRowsBuild; // this frame, filling

		// Where the open dropdown's row sits, and what it holds. Recorded by
		// the row painter, consumed by DrawDropdownList() after the slab
		// window closes -- see that function for why the list cannot be drawn
		// where the row is.
		//
		// The options pointer is borrowed for the rest of ONE frame: it is
		// set during the draw and read a few calls later in the same frame,
		// never stored across one. Registry entries are stable for a frame by
		// construction (SyncDynamicAreas runs at the top, before anything
		// reads a row), which is the same lifetime rule the rest of the shell
		// already relies on.
		Rect                       s_rcDropdownAnchor {};
		const std::vector<Option> *s_pDropdownOptions = nullptr;
		int                        s_nDropdownValue   = 0;

		bool DrawsAsDropdown( const std::string &sId )
		{
			return std::find( s_DropdownRows.begin(), s_DropdownRows.end(), sId )
			     != s_DropdownRows.end();
		}

		// P3b: the id of the destructive Action currently ARMED -- one press
		// in, one press from happening (Entry::Confirm). One string for the
		// same reason as the dropdown above: exactly one can be armed, and
		// per-row state is the category this shell exists to not have.
		//
		// It DISARMS on a timeout, which is the property that matters: an
		// action left armed by someone who walked away must not be one click
		// from destroying a file when they come back.
		std::string   s_sArmedAction;
		float         s_flArmedAt = 0.0f;
		constexpr float kArmTimeout = 6.0f;   // seconds

		// P3c: the id of the Text row currently being EDITED. controls::Text
		// owns the display/input transition but needs one bit of caller
		// state to hold it in; one string, for the same reason as the two
		// above -- exactly one field can be open, and per-row state is the
		// category this shell exists to not have.
		std::string   s_sEditingText;

		// The one animated quantity: SPEC §8.4's 160 ms region duration,
		// used for the rail's collapse so the icon rail does not snap.
		float s_flRailAnim = shelltok::kRailFull;

		enum class Region : unsigned char { Rail, Sheet, Inspector };
		Region s_eFocusRegion = Region::Sheet;

		// How far the rail is scrolled, in physical px. Non-zero only when
		// the item column is taller than the rail -- which it is from 2.0x
		// (and at 1.75x on a short slab). DrawRail() clamps it every frame,
		// so a scale change that makes the rail fit again resets it to 0
		// without needing to be told.
		float s_flRailScroll = 0.0f;

		// The last surface size Draw() saw, in physical pixels.
		//
		// WHY THIS EXISTS AND NOT ImGui::GetIO(). Every getter a registration
		// declares is reachable from the CONSOLE THREAD as well as from the
		// render thread -- overlay_e2_set, overlay_e2_palette and the glyph
		// sweep all call them -- and ImGui has no context there, so GetIO()
		// asserts and takes the compositor down with it. That is exactly the
		// class of bug D15 already fixed once for fonts::RebuildAll();
		// shell.layout's own Facts summary is FormatLadder(), so
		// `overlay_e2_palette query shell.` aborted gamescope. Two floats
		// written once a frame by the one thread that has a context, read by
		// anyone.
		std::atomic<float> s_flSurfaceW = 0.0f;
		std::atomic<float> s_flSurfaceH = 0.0f;

		// =================================================================
		//  P5-adjacent: the three keyboard holes P4 reported (D18)
		// =================================================================
		// P4 shipped Tab to the Inspector and then had nothing to do there:
		// the region took focus and every key fell through to the sheet
		// underneath it. Same for the mode strip, which was pointer-only, and
		// for a downgraded Choice's popup, which ImGui's own nav could not
		// reach because the shell never enables keyboard nav.
		//
		// ONE index covers the whole Inspector, rather than a focus flag per
		// thing in it:
		//
		//     -1  the CONFIGURE / DETAILS mode strip
		//      0  the entry's own row, under VALUES
		//    1..n its parameters, in declaration order
		//
		// The strip is index -1 and not a separate mode because SPEC §8.2
		// already says what arrows do -- "move selection within the focused
		// region", and "inside a control: adjust". The strip IS a segmented
		// control, so Up onto it and Left/Right across it is the grammar the
		// table already gives, with no new key to learn or document.
		int s_nInspectorFocus = 0;

		// The highlighted item in an open dropdown. -1 means "no keyboard
		// highlight yet", so a popup opened by a click does not silently move
		// its own selection on the first Enter.
		int s_nPopupFocus = -1;

		// The chip the keyboard is pointed at inside the focused Bank.
		//
		// A Bank is the one kind AdjustValue() refuses (Registry.cpp: "Text,
		// Bank and Action have no ordering an arrow key could follow"), and it
		// has no popup either -- so before this it was the only control in the
		// product that a keyboard could not operate AT ALL. Both drawn
		// instances are the Log's filters, and this project may not synthesise
		// a pointer, so they were unreachable and untestable at once.
		//
		// The grammar is SPEC §8.2's, unchanged: Left/Right is "inside a
		// control: adjust" and Space/Enter is "activate / toggle". One index,
		// reset with the selection like s_nInspectorFocus, because a chip
		// index must not outlive the bank it indexes.
		int s_nBankChip = 0;

		// SPEC §6.3's Reachability Law: "with the Inspector closed, `?` or
		// `Ctrl+/` on a selected row opens Configure AND Details as one
		// full-sheet page with a back crumb, replacing the sheet content for
		// as long as you read it."
		//
		// It is a bool and not a stored id: the page always shows the CURRENT
		// selection, so there is no second copy of "which row" to drift out
		// of step with the sheet's own.
		bool s_bExplainPage = false;

		// =================================================================
		//  SPEC §6.3's inline param expansion (D20.3)
		// =================================================================
		// "A row that owns Params renders those Params inline in the Sheet,
		// beneath itself, in the Sheet's own Row grammar, whenever the
		// Inspector is unavailable -- collapsed by default, expanded with
		// `->` / click / `Space`, and marked with a `>` disclosure in place
		// of the chevron."
		//
		// This is the Reachability Law's specified MECHANISM, and until now
		// it did not exist while a comment in this file claimed it did. The
		// guarantee held anyway -- Ctrl+/ and Ctrl+K both reach every param
		// -- but "the sheet alone is a complete UI" was being kept by two
		// routes the spec describes as supplements, not as the mechanism.
		//
		// ONE EXPANSION AT A TIME, deliberately. SPEC §6.4 concedes that
		// inline expansion "reflows the sheet", which §8.3 otherwise forbids,
		// and accepts it only because the expansion is user-initiated. A
		// single open row bounds that reflow to one place: with several open
		// at once, expanding a row near the bottom of a column can move rows
		// you are not looking at, in a column you are not in. (SPEC §8.2's
		// old Esc ladder named "inline expansion" in the singular for the
		// same reason; D26 retired the ladder, not the argument.)
		std::string s_sExpandedEntry;

		// Where the keyboard is inside an expanded row: -1 is the row itself,
		// 0..n-1 its params. The Inspector's own s_nInspectorFocus is NOT
		// reused, even though it indexes the same params -- the two hosts can
		// both be alive in one session (Ctrl+I moves between them) and a
		// shared index would carry a focus from one into the other.
		int s_nInlineFocus = -1;

		// =================================================================
		//  P4: the command palette (SPEC §8.2, API.md §10)
		// =================================================================
		// Three pieces of state and no more, for the same reason the shell
		// has one selected row: the palette is a VIEW of the registry, not a
		// second place settings live. It holds a query, a highlight and an
		// open bit; everything it draws is recomputed from the registry each
		// frame by ui::Build().
		//
		// The results are NOT cached across frames on purpose. A cached
		// vector would have to be invalidated whenever a dynamic area
		// rebuilt (Area::Rebuilds() frees every Entry), and the alternative
		// -- rebuilding ~130 items against a short query -- is a few
		// microseconds of string search. Correctness for free beats a cache
		// with an invalidation rule nobody will remember to update.
		bool        s_bPaletteOpen = false;
		std::string s_sPaletteQuery;
		int         s_nPaletteSel  = 0;

		// The caret's blink phase. Reset on every keystroke so the caret is
		// solid while you type -- a caret that blinks through your own typing
		// reads as dropped input.
		float s_flPaletteCaretAt = 0.0f;

		void OpenPalette()
		{
			s_bPaletteOpen = true;
			s_sPaletteQuery.clear();
			s_nPaletteSel = 0;
			s_flPaletteCaretAt = (float)ImGui::GetTime();

			// Drop whatever text this frame already queued. The palette's own
			// handler runs LATER IN THE SAME FRAME as the Ctrl+K that opened
			// it, so a backend that puts 'k' in the character queue alongside
			// the chord -- which is backend-dependent, and this context's
			// input comes from wlserver rather than a stock ImGui backend --
			// would open the palette with "k" already typed into it.
			ImGui::GetIO().InputQueueCharacters.resize( 0 );
		}

		// D22. Set from wlserver's hotkey path (another thread), consumed by
		// Draw() on the thread that owns the shell's state. See
		// shell::RequestPalette() in Shell.h for why it is a request.
		std::atomic<bool> s_bPaletteRequested{ false };

		// D25: the LAUNCHER -- the same palette, drawn alone over the game.
		//
		// One bit, deliberately, and it is a MODE OF THE PALETTE rather than a
		// second surface: the launcher IS the palette, and giving it its own
		// query string, its own highlight and its own draw path would be the
		// two-code-paths mistake CommandPalette.h's header comment already
		// refuses for browse-versus-search. Everything below reads
		// s_bPaletteOpen; this only changes what is drawn BEHIND it and what
		// Esc gives back.
		bool s_bLauncherOnly = false;

		// The published mirror of the above, for wlserver -- which decides on
		// its own thread whether Left+Right Ctrl means "launcher" or "palette
		// over the shell" and cannot read s_bLauncherOnly without racing the
		// frame that is drawing it.
		std::atomic<bool> s_bLauncherOnlyPublished{ false };

		std::atomic<bool> s_bLauncherRequested{ false };

		// The overlay was hidden from outside the shell. See
		// shell::NotifyOverlayHidden().
		std::atomic<bool> s_bOverlayHiddenNotice{ false };

		// =================================================================
		//  The console surface
		// =================================================================
		// Two commands that reach the same three pieces of state Ctrl+I and
		// a click reach, and nothing else. They exist because the shell's
		// state is otherwise only addressable by pointer, which makes it
		// unverifiable from a script and undiagnosable from a bug report --
		// "which host were you in?" has no answer today.
		//
		// This is the project's own idiom rather than a new one: ConVar and
		// ConCommand are gamescope's runtime tunable and debug-command
		// system, and every other subsystem exposes its state through them.
		// Deliberately NOT persisted and NOT a config field: they set live
		// state, exactly as clicking would.
		ConVar<int> cv_overlay_e2_host(
			"overlay_e2_host", 0,
			"E2 inspector host: 0 column, 1 drawer, 2 hidden. Same three Ctrl+I cycles." );

		InspectorHost Host()
		{
			return (InspectorHost)std::clamp( cv_overlay_e2_host.Get(), 0, 2 );
		}
		void SetHost( InspectorHost eHost )
		{
			cv_overlay_e2_host.SetValue( (int)eHost );
		}

		// D20.3. SPEC §6.3 renders params inline "whenever the Inspector is
		// unavailable", which is exactly the Hidden host: with a column or a
		// drawer the params are already on screen in the Inspector, and
		// drawing them twice would be two painters for one declaration.
		//
		// Asked of Host() -- the user's own preference -- rather than of the
		// solved ladder, because the ladder never produces Hidden on its own
		// (SPEC §8.3's step 3 is "reachable by choice and persisted, not only
		// by width"), so the two are the same answer wherever it matters and
		// this one is available without a Slab in hand.
		bool InlineMode()
		{
			return Host() == InspectorHost::Hidden;
		}

		// P3b. The Inspector body scrolls now (see DrawInspector), and a
		// scroll is a POINTER gesture -- precisely the input this project is
		// permanently forbidden to synthesise (D4). So the same argument that
		// produced overlay_e2_select and overlay_e2_set applies a third time:
		// without a console route, "does the Inspector actually scroll, and
		// does the sixth parameter become reachable?" can be answered only by
		// a human with a wheel, and the defect this fixes was one that no test
		// and no screenshot of the DEFAULT state could see.
		//
		// It is a REQUEST, not the scroll's storage. The value is pushed into
		// ImGui only on the frame it changes, so the wheel stays authoritative
		// the rest of the time and the two cannot fight each other.
		ConVar<float> cv_overlay_e2_scroll(
			"overlay_e2_scroll", 0.0f,
			"Scroll the E2 Inspector body to this pixel offset; a negative value scrolls to the "
			"bottom. Exists so the Inspector's scrolling is verifiable without pointer input." );
		float s_flScrollApplied = 0.0f;

		void SelectById( std::span<std::string_view> args );
		void SetById( std::span<std::string_view> args );
		void PaletteCmd( std::span<std::string_view> args );
		void GlyphSweep( std::span<std::string_view> args );
		void KeyCmd( std::span<std::string_view> args );
		void PointerCmd( std::span<std::string_view> args );
		void GetById( std::span<std::string_view> args );

		// The palette's two helpers the console command reaches before the
		// drawing section defines them.
		void        PaletteJump( const std::string &sId );
		std::string PaletteValueText( const std::string &sId );

		// P3. The same argument that produced overlay_e2_select above, applied
		// to the other half of the problem: a registration's SELECTION was
		// unaddressable from a script, and so is its VALUE.
		//
		// This matters more than it sounds. There is no synthetic-pointer
		// route to a control here -- pointer injection is permanently
		// forbidden for this project (AUTONOMOUS-DECISIONS.md D4, after
		// injected clicks twice landed in the user's own windows) -- so
		// without this, "does this row actually drive the compositor, or does
		// it merely render?" cannot be answered by anything except a human
		// with a mouse. That question is not academic: issues #25 (frame
		// limiter) and #68 (force grab cursor) were both controls that
		// rendered correctly while doing nothing, and both survived review.
		//
		// It writes through Entry::Binding().Set() -- the SAME call a click
		// makes, not a parallel path -- so a value that lands here lands
		// exactly where a click's would. Deliberately NOT a way to reach
		// anything a click cannot: it can only address registered ids, and it
		// refuses a read-only kind rather than pretending to set one.
		ConCommand cc_overlay_e2_set(
			"overlay_e2_set",
			"Set an E2 row or parameter by id: overlay_e2_set <id> <value>. Writes through the same "
			"binding a click writes through, which is what makes a binding verifiable without "
			"pointer input. Values: on/off for a switch, a number otherwise. "
			"Through gamescopectl the id and value must be ONE quoted argument: "
			"gamescopectl overlay_e2_set \"display.fps_limit 60\".",
			SetById );

		// D22. overlay_e2_set could WRITE a binding from a script but nothing
		// could READ one, so "did that click actually move the state?" -- the
		// only question a mouse walkthrough asks -- had no answer a script
		// could check. Every previous verification worked around it by
		// setting a value and looking at a screenshot, which confirms the
		// PAINT and not the binding. This reads through Binding().Get(), the
		// same value the row itself prints.
		ConCommand cc_overlay_e2_get(
			"overlay_e2_get",
			"Print an E2 row or parameter's current value: overlay_e2_get <id>. Reads through the "
			"same binding the row draws from, so it answers \"did that click move the state?\" "
			"rather than \"did something repaint?\". With no id, prints every registered row and "
			"its value.",
			GetById );

		ConCommand cc_overlay_e2_select(
			"overlay_e2_select",
			"Select an E2 rail area, and optionally a row inside it: overlay_e2_select <area-id> [row-id]. "
			"With no arguments, lists the registered area and row ids. "
			"Through gamescopectl the two ids must be ONE quoted argument -- gamescopectl collapses "
			"everything after the command name into a single field, which wlserver then re-splits: "
			"gamescopectl overlay_e2_select \"setup.shell shell.layout\".",
			SelectById );

		// P4. The palette is KEYBOARD-ONLY by design, and this project
		// permanently forbids synthetic input of any kind (D4, after injected
		// events twice landed in the user's own windows). Without a console
		// surface the palette would therefore be the one feature in the
		// product that cannot be verified at all except by a human at a
		// keyboard -- and "it renders but does nothing" is exactly the defect
		// class (#25, #68) this branch keeps finding.
		//
		// It drives the SAME state the keys drive -- there is no parallel
		// path -- and `list` prints the ranked results, so the RANKING is
		// checkable from a script even though the drawing is not.
		ConCommand cc_overlay_e2_palette(
			"overlay_e2_palette",
			"Drive the E2 command palette: overlay_e2_palette <verb> [arg]. "
			"Verbs: open [query] | query <text> | sel <n> | adjust <-1|1> | enter | close | "
			"list | reach. Through gamescopectl the verb and its argument must be ONE quoted "
			"argument: gamescopectl overlay_e2_palette \"open margin\".",
			PaletteCmd );

		// D18. The shell shipped "inspector ›" with a box in it, in the one
		// region you only see AFTER hiding the Inspector -- a corner nobody
		// visits, which is where a box glyph survives longest. Every string
		// the shell draws is a declaration in one of a dozen area files, so
		// no fixture-based test covers them; this walks the LIVE registry
		// instead, which is the only place all of them exist at once.
		ConCommand cc_overlay_e2_glyphs(
			"overlay_e2_glyphs",
			"Sweep every registered E2 string -- area titles, row and parameter names, help text, "
			"option labels and units -- for characters outside the font atlas's baked range "
			"(U+0020..U+00FF), which render as fallback boxes. Prints one line per offender and a "
			"total; prints nothing but the total when the registry is clean.",
			GlyphSweep );

		ConCommand cc_overlay_e2_key(
			"overlay_e2_key",
			"Send a REAL key event to the overlay: overlay_e2_key <chord> [chord...]. "
			"Chords are like \"ctrl+k\", \"shift+slash\", \"down\". It appends to the overlay's own "
			"input queue -- the same one wlserver_dispatch_key() writes to -- so the key is "
			"processed by the identical path a physical press takes, and it cannot reach any "
			"window, client or seat outside this overlay. Through gamescopectl the chords must be "
			"ONE quoted argument: gamescopectl overlay_e2_key \"tab down enter\".",
			KeyCmd );

		// D22. The pointer half of the same idea, and the reason this branch
		// could ship a mouse UI nobody had ever used a mouse on: D18 built
		// overlay_e2_key, every subsequent binding was verified with it, and
		// the CONTROLS -- which are driven by a pointer, not by a key -- kept
		// being "verified" by console commands that moved their bound state
		// directly. That proves the binding, never the hit box. See PointerCmd.
		ConCommand cc_overlay_e2_pointer(
			"overlay_e2_pointer",
			"Send a REAL pointer event to the overlay: overlay_e2_pointer <verb> [args]. "
			"Verbs: move <x> <y> | down [left|right|middle] | up [...] | click [x y] [button] | "
			"scroll <dx> <dy> | pos. Coordinates are PIXELS on the overlay surface, the same "
			"ones a screenshot shows, with the origin top-left. Like overlay_e2_key it appends "
			"to the overlay's own input queue, so the event takes the identical path a physical "
			"mouse takes and cannot reach any window, client or seat outside this overlay. "
			"Through gamescopectl the verb and its arguments must be ONE quoted argument: "
			"gamescopectl overlay_e2_pointer \"click 640 400\".",
			PointerCmd );

		// =================================================================
		//  Registration
		// =================================================================
		// Six areas hosting legacy panel bodies through the migration seam,
		// plus one genuinely-E2 area.
		//
		// WHY ONE REAL AREA IN A PHASE THAT MIGRATES NOTHING. The Inspector
		// is specified as a pure function of a registration (SPEC §5.2
		// clause 0), so with only escaped areas -- which by construction
		// have no entries -- there would be no selection anywhere in the
		// product and both modes would be dead code shipped untested. The
		// area below is the shell describing ITSELF: its rows bind to the
		// shell's own runtime state and to a ConVar, never to a config
		// field, so it adds no on-disk key and cannot change how any
		// existing config loads. It is also the natural home for the
		// ladder readout a bug report wants.
		//
		// SECTIONS: three, per SPEC §8.1 as amended by D8 -- `IMAGE` folds
		// into DISPLAY and `AUDIO` into SYSTEM, because a section header
		// that groups exactly one item costs a line and buys nothing. Area
		// ids keep their original prefixes (`image.shaders` is still
		// `image.shaders`); only the rail's grouping moved.
		const Option kHostOptions[] = {
			{ (int)InspectorHost::Column, "column" },
			{ (int)InspectorHost::Drawer, "drawer" },
			{ (int)InspectorHost::Hidden, "hidden" },
		};

		bool s_bSpineLabel = true;

		std::string FormatLadder();

		void RegisterAll( Registry &reg )
		{
			// ---- DISPLAY -------------------------------------------------
			// P3 part A: the four Gamescope tabs became three AREAS, not one
			// area with four group bands. SPEC §8.1's rail is the product's
			// only navigation and lists exactly these as rail items, so a tab
			// bar becomes rail items -- a four-group sheet would be the same
			// tab bar redrawn as headings. AUTONOMOUS-DECISIONS.md D13.1.
			PanelDisplay_RegisterAreas( reg );
			PanelShaders_RegisterArea( reg );

			// ---- SYSTEM --------------------------------------------------
			// Mixer leads SYSTEM, ahead of Monitor and Log -- SPEC §8.1's
			// amendment is explicit that the fold keeps the original
			// relative order.
			// P3 part B. The first DYNAMIC area: its rows are one per live
			// PipeWire stream, discovered at runtime rather than declared
			// here. See ui::Area::Rebuilds().
			PanelAudio_RegisterArea( reg );
			// P3 part C. FpsDisplay.cpp does two jobs; this declares the
			// SETTINGS half only. The HUD over the game is drawn from its
			// own separate context and was never in the redesign's scope.
			FpsDisplay_RegisterArea( reg );
			PanelLog_RegisterArea( reg );
			// P6. The second content area: version identity + the embedded
			// CHANGELOG.md. Sits next to Log because both answer a question
			// ABOUT the running system rather than configuring it.
			PanelChangelog_RegisterArea( reg );

			// ---- SETUP ---------------------------------------------------
			// P3 part B. The Config panel's three tabs became three areas,
			// on D13.1's reasoning -- setup.profiles, setup.pergame,
			// setup.appearance. The rail is now the eleven items SPEC §8.1
			// names.
			PanelConfig_RegisterAreas( reg );

			Area &shell = reg.Add( "setup.shell", "Shell", Section::Setup );
			shell.Summary( []{ return std::string( "How the settings surface itself is laid out." ); } );

			shell.Choice( "shell.inspector_host", "Inspector",
				AnyBind::Of<int>(
					[]{ return (int)Host(); },
					[]( int n ) { SetHost( (InspectorHost)std::clamp( n, 0, 2 ) ); } ),
				kHostOptions, 3 )
				.Help( "Where the Inspector lives. A column sits beside the sheet; a drawer "
				       "floats over its right edge; hidden leaves only the spine. Ctrl+I cycles "
				       "the same three. A slab too narrow for a column is given a drawer "
				       "regardless of this setting." )
				.Default( (int)InspectorHost::Column )
				.Keywords( "inspector drawer column hidden depth ctrl+i" )
				.Param( "spine_label", "Name the spine",
					AnyBind::Of<bool>( &s_bSpineLabel ) )
					.Help( "Draw the word \"inspector\" down the collapsed spine. Off leaves a "
					       "bare edge, which is quieter but no longer says what it opens." )
					.Default( true );

			shell.Facts( "shell.layout", "Layout", []{ return FormatLadder(); } )
				.Help( "What the responsive ladder decided for the current surface and "
				       "display scale. Read-only; every value here is derived." )
				.Keywords( "ladder slab scale columns rail step layout" )
				.Live( "slab", []{
					char sz[ 96 ];
					// s_flSurfaceW/H, not ImGui::GetIO() -- see the comment
					// above FormatLadder(). A .Live() runs on the render
					// thread today, but nothing in the registry's contract
					// says it must, and the two readings must not disagree.
					const Slab slab = Slab::For( s_flSurfaceW.load( std::memory_order_relaxed ),
					                             s_flSurfaceH.load( std::memory_order_relaxed ), Scale() );
					snprintf( sz, sizeof( sz ), "%.0f x %.0f px  (%.0f x %.0f base)",
						slab.flWidthPx, slab.flHeightPx, slab.flWidthBase, slab.flHeightBase );
					return Fact{ "slab", sz };
				} )
				.Live( "scale", []{
					char sz[ 32 ];
					snprintf( sz, sizeof( sz ), "%.2fx", Scale() );
					return Fact{ "display scale", sz };
				} )
				.Live( "regions", []{ return Fact{ "regions", FormatLadder() }; } );
		}

		Registry &Reg()
		{
			if ( !s_pRegistry )
			{
				// Leaked on purpose: this outlives every frame and there is
				// no shutdown path that would observe the destructor.
				s_pRegistry = new Registry();
				RegisterAll( *s_pRegistry );
				s_pRegistry->SelfTest();
				s_sSelectedArea = s_pRegistry->AreaCount() ? s_pRegistry->AreaAt( 0 ).Id() : std::string();
			}
			return *s_pRegistry;
		}

		const Area *SelectedArea()
		{
			const Area *pArea = Reg().FindArea( s_sSelectedArea );
			if ( pArea && pArea->Available() )
				return pArea;
			for ( size_t i = 0; i < Reg().AreaCount(); ++i )
				if ( Reg().AreaAt( i ).Available() )
					return &Reg().AreaAt( i );
			return nullptr;
		}

		const Entry *SelectedEntry()
		{
			if ( s_sSelectedEntry.empty() )
				return nullptr;
			const Entry *pEntry = Reg().FindEntry( s_sSelectedEntry );
			// A selection only survives while its own area is the one on
			// screen -- the Inspector is a function of THIS sheet's
			// selection, never a sticky pointer into a category the user
			// has navigated away from. Checked by identity against the
			// current area's own entries rather than by comparing id
			// prefixes: SPEC §2.6 is explicit that an area id is not a
			// promise about the ids inside it.
			const Area *pArea = SelectedArea();
			if ( pEntry && pArea )
			{
				for ( size_t i = 0; i < pArea->EntryCount(); ++i )
					if ( &pArea->EntryAt( i ) == pEntry )
						return pEntry;
			}
			return nullptr;
		}

		void Select( const Entry *pEntry )
		{
			s_sSelectedEntry  = pEntry ? pEntry->Id() : std::string();
			s_bModeOverridden = false;   // SPEC §5.1: the mode choice is not remembered
			if ( pEntry )
				s_eMode = ModeFor( pEntry->GetKind(), pEntry->GetCompositeKind() );

			// A new selection means new Inspector contents, so the Inspector's
			// focus goes back to the top rather than pointing at a parameter
			// index the new row may not even have. Done HERE and not at each
			// caller: every route into a selection -- click, arrows, palette,
			// console -- goes through this function, which is what stops the
			// index outliving the thing it indexes.
			s_nInspectorFocus = 0;
			s_nBankChip       = 0;

			// D20.3: an inline expansion belongs to the row that owns it.
			// Selecting a different row collapses it and takes the inline
			// focus back to the row, for the same reason the Inspector's
			// index resets -- an index must not outlive the thing it indexes,
			// and a sheet that silently kept a foreign row expanded would
			// reflow around a row the user is no longer on.
			if ( s_sExpandedEntry != s_sSelectedEntry )
				s_sExpandedEntry.clear();
			s_nInlineFocus = -1;

			// SPEC §3.9's two-stage arm only means anything while the armed
			// chip is the thing under the user's hands. Moving the selection
			// used to leave `s_sArmedAction` set -- so arming `Delete saved
			// config`, walking away, and coming back made the NEXT SINGLE
			// PRESS destroy the file, which is the exact failure Confirm()
			// exists to make impossible. (This function's own comment above
			// already claimed "walking away disarms it"; it did not.)
			// Disarmed here rather than at each caller for the same reason
			// the focus index is: every route into a selection comes through
			// this function.
			if ( !s_sArmedAction.empty() && s_sArmedAction != s_sSelectedEntry )
				s_sArmedAction.clear();

			// The explain page shows the current selection, so moving the
			// selection while it is open would silently swap the page out from
			// under the reader. Closing it puts them back on the sheet they
			// just moved in, which is where they were looking.
			s_bExplainPage = false;
		}

		// =================================================================
		//  D26: closing the overlay from inside the shell
		// =================================================================
		// Esc's last rung, and the only place the shell closes itself. It is
		// a function rather than a bare SettingsOverlay_SetVisible( false )
		// because closing has to LEAVE THE SHELL CLEAN: the next open must
		// not come back sitting on an explain page nobody asked for, with a
		// dropdown half-open or a delete still armed.
		//
		// What is cleared is exactly the transient set -- the things that
		// only make sense in the session that created them. What SURVIVES is
		// the arrangement the user chose and would be annoyed to lose: the
		// selected area, the selected row, and the Inspector host. Reopening
		// puts them back where they were.
		//
		// The palette is closed too. It is a transient layer by the same
		// definition, and a palette that reappeared over the shell on the
		// next open would be a search someone abandoned a session ago.
		void CloseShell()
		{
			s_sArmedAction.clear();
			s_sOpenDropdown.clear();
			s_nPopupFocus     = -1;
			s_sEditingText.clear();
			s_bExplainPage    = false;
			s_sExpandedEntry.clear();
			s_nInlineFocus    = -1;
			s_bPaletteOpen    = false;

			SettingsOverlay_SetVisible( false );
		}

		InspectorMode CurrentMode( const Entry *pEntry )
		{
			if ( !pEntry )
				return InspectorMode::Configure;
			if ( s_bModeOverridden )
				return s_eMode;
			return ModeFor( pEntry->GetKind(), pEntry->GetCompositeKind() );
		}

		// =================================================================
		//  D18: real key events, from a script
		// =================================================================
		// P4 had to report that NO REAL KEYPRESS WAS EVER SENT -- every
		// binding it added was verified by unit test and by console
		// equivalents that moved the same variables, never by a key. That is
		// a large gap for a shell whose headline feature is being
		// keyboard-driven, and it is the reason the three holes this commit
		// closes went unnoticed: the console commands drove the STATE, so
		// state-driving worked, and nobody had a way to press Tab.
		//
		// WHY THIS IS NOT THE BANNED THING. D4 forbids synthetic input
		// because injected events twice escaped into the user's own windows
		// -- ydotool and friends drive the whole seat, and anything focused
		// receives them. This drives NOTHING outside gamescope: it appends to
		// s_InputQueue, the overlay's own producer/consumer queue, which is
		// read by DrainInputQueue() on the steamcompmgr thread and fed to the
		// overlay's ImGui context and nowhere else. No compositor seat, no
		// wl_keyboard, no other client, no other window -- the events cannot
		// leave the overlay even in principle, because nothing else reads
		// that queue.
		//
		// It is also the SAME path a physical key takes, which is the whole
		// point: wlserver_dispatch_key() calls SettingsOverlay_QueueKeyEvent()
		// and so does this, with the same arguments. There is no parallel
		// input path to keep in step -- a key sent here is processed by
		// HandleKeyEvent(), mapped by ImGuiKeyForKeycode(), and delivered
		// through io.AddKeyEvent() exactly as the keyboard's would be, so a
		// binding verified this way is verified through its real mechanism.
		struct KeyName
		{
			const char *pszName;
			uint32_t    uCode;
			const char *pszText;   // the UTF-8 a real xkb lookup would yield, or null
		};

		// SPEC §8.2's table is the scope: the keys the shell actually binds,
		// plus the letters the palette's query line needs. Not a general
		// keyboard -- a general one would be a seat, which is the thing this
		// is careful not to be.
		constexpr KeyName kKeyNames[] = {
			{ "up",        KEY_UP,        nullptr },
			{ "down",      KEY_DOWN,      nullptr },
			{ "left",      KEY_LEFT,      nullptr },
			{ "right",     KEY_RIGHT,     nullptr },
			{ "enter",     KEY_ENTER,     nullptr },
			{ "space",     KEY_SPACE,     " "     },
			{ "esc",       KEY_ESC,       nullptr },
			{ "escape",    KEY_ESC,       nullptr },
			{ "tab",       KEY_TAB,       nullptr },
			{ "backspace", KEY_BACKSPACE, nullptr },
			{ "slash",     KEY_SLASH,     "/"     },
			{ "question",  KEY_SLASH,     "?"     },   // shift+slash on most layouts
		};

		bool LookupKey( const std::string &sName, uint32_t *puCode, const char **ppszText )
		{
			for ( const KeyName &k : kKeyNames )
			{
				if ( sName == k.pszName )
				{
					*puCode = k.uCode;
					*ppszText = k.pszText;
					return true;
				}
			}
			// A single letter or digit, which is what the palette's query
			// needs. KEY_A..KEY_Z are not contiguous in the Linux table, so
			// this is a lookup rather than arithmetic.
			if ( sName.size() == 1 )
			{
				static constexpr uint32_t kLetters[] = {
					KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
					KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
					KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z };
				static constexpr uint32_t kDigits[] = {
					KEY_0, KEY_1, KEY_2, KEY_3, KEY_4,
					KEY_5, KEY_6, KEY_7, KEY_8, KEY_9 };

				const char c = sName[ 0 ];
				if ( c >= 'a' && c <= 'z' ) { *puCode = kLetters[ c - 'a' ]; *ppszText = nullptr; return true; }
				if ( c >= '0' && c <= '9' ) { *puCode = kDigits[ c - '0' ];  *ppszText = nullptr; return true; }
			}
			return false;
		}

		void KeyCmd( std::span<std::string_view> args )
		{
			if ( args.size() < 2 )
			{
				console_log.infof( "overlay_e2_key <chord> [chord...]  e.g. \"ctrl+k\", \"down down enter\"" );
				console_log.infof( "  modifiers: ctrl shift alt   keys: a-z 0-9 up down left right" );
				console_log.infof( "             enter space esc tab backspace slash question" );
				return;
			}

			// gamescopectl collapses everything after the command name into
			// one field, which wlserver re-splits -- the same convention
			// overlay_e2_select and overlay_e2_palette already document. So
			// every argument is treated as a chord and they run in order.
			for ( size_t nArg = 1; nArg < args.size(); ++nArg )
			{
				std::string sChord( args[ nArg ] );
				std::transform( sChord.begin(), sChord.end(), sChord.begin(),
					[]( unsigned char c ) { return (char)std::tolower( c ); } );

				bool bCtrl = false, bShift = false, bAlt = false;
				std::string sKey;

				size_t nPos = 0;
				while ( nPos <= sChord.size() )
				{
					const size_t nPlus = sChord.find( '+', nPos );
					const std::string sPart = sChord.substr( nPos,
						nPlus == std::string::npos ? std::string::npos : nPlus - nPos );

					if      ( sPart == "ctrl"  || sPart == "control" ) bCtrl  = true;
					else if ( sPart == "shift" )                       bShift = true;
					else if ( sPart == "alt" )                         bAlt   = true;
					else if ( !sPart.empty() )                         sKey   = sPart;

					if ( nPlus == std::string::npos )
						break;
					nPos = nPlus + 1;
				}

				uint32_t uCode = 0;
				const char *pszText = nullptr;
				if ( !LookupKey( sKey, &uCode, &pszText ) )
				{
					console_log.errorf( "overlay_e2_key: unknown key \"%s\"", sKey.c_str() );
					return;
				}

				// Modifiers down, key down, key up, modifiers up -- the exact
				// order and the exact events a real keyboard produces, which
				// is what makes the chord's Ctrl actually held at the moment
				// the key arrives.
				if ( bCtrl )  SettingsOverlay_QueueKeyEvent( KEY_LEFTCTRL,  true );
				if ( bShift ) SettingsOverlay_QueueKeyEvent( KEY_LEFTSHIFT, true );
				if ( bAlt )   SettingsOverlay_QueueKeyEvent( KEY_LEFTALT,   true );

				// Text accompanies a press only when no Ctrl/Alt is held --
				// which is what xkb_state_key_get_utf8() yields on the real
				// path, where a modified key produces a control character
				// that wlserver_dispatch_key() already declines to forward.
				std::string sText;
				if ( pszText && !bCtrl && !bAlt )
					sText = pszText;
				else if ( !bCtrl && !bAlt && sKey.size() == 1 &&
				          ( ( sKey[ 0 ] >= 'a' && sKey[ 0 ] <= 'z' ) ||
				            ( sKey[ 0 ] >= '0' && sKey[ 0 ] <= '9' ) ) )
					sText = bShift ? std::string( 1, (char)std::toupper( sKey[ 0 ] ) ) : sKey;

				SettingsOverlay_QueueKeyEvent( uCode, true, sText );
				SettingsOverlay_QueueKeyEvent( uCode, false );

				if ( bAlt )   SettingsOverlay_QueueKeyEvent( KEY_LEFTALT,   false );
				if ( bShift ) SettingsOverlay_QueueKeyEvent( KEY_LEFTSHIFT, false );
				if ( bCtrl )  SettingsOverlay_QueueKeyEvent( KEY_LEFTCTRL,  false );
			}
		}

		// D22's read half. See cc_overlay_e2_get.
		void GetById( std::span<std::string_view> args )
		{
			const auto PrintOne = [ & ]( const std::string &sId, Kind eKind )
			{
				const std::string sValue = PaletteValueText( sId );
				console_log.infof( "%-40s %-10s %s", sId.c_str(), KindName( eKind ),
					sValue.empty() ? "-" : sValue.c_str() );
			};

			if ( args.size() >= 2 )
			{
				const std::string sId( args[ 1 ] );
				if ( const Entry *pE = Reg().FindEntry( sId ) )
					PrintOne( sId, pE->GetKind() );
				else if ( const Parameter *pP = Reg().FindParam( sId ) )
					PrintOne( sId, pP->GetKind() );
				else
					console_log.errorf( "overlay_e2_get: no such id \"%s\"", sId.c_str() );
				return;
			}

			// No id: the whole registry, rows and their parameters, in rail
			// order -- the same walk cc_overlay_e2_glyphs does, for the same
			// reason (these only ever coexist in the live registry).
			for ( size_t a = 0; a < Reg().AreaCount(); ++a )
			{
				const Area &area = Reg().AreaAt( a );
				for ( size_t e = 0; e < area.EntryCount(); ++e )
				{
					const Entry &entry = area.EntryAt( e );
					PrintOne( entry.Id(), entry.GetKind() );
					for ( size_t p = 0; p < entry.ParamCount(); ++p )
						PrintOne( entry.ParamAt( p ).Id(), entry.ParamAt( p ).GetKind() );
				}
			}
		}

		// =================================================================
		//  D22: real pointer events, from a script
		// =================================================================
		// WHY THIS IS NOT THE BANNED THING, restated for the pointer because
		// the pointer is the half that actually caused the ban. D4 forbids
		// synthetic input because injected ydotool CLICKS twice escaped into
		// the user's own windows -- ydotool drives the whole seat, so a click
		// lands wherever the seat's focus happens to be, which is exactly the
		// accident that happened. This cannot do that, and not as a matter of
		// care: it appends to s_InputQueue, the overlay's own producer/
		// consumer queue, which DrainInputQueue() reads on the steamcompmgr
		// thread and feeds to the overlay's ImGui context and nowhere else.
		// There is no seat, no wl_pointer, no other client and no other
		// window on the far side of that queue -- an event put in here can
		// only ever arrive at this overlay, because nothing else reads it.
		// The ydotool hazard is absent structurally, not by convention.
		//
		// It is also the SAME path a physical mouse takes:
		// wlserver_mousemotion()/wlserver_mousebutton()/wlserver_mousewheel()
		// call SettingsOverlay_QueueMouseMotionAbsolute()/QueueMouseButton()/
		// QueueMouseWheel(), and so does this, with the same arguments. So a
		// hit box verified this way is verified through its real mechanism --
		// which is the whole point, because the defect this branch keeps
		// shipping is a control that DRAWS in one rect and HIT-TESTS in
		// another, and no console command that pokes the bound value can see
		// that. Only a click at a coordinate can.
		//
		// Coordinates are surface PIXELS, not the normalized 0..1 the queue
		// carries, because the thing a person has in hand when writing a test
		// is a screenshot. The conversion is done here, against the live
		// surface size, so the caller never has to know the texture size and
		// a test written at one resolution does not silently aim elsewhere at
		// another.
		uint32_t LinuxButtonFromName( const std::string &sName )
		{
			if ( sName.empty() || sName == "left"   || sName == "l" ) return BTN_LEFT;
			if ( sName == "right"  || sName == "r" )                  return BTN_RIGHT;
			if ( sName == "middle" || sName == "m" )                  return BTN_MIDDLE;
			return 0;
		}

		// Pixels -> the normalized coordinate the queue carries. Returns
		// false when the overlay has never been opened (no surface yet), so
		// the caller can say that rather than aiming at a zero-sized surface.
		bool QueuePointerMovePx( double flPxX, double flPxY )
		{
			uint32_t uWidth = 0, uHeight = 0;
			if ( !SettingsOverlay_GetSurfaceSize( &uWidth, &uHeight ) || uWidth == 0 || uHeight == 0 )
				return false;

			SettingsOverlay_QueueMouseMotionAbsolute( flPxX / (double)uWidth, flPxY / (double)uHeight );
			return true;
		}

		void PointerCmd( std::span<std::string_view> args )
		{
			if ( args.size() < 2 )
			{
				console_log.infof( "overlay_e2_pointer <verb> [args]" );
				console_log.infof( "  move <x> <y>              move the cursor to a surface pixel" );
				console_log.infof( "  down|up [left|right|mid]  press / release a button where it is" );
				console_log.infof( "  click [x y] [button]      move (if given), press, release" );
				console_log.infof( "  scroll <dx> <dy>          wheel, in wlserver_mousewheel units" );
				console_log.infof( "  pos                       print the cursor and the surface size" );
				return;
			}

			std::string sVerb( args[ 1 ] );
			std::transform( sVerb.begin(), sVerb.end(), sVerb.begin(),
				[]( unsigned char c ) { return (char)std::tolower( c ); } );

			const auto Number = [ & ]( size_t n, double *pOut ) -> bool
			{
				if ( n >= args.size() )
					return false;
				// strtod, not stod: gamescope builds with exceptions off, so
				// the throwing parse is not available here.
				const std::string s( args[ n ] );
				char *pszEnd = nullptr;
				const double flValue = std::strtod( s.c_str(), &pszEnd );
				if ( pszEnd == s.c_str() || !std::isfinite( flValue ) )
					return false;
				*pOut = flValue;
				return true;
			};

			if ( sVerb == "pos" )
			{
				uint32_t uWidth = 0, uHeight = 0;
				if ( !SettingsOverlay_GetSurfaceSize( &uWidth, &uHeight ) )
				{
					console_log.infof( "overlay_e2_pointer: no surface yet -- open the overlay first" );
					return;
				}
				double flX = 0.0, flY = 0.0;
				if ( SettingsOverlay_GetCursorPosition( &flX, &flY ) )
					console_log.infof( "cursor %.1f,%.1f  surface %ux%u", flX, flY, uWidth, uHeight );
				else
					console_log.infof( "cursor <unset>  surface %ux%u", uWidth, uHeight );
				return;
			}

			if ( sVerb == "move" )
			{
				double flX = 0.0, flY = 0.0;
				if ( !Number( 2, &flX ) || !Number( 3, &flY ) )
				{
					console_log.errorf( "overlay_e2_pointer move <x> <y>" );
					return;
				}
				if ( !QueuePointerMovePx( flX, flY ) )
					console_log.errorf( "overlay_e2_pointer: no surface yet -- open the overlay first" );
				return;
			}

			if ( sVerb == "scroll" )
			{
				double flX = 0.0, flY = 0.0;
				if ( !Number( 2, &flX ) || !Number( 3, &flY ) )
				{
					console_log.errorf( "overlay_e2_pointer scroll <dx> <dy>" );
					return;
				}
				SettingsOverlay_QueueMouseWheel( flX, flY );
				return;
			}

			if ( sVerb == "down" || sVerb == "up" )
			{
				const uint32_t uButton = LinuxButtonFromName(
					args.size() > 2 ? std::string( args[ 2 ] ) : std::string() );
				if ( uButton == 0 )
				{
					console_log.errorf( "overlay_e2_pointer: unknown button" );
					return;
				}
				SettingsOverlay_QueueMouseButton( uButton, sVerb == "down" );
				return;
			}

			if ( sVerb == "click" )
			{
				// "click 640 400 right" and "click" and "click right" all
				// parse: the coordinate pair is optional, and what follows it
				// (or stands alone) is the button.
				size_t nButtonArg = 2;
				double flX = 0.0, flY = 0.0;
				if ( Number( 2, &flX ) && Number( 3, &flY ) )
				{
					if ( !QueuePointerMovePx( flX, flY ) )
					{
						console_log.errorf( "overlay_e2_pointer: no surface yet -- open the overlay first" );
						return;
					}
					nButtonArg = 4;
				}

				const uint32_t uButton = LinuxButtonFromName(
					args.size() > nButtonArg ? std::string( args[ nButtonArg ] ) : std::string() );
				if ( uButton == 0 )
				{
					console_log.errorf( "overlay_e2_pointer: unknown button" );
					return;
				}

				// Press and release, in that order and in one batch -- which
				// is what a real click is, and which ImGui's own input-event
				// trickling is built to spread across two frames (see
				// UpdateInputEvents(): a second transition on the same button
				// stops the queue and defers to the next NewFrame()). The
				// press therefore lands on one drawn frame and the release on
				// the next, so a Button that fires on release fires exactly
				// once. Doing it as two console commands instead would leave
				// the split to IPC timing, which is not a guarantee.
				SettingsOverlay_QueueMouseButton( uButton, true );
				SettingsOverlay_QueueMouseButton( uButton, false );
				return;
			}

			console_log.errorf( "overlay_e2_pointer: unknown verb \"%s\"", sVerb.c_str() );
		}

		// D18's live sweep. See cc_overlay_e2_glyphs for why this walks the
		// registry rather than being a unit test: the strings it checks are
		// declared across a dozen area files and only ever coexist here.
		void GlyphSweep( std::span<std::string_view> args )
		{
			(void)args;
			int nBad = 0;

			// One checker, called on every string, so a new text-bearing
			// field cannot be half-covered: it is either passed to this or it
			// is not swept at all, and "not swept at all" is visible here.
			const auto Check = [ & ]( const char *pszWhere, const char *pszWhat,
			                          const std::string &s ) {
				const uint32_t cp = fonts::FirstUnbakedCodepoint( s.c_str() );
				if ( cp == 0 )
					return;
				nBad++;
				console_log.errorf( "  U+%04X in %s %s: \"%s\"",
					cp, pszWhere, pszWhat, s.c_str() );
			};

			for ( size_t a = 0; a < Reg().AreaCount(); ++a )
			{
				const Area &area = Reg().AreaAt( a );
				Check( area.Id().c_str(), "title", area.Title() );

				for ( size_t i = 0; i < area.EntryCount(); ++i )
				{
					const Entry &e = area.EntryAt( i );
					Check( e.Id().c_str(), "title", e.Title() );
					Check( e.Id().c_str(), "help",  e.HelpText() );
					Check( e.Id().c_str(), "unit",  e.Unit() );
					for ( const Option &o : e.Options() )
						Check( e.Id().c_str(), "option", o.pszLabel ? o.pszLabel : "" );

					for ( size_t p = 0; p < e.ParamCount(); ++p )
					{
						const Parameter &pa = e.ParamAt( p );
						Check( pa.Id().c_str(), "param title", pa.Title() );
						Check( pa.Id().c_str(), "param help",  pa.HelpText() );
						Check( pa.Id().c_str(), "param unit",  pa.Unit() );
						for ( const Option &o : pa.Options() )
							Check( pa.Id().c_str(), "param option", o.pszLabel ? o.pszLabel : "" );
					}
				}
			}

			if ( nBad == 0 )
				console_log.infof( "E2 glyphs: registry clean -- every string inside U+%04X..U+%04X",
					fonts::kBakedFirst, fonts::kBakedLast );
			else
				console_log.errorf( "E2 glyphs: %d string(s) would draw a fallback box", nBad );
		}

		void SelectById( std::span<std::string_view> args )
		{
			if ( args.size() < 2 )
			{
				console_log.infof( "E2 areas:" );
				for ( size_t i = 0; i < Reg().AreaCount(); ++i )
				{
					const Area &a = Reg().AreaAt( i );
					console_log.infof( "  %-20s %s", a.Id().c_str(), a.Title().c_str() );
					for ( size_t j = 0; j < a.EntryCount(); ++j )
						console_log.infof( "      %s", a.EntryAt( j ).Id().c_str() );
				}
				return;
			}

			const std::string sArea( args[ 1 ] );
			if ( !Reg().FindArea( sArea ) )
			{
				console_log.errorf( "no such E2 area: %s", sArea.c_str() );
				return;
			}
			s_sSelectedArea = sArea;

			// Through Select(), never around it. This used to assign
			// s_sSelectedEntry/s_eMode by hand -- a second selection path,
			// which then skipped the Inspector focus reset, the explain-page
			// close and (since it was added) the destructive-action disarm.
			// A console selection has to leave exactly the state a click
			// leaves, or every keyboard test driven from here is testing a
			// state the product never actually reaches.
			Select( nullptr );

			if ( args.size() >= 3 )
			{
				const std::string sRow( args[ 2 ] );
				const Entry *pEntry = Reg().FindEntry( sRow );
				if ( !pEntry )
				{
					console_log.errorf( "no such E2 row: %s", sRow.c_str() );
					return;
				}
				Select( pEntry );
			}
		}

		// See cc_overlay_e2_set's comment for why this exists. Writes through
		// the binding, never around it.
		void SetById( std::span<std::string_view> args )
		{
			if ( args.size() < 3 )
			{
				console_log.errorf( "usage: overlay_e2_set <id> <value>" );
				return;
			}

			const std::string sId( args[ 1 ] );
			const std::string sVal( args[ 2 ] );

			// A Param is addressable by the id the Prefix Law synthesised,
			// exactly as the palette addresses it -- so a parameter is as
			// verifiable as a row, which is the point.
			const Entry     *pEntry = Reg().FindEntry( sId );
			const Parameter *pParam = pEntry ? nullptr : Reg().FindParam( sId );
			if ( !pEntry && !pParam )
			{
				console_log.errorf( "no such E2 id: %s", sId.c_str() );
				return;
			}

			const Kind    eKind = pEntry ? pEntry->GetKind() : pParam->GetKind();
			const AnyBind &bind = pEntry ? pEntry->Binding() : pParam->Binding();

			if ( IsReadOnly( eKind ) || !bind.IsBound() )
			{
				console_log.errorf( "'%s' is a %s -- there is nothing to set",
					sId.c_str(), KindName( eKind ) );
				return;
			}

			// The TYPE comes from what the binding currently holds, never from
			// the string's shape. Set() ignores a variant of the wrong
			// alternative, so guessing here would fail silently -- the one
			// failure mode this command exists to expose.
			const Value vNow = bind.Get();
			if ( std::holds_alternative<bool>( vNow ) )
			{
				const bool b = ( sVal == "1" || sVal == "on" || sVal == "true" );
				bind.Set( Value{ b } );
			}
			else if ( std::holds_alternative<int>( vNow ) )
			{
				bind.Set( Value{ (int)strtol( sVal.c_str(), nullptr, 10 ) } );
			}
			else if ( std::holds_alternative<float>( vNow ) )
			{
				bind.Set( Value{ strtof( sVal.c_str(), nullptr ) } );
			}
			else
			{
				bind.Set( Value{ sVal } );
			}

			// Read back rather than echo the request: what the binding
			// actually holds afterwards is the answer, and a setter that
			// clamped (SetFpsLimit does) should be seen to have clamped.
			console_log.infof( "%s = %s", sId.c_str(), ValueToString( bind.Get() ).c_str() );
		}

		// See cc_overlay_e2_palette. Every verb below moves the SAME three
		// variables the keyboard moves; none of them is a second code path.
		void PaletteCmd( std::span<std::string_view> args )
		{
			const std::string sVerb = args.size() >= 2 ? std::string( args[ 1 ] ) : "list";
			const std::string sArg  = args.size() >= 3 ? std::string( args[ 2 ] ) : "";

			if ( sVerb == "close" )
			{
				s_bPaletteOpen = false;
				console_log.infof( "palette closed" );
				return;
			}

			if ( sVerb == "reach" )
			{
				// Direction B's discoverability gate, reported over the LIVE
				// registry rather than a fixture. See AUTONOMOUS-DECISIONS
				// D16 for why this reports instead of aborting the build.
				std::string sWorst;
				const int nWorst = WorstCharsToReach( Reg(), 8, &sWorst );
				console_log.infof( "worst chars-to-reach (top 8): %d  (%s)",
					nWorst, sWorst.empty() ? "-" : sWorst.c_str() );
				return;
			}

			if ( sVerb == "open" )
			{
				s_bPaletteOpen = true;
				s_sPaletteQuery = sArg;
				s_nPaletteSel = 0;
			}
			else if ( sVerb == "query" )
			{
				s_bPaletteOpen = true;
				s_sPaletteQuery = sArg;
				s_nPaletteSel = 0;
			}
			else if ( sVerb == "sel" )
			{
				s_nPaletteSel = (int)strtol( sArg.c_str(), nullptr, 10 );
			}
			else if ( sVerb == "adjust" || sVerb == "enter" )
			{
				const std::vector<PaletteItem> items = Build( Reg(), s_sPaletteQuery );
				if ( items.empty() )
				{
					console_log.errorf( "palette: nothing matches '%s'", s_sPaletteQuery.c_str() );
					return;
				}
				const int nSel = std::clamp( s_nPaletteSel, 0, (int)items.size() - 1 );
				const std::string sId = items[ (size_t)nSel ].sId;

				if ( sVerb == "enter" )
				{
					PaletteJump( sId );
					console_log.infof( "palette: jumped to %s", sId.c_str() );
					return;
				}

				const int nDir = sArg.empty() ? 1 : (int)strtol( sArg.c_str(), nullptr, 10 );
				bool bOk = false;
				if ( const Entry *pE = Reg().FindEntry( sId ) )
					bOk = AdjustValue( Adjustable::Of( *pE ), nDir < 0 ? -1 : 1, false );
				else if ( const Parameter *pP = Reg().FindParam( sId ) )
					bOk = AdjustValue( Adjustable::Of( *pP ), nDir < 0 ? -1 : 1, false );

				// Read back, same contract as overlay_e2_set: what the
				// binding holds afterwards is the answer.
				console_log.infof( "palette: %s %s -> %s", sId.c_str(),
					bOk ? "adjusted" : "unchanged", PaletteValueText( sId ).c_str() );
				return;
			}
			else if ( sVerb != "list" )
			{
				console_log.errorf( "palette: unknown verb '%s'", sVerb.c_str() );
				return;
			}

			const std::vector<PaletteItem> items = Build( Reg(), s_sPaletteQuery );
			console_log.infof( "palette: query '%s' -> %zu results (sel %d)",
				s_sPaletteQuery.c_str(), items.size(), s_nPaletteSel );
			for ( size_t i = 0; i < items.size() && i < 12; i++ )
			{
				const PaletteItem &it = items[ i ];
				console_log.infof( "  %s%2zu  %-38s %-30s %s%s",
					(int)i == s_nPaletteSel ? "> " : "  ", i,
					it.sPath.c_str(), it.sLabel.c_str(),
					PaletteValueText( it.sId ).c_str(),
					it.bParam ? "  [param]" : "" );
			}
		}

		// =================================================================
		//  Small drawing helpers
		// =================================================================
		ImRect Rc( const Rect &r ) { return ImRect( ImVec2( r.x0, r.y0 ), ImVec2( r.x1, r.y1 ) ); }

		ImFont *FontFor( TypeRole eRole )
		{
			// The kit's six type roles onto the atlas's five baked styles.
			// Not a perfect map -- Fonts.cpp bakes what the LEGACY design
			// guide needed -- and deliberately not fixed here: a sixth
			// baked style is an atlas change, which is a rebuild, which is
			// issue #51's territory. P3 does the typography pass; P2 needs
			// the regions to be right, and a role that lands one style off
			// costs half a point of size, not a layout.
			using gamescope::fonts::Style;
			switch ( eRole )
			{
				case TypeRole::Title:   return gamescope::fonts::Get( Style::Title );
				case TypeRole::Section: return gamescope::fonts::Get( Style::Meta );
				case TypeRole::Label:   return gamescope::fonts::Get( Style::Label );
				case TypeRole::Body:    return gamescope::fonts::Get( Style::Label );
				case TypeRole::Value:   return gamescope::fonts::Get( Style::Value );
				case TypeRole::Meta:    return gamescope::fonts::Get( Style::Meta );
				default:                return gamescope::fonts::Get( Style::Label );
			}
		}

		void Fill( const Rect &r, ImU32 col )
		{
			if ( !r.Empty() )
				ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( r.x0, r.y0 ), ImVec2( r.x1, r.y1 ), col );
		}

		// Every rule in the shell goes through here, so SPEC §8.3's
		// "max(1, floor(1 x scale))" is stated once.
		void HLine( float x0, float x1, float y, ImU32 col )
		{
			ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( x0, y ), ImVec2( x1, y + Hairline() ), col );
		}
		void VLine( float x, float y0, float y1, ImU32 col )
		{
			ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( x, y0 ), ImVec2( x + Hairline(), y1 ), col );
		}

		void Label( const Rect &rc, TypeRole eRole, ImU32 col, const char *pszText,
		            TextAlign eAlign = TextAlign::Left )
		{
			ImGui::PushFont( FontFor( eRole ) );
			DrawText( Rc( rc ), eRole, col, pszText, eAlign );
			ImGui::PopFont();
		}

		// D20.2. Which areas cannot be split into columns -- asked in exactly
		// one place so `shell.layout`'s printed column count and the sheet's
		// drawn one are the same number by construction. A printed value that
		// disagrees with the screen is the defect this whole change removes;
		// re-deriving the predicate at the second call site would put it
		// straight back.
		bool AreaIsUnsplittable( const Area *pArea )
		{
			return pArea && pArea->HasContent();
		}

		std::string FormatLadder()
		{
			const float flW = s_flSurfaceW.load( std::memory_order_relaxed );
			const float flH = s_flSurfaceH.load( std::memory_order_relaxed );
			if ( flW <= 0.0f || flH <= 0.0f )
				return "not drawn yet";

			const Slab slab = Slab::For( flW, flH, Scale() );
			const Area *pArea = SelectedArea();
			const LadderResult L = Solve( slab, Host(), pArea ? (int)pArea->EntryCount() : 0,
			                              AreaIsUnsplittable( pArea ) );

			char sz[ 160 ];
			snprintf( sz, sizeof( sz ), "rail %.0f · %s %.0f · sheet %.0f · %d col · step %d",
				L.flRailBase,
				L.eHost == InspectorHost::Column ? "column" :
				L.eHost == InspectorHost::Drawer ? "drawer" : "spine",
				L.eHost == InspectorHost::Hidden ? shelltok::kSpine : L.flInspectorBase,
				L.flSheetBase, L.nColumns, L.nStep );
			return sz;
		}

		// =================================================================
		//  Region 1 -- the rail (SPEC §8.1)
		// =================================================================
		const char *SectionName( Section eSection )
		{
			switch ( eSection )
			{
				case Section::Display: return "DISPLAY";
				case Section::System:  return "SYSTEM";
				case Section::Setup:   return "SETUP";
			}
			return "";
		}

		// `bIcons` is the LADDER's answer, never a threshold re-derived from
		// the width this function was handed. The width is animated (SPEC
		// §8.4's 160 ms region duration) and Approach() is an exponential,
		// so it never lands exactly on 60 -- asking `width <= 60` here left
		// the rail permanently one pixel too wide to count as collapsed,
		// which drew the full labels into a 60-wide strip and clipped them
		// to nothing. A presentation value must not make a semantic
		// decision; that is the general rule this signature encodes.
		void DrawRail( const Rect &rc, bool bIcons )
		{
			Fill( rc, Col( Role::SurfaceRail ) );
			VLine( rc.x1 - Hairline(), rc.y0, rc.y1, Col( Role::LineRegion ) );

			const float flItemH = Px( 40.0f );        // index.html's .ri
			const float flPadX  = Px( 16.0f );
			const float flSecH  = Px( 26.0f );

			// ---- the rail's vertical walk, defined ONCE ------------------
			//
			// WHY A WALK AND NOT A DRAW LOOP. The rail's height is fixed by
			// the slab; its content is not. At 2.0x eleven items plus three
			// section breaks are TALLER than the rail, and until this the
			// surplus was drawn past rc.y1 and simply lost: Appearance and
			// Shell fell off the bottom, unreachable by pointer. (The
			// palette still found them, which is why P4 did not notice.)
			// Scrolling needs the content height BEFORE the first item is
			// drawn, so the walk below is the single definition of where an
			// item sits and how tall the whole column is; the measure pass
			// and the draw pass call it with different visitors rather than
			// keeping two copies of the same arithmetic in step by hand.
			const float flSecAdvance = bIcons ? Px( 20.0f ) : ( flSecH + Px( tok::kXS ) );

			const auto Walk = [ & ]( auto &&fnSection, auto &&fnItem ) -> float
			{
				float   y            = rc.y0 + Px( tok::kS );
				Section eLastSection = Section::Setup;
				bool    bFirst       = true;

				for ( size_t i = 0; i < Reg().AreaCount(); ++i )
				{
					const Area &area = Reg().AreaAt( i );
					if ( !area.Available() )
						continue;

					if ( bFirst || area.GetSection() != eLastSection )
					{
						eLastSection = area.GetSection();
						bFirst = false;
						fnSection( area.GetSection(), y );
						y += flSecAdvance;
					}

					fnItem( i, area, y );
					y += flItemH;
				}
				return y;
			};

			const auto NoSection = []( Section, float ) {};
			const auto NoItem    = []( size_t, const Area &, float ) {};

			// Measure, then decide the scroll offset. Content height carries
			// the same top pad at the bottom so the last item does not sit
			// flush against the edge when the rail is scrolled fully down.
			const float flContentH  = ( Walk( NoSection, NoItem ) - rc.y0 ) + Px( tok::kS );
			const float flMaxScroll = std::max( 0.0f, flContentH - rc.Height() );

			// Keep the ACTIVE item on screen. StepArea() moves the selection
			// with Up/Down in the rail region, so following the selection is
			// what makes every area keyboard-reachable again; the wheel is
			// the pointer's equivalent and is the only reason a hover test
			// appears here.
			if ( flMaxScroll > 0.0f
			  && ImGui::IsMouseHoveringRect( ImVec2( rc.x0, rc.y0 ), ImVec2( rc.x1, rc.y1 ), false ) )
				s_flRailScroll -= ImGui::GetIO().MouseWheel * flItemH;

			float flActiveTop = -1.0f;
			Walk( NoSection, [ & ]( size_t, const Area &area, float y ) {
				if ( &area == SelectedArea() )
					flActiveTop = y - rc.y0;      // RailScroll() works rail-relative
			} );

			s_flRailScroll = RailScroll( s_flRailScroll, flContentH, rc.Height(),
			                             flActiveTop, flItemH, Px( tok::kS ) );

			// Clip so an off-screen item is neither painted over the sheet
			// nor clickable -- ImGui culls an InvisibleButton outside the
			// window clip rect, which is exactly the wanted hit-test.
			ImGui::PushClipRect( ImVec2( rc.x0, rc.y0 ), ImVec2( rc.x1, rc.y1 ), true );

			Walk(
				[ & ]( Section eSection, float yRaw )
				{
					const float y = yRaw - s_flRailScroll;
					if ( bIcons )
					{
						// The icon rail keeps the section BREAK but drops
						// the word: a divider rule, not a heading. SPEC
						// §8.0's collapse is about width, and a heading is
						// the one thing that cannot survive it.
						HLine( rc.x0 + Px( tok::kM ), rc.x1 - Px( tok::kM ), y + Px( 10.0f ), Col( Role::Line ) );
					}
					else
					{
						Label( { rc.x0 + flPadX, y + Px( tok::kM ), rc.x1, y + flSecH },
						       TypeRole::Section, Col( Role::TextMeta ), SectionName( eSection ) );
					}
				},
				[ & ]( size_t i, const Area &area, float yRaw )
			{
				const float y = yRaw - s_flRailScroll;
				const Rect rcItem { rc.x0, y, rc.x1, y + flItemH };
				const bool bActive = ( &area == SelectedArea() );

				ImGui::SetCursorScreenPos( ImVec2( rcItem.x0, rcItem.y0 ) );
				ImGui::PushID( (int)i );
				if ( ImGui::InvisibleButton( "##railitem", ImVec2( rcItem.Width(), rcItem.Height() ) ) )
				{
					s_sSelectedArea = area.Id();
					Select( nullptr );          // a new category starts at Overview
					s_eFocusRegion = Region::Sheet;
				}
				const bool bHovered = ImGui::IsItemHovered();
				ImGui::PopID();

				if ( bActive )
				{
					Fill( rcItem, Accent( 0.10f ) );
					// The 2px accent left edge "survives the icon collapse"
					// (SPEC §8.1) -- so it is drawn from the item's rect,
					// which both rail widths share, and never from a
					// per-mode constant.
					Fill( { rcItem.x0, rcItem.y0, rcItem.x0 + Px( 2.0f ), rcItem.y1 }, Col( Role::AccentBase ) );
				}
				else if ( bHovered )
				{
					Fill( rcItem, IM_COL32( 255, 255, 255, 13 ) );
				}

				// SPEC §8.0's icon set (D20.1). The rail drew the area
				// title's INITIAL until now, which the pre-P5 shell test
				// found unusable the moment the ladder collapses the rail:
				// with the label gone, Mixer/Monitor are both `M`,
				// Profiles/Per-game both `P` and Shaders/Shell both `S`, so
				// three of eleven areas were unidentifiable at 1.5x and
				// above -- in the one state where the mark carries the
				// entire meaning of the item.
				//
				// The icon inherits the item's colour rather than owning
				// one: "TextLabel at rest, AccentIcon when the item is
				// active, so the icon is one of the accent's state jobs
				// (§7.7) and not decoration".
				const ImU32 colIcon = bActive ? Col( Role::AccentIcon ) : Col( Role::TextLabel );
				const float flIcon  = Px( tok::kIconBox );
				const Icon *pIcon   = IconFor( area.Id().c_str() );

				// An area with no glyph falls back to its initial -- the old
				// behaviour, for that one item. See Icons.h: a forgotten
				// icon must not remove an area from the rail.
				const char szInitial[ 2 ] = { area.Title().empty() ? '?' : area.Title()[ 0 ], '\0' };

				const auto DrawMark = [ & ]( const Rect &rcBox ) {
					if ( pIcon )
						glyph::RailIcon( *pIcon,
							ImVec2( ( rcBox.x0 + rcBox.x1 ) * 0.5f, ( rcBox.y0 + rcBox.y1 ) * 0.5f ),
							flIcon, colIcon );
					else
						Label( rcBox, TypeRole::Title, colIcon, szInitial, TextAlign::Center );
				};

				if ( bIcons )
				{
					DrawMark( { rcItem.x0, rcItem.y0, rcItem.x1, rcItem.y1 } );
				}
				else
				{
					DrawMark( { rcItem.x0 + flPadX, rcItem.y0, rcItem.x0 + flPadX + flIcon, rcItem.y1 } );
					Label( { rcItem.x0 + flPadX + flIcon + Px( tok::kM ), rcItem.y0, rcItem.x1 - Px( tok::kM ), rcItem.y1 },
					       TypeRole::Label, bActive ? Col( Role::TextPrimary ) : Col( Role::TextLabel ),
					       area.Title().c_str() );
				}
			} );

			ImGui::PopClipRect();

			// A scrollable rail says so: a thumb proportional to the visible
			// fraction, on the rail's own divider line. Without it the only
			// cue that Shell exists below the fold is pressing Down.
			if ( flMaxScroll > 0.0f )
			{
				const float flTrackX = rc.x1 - Px( 3.0f );
				const float flFrac   = rc.Height() / flContentH;
				const float flThumbH = std::max( Px( 24.0f ), rc.Height() * flFrac );
				const float flThumbY = rc.y0 + ( rc.Height() - flThumbH )
				                     * ( s_flRailScroll / flMaxScroll );
				Fill( { flTrackX, flThumbY, flTrackX + Px( 2.0f ), flThumbY + flThumbH },
				      Accent( 0.45f ) );
			}
		}

		// =================================================================
		//  Region 2 -- the sheet (SPEC §8.1)
		// =================================================================
		void DrawSheetHead( const Rect &rc, const Area *pArea, bool bInspectorHidden )
		{
			HLine( rc.x0, rc.x1, rc.y1 - Hairline(), Col( Role::LineRegion ) );

			char szCrumb[ 160 ];
			// D18: SPEC §6.3 asks the explanation page for "a back crumb".
			// It is the same crumb with one more segment, not a second
			// header -- so the page reads as somewhere you navigated TO, and
			// the way back is legible from the screen.
			//
			// D26: the crumb used to advertise `Esc back`, and Esc now closes
			// the overlay instead of unwinding a level. The key that opened
			// the page is a TOGGLE, so it is also the key that leaves it --
			// which is what the crumb names now. A crumb that still said
			// "Esc" would be a label promising the one thing Esc no longer
			// does, on the screen where that is most expensive.
			const Entry *pExplained = s_bExplainPage ? SelectedEntry() : nullptr;
			if ( pExplained )
				snprintf( szCrumb, sizeof( szCrumb ), "%s  /  %s  /  %s   -   ^/ back",
					pArea ? SectionName( pArea->GetSection() ) : "",
					pArea ? pArea->Title().c_str() : "",
					pExplained->Title().c_str() );
			else
				snprintf( szCrumb, sizeof( szCrumb ), "%s  /  %s",
					pArea ? SectionName( pArea->GetSection() ) : "",
					pArea ? pArea->Title().c_str() : "" );
			Label( { rc.x0 + Px( tok::kSheetPad ), rc.y0, rc.x1 - Px( tok::kSheetPad ), rc.y1 },
			       TypeRole::Section, Col( Role::TextPrimary ), szCrumb );

			if ( pArea )
			{
				// SPEC §8.1's header, and §1.1's D9 amendment naming exactly
				// what may live here: "the breadcrumb, the `differs N` chip,
				// and the `inspector hidden` chip -- never a live line-count,
				// a row-height count, a column count, a ladder-step number,
				// or a raw pixel-budget readout". The breadcrumb and the
				// layer badge shipped; the two chips did not, so a sheet
				// could not answer "have I changed anything here" without
				// scanning every row's state edge, and a hidden Inspector
				// announced itself only by the spine.
				//
				// `differs` is counted here from the area's own entries and
				// their params, on the rail counter's rule (SPEC §8.1: "the
				// counter means exactly one thing" -- a number nobody types
				// is a number that cannot lie about what it counts).
				int nDiffers = 0;
				for ( size_t i = 0; i < pArea->EntryCount(); ++i )
				{
					const Entry &e = pArea->EntryAt( i );
					if ( e.HasDefault() && !e.IsAtDefault() )
						nDiffers++;
					for ( size_t p = 0; p < e.ParamCount(); ++p )
					{
						const Parameter &pa = e.ParamAt( p );
						if ( pa.HasDefault() && !pa.IsAtDefault() )
							nDiffers++;
					}
				}

				std::string sChips;
				if ( bInspectorHidden )
					sChips = "inspector hidden";
				if ( nDiffers )
				{
					if ( !sChips.empty() )
						sChips += "   ";
					sChips += "differs " + std::to_string( nDiffers );
				}

				// Two labels, not one string, because the badge is the
				// area's own text and the chips are the shell's -- and the
				// badge keeps the right edge it has always had.
				const std::string sBadge = pArea->BadgeText();
				float flRight = rc.x1 - Px( tok::kSheetPad );
				if ( !sBadge.empty() )
				{
					Label( { rc.x0, rc.y0, flRight, rc.y1 },
					       TypeRole::Meta, Col( Role::TextMeta ), sBadge.c_str(), TextAlign::Right );
					flRight -= MeasureText( TypeRole::Meta, sBadge.c_str() ).x + Px( tok::kL );
				}
				if ( !sChips.empty() )
					Label( { rc.x0, rc.y0, flRight, rc.y1 },
					       TypeRole::Meta, Col( Role::TextMeta ), sChips.c_str(), TextAlign::Right );
			}
		}

		void DrawSheetFoot( const Rect &rc )
		{
			HLine( rc.x0, rc.x1, rc.y0, Col( Role::LineRegion ) );

			const Rect  rcText  = { rc.x0 + Px( tok::kSheetPad ), rc.y0,
			                        rc.x1 - Px( tok::kSheetPad ), rc.y1 };
			const float flAvail = rcText.x1 - rcText.x0;

			// The legend is the only place the shell advertises its own
			// shortcuts, and at 2.0x the full form does not fit the sheet --
			// so DrawText's left-align fallback clipped the TAIL, which is
			// where `Esc close` is. That is the worst possible thing to lose:
			// a user who cannot read the rest of the line is precisely the
			// user who needs to know how to get out of it.
			//
			// So the line drops hints from the LEFT instead, cheapest first,
			// and `Esc close` is the last thing standing. Chosen by
			// measurement rather than by scale, because the sheet's width
			// depends on the Inspector's host and the drawer as well as on
			// the ladder step.
			static const char *const kForms[] = {
				"^K  search      ^I  inspector      ^/  explain      Tab  region      Esc  close",
				"^K search    ^I inspector    ^/ explain    Tab region    Esc close",
				"^K search    ^I inspector    Tab region    Esc close",
				"^K search    Tab region    Esc close",
				"^K search    Esc close",
				"Esc close",
			};

			// Falls back to the shortest form, which is also the one that
			// fits every width this shell can produce.
			const char *pszLegend = kForms[ IM_ARRAYSIZE( kForms ) - 1 ];
			for ( const char *pszForm : kForms )
			{
				if ( MeasureText( TypeRole::Meta, pszForm ).x <= flAvail )
				{
					pszLegend = pszForm;
					break;
				}
			}

			Label( rcText, TypeRole::Meta, Col( Role::TextMeta ), pszLegend );
		}

		// =================================================================
		//  The row painter -- SPEC §2.2's grammar, one implementation
		// =================================================================
		// SPEC §3.13's disabled treatment ("row x 0.55") is applied with
		// ui::ScopedDim, which sits under Col()/Accent() in Colors.cpp rather
		// than at the call sites here -- see Colors.h for why the control
		// atoms cannot be dimmed any other way.

		// The value string for a row, with SPEC §2.3's two modifiers applied
		// in the order the spec states them: ZeroMeans replaces the whole
		// string (0 is a WORD -- "Unlimited", not "0 fps"), and the unit is
		// appended, never baked into the value by a call site. index.html's
		// rule, verbatim: "units are declared with `u`, never baked into the
		// value string."
		//
		// Templated over Entry and Parameter deliberately. They are distinct
		// TYPES on purpose -- that is what makes "a Param owning a Param"
		// unsayable (Registry.h) -- but SPEC §5.3 requires a Param to render
		// identically to the Entry it could be promoted into. One function
		// over both is what guarantees that; two functions would be two things
		// to keep in step, and they would drift.
		template <typename TDecl>
		std::string FormatDeclValue( const TDecl &decl )
		{
			if ( !decl.Binding().IsBound() )
				return {};
			const Value v = decl.Binding().Get();

			if ( !decl.ZeroWord().empty() )
			{
				const bool bZero =
					( std::holds_alternative<int>( v )   && std::get<int>( v ) == 0 ) ||
					( std::holds_alternative<float>( v ) && std::get<float>( v ) == 0.0f );
				if ( bZero )
					return decl.ZeroWord();
			}

			std::string s = ValueToString( v );
			if ( !decl.Unit().empty() )
				s += " " + decl.Unit();
			return s;
		}

		// The four kinds an Entry and a Parameter both have. Entry-only kinds
		// (Action, Facts, Meter, Composite) stay in DrawEntryRow, because a
		// Parameter cannot be one and the type system should keep saying so.
		//
		// A Slider's INT-vs-FLOAT form is decided by what the binding actually
		// holds, never by a second declaration -- the same rule that makes the
		// slider handle come from SliderBehavior() rather than a constant
		// (Controls.h). A caller cannot declare an int slider and bind a float.
		template <typename TDecl>
		bool DrawSharedControl( const TDecl &decl, const RowCtx &row, const char *pszId,
		                        const std::string &sPopupKey, int nBankChip = -1 )
		{
			if ( !decl.Binding().IsBound() )
				return false;

			const Value v = decl.Binding().Get();
			switch ( decl.GetKind() )
			{
				case Kind::Switch:
				{
					bool b = std::holds_alternative<bool>( v ) && std::get<bool>( v );
					if ( controls::Switch( row, pszId, &b ) )
					{
						decl.Binding().Set( Value{ b } );
						return true;
					}
					return false;
				}
				case Kind::Slider:
				{
					if ( std::holds_alternative<int>( v ) )
					{
						int n = std::get<int>( v );
						const int nDef = std::holds_alternative<int>( decl.DefaultValue() )
							? std::get<int>( decl.DefaultValue() ) : 0;
						if ( controls::SliderInt( row, pszId, &n, (int)decl.Lo(), (int)decl.Hi(),
							nDef, std::holds_alternative<int>( decl.DefaultValue() ) ) )
						{
							decl.Binding().Set( Value{ n } );
							return true;
						}
						return false;
					}
					float f = std::holds_alternative<float>( v ) ? std::get<float>( v ) : 0.0f;
					const bool bHasDef = std::holds_alternative<float>( decl.DefaultValue() );
					const float flDef = bHasDef ? std::get<float>( decl.DefaultValue() ) : 0.0f;
					if ( controls::Slider( row, pszId, &f, decl.Lo(), decl.Hi(), flDef, bHasDef ) )
					{
						decl.Binding().Set( Value{ f } );
						return true;
					}
					return false;
				}
				case Kind::Stepper:
				{
					int n = std::holds_alternative<int>( v ) ? std::get<int>( v ) : 0;
					const int nStep = decl.StepSize() > 0.0f ? (int)decl.StepSize() : 1;
					if ( controls::Stepper( row, pszId, &n, (int)decl.Lo(), (int)decl.Hi(), nStep ) )
					{
						decl.Binding().Set( Value{ n } );
						return true;
					}
					return false;
				}
				case Kind::Choice:
				{
					int n = std::holds_alternative<int>( v ) ? std::get<int>( v ) : 0;
					const bool bOpen = ( s_sOpenDropdown == sPopupKey );
					const controls::ChoiceResult res = controls::Choice(
						row, pszId, &n, decl.Options().data(), decl.Options().size(), bOpen );

					if ( res.bChanged )
					{
						decl.Binding().Set( Value{ n } );
						return true;
					}
					if ( !res.bSegmented )
					{
						// This row IS a dropdown this frame -- recorded so
						// the keyboard can tell, next frame, whether Enter
						// here should open a popup. See s_DropdownRows.
						s_DropdownRowsBuild.push_back( sPopupKey );

						if ( res.bWantsPopup )
						{
							s_sOpenDropdown = sPopupKey;
							s_nPopupFocus = -1;
						}

						// D18: the OPEN dropdown's list is not drawn here.
						// This records where it belongs; DrawDropdownList()
						// paints it after the slab window has closed.
						//
						// WHY IT MOVED. This used to be an ImGui popup
						// (OpenPopup / BeginPopup) and the list NEVER
						// APPEARED -- the caret lit up, the state was right,
						// and nothing was on screen. ImGui closes a popup
						// whose parent is not the focused window, and the
						// slab carries ImGuiWindowFlags_NoBringToFrontOnFocus
						// precisely so it can never come forward, so the
						// popup was opened and closed again every frame.
						// That is the SAME trap the palette hit and
						// documented (see DrawPalette's call site) -- and it
						// went unnoticed here because opening a dropdown
						// needed a click, which this project is forbidden to
						// synthesise, so no test and no screenshot had ever
						// opened one.
						if ( s_sOpenDropdown == sPopupKey )
						{
							const ImRect rcRow = row.Bounds();
							s_rcDropdownAnchor = { rcRow.Min.x, rcRow.Min.y,
							                       rcRow.Max.x, rcRow.Max.y };
							s_pDropdownOptions = &decl.Options();
							s_nDropdownValue   = n;
						}
					}
					return false;
				}
				case Kind::Bank:
				{
					// The mask travels as an int because Value has no
					// unsigned alternative; the atom wants uint32_t. One
					// conversion each way, in the one place that knows both.
					uint32_t nMask = (uint32_t)( std::holds_alternative<int>( v ) ? std::get<int>( v ) : 0 );
					if ( controls::Bank( row, pszId, &nMask,
						decl.Options().data(), decl.Options().size(), nBankChip ) )
					{
						decl.Binding().Set( Value{ (int)nMask } );
						return true;
					}
					return false;
				}
				case Kind::Text:
				{
					std::string s = std::holds_alternative<std::string>( v )
						? std::get<std::string>( v ) : std::string();
					bool bEditing = ( s_sEditingText == sPopupKey );
					if ( controls::Text( row, pszId, &s, &bEditing ) )
					{
						decl.Binding().Set( Value{ s } );
						// Fall through to the state sync below rather than
						// returning early: the atom can commit a value and
						// close the field on the same frame.
					}
					if ( bEditing )
						s_sEditingText = sPopupKey;
					else if ( s_sEditingText == sPopupKey )
						s_sEditingText.clear();
					return false;
				}
				default:
					return false;
			}
		}

		// SPEC §1's state-edge slot: "2px, at x=0 -- Accent / Accent@45% /
		// nothing". One slot, three values, and this function is the only
		// place that decides which.
		//
		// THIS IS D6's OTHER HALF. D6 chose exactly ONE encoding of "differs
		// from default" on the sheet -- this edge -- and deleted the other
		// two: the accent-recoloured value (which competed with the accent's
		// live/active job) and the reset dot (which DISPLACED the depth
		// chevron, so a row that both differed and had depth stopped
		// advertising its depth). The reset action moved to the Inspector,
		// and was built in P3b; the edge was not, which left the decision
		// half-implemented -- the sheet could not show which rows had
		// anything to reset, so "what have I changed here" was unanswerable
		// without opening every row in turn. The edge is the only one of the
		// three encodings that SCANS VERTICALLY, which is the whole reason
		// D6 kept it.
		//
		// Selection outranks differs because the two share the slot. A
		// selected row that also differs reads as selected, and the
		// Inspector it just opened is already showing the reset link.
		//
		// TEMPLATED over the declaration type (D20.3) so a Parameter drawn
		// inline in the Sheet gets the same edge from the same rule. D6's
		// amendment requires precisely that -- "the same rule now applies
		// uniformly to Params rendered inline in the Sheet (§6.3): they
		// gained the accent edge they never had" -- and a second copy of two
		// lines is exactly how two encodings of one state come back.
		template <typename TDecl>
		ImU32 StateEdgeColor( const TDecl &decl, bool bSelected )
		{
			if ( bSelected )
				return Col( Role::AccentBase );
			if ( decl.HasDefault() && !decl.IsAtDefault() )
				return Accent( 0.45f );
			return 0;
		}

		// =================================================================
		//  Composites -- SPEC §4.2's band
		// =================================================================
		// How many 44-tall lines a declaration occupies. ONE for every row in
		// the taxonomy; n for a Composite, and that n comes from Band.cpp's
		// kSpecs table rather than from anything a call site said (SPEC §4.2
		// clause 1: "n x 44, n in {2,3}. Nothing else.").
		//
		// Every place that advances a y cursor past a declaration goes
		// through this, so the sheet and the Inspector cannot disagree about
		// how tall a band is.
		int LinesFor( const Entry &entry )
		{
			if ( entry.GetKind() != Kind::Composite )
				return 1;
			return ImClamp( Band( entry.GetCompositeKind() ).nLines,
			                tok::kBandMinLines, tok::kBandMaxLines );
		}

		// The nine anchors, named. A lookup and not a format string, so the
		// value column cannot be told a corner the grid cannot express.
		const char *AnchorName( int nVert, int nHoriz )
		{
			static const char *const kNames[ 3 ][ 3 ] = {
				{ "top-left",    "top",    "top-right"    },
				{ "left",        "centre", "right"        },
				{ "bottom-left", "bottom", "bottom-right" },
			};
			return kNames[ ImClamp( nVert, 0, 2 ) ][ ImClamp( nHoriz, 0, 2 ) ];
		}

		int AsInt( const Value &v )
		{
			if ( std::holds_alternative<int>( v ) )   return std::get<int>( v );
			if ( std::holds_alternative<float>( v ) ) return (int)std::get<float>( v );
			return 0;
		}

		float AsFloat( const Value &v )
		{
			if ( std::holds_alternative<float>( v ) ) return std::get<float>( v );
			if ( std::holds_alternative<int>( v ) )   return (float)std::get<int>( v );
			return 0.0f;
		}

		// SPEC §4.2 clause 2: line 1 carries the composite's RESOLVED VALUE
		// in the value column -- "the value column already tells you they are
		// 32 / 32 without showing two steppers, which is the whole calming
		// move in miniature" (§4.3).
		// A Meter's value comes from its SCALAR, not a binding -- it has no
		// binding at all (Registry.h: the kind is read-only and Area::Meter()
		// takes a std::function<double()>). Both the sheet's value column and
		// the palette need the same string, so it is computed once here.
		//
		// This existing as a function is the fix for the third instance of
		// one bug: PaletteValueText() special-cased Composite and Facts and
		// fell through to the binding for everything else, so a Meter --
		// bindingless -- printed EMPTY in the palette while the sheet showed
		// a number. Exactly what the composite rows did before D19.7 printed
		// the raw axis-A integer. A kind whose value is not in its binding
		// has to be taught to every formatter, so there is now one formatter
		// to teach.
		std::string MeterValue( const Entry &entry )
		{
			char sz[ 32 ];
			snprintf( sz, sizeof( sz ), "%.0f%s", entry.Scalar(),
				entry.Unit().empty() ? "" : entry.Unit().c_str() );
			return sz;
		}

		std::string CompositeValue( const Entry &entry )
		{
			switch ( entry.GetCompositeKind() )
			{
				case CompositeKind::Anchor:
				{
					std::string s = AnchorName( AsInt( entry.Binding().Get() ),
					                            AsInt( entry.BindingB().Get() ) );
					// The margins are Params (§4.3), so they are read from
					// the params themselves -- there is no second copy of
					// them for the value line to drift away from.
					if ( entry.ParamCount() >= 2 )
					{
						s += "  ·  " + ValueToString( entry.ParamAt( 0 ).Binding().Get() )
						   + " / "   + ValueToString( entry.ParamAt( 1 ).Binding().Get() );
					}
					return s;
				}
				case CompositeKind::Hue:
				{
					char sz[ 16 ];
					snprintf( sz, sizeof( sz ), "%.0f°", AsFloat( entry.Binding().Get() ) );
					return sz;
				}
				case CompositeKind::Color:
				{
					char sz[ 16 ];
					snprintf( sz, sizeof( sz ), "#%06X", AsInt( entry.Binding().Get() ) & 0xFFFFFF );
					return sz;
				}
				case CompositeKind::Graph:
				case CompositeKind::Strip:
					return entry.SummaryText();
			}
			return {};
		}

		// One composite band. Structurally the same function as DrawEntryRow
		// -- same selection fill, same hairline, same label/value split, same
		// affordance column -- differing only in that its bounds are n rows
		// tall and its control is a body rather than an atom. That is clause
		// 2 made literal: "scanning the sheet, a composite is
		// indistinguishable from a row until your eye reaches the control
		// column."
		// SPEC §2.4's affordance column: 28 base units holding AT MOST ONE
		// glyph, by a fixed priority -- chevron for depth, lock for read-only,
		// otherwise nothing.
		//
		// `RowCtx::Affordance()` shipped in P1 and had NO CALL SITES through
		// P4 (recorded as still-open in D18), so the column was allocated on
		// every row of the product and never painted: a sheet gave no sign
		// whatever that a row had parameters or a Details page behind it. With
		// the Inspector hidden that is the whole advertisement of depth, and
		// it is why the mockup's `▸` was never needed in the C++.
		//
		// The priority is a chain of returns rather than three independent
		// tests, so "never two" is structural (SPEC §2.4's "first match wins").
		void DrawAffordance( const Entry &entry, const RowCtx &row )
		{
			const ImRect rc = row.Affordance();
			const ImVec2 vC = rc.GetCenter();
			const float  flSize = Px( 11.0f );

			if ( entry.ParamCount() > 0 || entry.GetKind() == Kind::Facts )
			{
				// D20.3. SPEC §6.3 marks an inline-expandable row "with a `>`
				// disclosure in place of the chevron". Both marks are the same
				// drawn chevron (D18), so the disclosure is expressed by its
				// DIRECTION: right while collapsed, down while open -- which
				// is the one convention a disclosure triangle has ever had,
				// and it means the mark answers "is this row open?" rather
				// than only "does this row have depth?".
				const bool bInline = InlineMode() && entry.ParamCount() > 0;
				const bool bOpen   = bInline && s_sExpandedEntry == entry.Id();

				// SPEC §6.3's "click" route. The chevron gets its own hit box
				// ONLY in inline mode, where it is a real control; everywhere
				// else it stays what §2.4 describes, a mark that advertises
				// depth. Submitted after the row's own button so it wins the
				// overlap.
				//
				// D22 correction: this used to claim "ImGui resolves a hover
				// to the last item added", and that is simply not true --
				// ItemHoverable() rejects a later item while an earlier one
				// still holds g.HoveredId. Submitting later is NECESSARY but
				// not SUFFICIENT; the earlier item must also have been marked
				// SetNextItemAllowOverlap(), which DrawEntryRow now does. That
				// wrong belief is why every overlapped control in this kit was
				// unreachable by mouse, so it is corrected here rather than
				// quietly deleted.
				if ( bInline )
				{
					ImGui::SetCursorScreenPos( rc.Min );
					if ( ImGui::InvisibleButton( "##disc", rc.GetSize() ) )
					{
						if ( bOpen )
						{
							s_sExpandedEntry.clear();
							s_nInlineFocus = -1;
						}
						else
						{
							Select( &entry );
							s_sExpandedEntry = entry.Id();
							s_eFocusRegion   = Region::Sheet;
						}
					}
				}

				glyph::Chevron( vC, flSize, bOpen ? glyph::Dir::Down : glyph::Dir::Right,
				                Col( Role::TextMeta ) );
				return;
			}
			if ( entry.ReadOnly() )
				glyph::Lock( vC, flSize, Col( Role::TextMeta ) );
		}

		bool DrawCompositeBand( const Entry &entry, const Lane &laneBase, float flOriginPx,
		                        float flTopPx, bool bSelected, bool bAffordance )
		{
			const BandLayout bl = LayOutBand( laneBase, flOriginPx, flTopPx, entry.GetCompositeKind() );
			const ImRect rcBand = bl.rcBand;

			ImGui::SetCursorScreenPos( rcBand.Min );
			ImGui::PushID( entry.Id().c_str() );

			// The band's own hit box covers line 1 ONLY. The body owns the
			// rest, and a click there must reach the grid rather than being
			// swallowed by a full-band selector drawn on top of it.
			const ImRect rcHit( rcBand.Min.x, rcBand.Min.y, bl.rcBody.Min.x, bl.line1.Bounds().Max.y );
			ImGui::SetCursorScreenPos( rcHit.Min );
			// D22, same reason as the row's own selector -- see DrawEntryRow.
			// Line 1 of a composite carries its value readout and, for some
			// composites, an affordance drawn after this button.
			ImGui::SetNextItemAllowOverlap();
			const bool bClicked = ImGui::InvisibleButton( "##band", rcHit.GetSize() );
			const bool bHovered = ImGui::IsItemHovered();

			if ( bSelected )
				Fill( { rcBand.Min.x, rcBand.Min.y, rcBand.Max.x, rcBand.Max.y }, Accent( 0.08f ) );
			else if ( bHovered )
				Fill( { rcBand.Min.x, rcBand.Min.y, rcBand.Max.x, rcBand.Max.y },
				      IM_COL32( 255, 255, 255, 10 ) );

			// Clause 1's consequence: the state edge spans the WHOLE band,
			// not just its first line, or a selected composite would look
			// like a selected row with n-1 unselected ones stacked under it.
			if ( const ImU32 colEdge = StateEdgeColor( entry, bSelected ) )
			{
				const ImRect rcEdge = bl.line1.StateEdge();
				Fill( { rcEdge.Min.x, rcBand.Min.y, rcEdge.Max.x, rcBand.Max.y }, colEdge );
			}
			HLine( rcBand.Min.x, rcBand.Max.x, rcBand.Max.y - Hairline(), Col( Role::Line ) );

			const bool bDisabled = !entry.DisabledReason().empty();
			const ScopedDim dim( bDisabled );

			// Clause 2: the label and the resolved value land in the SHEET's
			// own columns, through line 1's ordinary RowCtx.
			const std::string sValue = CompositeValue( entry );
			ImRect rcLabel, rcValue;
			const float flValueW = !sValue.empty()
				? MeasureText( TypeRole::Value, sValue.c_str() ).x : 0.0f;
			bl.line1.SplitLabelZone( flValueW, &rcLabel, &rcValue );

			Label( { rcLabel.Min.x, rcLabel.Min.y, rcLabel.Max.x, rcLabel.Max.y },
			       TypeRole::Label,
			       bSelected ? Col( Role::TextPrimary ) : Col( Role::TextLabel ),
			       entry.Title().c_str() );
			if ( flValueW > 0.0f )
				Label( { rcValue.Min.x, rcValue.Min.y, rcValue.Max.x, rcValue.Max.y },
				       TypeRole::Value, Col( Role::TextPrimary ), sValue.c_str(), TextAlign::Right );

			// Clause 4 needs nothing here: the only rect this function draws
			// into below line 1 is bl.rcBody, which Band.cpp right-bound. The
			// label column of lines 2..n is air because nothing allocates it.
			if ( bDisabled )
				ImGui::BeginDisabled();

			switch ( entry.GetCompositeKind() )
			{
				case CompositeKind::Anchor:
				{
					int nV = AsInt( entry.Binding().Get() );
					int nH = AsInt( entry.BindingB().Get() );
					if ( controls::AnchorGrid( bl.rcBody, "grid", &nV, &nH ) )
					{
						entry.Binding().Set( Value{ nV } );
						entry.BindingB().Set( Value{ nH } );
					}
					break;
				}
				case CompositeKind::Hue:
				{
					float flHue = AsFloat( entry.Binding().Get() );
					if ( controls::HueBody( bl.rcBody, "hue", &flHue ) )
						entry.Binding().Set( Value{ flHue } );
					break;
				}
				case CompositeKind::Color:
				{
					// The stored value is, and stays, a packed 0xRRGGBB int
					// -- the control edits OKLCH and converts back on every
					// edit, so no config format changed for this row to
					// exist.
					const int nPacked = AsInt( entry.Binding().Get() );
					float flL = 0.0f, flC = 0.0f, flH = 0.0f;
					palette::ImU32ToOklch( IM_COL32( ( nPacked >> 16 ) & 0xFF,
					                                 ( nPacked >> 8 ) & 0xFF,
					                                 nPacked & 0xFF, 255 ), &flL, &flC, &flH );
					if ( controls::ColorBody( bl.rcBody, "col", &flL, &flC, &flH ) )
					{
						const ImU32 col = palette::OklchToImU32( flL, flC, flH );
						entry.Binding().Set( Value{ (int)(
							( (int)( ( col >> IM_COL32_R_SHIFT ) & 0xFF ) << 16 ) |
							( (int)( ( col >> IM_COL32_G_SHIFT ) & 0xFF ) << 8 ) |
							  (int)( ( col >> IM_COL32_B_SHIFT ) & 0xFF ) ) } );
					}
					break;
				}
				case CompositeKind::Graph:
				{
					const SampleWindow win = entry.SampleData();
					controls::GraphBody( bl.rcBody, win.pflSamples, win.nCount,
					                     win.flCeiling, win.flOutlier, win.nAxisSlots );
					break;
				}
				case CompositeKind::Strip:
					// No call site registers a Strip: the audio fader stayed
					// a Slider row in P3b. Deliberately draws nothing rather
					// than inventing a body no declaration asks for -- a
					// control that renders and does nothing is #25 and #68.
					break;
			}

			if ( bDisabled )
				ImGui::EndDisabled();

			// Clause 2: "line 1 reads as a row" -- including its affordance,
			// which is how a Graph band advertises that it is read-only and
			// how the Anchor advertises its two margins.
			if ( bAffordance )
				DrawAffordance( entry, bl.line1 );

			ImGui::PopID();
			return bClicked;
		}

		// One Inspector/sheet row of the Row grammar.
		bool DrawEntryRow( const Entry &entry, const Lane &laneBase, float flOriginPx, float flTopPx,
		                   bool bSelected, bool bAffordance = true )
		{
			// A composite is NOT a row (SPEC §4.2), and this is the one place
			// that says so. Everything downstream -- selection, the keyboard,
			// Configure -- goes on treating it as one declaration.
			if ( entry.GetKind() == Kind::Composite )
				return DrawCompositeBand( entry, laneBase, flOriginPx, flTopPx, bSelected, bAffordance );

			const RowCtx row = RowCtx::ForRow( laneBase, flOriginPx, flTopPx );
			const ImRect rcRow = row.Bounds();

			ImGui::SetCursorScreenPos( rcRow.Min );
			ImGui::PushID( entry.Id().c_str() );
			// D22: THE ROW'S SELECTOR MUST ALLOW OVERLAP, or it eats every
			// control in the row.
			//
			// This full-width button is submitted FIRST and spans the whole
			// row, including the lane the switch/slider/segmented atom is
			// drawn into afterwards. ImGui does not resolve an overlap to
			// "the last item added" -- ItemHoverable() rejects a later item
			// outright while g.HoveredId is already held by an earlier one
			// (imgui.cpp: `if (g.HoveredId != 0 && g.HoveredId != id &&
			// !g.HoveredIdAllowOverlap) return false;`), and rejects it again
			// on g.ActiveId while a button is held. So without this call the
			// row's own button won the hit test for the ENTIRE row and no
			// atom inside it could ever be hovered, pressed or dragged: the
			// controls painted perfectly and were inert to the mouse, which
			// is exactly what shipped.
			//
			// SetNextItemAllowOverlap() sets HoveredIdAllowOverlap/
			// ActiveIdAllowOverlap for this item, which lets the atoms
			// submitted after it take the hit test where they cover the row,
			// while the row still takes it everywhere they do not -- so
			// clicking the label still selects the row.
			ImGui::SetNextItemAllowOverlap();
			const bool bClicked = ImGui::InvisibleButton( "##row", rcRow.GetSize() );
			const bool bHovered = ImGui::IsItemHovered();

			if ( bSelected )
				Fill( { rcRow.Min.x, rcRow.Min.y, rcRow.Max.x, rcRow.Max.y }, Accent( 0.08f ) );
			else if ( bHovered )
				Fill( { rcRow.Min.x, rcRow.Min.y, rcRow.Max.x, rcRow.Max.y }, IM_COL32( 255, 255, 255, 10 ) );

			if ( const ImU32 colEdge = StateEdgeColor( entry, bSelected ) )
			{
				const ImRect rcEdge = row.StateEdge();
				Fill( { rcEdge.Min.x, rcEdge.Min.y, rcEdge.Max.x, rcEdge.Max.y }, colEdge );
			}
			HLine( rcRow.Min.x, rcRow.Max.x, rcRow.Max.y - Hairline(), Col( Role::Line ) );

			// SPEC §3.13: a disabled row draws at 0.55 AND owes a reason,
			// which Configure shows. The reason is the predicate's own -- a
			// row is never greyed by a flag someone set separately, so a
			// greyed control that cannot say why is unrepresentable.
			const bool bDisabled = !entry.DisabledReason().empty();
			const ScopedDim dim( bDisabled );

			// The value string. Facts summarise themselves; everything else
			// prints its bound value, with unit and zero-word applied.
			std::string sValue;
			if ( entry.GetKind() == Kind::Facts )
				sValue = entry.SummaryText();
			else if ( entry.GetKind() == Kind::Meter )
				sValue = MeterValue( entry );
			else
				sValue = FormatDeclValue( entry );

			ImRect rcLabel, rcValue;
			const float flValueW = entry.UsesValue() && !sValue.empty()
				? MeasureText( TypeRole::Value, sValue.c_str() ).x : 0.0f;
			row.SplitLabelZone( flValueW, &rcLabel, &rcValue );

			Label( { rcLabel.Min.x, rcLabel.Min.y, rcLabel.Max.x, rcLabel.Max.y },
			       TypeRole::Label,
			       bSelected ? Col( Role::TextPrimary ) : Col( Role::TextLabel ),
			       entry.Title().c_str() );
			if ( flValueW > 0.0f )
				Label( { rcValue.Min.x, rcValue.Min.y, rcValue.Max.x, rcValue.Max.y },
				       TypeRole::Value, Col( Role::TextPrimary ),
				       sValue.c_str(), TextAlign::Right );

			// The control. Every atom is right-bound by construction --
			// RowCtx has no other kind of allocator (see Row.h).
			//
			// BeginDisabled() is what makes a greyed row NON-INTERACTIVE;
			// ScopedDim above is what makes it LOOK greyed. Both are needed --
			// the kit paints on the draw list, which ImGui's own alpha never
			// reaches -- and both are driven off the same one predicate, so
			// they cannot disagree about which rows are disabled.
			if ( bDisabled )
				ImGui::BeginDisabled();

			switch ( entry.GetKind() )
			{
				case Kind::Switch:
				case Kind::Slider:
				case Kind::Stepper:
				case Kind::Choice:
				// Bank and Text reached this switch's `default: break` until
				// P3c and therefore DREW NOTHING -- no area had used either
				// kind before the Log, so two declared, law-abiding, fully
				// bound controls rendered as an empty control column. That is
				// #25 and #68 exactly, produced by an unhandled enumerator
				// rather than by a missing implementation: both atoms have
				// existed in Controls.cpp since P1.
				case Kind::Bank:
				case Kind::Text:
					// bAffordance also tells the two hosts apart: it is true
					// only for the sheet. The focus ring therefore appears in
					// the one region the arrow keys are actually driving,
					// rather than on both copies of a selected row's bank.
					DrawSharedControl( entry, row, "ctl", entry.Id(),
						( bAffordance
							? ( s_eFocusRegion == Region::Sheet && bSelected )
							: ( s_eFocusRegion == Region::Inspector && s_nInspectorFocus == 0 ) )
						? s_nBankChip : -1 );
					break;
				case Kind::Action:
				{
					// A destructive action is ARMED by its first press and
					// performed only by its second (Entry::Confirm). The
					// armed row is remembered by id, so exactly one action
					// can be armed at a time and walking away disarms it.
					const bool bArmed = entry.NeedsConfirm() && s_sArmedAction == entry.Id();
					if ( controls::Verb( row, "verb",
						bArmed ? entry.ConfirmPrompt().c_str() : entry.Verb().c_str(),
						entry.NeedsConfirm() ? controls::Intent::Danger : controls::Intent::Accent ) )
					{
						if ( !entry.NeedsConfirm() )
						{
							entry.Invoke();
						}
						else if ( bArmed )
						{
							entry.Invoke();
							s_sArmedAction.clear();
						}
						else
						{
							s_sArmedAction = entry.Id();
							s_flArmedAt = (float)ImGui::GetTime();
						}
					}
					break;
				}
				case Kind::Meter:
					controls::Meter( row, (float)entry.Scalar(), entry.Lo(), entry.Hi() );
					break;
				case Kind::Facts:
				{
					// SPEC §2.3: a Facts row's summary is "a string in the
					// CONTROL zone, not a value" -- which is why
					// UsesValueColumn( Facts ) is false and the value
					// column above stayed empty. It is still right-bound,
					// through the same allocator every control uses.
					if ( !sValue.empty() )
					{
						const ImRect rc = row.PlacePx( MeasureText( TypeRole::Value, sValue.c_str() ).x );
						Label( { rc.Min.x, rc.Min.y, rc.Max.x, rc.Max.y },
						       TypeRole::Value, Col( Role::TextMeta ), sValue.c_str(), TextAlign::Right );
					}
					break;
				}
				default:
					break;   // the rest carry no control
			}

			if ( bDisabled )
				ImGui::EndDisabled();

			if ( bAffordance )
				DrawAffordance( entry, row );

			ImGui::PopID();
			return bClicked;
		}

		// =================================================================
		//  SPEC §6.3's inline param expansion (D20.3)
		// =================================================================
		// The Reachability Law's mechanism: "a row that owns Params renders
		// those Params inline in the Sheet, beneath itself, IN THE SHEET'S
		// OWN ROW GRAMMAR, whenever the Inspector is unavailable".
		//
		// THE PHRASE THAT MATTERS IS "the Sheet's own Row grammar", and it is
		// why this function is thirty lines rather than three hundred. It
		// allocates a RowCtx from the SAME lane the rows above it came from,
		// and hands it to the SAME DrawSharedControl the sheet row and the
		// Inspector both call. SPEC §6.3 clause 1 asks for exactly one
		// property -- "One code path. Params render with the Row grammar in
		// either host; the shell picks the host from the ladder, the painter
		// does not know which it is in" -- and a second painter here would
		// have made that sentence false in the act of implementing it.
		//
		// Returns the new y cursor, so a collapsed row costs the caller
		// nothing and an expanded one is just taller.
		float DrawInlineParams( const Entry &entry, const Lane &lane, float flOriginPx, float y )
		{
			if ( !InlineMode() || entry.ParamCount() == 0 || s_sExpandedEntry != entry.Id() )
				return y;

			// A disabled parent disables its params -- SPEC §3.13's
			// inheritance, the same OR of two predicates DrawConfigure uses.
			// Read once here rather than per param, because it is the
			// parent's answer and cannot differ between them.
			const std::string sParentReason = entry.DisabledReason();

			for ( size_t i = 0; i < entry.ParamCount(); ++i )
			{
				const Parameter &param = entry.ParamAt( i );
				const RowCtx     row   = RowCtx::ForRow( lane, flOriginPx, y );
				const ImRect     rcRow = row.Bounds();

				ImGui::SetCursorScreenPos( rcRow.Min );
				ImGui::PushID( entry.Id().c_str() );
				ImGui::PushID( (int)i + 2000 );

				// D22, same reason as the row's own selector -- see
				// DrawEntryRow. An expanded parameter row carries a real
				// control in its lane just as its entry row does.
				ImGui::SetNextItemAllowOverlap();
				const bool bClicked = ImGui::InvisibleButton( "##inlinerow", rcRow.GetSize() );
				const bool bHovered = ImGui::IsItemHovered();

				// The keyboard's own place inside the expansion. Same Accent
				// 8% a selected sheet row uses -- D18's "one visual language
				// for 'the keys are pointed here', wherever here happens to
				// be" -- so a focused param reads identically in all three
				// hosts.
				const bool bFocused = ( s_eFocusRegion == Region::Sheet &&
				                        SelectedEntry() == &entry &&
				                        s_nInlineFocus == (int)i );
				if ( bFocused )
					Fill( { rcRow.Min.x, rcRow.Min.y, rcRow.Max.x, rcRow.Max.y }, Accent( 0.08f ) );
				else if ( bHovered )
					Fill( { rcRow.Min.x, rcRow.Min.y, rcRow.Max.x, rcRow.Max.y },
					      IM_COL32( 255, 255, 255, 10 ) );

				if ( bClicked )
				{
					s_nInlineFocus = (int)i;
					s_eFocusRegion = Region::Sheet;
				}

				// D6's amendment, applied to a param in the Sheet: "they
				// gained the accent edge they never had, and lost the reset
				// dot they did have, so a Param-in-Sheet reads exactly like a
				// top-level row."
				if ( const ImU32 colEdge = StateEdgeColor( param, false ) )
				{
					const ImRect rcEdge = row.StateEdge();
					Fill( { rcEdge.Min.x, rcEdge.Min.y, rcEdge.Max.x, rcEdge.Max.y }, colEdge );
				}
				HLine( rcRow.Min.x, rcRow.Max.x, rcRow.Max.y - Hairline(), Col( Role::Line ) );

				const std::string sReason = param.DisabledReason();
				const bool bDisabled = !sReason.empty() || !sParentReason.empty();
				const ScopedDim dim( bDisabled );

				ImRect rcLabel, rcValue;
				const std::string sValue = FormatDeclValue( param );
				const float flValueW = param.UsesValue() && !sValue.empty()
					? MeasureText( TypeRole::Value, sValue.c_str() ).x : 0.0f;
				row.SplitLabelZone( flValueW, &rcLabel, &rcValue );

				Label( { rcLabel.Min.x, rcLabel.Min.y, rcLabel.Max.x, rcLabel.Max.y },
				       TypeRole::Label, Col( Role::TextLabel ), param.Title().c_str() );
				if ( flValueW > 0.0f )
					Label( { rcValue.Min.x, rcValue.Min.y, rcValue.Max.x, rcValue.Max.y },
					       TypeRole::Value, Col( Role::TextPrimary ),
					       sValue.c_str(), TextAlign::Right );

				if ( bDisabled )
					ImGui::BeginDisabled();
				DrawSharedControl( param, row, "ictl", param.Id(),
					bFocused ? s_nBankChip : -1 );
				if ( bDisabled )
					ImGui::EndDisabled();

				ImGui::PopID();
				ImGui::PopID();

				y += Px( tok::kRowH );
			}
			return y;
		}

		// SPEC §2.5's group band: "Mono 500 10.5 UPPER TextMeta, 16 above /
		// 8 below, no box, no fill, no border. Right slot carries at most one
		// thing: a `4 / 7` count for a switch set, or all/none, or nothing."
		//
		// The count is computed here from the band's own entries rather than
		// supplied by a call site -- GroupCount() takes a name and nothing
		// else. That is the same rule as the rail counter (SPEC §8.1: "the
		// counter means exactly one thing"): a number nobody can type is a
		// number that cannot lie about what it counts.
		float DrawGroupBand( const Area &area, size_t nGroup, const Rect &rcCol, float y )
		{
			const Area::GroupBand &band = area.Groups()[ nGroup ];
			if ( band.sName.empty() )
				return y;

			y += Px( tok::kGroupSpaceAbove );
			const float flH = Px( 14.0f );

			std::string sUpper = band.sName;
			for ( char &c : sUpper )
				c = (char)toupper( (unsigned char)c );
			Label( { rcCol.x0, y, rcCol.x1, y + flH }, TypeRole::Section,
			       Col( Role::TextMeta ), sUpper.c_str() );

			if ( band.bCounted )
			{
				int nOn = 0, nTotal = 0;
				for ( size_t i = 0; i < area.EntryCount(); ++i )
				{
					const Entry &e = area.EntryAt( i );
					if ( area.GroupOf( i ) != nGroup || e.GetKind() != Kind::Switch )
						continue;
					++nTotal;
					const Value v = e.Binding().Get();
					if ( std::holds_alternative<bool>( v ) && std::get<bool>( v ) )
						++nOn;
				}
				if ( nTotal )
				{
					char sz[ 24 ];
					snprintf( sz, sizeof( sz ), "%d / %d", nOn, nTotal );
					Label( { rcCol.x0, y, rcCol.x1, y + flH }, TypeRole::Section,
					       Col( Role::TextMeta ), sz, TextAlign::Right );
				}
			}

			return y + flH + Px( tok::kGroupSpaceBelow );
		}

		// ---- a content area's body (Area::Content) -----------------------
		// The Log is the only one. Everything about how it LOOKS is decided
		// here, in the shell, because the registration hands over data and
		// nothing else -- no font, no colour, no width, no cursor. That is
		// what makes this a content view rather than an escape hatch.
		//
		// Severity is a small int on the line, mapped to a role here, so a
		// category cannot introduce a colour the palette does not have.
		ImU32 SeverityColor( int nSeverity )
		{
			switch ( nSeverity )
			{
				case 3:  return Col( Role::DangerText );
				case 2:  return Col( Role::WarnText );
				case 1:  return Col( Role::TextMeta );
				default: return Col( Role::TextBody );
			}
		}

		// A line's time-of-day column. EMPTY when the line carries no
		// timestamp, which is the whole reason ulTimeMs has 0 as a sentinel:
		// the rings only started stamping lines when the field was added, so
		// anything captured before that -- or arriving from a path that does
		// not stamp -- has no time to show. Rendering 0 through a clock would
		// print "01:00:00.000", a confident, precise and entirely invented
		// timestamp. Blank is the honest answer, and it keeps the column
		// aligned so the text still starts in the same place.
		std::string ContentTimeText( uint64_t ulTimeMs )
		{
			if ( ulTimeMs == 0 )
				return std::string();

			const std::time_t t = (std::time_t)( ulTimeMs / 1000 );
			std::tm tm {};
			localtime_r( &t, &tm );

			char sz[ 16 ];
			std::snprintf( sz, sizeof( sz ), "%02d:%02d:%02d.%03d",
				tm.tm_hour, tm.tm_min, tm.tm_sec, (int)( ulTimeMs % 1000 ) );
			return std::string( sz );
		}

		void DrawContentBody( const Area &area, const Rect &rcCol, float flTop, float flBottom )
		{
			const std::vector<ContentLine> vecLines = area.ContentLines();

			const float flLineH = MeasureText( TypeRole::Meta, "Xg" ).y + Px( 3.0f );
			const float flH     = ImMax( flLineH * 3.0f, flBottom - flTop - Px( tok::kM ) );

			const Rect rcBody { rcCol.x0, flTop, rcCol.x1, flTop + flH };
			Fill( rcBody, Col( Role::SurfaceRaised ) );

			if ( vecLines.empty() )
			{
				// SPEC §3.13's empty state: one centred line, TextMeta.
				Label( { rcBody.x0, rcBody.y0, rcBody.x1, rcBody.y0 + Px( 96.0f ) },
				       TypeRole::Meta, Col( Role::TextMeta ),
				       "nothing captured yet", TextAlign::Center );
				return;
			}

			// ---- fixed columns ----------------------------------------------
			// Measured once from the WIDEST value each column can hold, not
			// per line, so the gutter and the text edge are two straight
			// vertical lines down the whole body rather than a ragged edge
			// that shifts as line numbers gain a digit or a stamp goes blank.
			// This is the same right-binding rule the row lane follows.
			bool bAnyNumbered = false, bAnyTimed = false;
			uint64_t ulMaxSeq = 0;
			for ( const ContentLine &l : vecLines )
			{
				bAnyNumbered |= ( l.ulSeq != 0 );
				bAnyTimed    |= ( l.ulTimeMs != 0 );
				ulMaxSeq      = ImMax( ulMaxSeq, l.ulSeq );
			}

			char szWidest[ 24 ];
			std::snprintf( szWidest, sizeof( szWidest ), "%llu", (unsigned long long)ulMaxSeq );
			const float flNumW  = bAnyNumbered
				? MeasureText( TypeRole::Meta, szWidest ).x + Px( tok::kM ) : 0.0f;
			const float flTimeW = bAnyTimed
				? MeasureText( TypeRole::Meta, "00:00:00.000" ).x + Px( tok::kM ) : 0.0f;

			ImGui::SetCursorScreenPos( ImVec2( rcBody.x0, rcBody.y0 ) );
			if ( ImGui::BeginChild( "##content", ImVec2( rcBody.Width(), rcBody.Height() ),
				ImGuiChildFlags_None,
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_HorizontalScrollbar ) )
			{
				// Every line is exactly one visual row, so a uniform-height
				// clipper is exact at any buffer size -- only what is on
				// screen ever becomes draw data.
				const float flPadX = Px( tok::kS );
				ImGuiListClipper clipper;
				clipper.Begin( (int)vecLines.size(), flLineH );
				while ( clipper.Step() )
				{
					// The row origin is captured ONCE per step. It cannot be
					// read inside the loop: the per-line InvisibleButton that
					// makes a line selectable advances ImGui's cursor, so
					// re-reading it each iteration compounds the advance and
					// every line drifts further down than the last.
					const float flBaseY = ImGui::GetCursorScreenPos().y;

					for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i )
					{
						const ContentLine &line = vecLines[ (size_t)i ];
						const float y = flBaseY
						              + (float)( i - clipper.DisplayStart ) * flLineH;

						// ---- selection -----------------------------------
						// Only numbered content is selectable: a line with no
						// seq has no identity to remember, so offering to
						// select it would be offering a selection that cannot
						// survive the next frame. That is what makes the
						// Changelog's prose inert here without a second code
						// path -- it simply carries no seq.
						const bool bSelectable = ( line.ulSeq != 0 );
						const bool bSel = bSelectable && line.ulSeq == s_ulSelectedLine;
						bool bHovered = false;

						if ( bSelectable )
						{
							ImGui::SetCursorScreenPos( ImVec2( rcBody.x0, y ) );
							ImGui::PushID( (int)( line.ulSeq & 0x7fffffffu ) );
							ImGui::SetNextItemAllowOverlap();
							if ( ImGui::InvisibleButton( "l",
								ImVec2( ImMax( 1.0f, rcBody.Width() ), flLineH ) ) )
							{
								s_ulSelectedLine = line.ulSeq;
								// A line and a row are alternative answers to
								// "what is the Inspector describing", so
								// taking one clears the other.
								Select( nullptr );
								s_bContentFilterHost = false;
								s_eFocusRegion = Region::Sheet;
							}
							bHovered = ImGui::IsItemHovered();
							ImGui::PopID();
						}

						// The SAME selected/hover treatment an ordinary row
						// gets (see DrawEntryRow) -- a log line is a different
						// kind of content, not a different design language.
						if ( bSel )
							Fill( { rcBody.x0, y, rcBody.x1, y + flLineH }, Accent( 0.08f ) );
						else if ( bHovered )
							Fill( { rcBody.x0, y, rcBody.x1, y + flLineH },
							      IM_COL32( 255, 255, 255, 10 ) );

						float x = rcBody.x0 + flPadX;

						// Line number, right-aligned in its column so the
						// digits line up as the numbers grow.
						if ( flNumW > 0.0f )
						{
							if ( line.ulSeq != 0 )
							{
								char szNum[ 24 ];
								std::snprintf( szNum, sizeof( szNum ), "%llu",
									(unsigned long long)line.ulSeq );
								Label( { x, y, x + flNumW - Px( tok::kS ), y + flLineH },
								       TypeRole::Meta, Col( Role::TextMeta ), szNum,
								       TextAlign::Right );
							}
							x += flNumW;
						}

						if ( flTimeW > 0.0f )
						{
							const std::string sTime = ContentTimeText( line.ulTimeMs );
							if ( !sTime.empty() )
								Label( { x, y, x + flTimeW - Px( tok::kS ), y + flLineH },
								       TypeRole::Meta, Col( Role::TextMeta ), sTime.c_str() );
							x += flTimeW;
						}

						if ( !line.sScope.empty() )
						{
							const std::string sTag = "[" + line.sScope + "]";
							const float flW = MeasureText( TypeRole::Meta, sTag.c_str() ).x;
							Label( { x, y, x + flW, y + flLineH }, TypeRole::Meta,
							       Col( Role::TextMeta ), sTag.c_str() );
							x += flW + Px( tok::kXS );
						}
						Label( { x, y, rcBody.x1 - flPadX, y + flLineH }, TypeRole::Meta,
						       SeverityColor( line.nSeverity ), line.sText.c_str() );
					}
				}
				clipper.End();

				// The standard log-window idiom, kept: stick to the bottom
				// while the view is already at the bottom, stop the instant
				// the reader scrolls up, resume when they scroll back down.
				// The scroll position IS the state -- but the area can
				// withdraw the behaviour outright, which is what Log's
				// auto-scroll switch does.
				if ( area.FollowTail() && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f )
					ImGui::SetScrollHereY( 1.0f );
			}
			ImGui::EndChild();
		}

		// flOccludedPx: how much of `rc`'s right side the Inspector drawer
		// floats over, 0 when it does not (D17). The sheet's REGION is
		// deliberately unchanged -- only the lane inside it gives way.
		void DrawSheetBody( const Rect &rcRegion, const Area *pArea, float flOccludedPx = 0.0f,
		                    int nColumns = 1 )
		{
			if ( !pArea )
				return;

			ImGui::SetCursorScreenPos( ImVec2( rcRegion.x0, rcRegion.y0 ) );
			if ( ImGui::BeginChild( "##sheetrows", ImVec2( rcRegion.Width(), rcRegion.Height() ),
				ImGuiChildFlags_None, ImGuiWindowFlags_NoSavedSettings ) )
			{
				// D26: THE SHEET SCROLLS BY THE SAME ONE MECHANISM THE
				// INSPECTOR DOES -- see ScrollView in Layout.h.
				//
				// This region is where "the scrollbar moves but the content
				// doesn't" came from, and it is worth being precise about
				// why, because the shape of the bug is invisible: every row
				// below was laid out from `rc.y0`, the sheet body's fixed
				// SCREEN coordinate. ImGui scrolls a child by moving its
				// cursor, not by translating the draw list, so a body that
				// never reads the cursor is pinned to the screen no matter
				// what the scroll offset is. The scrollbar was real -- the
				// rows' own InvisibleButtons pushed the content extent past
				// the child's height, so a range existed and the thumb slid
				// along it -- and it drove nothing at all.
				//
				// P3b fixed exactly this in the Inspector body and the
				// explain page, and did it inline in both. The sheet, the
				// largest scrolling region in the shell, was simply not one
				// of the places that got the four lines. That is the reason
				// the arithmetic is a named function now rather than a third
				// copy: the next body gets it by calling it.
				//
				// `rc` from here down is the SCROLLED body -- same width,
				// same height, origin moved by the scroll -- so every
				// existing absolute-y calculation below pans as one piece
				// with no other edit.
				const ScrollView view =
					ScrollView::Begin( rcRegion, ImGui::GetCursorScreenPos().y );
				const Rect rc = view.rcBody;

				// D20.2. Column geometry -- widths, origins and the per-column
				// lane the drawer may have narrowed -- comes from Layout.cpp
				// and nowhere else. This function decides WHICH ROWS go in a
				// column; it does not decide where a column is.
				const SheetColumnSet cols =
					LayOutSheetColumns( rc.Width(), flOccludedPx, nColumns, Scale() );

				// ---- pack the groups into the columns --------------------
				// THE UNIT OF PACKING IS A GROUP, NEVER A ROW. index.html's
				// own sheet does this ("greedy balance by row weight, not by
				// group count") and it is the right rule for a reason the
				// mockup does not have to state: a group split across a
				// column boundary either orphans its rows under no heading or
				// forces the band to be repeated at the top of the next
				// column. The first is unreadable and the second makes one
				// declared group look like two. Keeping a group whole costs
				// some balance and buys the row grammar intact.
				//
				// Weight is LINES, not rows -- a composite is n x 44 (SPEC
				// §4.2), so counting it as one would let a column holding
				// three composites overflow while looking short.
				struct Block { size_t nFirst = 0, nCount = 0; size_t nGroup = 0; int nLines = 0; };
				std::vector<Block> blocks;

				// Area::RowsInInspector(): the rows are hosted by the
				// Inspector, so the sheet packs NOTHING and the content body
				// below gets the whole region. Skipping the packing loop --
				// rather than packing and then not drawing -- is what makes
				// the body start at the top instead of below an invisible
				// 360px of reserved row space.
				const bool bRowsElsewhere = pArea->AreRowsInInspector();

				for ( size_t i = 0; !bRowsElsewhere && i < pArea->EntryCount(); ++i )
				{
					const size_t nGroup = pArea->GroupOf( i );
					// A band is emitted when the group index CHANGES, so a
					// group declared with no entries under it draws nothing
					// at all -- an empty heading is the one thing a band
					// must never be. Same rule, now expressed as "a block
					// starts where the group changes".
					if ( blocks.empty() || nGroup != blocks.back().nGroup )
						blocks.push_back( Block{ i, 0, nGroup, 0 } );

					blocks.back().nCount++;
					blocks.back().nLines += LinesFor( pArea->EntryAt( i ) );
				}

				// Greedy: each group goes to the column that is shortest so
				// far. index.html's algorithm, kept identical so the mockup
				// stays the tiebreaker it is declared to be.
				std::vector<int> colOf( blocks.size(), 0 );
				float flWeight[ kMaxSheetColumns ] = {};
				for ( size_t b = 0; b < blocks.size(); ++b )
				{
					int nBest = 0;
					for ( int c = 1; c < cols.nColumns; ++c )
						if ( flWeight[ c ] < flWeight[ nBest ] )
							nBest = c;

					colOf[ b ] = nBest;
					// The band's own height counts too, or a column of many
					// small groups packs short against one of few big ones.
					flWeight[ nBest ] += (float)blocks[ b ].nLines + 1.0f;
				}

				// ---- draw ------------------------------------------------
				float yOf[ kMaxSheetColumns ];
				for ( int c = 0; c < cols.nColumns; ++c )
					yOf[ c ] = rc.y0 + Px( tok::kM );

				for ( size_t b = 0; b < blocks.size(); ++b )
				{
					const Block &blk = blocks[ b ];
					const int    c   = colOf[ b ];

					// Each column has its OWN lane, and right-binding holds
					// within it: every rect a row places comes from this
					// column's lane and this column's origin, so SPEC §2.2's
					// two hard vertical lines exist once per column rather
					// than once per sheet.
					const Lane &lane      = cols.cols[ c ].lane;
					const float flOriginX = rc.x0 + cols.cols[ c ].rc.x0;
					const Rect  rcCol { flOriginX, rc.y0,
					                    flOriginX + Px( lane.flWidth ), rc.y1 };

					if ( blk.nGroup < pArea->Groups().size() )
						yOf[ c ] = DrawGroupBand( *pArea, blk.nGroup, rcCol, yOf[ c ] );

					for ( size_t k = 0; k < blk.nCount; ++k )
					{
						const Entry &entry = pArea->EntryAt( blk.nFirst + k );
						const bool   bSel  = ( SelectedEntry() == &entry );

						if ( DrawEntryRow( entry, lane, flOriginX, yOf[ c ], bSel ) )
						{
							Select( &entry );
							s_eFocusRegion = Region::Sheet;
						}
						// n x 44 for a composite, 44 for everything else --
						// from Band.cpp, never from a call site (SPEC §4.2
						// clause 1).
						yOf[ c ] += Px( tok::kRowH ) * (float)LinesFor( entry );

						// SPEC §6.3's inline expansion, beneath the row that
						// owns the params -- see DrawInlineParams.
						yOf[ c ] = DrawInlineParams( entry, lane, flOriginX, yOf[ c ] );
					}
				}

				// A content area's body, beneath its own rows (Area::Content).
				// Only ever one column here: Solve() forces nColumns to 1 for
				// an area with a content body, because one scrolling body
				// cannot be cut in half.
				bool bFillsRegion = false;
				if ( pArea->HasContent() )
				{
					bFillsRegion = true;
					const Lane &lane      = cols.cols[ 0 ].lane;
					const float flOriginX = rc.x0 + cols.cols[ 0 ].rc.x0;
					const Rect  rcCol { flOriginX, rc.y0,
					                    flOriginX + Px( lane.flWidth ), rc.y1 };
					DrawContentBody( *pArea, rcCol, yOf[ 0 ] + Px( tok::kM ), rc.y1 );

					// A content body SIZES ITSELF to what is left of the
					// region and scrolls internally, in its own nested
					// child. So the sheet's extent is the region, not the
					// rows above it -- and it takes no trailing pad, or the
					// sheet would grow a few pixels of scroll range that
					// exist purely to be scrolled past.
					yOf[ 0 ] = std::max( yOf[ 0 ], rc.y1 );
				}

				// ---- hand ImGui the extent ------------------------------
				// The other half of the mechanism. Without this the child
				// has no scroll range and everything past the bottom edge
				// is drawn and clipped away; the rows' own InvisibleButtons
				// give a range only by accident, and one measured from
				// where the buttons happened to land rather than from the
				// TALLEST column -- which for a two-column sheet is not
				// generally the last one drawn. A Dummy at the origin
				// states it exactly, once, however many columns there are.
				float flBottom = view.flOriginY;
				for ( int c = 0; c < cols.nColumns; ++c )
					flBottom = std::max( flBottom, yOf[ c ] );

				ImGui::SetCursorScreenPos( ImVec2( rc.x0, view.flOriginY ) );
				ImGui::Dummy( ImVec2( 1.0f, view.ContentHeight(
					flBottom, bFillsRegion ? 0.0f : Px( tok::kM ) ) ) );
			}
			ImGui::EndChild();
		}

		// =================================================================
		//  Region 3 -- the Inspector (SPEC §5)
		// =================================================================
		// NOTE THE SHAPE OF THIS CODE, because it is the design's main
		// defence: every function below takes a `const Entry &` and reads
		// it. None of them takes a callback, a lambda, a draw list from a
		// category, or a string a panel typed. There is no parameter
		// anywhere in this section through which a category file could
		// place a pixel in the Inspector -- that is SPEC §5.2 clause 0
		// enforced by there being nothing to call rather than by a rule
		// someone has to remember.
		void DrawModeStrip( const Rect &rc, const Entry *pEntry )
		{
			HLine( rc.x0, rc.x1, rc.y1 - Hairline(), Col( Role::LineRegion ) );
			if ( !pEntry )
			{
				// SPEC §5.1: Overview "replaces the strip entirely".
				Label( { rc.x0 + Px( 14.0f ), rc.y0, rc.x1 - Px( 14.0f ), rc.y1 },
				       TypeRole::Section, Col( Role::TextMeta ), "OVERVIEW" );
				return;
			}

			const ModeCounts counts = CountsFor( *pEntry );
			const InspectorMode eActive = CurrentMode( pEntry );
			const float flCloseW = Px( 22.0f ) + Px( tok::kS );
			const float flCellW  = ( rc.Width() - 2.0f * Px( 14.0f ) - flCloseW ) * 0.5f;

			for ( int i = 0; i < 2; ++i )
			{
				const InspectorMode eMode = i == 0 ? InspectorMode::Configure : InspectorMode::Details;
				const bool bOn = eMode == eActive;
				const Rect rcCell { rc.x0 + Px( 14.0f ) + i * flCellW, rc.y0,
				                    rc.x0 + Px( 14.0f ) + ( i + 1 ) * flCellW, rc.y1 };

				ImGui::SetCursorScreenPos( ImVec2( rcCell.x0, rcCell.y0 ) );
				ImGui::PushID( i );
				if ( ImGui::InvisibleButton( "##mode", ImVec2( rcCell.Width(), rcCell.Height() ) ) )
				{
					s_eMode = eMode;
					s_bModeOverridden = true;
				}
				ImGui::PopID();

				// SPEC §5.1: the cells "are not tabs the designer fills;
				// they are a readout of what depth this selection actually
				// has". The counter is therefore computed from the
				// registration every frame (Layout.cpp's CountsFor) and is
				// never a number anyone typed.
				char szCell[ 48 ];
				const bool bRo = eMode == InspectorMode::Configure && counts.bReadOnly;
				const int  nCount = eMode == InspectorMode::Configure ? counts.nConfigure : counts.nDetails;
				if ( bRo )
					snprintf( szCell, sizeof( szCell ), "CONFIGURE  ro" );
				else
					snprintf( szCell, sizeof( szCell ), "%s  %d",
						eMode == InspectorMode::Configure ? "CONFIGURE" : "DETAILS", nCount );

				// D18: the strip is keyboard-reachable now (Tab to the
				// Inspector, Up onto index -1), so it needs to be able to
				// SHOW that it has the focus. Without this the keys worked
				// and nothing on screen said so, which is the same "renders
				// but does nothing" in reverse and just as hard to trust.
				if ( s_eFocusRegion == Region::Inspector && s_nInspectorFocus < 0 )
					Fill( { rcCell.x0, rcCell.y0, rcCell.x1, rcCell.y1 }, Accent( 0.10f ) );

				Label( rcCell, TypeRole::Section,
				       bOn ? Col( Role::AccentSeg ) : ( bRo ? Col( Role::TextMeta ) : Col( Role::TextLabel ) ),
				       szCell, TextAlign::Center );
				if ( bOn )
					Fill( { rcCell.x0, rcCell.y1 - Px( 2.0f ), rcCell.x1, rcCell.y1 }, Col( Role::AccentBase ) );
			}

			// The mode strip's own close glyph -- SPEC §8.05 names it as one
			// of the three ways the Inspector is hidden.
			const Rect rcClose { rc.x1 - Px( 14.0f ) - Px( 22.0f ), rc.y0, rc.x1 - Px( 14.0f ), rc.y1 };
			ImGui::SetCursorScreenPos( ImVec2( rcClose.x0, rcClose.y0 + ( rcClose.Height() - Px( 22.0f ) ) * 0.5f ) );
			if ( ImGui::InvisibleButton( "##inspclose", ImVec2( Px( 22.0f ), Px( 22.0f ) ) ) )
				SetHost( InspectorHost::Hidden );
			Label( rcClose, TypeRole::Meta,
			       ImGui::IsItemHovered() ? Col( Role::TextPrimary ) : Col( Role::TextMeta ),
			       "x", TextAlign::Center );
		}

		float DrawWrapped( const Rect &rc, TypeRole eRole, ImU32 col, const char *pszText, float y )
		{
			ImGui::PushFont( FontFor( eRole ) );
			const float flWrap = rc.Width();
			const ImVec2 size = ImGui::CalcTextSize( pszText, nullptr, false, flWrap );
			ImGui::GetWindowDrawList()->AddText( nullptr, ImGui::GetFontSize(),
				ImVec2( rc.x0, y ), col, pszText, nullptr, flWrap );
			ImGui::PopFont();
			return y + size.y;
		}

		// ---- CONFIGURE (SPEC §5.1, §5.3) ---------------------------------
		// Returns the y of the content's bottom edge. Every Inspector body
		// returns it, and DrawInspector turns the total into the child's
		// scroll range -- see the note there. The layout below is otherwise
		// untouched: still one absolute `y` walking down, still the same row
		// grammar and the same lane.
		float DrawConfigure( const Rect &rc, const Entry &entry )
		{
			const float flPad = Px( tok::kInspectorPad );
			const Rect  rcIn  { rc.x0 + flPad, rc.y0 + flPad, rc.x1 - flPad, rc.y1 };
			float y = rcIn.y0;

			Label( { rcIn.x0, y, rcIn.x1, y + Px( 18.0f ) }, TypeRole::Title,
			       Col( Role::TextPrimary ), entry.Title().c_str() );
			y += Px( shelltok::kTitleLine );

			// Generator 1: .Help(). Required by law, so this is never empty.
			y = DrawWrapped( rcIn, TypeRole::Body, Col( Role::TextBody ), entry.HelpText().c_str(), y );
			y += Px( tok::kM );

			// The disabled reason or the validation error, if there is one.
			const std::string sReason = entry.DisabledReason();
			if ( !sReason.empty() )
			{
				y = DrawWrapped( rcIn, TypeRole::Body, Col( Role::WarnText ), sReason.c_str(), y );
				y += Px( tok::kS );
			}

			if ( entry.ReadOnly() )
			{
				// SPEC §5.1: "For a read-only row the values block is
				// replaced by one sentence saying so and pointing at
				// Details."
				return DrawWrapped( rcIn, TypeRole::Body, Col( Role::TextMeta ),
					"This row is a readout -- there is nothing here to set. Its live values are in DETAILS.", y );
			}

			// The values block: the row's own control as an Inspector row,
			// then its Params, all at the same 44 height and the same
			// grammar (SPEC §5.3's amendment -- 44, not 40, because the
			// Sheet is the host a promoted parameter ends up in).
			HLine( rcIn.x0, rcIn.x1, y, Col( Role::Line ) );
			y += Px( tok::kS );
			Label( { rcIn.x0, y, rcIn.x1, y + Px( 14.0f ) }, TypeRole::Section,
			       Col( Role::TextMeta ), "VALUES" );

			// The reset link, right-aligned on the VALUES header -- D6's
			// "reset moves into the Inspector", and the successor to the
			// legacy Config panel's per-group links (issue #43). It resets
			// the row AND its parameters together, which is what makes it a
			// GROUP reset and not merely a row one: the old "UI Scale" group
			// is exactly the `UI scale` row plus its two params.
			//
			// It appears only when there is something to undo, so the
			// Inspector does not carry a permanently-dead affordance.
			if ( entry.HasDefault() && !entry.IsAtDefault() )
			{
				const char *pszReset = "reset";
				const float flResetW = MeasureText( TypeRole::Meta, pszReset ).x + Px( tok::kS ) * 2.0f;
				const Rect rcReset { rcIn.x1 - flResetW, y - Px( tok::kXS ),
				                     rcIn.x1, y + Px( 18.0f ) };
				ImGui::SetCursorScreenPos( ImVec2( rcReset.x0, rcReset.y0 ) );
				ImGui::PushID( "##resetrow" );
				const bool bReset = ImGui::InvisibleButton( "r",
					ImVec2( rcReset.Width(), rcReset.Height() ) );
				const bool bHover = ImGui::IsItemHovered();
				ImGui::PopID();
				Label( rcReset, TypeRole::Meta,
				       bHover ? Col( Role::AccentBase ) : Col( Role::TextMeta ),
				       pszReset, TextAlign::Center );
				if ( bReset )
					entry.ResetToDefault();
			}
			y += Px( shelltok::kSectionLine );

			const Lane lane = Lane::ForColumn( rcIn.Width() / Scale() );
			// D18: index 0 is the entry's own row. It reuses the row's
			// existing SELECTED fill rather than inventing a focus treatment
			// -- inside the Inspector "selected" and "focused" are the same
			// thing, because the Inspector only ever shows one entry.
			const bool bFocusOwnRow =
				( s_eFocusRegion == Region::Inspector && s_nInspectorFocus == 0 );
			// SPEC §2.4's affordance column belongs to the SHEET: it
			// advertises that there is depth behind a row. Inside the
			// Inspector you are already in that depth, so a chevron here
			// would point at nothing -- and a lock is redundant next to the
			// mode strip's own `ro` marker.
			DrawEntryRow( entry, lane, rcIn.x0, y, bFocusOwnRow, /* bAffordance */ false );
			y += Px( tok::kRowH ) * (float)LinesFor( entry );

			// index.html's `parameters  <n> of 6` header. The denominator is
			// the Six Budget itself, so the header is also the place the
			// budget is visible to a user rather than only to a test.
			if ( entry.ParamCount() )
			{
				y += Px( tok::kM );
				char szHead[ 48 ];
				snprintf( szHead, sizeof( szHead ), "PARAMETERS   %d of 6", (int)entry.ParamCount() );
				Label( { rcIn.x0, y, rcIn.x1, y + Px( 14.0f ) }, TypeRole::Section,
				       Col( Role::TextMeta ), szHead );
				y += Px( shelltok::kSectionLine );
			}

			for ( size_t i = 0; i < entry.ParamCount(); ++i )
			{
				const Parameter &param = entry.ParamAt( i );
				const RowCtx row = RowCtx::ForRow( lane, rcIn.x0, y );

				// D18: params are keyboard-focusable (index 1..n), and the
				// fill is the same Accent 8% a selected sheet row uses -- one
				// visual language for "the keys are pointed here", wherever
				// here happens to be.
				if ( s_eFocusRegion == Region::Inspector &&
				     s_nInspectorFocus == (int)i + 1 )
				{
					const ImRect rcF = row.Bounds();
					Fill( { rcF.Min.x, rcF.Min.y, rcF.Max.x, rcF.Max.y }, Accent( 0.08f ) );
				}

				HLine( row.Bounds().Min.x, row.Bounds().Max.x, row.Bounds().Max.y - Hairline(), Col( Role::Line ) );

				// SPEC §3.13's inheritance: a param under a disabled parent is
				// itself disabled, EXCEPT when it is the cause -- which is why
				// this is an OR of two independent predicates rather than a
				// walk up to the owner. `bReason` is the param's own; `sReason`
				// (the parent's, computed above) supplies the inherit half.
				const std::string sParamReason = param.DisabledReason();
				const bool bParamDisabled = !sParamReason.empty() || !sReason.empty();
				const ScopedDim dimParam( bParamDisabled );

				ImRect rcLabel, rcValue;
				const std::string sValue = FormatDeclValue( param );
				const float flValueW = param.UsesValue() && !sValue.empty()
					? MeasureText( TypeRole::Value, sValue.c_str() ).x : 0.0f;
				row.SplitLabelZone( flValueW, &rcLabel, &rcValue );

				Label( { rcLabel.Min.x, rcLabel.Min.y, rcLabel.Max.x, rcLabel.Max.y },
				       TypeRole::Label, Col( Role::TextLabel ), param.Title().c_str() );
				if ( flValueW > 0.0f )
					Label( { rcValue.Min.x, rcValue.Min.y, rcValue.Max.x, rcValue.Max.y },
					       TypeRole::Value, Col( Role::TextPrimary ),
					       sValue.c_str(), TextAlign::Right );

				ImGui::PushID( (int)i + 1000 );
				if ( bParamDisabled )
					ImGui::BeginDisabled();
				// The SAME painter the sheet row uses -- SPEC §5.3's "a
				// promoted parameter ends up in the Sheet" only holds if the
				// two are literally one code path.
				DrawSharedControl( param, row, "pctl", param.Id(),
					( s_eFocusRegion == Region::Inspector &&
					  s_nInspectorFocus == (int)i + 1 ) ? s_nBankChip : -1 );
				if ( bParamDisabled )
					ImGui::EndDisabled();
				ImGui::PopID();
				y += Px( tok::kRowH );

				// The param's own reason, under its row. The parent's is
				// already printed once at the top; repeating it per param
				// would be noise.
				if ( !sParamReason.empty() )
				{
					y = DrawWrapped( rcIn, TypeRole::Meta, Col( Role::WarnText ),
					                 sParamReason.c_str(), y ) + Px( tok::kXS );
				}
			}
			return y;
		}

		// ---- DETAILS (SPEC §5.1, §5.4) -----------------------------------
		float DrawDetails( const Rect &rc, const Entry &entry )
		{
			const float flPad = Px( tok::kInspectorPad );
			const Rect  rcIn  { rc.x0 + flPad, rc.y0 + flPad, rc.x1 - flPad, rc.y1 };
			float y = rcIn.y0;

			Label( { rcIn.x0, y, rcIn.x1, y + Px( 18.0f ) }, TypeRole::Title,
			       Col( Role::TextPrimary ), entry.Title().c_str() );
			y += Px( 24.0f );

			// The binding grid. Typed by nobody -- every row below is read
			// off the registration, which is what makes Details impossible
			// to fill with prose someone wanted somewhere.
			const float flKeyW = rcIn.Width() * 0.34f;
			const auto Grid = [ & ]( const char *pszKey, const std::string &sVal )
			{
				if ( sVal.empty() )
					return;
				Label( { rcIn.x0, y, rcIn.x0 + flKeyW, y + Px( 16.0f ) },
				       TypeRole::Section, Col( Role::TextMeta ), pszKey );
				// Wrapped, not clipped: a binding grid whose whole job is
				// to answer "what is this bound to" cannot afford to
				// truncate the answer, and `now` for a Facts row is a
				// whole sentence.
				const float yAfter = DrawWrapped( { rcIn.x0 + flKeyW, y, rcIn.x1, rcIn.y1 },
				                                  TypeRole::Meta, Col( Role::TextPrimary ), sVal.c_str(), y );
				y = std::max( y + Px( 20.0f ), yAfter + Px( tok::kXS ) );
			};

			// A composite's `now` is its RESOLVED value, the same string the
			// band's line 1 and the palette show. Printing its A-axis
			// binding put `0` under a VALUES line reading
			// `top-right · 32 / 32`, two lines apart on the same page.
			Grid( "NOW", entry.GetKind() == Kind::Facts
				? entry.SummaryText()
				: entry.GetKind() == Kind::Composite
					? CompositeValue( entry )
				: ( entry.Binding().IsBound() ? ValueToString( entry.Binding().Get() ) : std::string() ) );
			Grid( "DEFAULT", ValueToString( entry.DefaultValue() ) );
			if ( entry.HasRange() )
			{
				char sz[ 64 ];
				snprintf( sz, sizeof( sz ), "%g .. %g", entry.Lo(), entry.Hi() );
				Grid( "RANGE", sz );
			}
			if ( !entry.Options().empty() )
			{
				std::string sOpts;
				for ( const Option &opt : entry.Options() )
					sOpts += ( sOpts.empty() ? "" : " · " ) + std::string( opt.pszLabel ? opt.pszLabel : "" );
				Grid( "OPTIONS", sOpts );
			}
			Grid( "KIND", KindName( entry.GetKind() ) );
			Grid( "KEY", entry.Id() );

			// The .Live() block -- generator 4. SPEC §5.4: the lambda runs
			// "only for the selected entry", which is exactly what this
			// loop's placement guarantees; nothing calls it for a row that
			// is not on screen in this mode.
			if ( entry.LiveCount() )
			{
				y += Px( tok::kS );
				HLine( rcIn.x0, rcIn.x1, y, Col( Role::Line ) );
				y += Px( tok::kM );
				Label( { rcIn.x0, y, rcIn.x1, y + Px( 14.0f ) }, TypeRole::Section,
				       Col( Role::TextMeta ), "LIVE" );
				y += Px( shelltok::kSectionLine );

				for ( size_t i = 0; i < entry.LiveCount(); ++i )
				{
					const Fact fact = entry.LiveAt( i ).second();
					Label( { rcIn.x0, y, rcIn.x0 + flKeyW, y + Px( 16.0f ) },
					       TypeRole::Section, Col( Role::TextMeta ), fact.sLabel.c_str() );
					y = DrawWrapped( { rcIn.x0 + flKeyW, y, rcIn.x1, rcIn.y1 },
					                 TypeRole::Meta, Col( Role::AccentValue ), fact.sValue.c_str(), y );
					y += Px( tok::kXS );
				}
			}
			return y;
		}

		// ---- OVERVIEW (SPEC §5.5) ----------------------------------------
		float DrawOverview( const Rect &rc, const Area *pArea )
		{
			const float flPad = Px( tok::kInspectorPad );
			const Rect  rcIn  { rc.x0 + flPad, rc.y0 + flPad, rc.x1 - flPad, rc.y1 };
			float y = rcIn.y0;

			if ( !pArea )
				return y;

			char szTitle[ 128 ];
			snprintf( szTitle, sizeof( szTitle ), "%s / %s",
				SectionName( pArea->GetSection() ), pArea->Title().c_str() );
			Label( { rcIn.x0, y, rcIn.x1, y + Px( 18.0f ) }, TypeRole::Title,
			       Col( Role::TextPrimary ), szTitle );
			y += Px( 26.0f );

			const std::string sSummary = pArea->SummaryText();
			if ( !sSummary.empty() )
			{
				y = DrawWrapped( rcIn, TypeRole::Body, Col( Role::TextBody ), sSummary.c_str(), y );
				y += Px( tok::kM );
			}

			// SPEC §5.5's budget line: "sheet 9 rows · inspector 14 params
			// · 0 unreachable". The count that would say so if the
			// Inspector ever did become a junk drawer.
			int nParams = 0;
			for ( size_t i = 0; i < pArea->EntryCount(); ++i )
				nParams += (int)pArea->EntryAt( i ).ParamCount();

			HLine( rcIn.x0, rcIn.x1, y, Col( Role::Line ) );
			y += Px( tok::kM );
			char szBudget[ 128 ];
			snprintf( szBudget, sizeof( szBudget ), "sheet %d rows · inspector %d params · 0 unreachable",
				(int)pArea->EntryCount(), nParams );
			return DrawWrapped( rcIn, TypeRole::Meta, Col( Role::TextMeta ), szBudget, y );
		}

		// =================================================================
		//  The Inspector's two CONTENT hosts (P6)
		// =================================================================
		// An area whose rows live in the Inspector (Area::RowsInInspector)
		// replaces CONFIGURE/DETAILS with LINE/FILTER:
		//
		//   LINE   -- what the selected content line is.
		//   FILTER -- the rows that decide which lines are shown at all.
		//
		// Both still obey SPEC §5.2 clause 0: they take a `const Area &` and
		// read it. Nothing a category registered can place a pixel here; the
		// Log supplies lines and rows and the shell decides everything about
		// how either is drawn.
		void DrawContentModeStrip( const Rect &rc, const Area &area )
		{
			HLine( rc.x0, rc.x1, rc.y1 - Hairline(), Col( Role::LineRegion ) );

			const float flCloseW = Px( 22.0f ) + Px( tok::kS );
			const float flCellW  = ( rc.Width() - 2.0f * Px( 14.0f ) - flCloseW ) * 0.5f;

			for ( int i = 0; i < 2; ++i )
			{
				const bool bFilter = ( i == 1 );
				const bool bOn     = ( bFilter == s_bContentFilterHost );
				const Rect rcCell { rc.x0 + Px( 14.0f ) + i * flCellW, rc.y0,
				                    rc.x0 + Px( 14.0f ) + ( i + 1 ) * flCellW, rc.y1 };

				ImGui::SetCursorScreenPos( ImVec2( rcCell.x0, rcCell.y0 ) );
				ImGui::PushID( 200 + i );
				if ( ImGui::InvisibleButton( "##cmode", ImVec2( rcCell.Width(), rcCell.Height() ) ) )
					s_bContentFilterHost = bFilter;
				ImGui::PopID();

				// Same rule as the ordinary strip: the counter is a READOUT of
				// what depth the selection actually has, computed here, never
				// a number anyone typed.
				char szCell[ 48 ];
				if ( bFilter )
					snprintf( szCell, sizeof( szCell ), "FILTER  %d", (int)area.EntryCount() );
				else if ( s_ulSelectedLine )
					snprintf( szCell, sizeof( szCell ), "LINE  %llu",
						(unsigned long long)s_ulSelectedLine );
				else
					snprintf( szCell, sizeof( szCell ), "LINE" );

				if ( s_eFocusRegion == Region::Inspector && s_nInspectorFocus < 0 )
					Fill( { rcCell.x0, rcCell.y0, rcCell.x1, rcCell.y1 }, Accent( 0.10f ) );

				Label( rcCell, TypeRole::Section,
				       bOn ? Col( Role::AccentSeg ) : Col( Role::TextLabel ),
				       szCell, TextAlign::Center );
				if ( bOn )
					Fill( { rcCell.x0, rcCell.y1 - Px( 2.0f ), rcCell.x1, rcCell.y1 },
					      Col( Role::AccentBase ) );
			}

			const Rect rcClose { rc.x1 - Px( 14.0f ) - Px( 22.0f ), rc.y0, rc.x1 - Px( 14.0f ), rc.y1 };
			ImGui::SetCursorScreenPos( ImVec2( rcClose.x0,
				rcClose.y0 + ( rcClose.Height() - Px( 22.0f ) ) * 0.5f ) );
			if ( ImGui::InvisibleButton( "##cinspclose", ImVec2( Px( 22.0f ), Px( 22.0f ) ) ) )
				SetHost( InspectorHost::Hidden );
			Label( rcClose, TypeRole::Meta,
			       ImGui::IsItemHovered() ? Col( Role::TextPrimary ) : Col( Role::TextMeta ),
			       "x", TextAlign::Center );
		}

		// ---- LINE: what the selected content line is ----------------------
		float DrawContentLine( const Rect &rc, const Area &area )
		{
			const float flPad = Px( tok::kInspectorPad );
			const Rect  rcIn  { rc.x0 + flPad, rc.y0 + flPad, rc.x1 - flPad, rc.y1 };
			float y = rcIn.y0;

			if ( !s_ulSelectedLine )
			{
				return DrawWrapped( rcIn, TypeRole::Body, Col( Role::TextMeta ),
					"No line selected. Click a line in the log to see what it is.", y );
			}

			const std::vector<ContentLine> vecLines = area.ContentLines();
			const ContentLine *pLine = nullptr;
			for ( const ContentLine &l : vecLines )
				if ( l.ulSeq == s_ulSelectedLine )
					pLine = &l;

			if ( !pLine )
			{
				// The selection is real but currently filtered out or evicted.
				// Saying which is far more useful than showing nothing -- the
				// line number is still on screen in the strip.
				return DrawWrapped( rcIn, TypeRole::Body, Col( Role::TextMeta ),
					"That line is no longer in view -- the current filter hides it, or the "
					"capture buffer has since dropped it.", y );
			}

			char szTitle[ 48 ];
			std::snprintf( szTitle, sizeof( szTitle ), "line %llu",
				(unsigned long long)pLine->ulSeq );
			Label( { rcIn.x0, y, rcIn.x1, y + Px( 18.0f ) }, TypeRole::Title,
			       Col( Role::TextPrimary ), szTitle );
			y += Px( shelltok::kTitleLine );

			// The text itself, in the severity colour it has in the body, so
			// the Inspector and the line agree at a glance.
			y = DrawWrapped( rcIn, TypeRole::Body, SeverityColor( pLine->nSeverity ),
			                 pLine->sText.c_str(), y );
			y += Px( tok::kM );

			HLine( rcIn.x0, rcIn.x1, y, Col( Role::Line ) );
			y += Px( tok::kS );
			Label( { rcIn.x0, y, rcIn.x1, y + Px( 14.0f ) }, TypeRole::Section,
			       Col( Role::TextMeta ), "FACTS" );
			y += Px( shelltok::kSectionLine );

			static const char *kSeverityName[] = { "info", "debug", "warn", "error" };
			const std::string sTime = ContentTimeText( pLine->ulTimeMs );

			const std::pair<const char *, std::string> facts[] = {
				{ "time",     sTime.empty() ? std::string( "not recorded" ) : sTime },
				{ "severity", kSeverityName[ std::clamp( pLine->nSeverity, 0, 3 ) ] },
				{ "source",   pLine->sScope.empty() ? std::string( "--" ) : pLine->sScope },
			};

			for ( const auto &f : facts )
			{
				Label( { rcIn.x0, y, rcIn.x1, y + Px( 18.0f ) }, TypeRole::Meta,
				       Col( Role::TextMeta ), f.first );
				Label( { rcIn.x0, y, rcIn.x1, y + Px( 18.0f ) }, TypeRole::Meta,
				       Col( Role::TextBody ), f.second.c_str(), TextAlign::Right );
				y += Px( 22.0f );
			}

			return y;
		}

		// ---- FILTER: the rows that decide what the body shows -------------
		float DrawContentFilter( const Rect &rc, const Area &area )
		{
			const float flPad = Px( tok::kInspectorPad );
			const Rect  rcIn  { rc.x0 + flPad, rc.y0 + flPad, rc.x1 - flPad, rc.y1 };
			float y = rcIn.y0;

			const Lane lane = Lane::ForColumn( rcIn.Width() / Scale() );

			// The rows are drawn with the SAME grammar as the sheet's -- same
			// lane, same 44 height, same selected fill, same controls. That is
			// the point of hosting rather than reimplementing: moving a row
			// between regions must not change what it is.
			size_t nLastGroup = (size_t)-1;
			// Whether the selected row is one of THIS area's -- decided while
			// walking the rows we are already walking, rather than by adding
			// an Entry->Area accessor for one comparison.
			bool bSelectedIsOurs = false;
			for ( size_t i = 0; i < area.EntryCount(); ++i )
			{
				const size_t nGroup = area.GroupOf( i );
				if ( nGroup != nLastGroup && nGroup < area.Groups().size() )
				{
					const Rect rcCol { rcIn.x0, y, rcIn.x0 + Px( lane.flWidth ), rc.y1 };
					y = DrawGroupBand( area, nGroup, rcCol, y );
					nLastGroup = nGroup;
				}

				const Entry &entry = area.EntryAt( i );
				const bool bSel = ( SelectedEntry() == &entry );
				bSelectedIsOurs |= bSel;

				// No affordance column: SPEC §2.4's chevron advertises that
				// there is depth BEHIND a row, and inside the Inspector you
				// are already in that depth.
				if ( DrawEntryRow( entry, lane, rcIn.x0, y, bSel, false ) )
				{
					Select( &entry );
					s_eFocusRegion = Region::Inspector;
				}
				y += Px( tok::kRowH ) * (float)LinesFor( entry );
			}

			// The selected row's help and its reset, beneath the list. This is
			// the substance of CONFIGURE without a third mode cell: help is
			// required by law so it is never empty, and reset is the one
			// action a filter row actually needs.
			if ( const Entry *pSel = SelectedEntry() )
			{
				if ( bSelectedIsOurs )
				{
					y += Px( tok::kM );
					HLine( rcIn.x0, rcIn.x1, y, Col( Role::Line ) );
					y += Px( tok::kS );

					Label( { rcIn.x0, y, rcIn.x1, y + Px( 14.0f ) }, TypeRole::Section,
					       Col( Role::TextMeta ), pSel->Title().c_str() );
					y += Px( shelltok::kSectionLine );

					y = DrawWrapped( rcIn, TypeRole::Body, Col( Role::TextBody ),
					                 pSel->HelpText().c_str(), y );

					const std::string sReason = pSel->DisabledReason();
					if ( !sReason.empty() )
					{
						y += Px( tok::kS );
						y = DrawWrapped( rcIn, TypeRole::Body, Col( Role::WarnText ),
						                 sReason.c_str(), y );
					}

					if ( pSel->HasDefault() && !pSel->IsAtDefault() )
					{
						y += Px( tok::kS );
						const char *pszReset = "reset";
						const float flResetW =
							MeasureText( TypeRole::Meta, pszReset ).x + Px( tok::kS ) * 2.0f;
						const Rect rcReset { rcIn.x0, y, rcIn.x0 + flResetW, y + Px( 20.0f ) };
						ImGui::SetCursorScreenPos( ImVec2( rcReset.x0, rcReset.y0 ) );
						ImGui::PushID( "##cfgreset" );
						const bool bReset = ImGui::InvisibleButton( "r",
							ImVec2( rcReset.Width(), rcReset.Height() ) );
						const bool bHover = ImGui::IsItemHovered();
						ImGui::PopID();
						Label( rcReset, TypeRole::Meta,
						       bHover ? Col( Role::AccentBase ) : Col( Role::TextMeta ),
						       pszReset, TextAlign::Center );
						if ( bReset )
							pSel->ResetToDefault();
						y += Px( 22.0f );
					}
				}
			}

			return y;
		}

		void DrawInspector( const Regions &regions, const LadderResult &ladder )
		{
			const Entry *pEntry = SelectedEntry();
			const bool bDrawer = ladder.eHost == InspectorHost::Drawer;

			// ---------------------------------------------------------
			// WHY THE WHOLE REGION IS ONE CHILD WINDOW.
			// ---------------------------------------------------------
			// The drawer must paint OVER the sheet, and ImGui's stock
			// z-order is not negotiable: every child window renders after
			// its parent's own draw commands, in the order the children
			// were begun. So an inspector background painted onto the
			// slab's draw list -- however late in the frame -- still loses
			// to the sheet's child, and the sheet's rows show straight
			// through the drawer. (They did; that is what this shape
			// fixes.)
			//
			// Putting the entire region in a child begun AFTER the sheet's
			// puts it above by the same rule, with no draw-list surgery
			// and no channel splitting. The outer child does not scroll --
			// it exists to own the z-order and the clip rect; the mode
			// strip is fixed inside it and only the body scrolls, in a
			// nested child.
			ImGui::SetCursorScreenPos( ImVec2( regions.rcInspector.x0, regions.rcInspector.y0 ) );
			ImGui::PushStyleColor( ImGuiCol_ChildBg, bDrawer
				? IM_COL32( 12, 14, 17, 251 )       // index.html's .insp.drawer
				: Col( Role::SurfaceInspector ) );
			ImGui::PushStyleVar( ImGuiStyleVar_ChildBorderSize, 0.0f );

			const bool bOpen = ImGui::BeginChild( "##insp",
				ImVec2( regions.rcInspector.Width(), regions.rcInspector.Height() ),
				ImGuiChildFlags_None,
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
				ImGuiWindowFlags_NoScrollWithMouse );
			if ( bOpen )
			{
				VLine( regions.rcInspector.x0, regions.rcInspector.y0, regions.rcInspector.y1,
				       bDrawer ? Col( Role::AccentBase ) : Col( Role::LineRegion ) );

				// P6. A content area whose rows live here shows LINE/FILTER
				// instead of CONFIGURE/DETAILS -- see DrawContentModeStrip.
				const Area *pContentArea = SelectedArea();
				const bool  bContentHost = pContentArea && pContentArea->AreRowsInInspector();

				if ( bContentHost )
					DrawContentModeStrip( regions.rcModeStrip, *pContentArea );
				else
					DrawModeStrip( regions.rcModeStrip, pEntry );

				ImGui::SetCursorScreenPos( ImVec2( regions.rcInspectorBody.x0, regions.rcInspectorBody.y0 ) );
				if ( ImGui::BeginChild( "##inspbody",
					ImVec2( regions.rcInspectorBody.Width(), regions.rcInspectorBody.Height() ),
					ImGuiChildFlags_None, ImGuiWindowFlags_NoSavedSettings ) )
				{
					// -------------------------------------------------
					// WHY THIS IS NOT A SECOND LAYOUT PATH.
					// -------------------------------------------------
					// The bodies lay out with an absolute `y` painted
					// onto the draw list, which is what keeps the row
					// grammar and the lane identical to the sheet's. The
					// bug was never that grammar -- it was that `y`
					// started at the REGION's y0, a fixed screen
					// coordinate, and that nothing ever told ImGui how
					// tall the result was.
					//
					// Both halves are ScrollView's job now (Layout.h);
					// this call site used to spell them out inline, and
					// the sheet -- which needed the same four lines --
					// never got them. See D26.
					//
					// The alternative -- rewriting the bodies onto
					// ImGui's cursor with Dummy/SameLine spacing -- would
					// have meant one layout model in the sheet and
					// another in the Inspector, which is precisely the
					// second path SPEC §5.3 exists to prevent (a promoted
					// parameter has to land in the sheet unchanged).

					// The console's scroll request, applied only on the frame
					// it changes -- see cv_overlay_e2_scroll.
					const float flScrollReq = cv_overlay_e2_scroll.Get();
					if ( flScrollReq != s_flScrollApplied )
					{
						s_flScrollApplied = flScrollReq;
						ImGui::SetScrollY( flScrollReq < 0.0f
							? ImGui::GetScrollMaxY() : flScrollReq );
					}

					const ScrollView view = ScrollView::Begin(
						regions.rcInspectorBody, ImGui::GetCursorScreenPos().y );
					const Rect rcBody = view.rcBody;

					float flBottom = view.flOriginY;
					if ( bContentHost )
						flBottom = s_bContentFilterHost
							? DrawContentFilter( rcBody, *pContentArea )
							: DrawContentLine( rcBody, *pContentArea );
					else if ( !pEntry )
						flBottom = DrawOverview( rcBody, SelectedArea() );
					else if ( CurrentMode( pEntry ) == InspectorMode::Configure )
						flBottom = DrawConfigure( rcBody, *pEntry );
					else
						flBottom = DrawDetails( rcBody, *pEntry );

					// The trailing pad is the same one the top has, so a
					// fully-scrolled body does not end flush against the
					// frame.
					ImGui::SetCursorScreenPos( ImVec2( rcBody.x0, view.flOriginY ) );
					ImGui::Dummy( ImVec2( 1.0f,
						view.ContentHeight( flBottom, Px( tok::kInspectorPad ) ) ) );
				}
				ImGui::EndChild();
			}
			ImGui::EndChild();
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();
		}

		// =================================================================
		//  The full-sheet explanation page (SPEC §6.3, §8.2 -- D18)
		// =================================================================
		// "With the Inspector closed, `?` or `Ctrl+/` on a selected row opens
		// Configure AND Details as one full-sheet page with a back crumb,
		// replacing the sheet content for as long as you read it."
		//
		// This is the Reachability Law's third clause. All three are now
		// built: params render inline in the Sheet (D20.3, DrawInlineParams
		// above), the palette reaches every setting (P4), and this page is
		// how explanation is reached without the Inspector.
		//
		// THIS COMMENT USED TO SAY "params render inline (P3)" WHILE NO SUCH
		// CODE EXISTED. That is worth recording rather than quietly fixing:
		// the pre-P5 shell test found it, and a comment asserting a mechanism
		// that was never written is how the next reader concludes a law is
		// covered when it is not. The claim is true now because
		// DrawInlineParams exists -- not because the sentence was edited.
		//
		// It calls the SAME two bodies the Inspector calls, in the same
		// order, with a wider rect. That is the whole implementation, and it
		// is deliberate: an explanation page that formatted its own version
		// of Configure would be a second painter for the same declaration --
		// SPEC §5.2 clause 0's exact prohibition, and the thing that makes
		// "one code path" true here rather than merely claimed.
		void DrawExplainPage( const Rect &rc, const Entry &entry )
		{
			ImGui::SetCursorScreenPos( ImVec2( rc.x0, rc.y0 ) );
			if ( ImGui::BeginChild( "##explain", ImVec2( rc.Width(), rc.Height() ),
				ImGuiChildFlags_None, ImGuiWindowFlags_NoSavedSettings ) )
			{
				// The same one scroll mechanism the sheet and the Inspector
				// body use -- ScrollView in Layout.h. Configure + Details
				// together are taller than the Inspector's body ever is, so
				// this page needs it more than either.
				const ScrollView view =
					ScrollView::Begin( rc, ImGui::GetCursorScreenPos().y );
				Rect rcBody = view.rcBody;

				float flBottom = DrawConfigure( rcBody, entry );

				// The seam between the two halves, so the page reads as two
				// sections rather than one long run of rows.
				flBottom += Px( tok::kXL );
				HLine( rc.x0 + Px( tok::kInspectorPad ), rc.x1 - Px( tok::kInspectorPad ),
				       flBottom, Col( Role::LineRegion ) );
				flBottom += Px( tok::kM );

				rcBody.y0 = flBottom;
				flBottom = DrawDetails( rcBody, entry );

				ImGui::SetCursorScreenPos( ImVec2( rc.x0, view.flOriginY ) );
				ImGui::Dummy( ImVec2( 1.0f,
					view.ContentHeight( flBottom, Px( tok::kInspectorPad ) ) ) );
			}
			ImGui::EndChild();
		}

		// ---- the hidden Inspector's spine (SPEC §8.05, from E1) -----------
		void DrawSpine( const Rect &rc )
		{
			ImGui::SetCursorScreenPos( ImVec2( rc.x0, rc.y0 ) );
			const bool bClicked = ImGui::InvisibleButton( "##spine", ImVec2( rc.Width(), rc.Height() ) );
			const bool bHovered = ImGui::IsItemHovered();
			if ( bClicked )
				SetHost( InspectorHost::Column );

			Fill( rc, bHovered ? Accent( 0.16f ) : IM_COL32( 255, 255, 255, 10 ) );
			VLine( rc.x0, rc.y0, rc.y1, bHovered ? Col( Role::AccentBase ) : Col( Role::LineRegion ) );

			if ( !s_bSpineLabel )
				return;

			// SPEC §8.05: the spine "names itself, so the region is
			// discoverable by someone who never learned Ctrl+I". CSS gets
			// that from `writing-mode: vertical-rl`; ImGui has no rotated
			// text and no vertical writing mode, so the letters are stacked
			// one per line instead.
			//
			// Chosen over the two alternatives on purpose: rotating a glyph
			// run by hand means pushing rotated quads into the draw list
			// and losing the atlas's hinting at 0.5x, and a rotated font
			// variant means a second baked atlas for eleven characters.
			// Stacked letters keep both, and at 20 base units wide the
			// column is one glyph wide either way.
			// D18: the marker after the word was "›" (U+203A), which is
			// OUTSIDE Fonts.cpp's baked Latin-1 range and drew as a fallback
			// box -- the one real box glyph the shell had. It is a chevron
			// now, and a drawn one.
			//
			// It points DOWN, which is the faithful reading of the mockup
			// rather than a departure from it: index.html sets the spine
			// `writing-mode: vertical-rl`, so its `›` is rotated a quarter
			// turn clockwise along with the letters and points down on
			// screen. The letters here are stacked instead of rotated, so
			// the mark is rotated to match what the mockup actually shows.
			static const char *const kSpineText = "inspector";
			const ImU32 col = bHovered ? Col( Role::AccentSeg ) : Col( Role::TextMeta );
			ImGui::PushFont( FontFor( TypeRole::Meta ) );
			const float flLineH = ImGui::GetFontSize() * 0.92f;
			// Nine letters, one blank line, one chevron -- the same eleven
			// slots the string used to occupy, so the spine's vertical
			// centring is unchanged.
			const float flTotal = flLineH * 11.0f;
			float y = rc.y0 + ( rc.Height() - flTotal ) * 0.5f;
			for ( const char *p = kSpineText; *p; ++p )
			{
				const ImVec2 size = ImGui::CalcTextSize( p, p + 1 );
				ImGui::GetWindowDrawList()->AddText(
					ImVec2( rc.x0 + ( rc.Width() - size.x ) * 0.5f, y ), col, p, p + 1 );
				y += flLineH;
			}
			ImGui::PopFont();

			y += flLineH;   // the blank slot the space used to hold
			glyph::Chevron( ImVec2( rc.x0 + rc.Width() * 0.5f, y + flLineH * 0.5f ),
			                Px( tok::kGlyphChevron ), glyph::Dir::Down, col );
		}

		// =================================================================
		//  The slab bar (SPEC §8.1's 40-base title strip)
		// =================================================================
		void DrawSlabBar( const Rect &rc )
		{
			Fill( rc, Accent( 0.10f ) );

			// D26: THE BAR'S BOTTOM RULE IS THE ACCENT, AND IT IS THE TOKEN.
			//
			// The slab's own frame is Accent( 0.42f ) (the Begin() border
			// below), so a LineRegion rule here read as a stray grey seam
			// crossing a blue frame -- which is what was reported. Matching
			// the frame's own alpha makes the bar's underline a continuation
			// of the border rather than a second, different boundary.
			//
			// Accent() is the C++ equivalent of the mockup's
			// `rgba(var(--accRGB), a)`: it resolves against the user's
			// configured accent hue every call. A literal here would be a
			// blue that stayed blue after the accent changed -- the exact
			// defect reported against the mockup, and the reason this is
			// routed through the token rather than through an IM_COL32.
			//
			// This DIVERGES FROM index.html, deliberately: the mockup's
			// .slabbar uses `border-bottom: 1px solid var(--lineRegion)`.
			// The mockup is the tiebreaker where the design is silent; it is
			// not the tiebreaker against the user looking at the result and
			// saying the line is the wrong colour.
			HLine( rc.x0, rc.x1, rc.y1 - Hairline(), Accent( 0.42f ) );

			const float flDot = Px( 6.0f );
			Fill( { rc.x0 + Px( tok::kM ), rc.y0 + ( rc.Height() - flDot ) * 0.5f,
			        rc.x0 + Px( tok::kM ) + flDot, rc.y0 + ( rc.Height() + flDot ) * 0.5f },
			      Col( Role::AccentBase ) );

			Label( { rc.x0 + Px( tok::kM ) + flDot + Px( tok::kS ), rc.y0, rc.x1, rc.y1 },
			       TypeRole::Title, Col( Role::TextPrimary ), "GAMESCOPE-RITZ" );

			// D26: the right-aligned "settings" was REMOVED. It was a bare
			// Label -- no id, no hit box, no state read and nothing keyed off
			// it -- so it labelled nothing and was not a signpost to anything;
			// it restated the window's own purpose in the window's own title
			// bar. SPEC §8.1 puts `app <id>` and the config-file chip on the
			// left of this bar and the ⌕ ▤ ✕ glyphs on the right; that is queued
			// separately, and this deliberately leaves the right end EMPTY for
			// it rather than parking a placeholder in the slot.
		}

		// =================================================================
		//  The command palette (SPEC §8.2, API.md §10)
		// =================================================================
		// Which area owns an entry. The palette jumps by id, and the shell's
		// selection is (area, entry) -- so this is the one lookup that turns
		// a flat search result back into a place in the rail. By identity,
		// never by id prefix: SPEC §2.6 states outright that an area id is
		// not a promise about the keys inside it.
		const Area *AreaOwning( const Entry *pEntry )
		{
			if ( !pEntry )
				return nullptr;
			for ( size_t a = 0; a < Reg().AreaCount(); ++a )
			{
				const Area &area = Reg().AreaAt( a );
				for ( size_t i = 0; i < area.EntryCount(); ++i )
					if ( &area.EntryAt( i ) == pEntry )
						return &area;
			}
			return nullptr;
		}

		// "Enter -- jump & select." Selecting the parent row, opening the
		// Inspector in Configure and landing on the param is what makes
		// SPEC §5.2's "one keystroke from anywhere" true for a Param as well
		// as a row.
		void PaletteJump( const std::string &sId )
		{
			const Entry     *pEntry = Reg().FindEntry( sId );
			const Parameter *pParam = pEntry ? nullptr : Reg().FindParam( sId );

			if ( pParam )
				pEntry = pParam->Owner();
			if ( !pEntry )
				return;

			const Area *pArea = AreaOwning( pEntry );
			if ( !pArea )
				return;

			s_sSelectedArea = pArea->Id();
			Select( pEntry );

			// A param lives in Configure by definition (SPEC §5.1: "arriving
			// from the palette on a parameter -- opens Configure"), even when
			// the parent row's own kind would have opened Details.
			if ( pParam )
			{
				s_eMode = InspectorMode::Configure;
				s_bModeOverridden = true;
			}

			// The Reachability Law (SPEC §6.3): with the Inspector hidden the
			// param has no column to be focused in, so the host is promoted
			// to one. Jumping to a setting and landing somewhere it is not
			// visible would be the one failure this whole index exists to
			// prevent.
			if ( pParam && Host() == InspectorHost::Hidden )
				SetHost( InspectorHost::Drawer );

			s_bPaletteOpen = false;
			s_eFocusRegion = Region::Sheet;

			// D25: Enter is what promotes the LAUNCHER to the full shell.
			//
			// This is the answer to "what happens when a result cannot be
			// adjusted in place". A composite, a text field, a bank -- their
			// controls do not fit a 38px result row, and the launcher does not
			// grow one. Enter takes you to the row in the full overlay, which
			// is exactly what Enter already meant here ("jump & select"), so
			// the launcher does not invent a second Enter.
			//
			// It costs the launcher's purity, and that is the right trade: the
			// user's complaint was that the KEYBIND dragged the shell in
			// unasked. An Enter the user pressed on a row they chose is not
			// that. The alternative -- showing a result you cannot act on --
			// turns a dead row into a puzzle, and the Reachability Law (SPEC
			// §6.3) is the whole reason this index exists.
			s_bLauncherOnly = false;
		}

		// The query field, hand-rolled on io.InputQueueCharacters.
		//
		// WHY NOT ImGui::InputText -- direction B's FEASIBILITY.md §2 worked
		// this out and E2 reaches the same answer, so the reasoning is
		// recorded here rather than re-derived by the next reader:
		//
		//   * IT OWNS LEFT/RIGHT. InputTextEx() calls SetKeyOwner() on
		//     ImGuiKey_LeftArrow/RightArrow while active and offers no
		//     callback to decline them. SPEC §8.2 gives those two keys to
		//     "adjust the highlighted entry's value IN PLACE" -- the
		//     palette's headline behaviour -- so the field cannot have them.
		//   * ITS ESC IS THE WRONG VERB. Esc on an active InputText reverts
		//     the buffer to what it held on focus; SPEC's Esc ladder wants
		//     the palette DISMISSED.
		//   * IT BUYS NO IME HERE. InputText positions a candidate window
		//     through io.SetPlatformImeDataFn, and this ImGui context has no
		//     platform backend at all (custom Wayland input, ImGui_ImplVulkan
		//     for rendering), so that pointer is null either way.
		//
		// What we lose against InputText: no selection, no caret placement,
		// no clipboard paste, no dead-key composition. All four are
		// acceptable for a search box that is at most a couple of words, and
		// three of the four are already absent everywhere else in this
		// overlay.
		//
		// io.InputQueueCharacters is filled by wlserver from the compositor's
		// own xkb state, so what arrives here is already layout-correct UTF-8
		// -- a German keyboard's 'z' arrives as 'z'.
		void PaletteConsumeInput()
		{
			ImGuiIO &io = ImGui::GetIO();

			bool bTyped = false;
			for ( int i = 0; i < io.InputQueueCharacters.Size; i++ )
			{
				const ImWchar c = io.InputQueueCharacters[ i ];
				// Printable only. Control characters arrive here too and
				// would otherwise become invisible query bytes that match
				// nothing -- a search box that silently stops working.
				if ( c >= 0x20 && c != 0x7F )
				{
					AppendUtf8( s_sPaletteQuery, (unsigned int)c );
					bTyped = true;
				}
			}
			// We are the SOLE consumer while the palette is open, so the
			// queue is cleared here rather than left for a widget below to
			// pick up the same characters a second time.
			io.InputQueueCharacters.resize( 0 );

			if ( ImGui::IsKeyPressed( ImGuiKey_Backspace, true ) )
			{
				if ( io.KeyCtrl )
					PopWord( s_sPaletteQuery );
				else
					PopUtf8( s_sPaletteQuery );
				bTyped = true;
			}

			if ( bTyped )
			{
				// Any edit re-ranks the list, so the old highlight index
				// points at a different setting. Returning to the top is the
				// only answer that cannot silently adjust the wrong row when
				// the next key is an arrow.
				s_nPaletteSel = 0;
				s_flPaletteCaretAt = (float)ImGui::GetTime();
			}
		}

		// SPEC §8.2's palette keys. Returns true when the palette consumed
		// the frame's keyboard, so the shell's own navigation stays quiet
		// underneath it.
		bool RunPaletteKeyboard( const std::vector<PaletteItem> &items )
		{
			if ( !s_bPaletteOpen )
				return false;

			ImGuiIO &io = ImGui::GetIO();

			if ( ImGui::IsKeyPressed( ImGuiKey_Escape, false ) )
			{
				s_bPaletteOpen = false;
				return true;
			}

			const int nCount = (int)items.size();
			if ( nCount > 0 )
			{
				if ( ImGui::IsKeyPressed( ImGuiKey_DownArrow, true ) )
					s_nPaletteSel = std::min( s_nPaletteSel + 1, nCount - 1 );
				if ( ImGui::IsKeyPressed( ImGuiKey_UpArrow, true ) )
					s_nPaletteSel = std::max( s_nPaletteSel - 1, 0 );

				s_nPaletteSel = std::clamp( s_nPaletteSel, 0, nCount - 1 );
				const PaletteItem &sel = items[ (size_t)s_nPaletteSel ];

				// ADJUST IN PLACE. The palette is not only a jump list --
				// this is the half that makes a known setting changeable
				// without leaving the search results at all.
				const int nDir = ImGui::IsKeyPressed( ImGuiKey_RightArrow, true ) ? +1
				               : ImGui::IsKeyPressed( ImGuiKey_LeftArrow, true )  ? -1 : 0;
				if ( nDir != 0 )
				{
					// The SAME adjuster the Sheet's arrow keys use, so a
					// value cannot step differently in the two hosts.
					if ( const Entry *pE = Reg().FindEntry( sel.sId ) )
						AdjustValue( Adjustable::Of( *pE ), nDir, io.KeyShift );
					else if ( const Parameter *pP = Reg().FindParam( sel.sId ) )
						AdjustValue( Adjustable::Of( *pP ), nDir, io.KeyShift );
				}

				if ( ImGui::IsKeyPressed( ImGuiKey_Enter, false ) ||
				     ImGui::IsKeyPressed( ImGuiKey_KeypadEnter, false ) )
				{
					PaletteJump( sel.sId );
					return true;
				}
			}

			PaletteConsumeInput();
			return true;
		}

		// The palette's own value readout. A control that self-displays
		// (Choice, Text, Bank) still needs a string here, because the palette
		// has no room to draw the control itself -- so this resolves a
		// Choice to its option LABEL rather than printing the raw int, which
		// would be the one place in the product a user sees a magic number.
		std::string PaletteValueText( const std::string &sId )
		{
			const Entry     *pE = Reg().FindEntry( sId );
			const Parameter *pP = pE ? nullptr : Reg().FindParam( sId );
			if ( !pE && !pP )
				return {};

			const Kind eKind = pE ? pE->GetKind() : pP->GetKind();
			if ( eKind == Kind::Action )
				return {};
			// A composite's value is its RESOLVED value -- `top-right ·
			// 32 / 32`, `#6ED274` -- which the band already computes. Reading
			// its A-axis binding straight out printed the raw int (`0` for an
			// anchor, `8116985` for a colour), so the palette showed one
			// thing and the row it jumps to showed another.
			if ( eKind == Kind::Composite && pE )
				return CompositeValue( *pE );
			if ( eKind == Kind::Facts )
				return pE ? pE->SummaryText() : std::string();
			// A Meter's value is its scalar; it has no binding to fall
			// through to, so without this the palette printed nothing for
			// the one row whose whole point is a live number.
			if ( eKind == Kind::Meter && pE )
				return MeterValue( *pE );

			const AnyBind &bind = pE ? pE->Binding() : pP->Binding();
			if ( !bind.IsBound() )
				return {};

			const Value v = bind.Get();
			if ( eKind == Kind::Choice )
			{
				const std::vector<Option> &opts = pE ? pE->Options() : pP->Options();
				if ( const int *p = std::get_if<int>( &v ) )
					for ( const Option &o : opts )
						if ( o.nValue == *p && o.pszLabel )
							return o.pszLabel;
			}

			std::string s = ValueToString( v );
			const std::string &sUnit = pE ? pE->Unit() : pP->Unit();
			if ( !s.empty() && !sUnit.empty() )
				s += " " + sUnit;
			return s;
		}

		// The panel itself. index.html's `.pal`: 820 wide, anchored 14% down
		// the slab, a 52-tall query line, a scrolling result list of 38-tall
		// rows and a 36-tall legend.
		// =================================================================
		//  The open dropdown's list (D18)
		// =================================================================
		// Drawn from the shell's own draw list in a sibling window opened
		// AFTER the slab's, for the two reasons the palette records at its
		// own call site and one more that is specific to popups:
		//
		//   1. z-order. The rows live in a CHILD window (`##sheetrows`,
		//      `##inspbody`), and a child's draw list is emitted after its
		//      parent's regardless of fill order -- so a list painted into
		//      the slab's list renders UNDERNEATH the very row it drops from.
		//   2. focus. ImGui closes an open popup whose parent window is not
		//      the focused one, and the slab carries NoBringToFrontOnFocus
		//      specifically so it never comes forward. An ImGui popup here
		//      was therefore opened and closed on the same frame, forever.
		//
		// So the list is ours: our rect, our hit test, our keyboard. The
		// keyboard state it draws (s_nPopupFocus) is the same state
		// RunKeyboard() moves -- there is no second copy and no parallel
		// path, which is the property that lets the console drive it too.
		void DrawDropdownList( const Rect &rcSlab )
		{
			if ( s_sOpenDropdown.empty() )
				return;

			// Self-heal: the row that owns this dropdown was not drawn this
			// frame -- the user navigated to another area, or a dynamic area
			// rebuilt underneath it. Leaving the state set would leave the
			// keyboard captured by a list that is not on screen, which is the
			// worst of the failure modes this whole commit is closing.
			if ( s_pDropdownOptions == nullptr || s_pDropdownOptions->empty() )
			{
				s_sOpenDropdown.clear();
				s_nPopupFocus = -1;
				return;
			}

			const std::vector<Option> &opts = *s_pDropdownOptions;
			const float flRowH = Px( 34.0f );
			const float flPadY = Px( tok::kXS );

			// Right-aligned under the control, because the control is
			// right-aligned in its lane and a list that dropped from the
			// row's LEFT edge would not line up with the thing it came from.
			float flW = Px( 180.0f );
			for ( const Option &o : opts )
				flW = std::max( flW, MeasureText( TypeRole::Value,
					o.pszLabel ? o.pszLabel : "" ).x + Px( tok::kXL ) * 2.0f );
			flW = std::min( flW, s_rcDropdownAnchor.Width() );

			const float flH = flRowH * (float)opts.size() + flPadY * 2.0f;
			const float x1  = std::min( s_rcDropdownAnchor.x1, rcSlab.x1 - Px( tok::kS ) );
			const float x0  = x1 - flW;

			// Flip above the row when there is no room below it -- a list
			// that runs off the bottom of the slab is the same "the control
			// is there but you cannot reach it" failure the drawer had.
			float y0 = s_rcDropdownAnchor.y1;
			if ( y0 + flH > rcSlab.y1 - Px( tok::kS ) )
				y0 = std::max( rcSlab.y0, s_rcDropdownAnchor.y0 - flH );

			const Rect rc { x0, y0, x1, y0 + flH };

			ImGui::SetNextWindowPos( ImVec2( rcSlab.x0, rcSlab.y0 ) );
			ImGui::SetNextWindowSize( ImVec2( rcSlab.Width(), rcSlab.Height() ) );
			ImGui::SetNextWindowFocus();
			ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
			ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );

			const ImGuiWindowFlags flags =
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
				ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoBackground;

			if ( ImGui::Begin( "##e2dropdown", nullptr, flags ) )
			{
				// Two coats, as the palette does: Role::Surface is the slab's
				// own translucency, which is right for a region and wrong for
				// a list that must be read against the rows behind it.
				Fill( rc, Col( Role::Surface ) );
				Fill( rc, Col( Role::Surface ) );
				ImGui::GetWindowDrawList()->AddRect(
					ImVec2( rc.x0, rc.y0 ), ImVec2( rc.x1, rc.y1 ),
					Col( Role::AccentBase ), 0.0f, 0, Hairline() );

				for ( size_t i = 0; i < opts.size(); ++i )
				{
					const Rect rcItem { rc.x0, rc.y0 + flPadY + flRowH * (float)i,
					                    rc.x1, rc.y0 + flPadY + flRowH * (float)( i + 1 ) };

					ImGui::SetCursorScreenPos( ImVec2( rcItem.x0, rcItem.y0 ) );
					ImGui::PushID( (int)i );
					const bool bClicked = ImGui::InvisibleButton( "##opt",
						ImVec2( rcItem.Width(), rcItem.Height() ) );
					const bool bHovered = ImGui::IsItemHovered();
					ImGui::PopID();

					// The keyboard's highlight wins once it exists; before
					// that the CURRENT value is marked. Exactly one row is
					// marked either way -- see RunKeyboard's popup rung.
					const bool bMarked = s_nPopupFocus >= 0
						? ( (int)i == s_nPopupFocus )
						: ( opts[ i ].nValue == s_nDropdownValue );

					if ( bMarked )
						Fill( rcItem, Accent( 0.16f ) );
					else if ( bHovered )
						Fill( rcItem, palette::White( 0.06f ) );

					Label( { rcItem.x0 + Px( tok::kM ), rcItem.y0, rcItem.x1 - Px( tok::kM ), rcItem.y1 },
					       TypeRole::Value,
					       bMarked ? Col( Role::AccentSeg ) : Col( Role::TextPrimary ),
					       opts[ i ].pszLabel ? opts[ i ].pszLabel : "", TextAlign::Right );

					if ( bClicked )
					{
						if ( const Entry *pE = Reg().FindEntry( s_sOpenDropdown ) )
							pE->Binding().Set( Value{ opts[ i ].nValue } );
						else if ( const Parameter *pP = Reg().FindParam( s_sOpenDropdown ) )
							pP->Binding().Set( Value{ opts[ i ].nValue } );
						s_sOpenDropdown.clear();
						s_nPopupFocus = -1;
					}
				}

				// A click anywhere else dismisses, which is what every
				// dropdown everywhere does and what the ImGui popup used to
				// give for free.
				if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) &&
				     !rc.Contains( ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y ) &&
				     !s_rcDropdownAnchor.Contains( ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y ) )
				{
					s_sOpenDropdown.clear();
					s_nPopupFocus = -1;
				}
			}
			ImGui::End();
			ImGui::PopStyleVar( 2 );
		}

		// D25: can the highlighted result be stepped from the list itself, or
		// does it need the full overlay? One lookup, used by the legend, by
		// the row's own affordance and by the mouse, so the three cannot
		// disagree about a row.
		bool PaletteItemAdjustable( const std::string &sId )
		{
			if ( const Entry *pE = Reg().FindEntry( sId ) )
				return CanAdjust( Adjustable::Of( *pE ) );
			if ( const Parameter *pP = Reg().FindParam( sId ) )
				return CanAdjust( Adjustable::Of( *pP ) );
			return false;
		}

		// D25: `rcFrame` is the rectangle the panel is centred inside, and
		// `bLauncher` says whether there is a shell behind it.
		//
		// The launcher passes the WHOLE SURFACE and suppresses the scrim. Both
		// follow from there being nothing behind it to dim or to be measured
		// against: a scrim with no shell under it is a hard-edged dark
		// rectangle floating on the game, and centring inside a slab that is
		// not drawn would put the launcher off-centre on screen for no visible
		// reason.
		void DrawPalette( const Rect &rcSlab, const std::vector<PaletteItem> &items,
		                  bool bLauncher )
		{
			const float flW    = std::min( Px( 820.0f ), ( rcSlab.x1 - rcSlab.x0 ) - Px( 2.0f * tok::kXL ) );
			const float flQH   = Px( 52.0f );
			const float flRowH = Px( 38.0f );
			const float flFootH= Px( 36.0f );
			const float flPad  = Px( 16.0f );

			// The list is capped at 60 rows, exactly as the mockup caps it.
			// Beyond that the answer is a better query, not more scrolling --
			// and it bounds the per-frame draw cost regardless of registry
			// size.
			const int nShown = std::min( (int)items.size(), 60 );

			const float x0 = rcSlab.x0 + ( ( rcSlab.x1 - rcSlab.x0 ) - flW ) * 0.5f;
			const float y0 = rcSlab.y0 + ( rcSlab.y1 - rcSlab.y0 ) * 0.14f;

			// HOW MANY ROWS ACTUALLY FIT, decided BEFORE the panel is sized.
			//
			// This used to take nine rows unconditionally and then clamp the
			// finished panel to the slab. The clamp moved rc.y1, and the
			// footer is drawn from rc.y1 -- so at 2.0x, where nine rows plus
			// the query line no longer fit, the footer slid up over the last
			// result row. The rows themselves were still laid out against
			// the unclamped flListH, so the two disagreed by exactly the
			// amount the clamp had removed.
			//
			// Deciding the row count from the space available makes the
			// panel's height a CONSEQUENCE of what it contains rather than
			// something trimmed afterwards, so there is nothing left to
			// clamp and the footer cannot be reached by the list.
			const float flAvailH = ( rcSlab.y1 - Px( tok::kM ) ) - y0;
			const int   nFits    = (int)std::floor( ( flAvailH - flQH - flFootH ) / flRowH );
			// At least one row: a palette showing none of its results is
			// worse than one that overlaps its legend.
			const int   nVisible = std::clamp( std::min( nShown, 9 ), 1, std::max( 1, nFits ) );

			const float flListH = nShown > 0 ? flRowH * (float)nVisible
			                                 : std::min( Px( 60.0f ), std::max( 0.0f, flAvailH - flQH - flFootH ) );

			const float flH = flQH + flListH + flFootH;
			const Rect  rc  = { x0, y0, x0 + flW, y0 + flH };

			ImDrawList *pDraw = ImGui::GetWindowDrawList();

			// A scrim, so the palette reads as ABOVE the shell rather than as
			// another region of it. It is also what makes the 2px accent
			// edges behind it stop competing for attention.
			//
			// D25: not in launcher mode. There is no shell to separate it
			// from, and dimming the game is the opposite of what a launcher is
			// for -- you are looking at the game while you change the setting.
			if ( !bLauncher )
			{
				pDraw->AddRectFilled( ImVec2( rcSlab.x0, rcSlab.y0 ), ImVec2( rcSlab.x1, rcSlab.y1 ),
				                      IM_COL32( 0, 0, 0, 150 ) );
			}

			// The panel is filled TWICE, deliberately. Role::Surface is
			// rgba(9,10,12,.88) -- the slab's own translucency, which is
			// correct for a region that wants the game behind it but wrong
			// for a modal list: at 88% the sheet's controls read straight
			// through the result rows and the two sets of text compete. Two
			// coats reach ~98.6% without inventing a colour token that exists
			// nowhere in SPEC §7.1.
			Fill( rc, Col( Role::Surface ) );
			Fill( rc, Col( Role::Surface ) );
			pDraw->AddRect( ImVec2( rc.x0, rc.y0 ), ImVec2( rc.x1, rc.y1 ), Accent( 0.42f ),
			                0.0f, 0, Hairline() );

			// ---- query line ------------------------------------------------
			// D18: the prompt is the mockup's magnifier again. P4 substituted
			// ">" because U+2315 falls outside Fonts.cpp's baked Latin-1
			// range -- correct at the time, but widening the range would not
			// have helped: no bundled Geist face carries U+2315 at all. It is
			// drawn instead, so it is immune to both.
			const Rect rcQ = { rc.x0, rc.y0, rc.x1, rc.y0 + flQH };
			glyph::Magnifier( ImVec2( rcQ.x0 + flPad + Px( tok::kGlyphSearch ) * 0.5f,
			                          ( rcQ.y0 + rcQ.y1 ) * 0.5f ),
			                  Px( tok::kGlyphSearch ), Col( Role::AccentValue ) );

			const float flQTextX = rcQ.x0 + flPad + Px( 26.0f );
			if ( s_sPaletteQuery.empty() )
			{
				Label( { flQTextX, rcQ.y0, rcQ.x1 - flPad, rcQ.y1 }, TypeRole::Value,
				       Col( Role::TextMeta ), "search every setting and parameter" );
			}
			else
			{
				Label( { flQTextX, rcQ.y0, rcQ.x1 - flPad, rcQ.y1 }, TypeRole::Value,
				       Col( Role::TextPrimary ), s_sPaletteQuery.c_str() );
			}

			// The caret. Solid for 500 ms after the last keystroke, then a
			// 1 Hz blink -- so it never blinks while you are typing.
			{
				const float flAge = (float)ImGui::GetTime() - s_flPaletteCaretAt;
				if ( flAge < 0.5f || std::fmod( flAge, 1.0f ) < 0.5f )
				{
					ImGui::PushFont( FontFor( TypeRole::Value ) );
					const float flTextW = s_sPaletteQuery.empty() ? 0.0f
						: MeasureText( TypeRole::Value, s_sPaletteQuery.c_str() ).x;
					ImGui::PopFont();
					const float flCx = flQTextX + flTextW + Px( 2.0f );
					const float flCh = Px( 20.0f );
					pDraw->AddRectFilled(
						ImVec2( flCx, ( rcQ.y0 + rcQ.y1 - flCh ) * 0.5f ),
						ImVec2( flCx + std::max( 1.0f, Px( 1.5f ) ), ( rcQ.y0 + rcQ.y1 + flCh ) * 0.5f ),
						Accent( 1.0f ) );
				}
			}

			// The count chip: what this index actually covers. It is the
			// Overview card's "0 unreachable" argument in the one place a
			// user is asking "is it in here?".
			{
				size_t nAreas = 0, nEntries = 0, nParams = 0;
				for ( size_t a = 0; a < Reg().AreaCount(); ++a )
				{
					const Area &area = Reg().AreaAt( a );
					if ( !area.Available() )
						continue;
					nAreas++;
					nEntries += area.EntryCount();
					for ( size_t i = 0; i < area.EntryCount(); ++i )
						nParams += area.EntryAt( i ).ParamCount();
				}
				char sz[ 96 ];
				snprintf( sz, sizeof( sz ), "%zu areas · %zu settings · %zu params",
					nAreas, nEntries, nParams );
				Label( { rcQ.x0, rcQ.y0, rcQ.x1 - flPad, rcQ.y1 }, TypeRole::Meta,
				       Col( Role::TextMeta ), sz, TextAlign::Right );
			}
			HLine( rc.x0, rc.x1, rcQ.y1, Col( Role::Line ) );

			// ---- results ---------------------------------------------------
			const Rect rcList = { rc.x0, rcQ.y1, rc.x1, rcQ.y1 + flListH };

			// D25: THE PALETTE IS NOW CLICKABLE.
			//
			// It stays keyboard-DRIVEN -- it is a launcher, and the user said
			// so outright -- but "keyboard-driven" was being used to mean "has
			// no pointer contract at all", and the mouse works now. So every
			// key the legend advertises has a pointer equivalent here and
			// nothing else does: click a row to highlight it, click a chevron
			// to step it, click `open` to go to it in the full overlay.
			//
			// Hit-tested against our own rects rather than through
			// InvisibleButton, for the same reason DrawDropdownList is: the
			// chevrons sit INSIDE the row's own rect, and two overlapping
			// ImGui items need overlap flags and a submission order to
			// disambiguate, where two rect tests need neither. The pointer
			// state is read once, here, and applied after the loop so no
			// action mutates the list it is being chosen from.
			const ImVec2 vMouse  = ImGui::GetIO().MousePos;
			const bool   bClick  = ImGui::IsMouseClicked( ImGuiMouseButton_Left );
			int  nClickRow  = -1;   // row the pointer picked
			int  nClickStep = 0;    // -1 / +1 from a chevron
			bool bClickOpen = false;

			if ( nShown == 0 )
			{
				Label( { rcList.x0 + flPad, rcList.y0, rcList.x1 - flPad, rcList.y1 },
				       TypeRole::Meta, Col( Role::TextMeta ),
				       "nothing matches. Every setting and every parameter is in this index." );
			}
			else
			{
				// Scroll the window of visible rows so the highlight is
				// always inside it -- the list has no scrollbar and no
				// pointer contract, so this is the only thing keeping the
				// selection on screen.
				int nFirst = 0;
				if ( s_nPaletteSel >= nVisible )
					nFirst = s_nPaletteSel - nVisible + 1;
				nFirst = std::clamp( nFirst, 0, std::max( 0, nShown - nVisible ) );

				for ( int i = 0; i < nVisible; i++ )
				{
					const int nIdx = nFirst + i;
					if ( nIdx >= nShown )
						break;
					const PaletteItem &it = items[ (size_t)nIdx ];

					const Rect rcRow = { rcList.x0, rcList.y0 + flRowH * (float)i,
					                     rcList.x1, rcList.y0 + flRowH * (float)( i + 1 ) };
					const bool bOn    = ( nIdx == s_nPaletteSel );
					const bool bHover = rcRow.Contains( vMouse.x, vMouse.y );
					if ( bOn )
					{
						Fill( rcRow, Accent( 0.14f ) );
						Fill( { rcRow.x0, rcRow.y0, rcRow.x0 + Px( 2.0f ), rcRow.y1 },
						      Col( Role::AccentBase ) );
					}
					else if ( bHover )
					{
						// The same 6% wash the dropdown's hover uses, so
						// "the pointer is on this" reads identically
						// everywhere in the shell.
						Fill( rcRow, palette::White( 0.06f ) );
					}

					// D5: the path column is the CONFIG KEY. See SPEC §2.6 --
					// it is what a reviewer checks against the on-disk JSON,
					// and an area id would print a prefix that does not exist
					// there for four of eleven areas.
					//
					// 260 rather than the mockup's 210: a real Param key
					// (`image.shaders.adaptive_brightness.down_speed`) is
					// longer than any key the mockup's demo registry holds,
					// and a truncated key is worth less than no key -- the
					// whole point of showing it is that it can be checked
					// against the file.
					const float flPathW = Px( 260.0f );
					Label( { rcRow.x0 + flPad, rcRow.y0, rcRow.x0 + flPad + flPathW, rcRow.y1 },
					       TypeRole::Meta, Col( Role::TextMeta ), it.sPath.c_str() );

					const float flValW  = Px( 120.0f );
					const float flChipW = it.bParam ? Px( 52.0f ) : 0.0f;

					// D25: the action zone -- the pointer's half of the
					// legend. Its width is reserved on EVERY row, not only
					// the one that draws it, so the label column does not
					// reflow as the highlight moves down the list.
					const float flActW = Px( 62.0f );
					const float flActR = rcRow.x1 - flPad - flValW - Px( tok::kS );
					const Rect  rcAct  = { flActR - flActW, rcRow.y0, flActR, rcRow.y1 };

					// The chip sits between the label and the value with a
					// gap on both sides, so a wide value cannot collide with
					// it the way a shared edge would let it.
					const float flChipR = rcAct.x0 - Px( tok::kS );
					Label( { rcRow.x0 + flPad + flPathW + Px( tok::kM ), rcRow.y0,
					         flChipR - flChipW - Px( tok::kS ), rcRow.y1 },
					       TypeRole::Label,
					       bOn ? Col( Role::TextPrimary ) : Col( Role::TextLabel ),
					       it.sLabel.c_str() );

					// The `param` chip is load-bearing for the
					// anti-junk-drawer argument (API.md §10): it is the
					// visible proof that a setting in the Inspector is
					// indexed exactly like a Sheet row.
					if ( it.bParam )
					{
						const Rect rcChip = { flChipR - flChipW, rcRow.y0 + Px( 9.0f ),
						                      flChipR, rcRow.y1 - Px( 9.0f ) };
						pDraw->AddRectFilled( ImVec2( rcChip.x0, rcChip.y0 ), ImVec2( rcChip.x1, rcChip.y1 ),
						                      Accent( 0.16f ) );
						Label( rcChip, TypeRole::Meta, Col( Role::AccentText ), "param", TextAlign::Center );
					}

					const std::string sVal = PaletteValueText( it.sId );
					if ( !sVal.empty() )
					{
						Label( { rcRow.x1 - flPad - flValW, rcRow.y0, rcRow.x1 - flPad, rcRow.y1 },
						       TypeRole::Value, Col( Role::AccentValue ), sVal.c_str(), TextAlign::Right );
					}

					// D25: the action zone, drawn for the row the pointer or
					// the keyboard is on. Two chevrons when the row can be
					// stepped from here, an `open` when it cannot -- which is
					// the whole answer to "what does a result you can't
					// adjust in place do", made visible on the row itself
					// rather than left for the user to discover by pressing
					// an arrow and watching nothing happen.
					if ( bOn || bHover )
					{
						const bool  bAdj = PaletteItemAdjustable( it.sId );
						const ImU32 colA = bOn ? Col( Role::AccentValue ) : Col( Role::TextMeta );

						if ( bAdj )
						{
							const float flHalf = rcAct.Width() * 0.5f;
							const Rect  rcDec { rcAct.x0, rcAct.y0, rcAct.x0 + flHalf, rcAct.y1 };
							const Rect  rcInc { rcAct.x0 + flHalf, rcAct.y0, rcAct.x1, rcAct.y1 };
							const float flCy = ( rcAct.y0 + rcAct.y1 ) * 0.5f;

							const bool bHovDec = rcDec.Contains( vMouse.x, vMouse.y );
							const bool bHovInc = rcInc.Contains( vMouse.x, vMouse.y );
							if ( bHovDec ) Fill( rcDec, palette::White( 0.08f ) );
							if ( bHovInc ) Fill( rcInc, palette::White( 0.08f ) );

							glyph::Chevron( ImVec2( ( rcDec.x0 + rcDec.x1 ) * 0.5f, flCy ),
							                Px( 14.0f ), glyph::Dir::Left, colA );
							glyph::Chevron( ImVec2( ( rcInc.x0 + rcInc.x1 ) * 0.5f, flCy ),
							                Px( 14.0f ), glyph::Dir::Right, colA );

							if ( bClick && bHovDec ) { nClickRow = nIdx; nClickStep = -1; }
							if ( bClick && bHovInc ) { nClickRow = nIdx; nClickStep = +1; }
						}
						else
						{
							if ( rcAct.Contains( vMouse.x, vMouse.y ) )
								Fill( rcAct, palette::White( 0.08f ) );
							Label( rcAct, TypeRole::Meta, colA, "open", TextAlign::Center );
							if ( bClick && rcAct.Contains( vMouse.x, vMouse.y ) )
							{
								nClickRow  = nIdx;
								bClickOpen = true;
							}
						}
					}

					// A click anywhere else on the row just moves the
					// highlight. Deliberately NOT "click to activate": the
					// launcher's headline behaviour is adjusting in place,
					// and a click that teleported you into the full overlay
					// would make the mouse the one input that cannot use it.
					if ( bClick && bHover && nClickRow != nIdx )
						nClickRow = nIdx;
				}
			}

			// D25: the pointer's actions, applied after the list has been
			// drawn -- PaletteJump() rebuilds the shell's selection and would
			// otherwise run while the loop was still walking `items`.
			if ( nClickRow >= 0 )
			{
				s_nPaletteSel = std::clamp( nClickRow, 0, nShown - 1 );
				const std::string sId = items[ (size_t)s_nPaletteSel ].sId;

				if ( nClickStep != 0 )
				{
					// The SAME adjuster the arrow keys and the sheet use.
					if ( const Entry *pE = Reg().FindEntry( sId ) )
						AdjustValue( Adjustable::Of( *pE ), nClickStep, ImGui::GetIO().KeyShift );
					else if ( const Parameter *pP = Reg().FindParam( sId ) )
						AdjustValue( Adjustable::Of( *pP ), nClickStep, ImGui::GetIO().KeyShift );
				}
				else if ( bClickOpen )
				{
					PaletteJump( sId );
				}
			}
			// D25: in launcher mode a click on the game AROUND the panel is a
			// dismissal, the way clicking off any launcher is. Not done when
			// there is a shell behind it -- there the click belongs to the
			// shell's own surface, and swallowing it would be a second,
			// invisible meaning for a click on a row.
			//
			// GUARDED ON THE POINTER ACTUALLY BEING SOMEWHERE. ImGui's MousePos
			// is (-FLT_MAX, -FLT_MAX) until the first motion event, and that
			// position is "outside the panel" by any rect test -- so without
			// this the FIRST click of a session dismissed the launcher no
			// matter where it was aimed. Caught in the acceptance run: a click
			// injected straight after opening, with no motion before it, closed
			// the launcher instead of hitting the row under the cursor.
			else if ( bLauncher && bClick &&
			          vMouse.x >= rcSlab.x0 && vMouse.x <= rcSlab.x1 &&
			          vMouse.y >= rcSlab.y0 && vMouse.y <= rcSlab.y1 &&
			          !rc.Contains( vMouse.x, vMouse.y ) )
			{
				s_bPaletteOpen = false;
			}

			// ---- legend ----------------------------------------------------
			// D25: the legend describes THE HIGHLIGHTED ROW, not the palette
			// in the abstract. A fixed legend promising "left/right adjust in
			// place" over a row that cannot be adjusted is worse than no
			// legend: it is an instruction that does nothing, which is the
			// same defect class as a control that renders and does nothing.
			//
			// Esc's wording changes too, because in launcher mode Esc gives
			// the GAME back rather than uncovering a shell.
			const Rect rcFoot = { rc.x0, rc.y1 - flFootH, rc.x1, rc.y1 };
			HLine( rc.x0, rc.x1, rcFoot.y0, Col( Role::Line ) );

			const bool bSelAdjustable =
				nShown > 0 && s_nPaletteSel >= 0 && s_nPaletteSel < nShown &&
				PaletteItemAdjustable( items[ (size_t)s_nPaletteSel ].sId );

			std::string sLegend = "up/down move    ";
			if ( bSelAdjustable )
				sLegend += "left/right adjust in place    Enter jump & select    ";
			else if ( nShown > 0 )
				sLegend += "Enter open in the full overlay    ";
			sLegend += bLauncher ? "Esc back to the game" : "Esc dismiss";

			Label( { rcFoot.x0 + flPad, rcFoot.y0, rcFoot.x1 - flPad, rcFoot.y1 },
			       TypeRole::Meta, Col( Role::TextMeta ), sLegend.c_str() );
		}

		// =================================================================
		//  Keyboard (SPEC §8.2)
		// =================================================================
		// The rail's visible items, in drawn order. The rail skips
		// unavailable areas, so Ctrl+Left/Right must walk the same filtered
		// list the eye sees -- stepping the registry's raw index would skip
		// over a hidden area and look like a dropped keypress.
		std::vector<const Area *> VisibleAreas()
		{
			std::vector<const Area *> out;
			for ( size_t i = 0; i < Reg().AreaCount(); ++i )
				if ( Reg().AreaAt( i ).Available() )
					out.push_back( &Reg().AreaAt( i ) );
			return out;
		}

		void StepArea( int nDir )
		{
			const std::vector<const Area *> areas = VisibleAreas();
			if ( areas.empty() )
				return;
			int nAt = 0;
			for ( size_t i = 0; i < areas.size(); i++ )
				if ( areas[ i ]->Id() == s_sSelectedArea )
					nAt = (int)i;
			const int nNext = std::clamp( nAt + nDir, 0, (int)areas.size() - 1 );
			if ( nNext == nAt )
				return;
			s_sSelectedArea = areas[ (size_t)nNext ]->Id();
			// The selection cannot survive an area change (SelectedEntry()
			// checks by identity against the current area), so clear it
			// rather than leave a stale id behind.
			Select( nullptr );
		}

		// Move the selection within the current sheet. A composite and a
		// Facts row are both selectable -- selecting a Facts row is the only
		// way to reach Details -- so nothing is skipped here.
		void StepRow( int nDir )
		{
			const Area *pArea = SelectedArea();
			if ( !pArea || pArea->EntryCount() == 0 )
				return;

			const Entry *pSel = SelectedEntry();
			int nAt = -1;
			for ( size_t i = 0; i < pArea->EntryCount(); ++i )
				if ( &pArea->EntryAt( i ) == pSel )
					nAt = (int)i;

			// From no selection, Down lands on the first row and Up on the
			// last -- so a fresh sheet is enterable from either key.
			const int nNext = nAt < 0
				? ( nDir > 0 ? 0 : (int)pArea->EntryCount() - 1 )
				: std::clamp( nAt + nDir, 0, (int)pArea->EntryCount() - 1 );
			Select( &pArea->EntryAt( (size_t)nNext ) );
		}

		// SPEC §3.12's bank, driven by SPEC §8.2's keys. One implementation,
		// shared by the sheet and the Inspector, so a bank cannot behave
		// differently in its two hosts -- the same rule DrawSharedControl
		// exists for. Returns true when it consumed the press.
		template <typename TDecl>
		bool RunBankKeyboard( const TDecl &decl, bool bActivate, int nDir )
		{
			const size_t nOpts = decl.Options().size();
			if ( decl.GetKind() != Kind::Bank || nOpts == 0 || !decl.Binding().IsBound() )
				return false;

			s_nBankChip = std::clamp( s_nBankChip, 0, (int)nOpts - 1 );

			if ( nDir != 0 )
			{
				// Stops at both ends rather than wrapping -- D16.6's rule for
				// a choice, and the same reason: a held key has to settle.
				const int nNext = std::clamp( s_nBankChip + nDir, 0, (int)nOpts - 1 );
				if ( nNext == s_nBankChip )
					return false;   // at an end: let the caller treat it as a region edge
				s_nBankChip = nNext;
				return true;
			}

			if ( bActivate )
			{
				const Value v = decl.Binding().Get();
				const uint32_t nMask = (uint32_t)( std::holds_alternative<int>( v ) ? std::get<int>( v ) : 0 );
				const uint32_t nBit  = 1u << (uint32_t)decl.Options()[ (size_t)s_nBankChip ].nValue;
				decl.Binding().Set( Value{ (int)( nMask ^ nBit ) } );
				return true;
			}
			return false;
		}

		void RunKeyboard()
		{
			const ImGuiIO &io = ImGui::GetIO();

			// Ctrl+K: the command palette. Checked FIRST and before the
			// palette's own handler, so it also works while the palette is
			// already open (re-opening clears the query, which is what every
			// palette in every editor does).
			if ( io.KeyCtrl && ImGui::IsKeyPressed( ImGuiKey_K, false ) )
			{
				OpenPalette();
				return;
			}

			// While the palette is open it owns the keyboard entirely --
			// including plain character keys, which are query text rather
			// than shell shortcuts. Handled by the caller, which already has
			// the built item list; nothing below may run underneath it.
			if ( s_bPaletteOpen )
				return;

			// D18: an OPEN DROPDOWN owns the keyboard next, for the same
			// reason the palette does -- it is a list on top of the shell,
			// and an arrow key belongs to the thing in front.
			//
			// Why this is hand-driven rather than ImGui's popup nav: the
			// overlay never sets ImGuiConfigFlags_NavEnableKeyboard, so
			// BeginPopup() gets no nav focus and Selectable() never becomes
			// keyboard-reachable. Turning nav on globally would hand every
			// arrow key in the shell to ImGui's navigation and take the
			// adjust grammar of SPEC §8.2 away from the rows -- the same
			// trade D16.3 refused for InputText, refused again here.
			if ( !s_sOpenDropdown.empty() )
			{
				const Entry     *pE = Reg().FindEntry( s_sOpenDropdown );
				const Parameter *pP = pE ? nullptr : Reg().FindParam( s_sOpenDropdown );
				const std::vector<Option> *pOpts =
					pE ? &pE->Options() : pP ? &pP->Options() : nullptr;

				if ( !pOpts || pOpts->empty() )
				{
					s_sOpenDropdown.clear();
					s_nPopupFocus = -1;
					return;
				}

				if ( ImGui::IsKeyPressed( ImGuiKey_Escape, false ) )
				{
					s_sOpenDropdown.clear();
					s_nPopupFocus = -1;
					return;
				}

				const int nCount = (int)pOpts->size();
				const int nStep  = ImGui::IsKeyPressed( ImGuiKey_DownArrow, true ) ? +1
				                 : ImGui::IsKeyPressed( ImGuiKey_UpArrow,   true ) ? -1 : 0;
				if ( nStep != 0 )
				{
					// First arrow lands on the CURRENT value rather than
					// jumping off it, so "open, press Down, press Enter"
					// moves by exactly one -- which is what the same gesture
					// does in the sheet.
					if ( s_nPopupFocus < 0 )
					{
						const Value v = pE ? pE->Binding().Get() : pP->Binding().Get();
						const int nNow = std::holds_alternative<int>( v ) ? std::get<int>( v ) : 0;
						s_nPopupFocus = 0;
						for ( int i = 0; i < nCount; ++i )
							if ( ( *pOpts )[ i ].nValue == nNow )
								s_nPopupFocus = i;
					}
					else
					{
						// Stops at both ends rather than wrapping -- D16.6's
						// rule for a choice, applied to the same choice in
						// its other host.
						s_nPopupFocus = std::clamp( s_nPopupFocus + nStep, 0, nCount - 1 );
					}
					return;
				}

				if ( ImGui::IsKeyPressed( ImGuiKey_Enter, false ) ||
				     ImGui::IsKeyPressed( ImGuiKey_KeypadEnter, false ) ||
				     ImGui::IsKeyPressed( ImGuiKey_Space, false ) )
				{
					if ( s_nPopupFocus >= 0 && s_nPopupFocus < nCount )
					{
						const Value vNew{ ( *pOpts )[ s_nPopupFocus ].nValue };
						if ( pE ) pE->Binding().Set( vNew );
						else      pP->Binding().Set( vNew );
					}
					s_sOpenDropdown.clear();
					s_nPopupFocus = -1;
					return;
				}
				return;   // the popup swallows everything else
			}

			// D18: Ctrl+/ or ? -- "Configure + Details for the selected row
			// (full-sheet page when the Inspector is hidden)", SPEC §8.2 and
			// §6.3. The page is drawn whatever the host is; with a column or
			// a drawer up it is the same two bodies in a wider place, which
			// is strictly more readable rather than a different answer.
			//
			// Both spellings are accepted because "?" is Shift+Slash only on
			// some layouts, and this shell reads keys through ImGui's
			// physical key enum -- so the layout-independent binding is the
			// Ctrl one and the "?" is the convenience.
			if ( ImGui::IsKeyPressed( ImGuiKey_Slash, false ) &&
			     ( io.KeyCtrl || io.KeyShift ) )
			{
				if ( SelectedEntry() )
					s_bExplainPage = !s_bExplainPage;
				return;
			}

			// Ctrl+I: "cycle Inspector host: column -> drawer -> hidden".
			if ( io.KeyCtrl && ImGui::IsKeyPressed( ImGuiKey_I, false ) )
			{
				SetHost(
					Host() == InspectorHost::Column ? InspectorHost::Drawer :
					Host() == InspectorHost::Drawer ? InspectorHost::Hidden :
					                                  InspectorHost::Column );
				return;
			}

			// Ctrl+Left/Right: "previous / next rail item without leaving the
			// Sheet" (SPEC §8.2). Tested before the bare arrows below, which
			// would otherwise eat the same press as an adjust.
			if ( io.KeyCtrl && ImGui::IsKeyPressed( ImGuiKey_LeftArrow, true ) )
			{
				StepArea( -1 );
				return;
			}
			if ( io.KeyCtrl && ImGui::IsKeyPressed( ImGuiKey_RightArrow, true ) )
			{
				StepArea( +1 );
				return;
			}

			// Ctrl+D: "reset selected row AND its parameters to default".
			if ( io.KeyCtrl && ImGui::IsKeyPressed( ImGuiKey_D, false ) )
			{
				if ( const Entry *pE = SelectedEntry() )
				{
					pE->ResetToDefault();
					for ( size_t i = 0; i < pE->ParamCount(); ++i )
						pE->ParamAt( i ).ResetToDefault();
				}
				return;
			}

			// Tab / Shift+Tab: cycle region Rail -> Sheet -> Inspector.
			// Only when no item is active, so Tab inside a text field still
			// belongs to the text field.
			//
			// D22: Tab and the arrow keys are NAVIGATION -- moving a selection
			// around -- and they are the part settings_overlay_keyboard_nav
			// governs. COMMANDS (Esc, the palette, the Inspector host, reset,
			// the explain page) stay unconditional, because a command has no
			// mouse equivalent to fall back to and Esc in particular must
			// never become unreachable. The sheet, rail and inspector are a
			// mouse UI; this is the accessibility route to the same controls,
			// kept on by default and now switchable rather than silently
			// fighting ImGui's own nav for the same keys.
			if ( SettingsOverlay_IsShellKeyboardNavEnabled() &&
			     !io.KeyCtrl && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed( ImGuiKey_Tab, false ) )
			{
				const int n = (int)s_eFocusRegion + ( io.KeyShift ? 2 : 1 );
				s_eFocusRegion = (Region)( n % 3 );
				return;
			}

			// =============================================================
			//  Esc (D26): dismiss what is ON TOP, or CLOSE THE OVERLAY
			// =============================================================
			// THE RULE, in one line: Esc closes the UI unless something
			// transient is up in front of it, in which case Esc takes that
			// away and the UI stays.
			//
			// WHAT CHANGED AND WHY. SPEC §8.2's ladder was "palette ->
			// drawer -> inline expansion -> overlay", and it made Esc a
			// general UNDO of the last navigation: from a fresh open with a
			// row selected it took three presses to get back to the game,
			// and each one silently rearranged the shell instead. The user's
			// report is the whole argument -- "pressing escape should close
			// the UI" -- and it is the behaviour every other surface here
			// already has: the launcher (D25) gives the game straight back
			// on Esc, and the two really-on-top layers below already
			// consumed Esc and stopped.
			//
			// WHAT COUNTS AS "ON TOP", and it is a short list on purpose:
			//
			//   * the command palette      -- RunPaletteKeyboard(), above
			//   * an open dropdown popup   -- the block above
			//   * a text field being edited
			//   * an ARMED destructive action
			//
			// All four are things a user put in front of the shell seconds
			// ago and can point at. What is deliberately NOT on the list:
			// the drawer, the explain page, the inline expansion and the
			// selection. Those are the shell's own arrangement -- they
			// persist, they have their own controls (Ctrl+I, the back
			// crumb), and unwinding them one Esc at a time is exactly the
			// behaviour being removed. Esc from any of them closes the UI.
			if ( ImGui::IsKeyPressed( ImGuiKey_Escape, false ) )
			{
				// SPEC §3.9: "the second fires, and Esc disarms." This is
				// UNCONDITIONAL and happens before any branch below, because
				// an armed delete surviving an Esc is a bug that has already
				// been found here once: the arm outlived the press and could
				// fire on a later Enter. Esc is the user saying "no", so
				// after this line nothing is armed no matter which rung runs.
				const bool bWasArmed = !s_sArmedAction.empty();
				s_sArmedAction.clear();
				if ( bWasArmed )
					return;

				// A field mid-edit owns Esc: it means "cancel this rename",
				// never "throw the whole overlay away". ImGui deactivates and
				// reverts the InputText itself; the shell's one bit of
				// caller state has to be dropped with it or the row would
				// stay stuck in its editing form.
				if ( !s_sEditingText.empty() || ImGui::IsAnyItemActive() )
				{
					s_sEditingText.clear();
					ImGui::ClearActiveID();
					return;
				}

				// Nothing is in front. Close.
				CloseShell();
				return;
			}

			// ---- movement and adjustment ---------------------------------
			// D22: the other half of what settings_overlay_keyboard_nav
			// governs (Tab is gated above) -- everything below moves or
			// adjusts a selection, which is the navigation model the mouse
			// now leads. Esc and the commands are already handled above, so
			// turning navigation off never traps anyone in the overlay.
			if ( !SettingsOverlay_IsShellKeyboardNavEnabled() )
				return;

			// A text field being edited owns every key below it; without this
			// guard typing "5" into a profile name would also step whatever
			// row happened to be selected.
			if ( ImGui::IsAnyItemActive() || !s_sEditingText.empty() )
				return;

			if ( s_eFocusRegion == Region::Rail )
			{
				// In the rail, up/down IS the rail -- SPEC §8.2's "move
				// selection within the focused region".
				if ( ImGui::IsKeyPressed( ImGuiKey_DownArrow, true ) ) StepArea( +1 );
				if ( ImGui::IsKeyPressed( ImGuiKey_UpArrow,   true ) ) StepArea( -1 );
				// Right crosses into the sheet, which is the region edge
				// rule ("At a region edge: cross").
				if ( ImGui::IsKeyPressed( ImGuiKey_RightArrow, true ) )
					s_eFocusRegion = Region::Sheet;
				return;
			}

			// D18: the Inspector, which Tab could reach and nothing could
			// then drive. Everything below this block is the SHEET's
			// keyboard; without the early return the Inspector's arrows fell
			// through to it and moved the sheet selection instead -- which is
			// what "focused but unreachable" looked like from the outside.
			if ( s_eFocusRegion == Region::Inspector )
			{
				const Entry *pIn = SelectedEntry();
				if ( !pIn )
				{
					// Overview: nothing to focus. Left crosses back, so the
					// region is never a dead end you can only Tab out of.
					if ( ImGui::IsKeyPressed( ImGuiKey_LeftArrow, true ) )
						s_eFocusRegion = Region::Sheet;
					return;
				}

				const InspectorMode eMode = CurrentMode( pIn );

				// What is focusable right now. DETAILS holds readouts only,
				// so the strip is the only stop in it -- the index is clamped
				// rather than the keys being special-cased, so there is one
				// rule instead of two.
				const int nLast = ( eMode == InspectorMode::Configure && !pIn->ReadOnly() )
					? (int)pIn->ParamCount() : -1;
				s_nInspectorFocus = std::clamp( s_nInspectorFocus, -1, nLast );

				if ( ImGui::IsKeyPressed( ImGuiKey_DownArrow, true ) )
					s_nInspectorFocus = std::clamp( s_nInspectorFocus + 1, -1, nLast );
				if ( ImGui::IsKeyPressed( ImGuiKey_UpArrow, true ) )
					s_nInspectorFocus = std::clamp( s_nInspectorFocus - 1, -1, nLast );

				const int nDirIn = ImGui::IsKeyPressed( ImGuiKey_RightArrow, true ) ? +1
				                 : ImGui::IsKeyPressed( ImGuiKey_LeftArrow,  true ) ? -1 : 0;

				// ---- the mode strip ------------------------------------
				if ( s_nInspectorFocus < 0 )
				{
					if ( nDirIn != 0 )
					{
						// The strip is a two-cell segmented control, so
						// Left/Right pick a cell -- and, like every other
						// choice in the shell (D16.6), take their value from
						// the DIRECTION rather than toggling, so holding a
						// key settles instead of oscillating.
						s_eMode = nDirIn > 0 ? InspectorMode::Details : InspectorMode::Configure;
						s_bModeOverridden = true;
					}
					return;
				}

				// ---- a row: the entry itself (0) or a parameter (1..n) ---
				const bool bOwnRow = ( s_nInspectorFocus == 0 );
				const Parameter *pParam = bOwnRow ? nullptr
					: &pIn->ParamAt( (size_t)( s_nInspectorFocus - 1 ) );

				const std::string sReason = bOwnRow ? pIn->DisabledReason() : pParam->DisabledReason();
				const bool bBlocked = !sReason.empty() || !pIn->DisabledReason().empty();

				const bool bActivateIn = ImGui::IsKeyPressed( ImGuiKey_Space, false ) ||
				                         ImGui::IsKeyPressed( ImGuiKey_Enter, false ) ||
				                         ImGui::IsKeyPressed( ImGuiKey_KeypadEnter, false );
				if ( bActivateIn && !bBlocked )
				{
					const Kind eKind = bOwnRow ? pIn->GetKind() : pParam->GetKind();
					const AnyBind &bind = bOwnRow ? pIn->Binding() : pParam->Binding();

					if ( eKind == Kind::Bank )
					{
						if ( bOwnRow ) RunBankKeyboard( *pIn,    true, 0 );
						else           RunBankKeyboard( *pParam, true, 0 );
					}
					else if ( eKind == Kind::Switch && bind.IsBound() )
					{
						const Value v = bind.Get();
						if ( const bool *p = std::get_if<bool>( &v ) )
							bind.Set( Value{ !*p } );
					}
					else if ( eKind == Kind::Choice )
					{
						// Opening the dropdown from the keyboard is what
						// makes a downgraded Choice reachable here at all;
						// the popup's own keys are handled at the top of
						// this function. Only when it really drew as one --
						// the Inspector's narrow lane downgrades far more
						// often than the sheet's, but not always.
						const std::string sId = bOwnRow ? pIn->Id() : pParam->Id();
						if ( DrawsAsDropdown( sId ) )
						{
							s_sOpenDropdown = sId;
							s_nPopupFocus = -1;
						}
					}
					else if ( eKind == Kind::Text )
					{
						s_sEditingText = bOwnRow ? pIn->Id() : pParam->Id();
					}
					else if ( bOwnRow && eKind == Kind::Action )
					{
						if ( pIn->NeedsConfirm() && s_sArmedAction != pIn->Id() )
						{
							s_sArmedAction = pIn->Id();
							s_flArmedAt = (float)ImGui::GetTime();
						}
						else
						{
							pIn->Invoke();
							s_sArmedAction.clear();
						}
					}
					return;
				}

				if ( nDirIn != 0 && !bBlocked )
				{
					// The SAME adjuster the sheet and the palette use, over
					// the same Adjustable view -- D16.6's "one adjuster", now
					// with a third host rather than a third implementation.
					const Kind eDirKind = bOwnRow ? pIn->GetKind() : pParam->GetKind();
					const bool bMoved = eDirKind == Kind::Bank
						? ( bOwnRow ? RunBankKeyboard( *pIn,    false, nDirIn )
						            : RunBankKeyboard( *pParam, false, nDirIn ) )
						: bOwnRow
							? AdjustValue( Adjustable::Of( *pIn ), nDirIn, io.KeyShift )
							: AdjustValue( Adjustable::Of( *pParam ), nDirIn, io.KeyShift );

					// A kind with no ordered value (Text, Bank, Action) has
					// nothing to adjust, so Left there is a region edge --
					// "at a region edge: cross", SPEC §8.2.
					if ( !bMoved && nDirIn < 0 )
						s_eFocusRegion = Region::Sheet;
				}
				return;
			}

			// D20.3. Up/Down walk THROUGH an open inline expansion rather
			// than over it. SPEC §8.2 says the arrows "move selection within
			// the focused region", and once the params are drawn in the Sheet
			// they are in that region -- stepping straight past them would
			// make the expansion a display that the keyboard cannot enter,
			// which is the same defect as the pointer-only chip bank (§3.4)
			// in a project that cannot synthesise a pointer to compensate.
			{
				const Entry *pRow = SelectedEntry();
				const bool bExpanded = InlineMode() && pRow &&
				                       pRow->ParamCount() > 0 &&
				                       s_sExpandedEntry == pRow->Id();
				const int nLastParam = bExpanded ? (int)pRow->ParamCount() - 1 : -1;

				if ( ImGui::IsKeyPressed( ImGuiKey_DownArrow, true ) )
				{
					if ( bExpanded && s_nInlineFocus < nLastParam )
						s_nInlineFocus++;
					else
						StepRow( +1 );   // Select() collapses and resets the index
				}
				if ( ImGui::IsKeyPressed( ImGuiKey_UpArrow, true ) )
				{
					if ( bExpanded && s_nInlineFocus >= 0 )
						s_nInlineFocus--;
					else
						StepRow( -1 );
				}
			}

			const Entry *pSel = SelectedEntry();
			if ( !pSel )
				return;

			// ---- the keyboard is inside an expansion ---------------------
			// A focused param owns every key below, exactly as the Inspector's
			// focused param does -- and through the SAME adjuster and the same
			// bank helper, so a param cannot behave one way in the Inspector
			// and another in the Sheet. That is SPEC §6.3 clause 1 ("one code
			// path") holding for input as well as for drawing.
			if ( InlineMode() && s_sExpandedEntry == pSel->Id() &&
			     s_nInlineFocus >= 0 && s_nInlineFocus < (int)pSel->ParamCount() )
			{
				const Parameter &param = pSel->ParamAt( (size_t)s_nInlineFocus );

				// SPEC §3.13's inheritance, same OR of two predicates the
				// Inspector uses: a param under a disabled parent is disabled
				// EXCEPT when it is itself the cause.
				const bool bBlocked = !param.DisabledReason().empty() ||
				                      !pSel->DisabledReason().empty();

				const bool bActParam = ImGui::IsKeyPressed( ImGuiKey_Space, false ) ||
				                       ImGui::IsKeyPressed( ImGuiKey_Enter, false ) ||
				                       ImGui::IsKeyPressed( ImGuiKey_KeypadEnter, false );
				if ( bActParam && !bBlocked )
				{
					if ( param.GetKind() == Kind::Bank )
					{
						RunBankKeyboard( param, true, 0 );
					}
					else if ( param.GetKind() == Kind::Switch && param.Binding().IsBound() )
					{
						const Value v = param.Binding().Get();
						if ( const bool *p = std::get_if<bool>( &v ) )
							param.Binding().Set( Value{ !*p } );
					}
					else if ( param.GetKind() == Kind::Text )
					{
						s_sEditingText = param.Id();
					}
					else if ( param.GetKind() == Kind::Choice && DrawsAsDropdown( param.Id() ) )
					{
						s_sOpenDropdown = param.Id();
						s_nPopupFocus = -1;
					}
					return;
				}

				const int nDirP = ImGui::IsKeyPressed( ImGuiKey_RightArrow, true ) ? +1
				                : ImGui::IsKeyPressed( ImGuiKey_LeftArrow,  true ) ? -1 : 0;
				if ( nDirP != 0 && !bBlocked )
				{
					const bool bMoved = param.GetKind() == Kind::Bank
						? RunBankKeyboard( param, false, nDirP )
						: AdjustValue( Adjustable::Of( param ), nDirP, io.KeyShift );

					// A kind with nothing to adjust makes Left a way back out
					// of the expansion -- the same "at a region edge: cross"
					// rule the Inspector applies, with the expansion as the
					// thing being left.
					if ( !bMoved && nDirP < 0 )
						s_nInlineFocus = -1;
				}
				return;
			}

			// Space toggles a switch without leaving the row (SPEC §8.2), and
			// fires an Action. Enter does the same, so a user who never
			// learned which is which is not punished.
			const bool bActivate = ImGui::IsKeyPressed( ImGuiKey_Space, false ) ||
			                       ImGui::IsKeyPressed( ImGuiKey_Enter, false ) ||
			                       ImGui::IsKeyPressed( ImGuiKey_KeypadEnter, false );
			if ( bActivate && pSel->DisabledReason().empty() )
			{
				if ( pSel->GetKind() == Kind::Bank )
				{
					RunBankKeyboard( *pSel, true, 0 );
				}
				else if ( pSel->GetKind() == Kind::Switch && pSel->Binding().IsBound() )
				{
					const Value v = pSel->Binding().Get();
					if ( const bool *p = std::get_if<bool>( &v ) )
						pSel->Binding().Set( Value{ !*p } );
				}
				// D18: "Enter / Space -- activate / toggle / BEGIN ENTRY"
				// (SPEC §8.2). The last third was missing: controls::Text
				// owns the display/input transition but only ever entered it
				// from a click, so a text row was pointer-only -- and a
				// Choice that had downgraded to a dropdown could not be
				// opened at all without one.
				else if ( pSel->GetKind() == Kind::Text )
				{
					s_sEditingText = pSel->Id();
				}
				else if ( pSel->GetKind() == Kind::Choice && DrawsAsDropdown( pSel->Id() ) )
				{
					// Only when it ACTUALLY drew as a dropdown. A segmented
					// Choice is already adjustable with Left/Right and draws
					// no popup, so opening one would swallow the keyboard
					// with nothing on screen. See s_DropdownRows.
					s_sOpenDropdown = pSel->Id();
					s_nPopupFocus = -1;
				}
				else if ( pSel->GetKind() == Kind::Action )
				{
					// A destructive action still ARMS first -- the keyboard
					// must not be a route around Entry::Confirm()'s two-stage
					// arm, or the confirmation would be pointer-only.
					if ( pSel->NeedsConfirm() && s_sArmedAction != pSel->Id() )
					{
						s_sArmedAction = pSel->Id();
						s_flArmedAt = (float)ImGui::GetTime();
					}
					else
					{
						pSel->Invoke();
						s_sArmedAction.clear();
					}
				}
				// D20.3: SPEC §6.3 lists `Space` as the third way to expand,
				// alongside `->` and a click. It is LAST in this chain on
				// purpose -- it only fires when the row's own control had
				// nothing to do with the press, so Space still toggles a
				// switch and still fires an action, and expansion gets the
				// rows where Space would otherwise have done nothing at all.
				else if ( InlineMode() && pSel->ParamCount() > 0 )
				{
					if ( s_sExpandedEntry == pSel->Id() )
					{
						s_sExpandedEntry.clear();
						s_nInlineFocus = -1;
					}
					else
					{
						s_sExpandedEntry = pSel->Id();
					}
				}
				return;
			}

			// Left/Right adjust the focused control, through the SAME
			// function the palette uses -- so a slider cannot step by one
			// amount here and another there.
			const int nDir = ImGui::IsKeyPressed( ImGuiKey_RightArrow, true ) ? +1
			               : ImGui::IsKeyPressed( ImGuiKey_LeftArrow,  true ) ? -1 : 0;
			if ( nDir != 0 && pSel->DisabledReason().empty() )
			{
				const bool bMoved = pSel->GetKind() == Kind::Bank
					? RunBankKeyboard( *pSel, false, nDir )
					: AdjustValue( Adjustable::Of( *pSel ), nDir, io.KeyShift );

				// D20.3, and A SPEC AMBIGUITY RESOLVED. SPEC §8.2 gives
				// Left/Right three jobs at once -- "inside a control:
				// adjust. At a region edge: cross. On a row with depth:
				// expand / collapse (inline mode)" -- without saying which
				// wins on a row that is both, and most rows that own params
				// also own an adjustable control.
				//
				// ADJUSTING WINS; expansion takes what is left over. The
				// alternative -- expansion first -- would silently stop the
				// arrow keys from changing a slider the moment somebody gave
				// that slider a parameter, i.e. a registration change would
				// alter an unrelated row's keyboard. This way the precedence
				// is a property of the row's own kind, and it reuses the
				// "didn't move" signal the region-edge rule already runs on.
				//
				// A row whose control cannot be adjusted (Facts, Text, Bank
				// at an end, Action) therefore expands on the first Right,
				// which is the common case for a row that owns params.
				// Everything else still reaches its params through the
				// chevron, Ctrl+/ and Ctrl+K.
				if ( !bMoved && InlineMode() && pSel->ParamCount() > 0 )
				{
					if ( nDir > 0 )
					{
						s_sExpandedEntry = pSel->Id();
					}
					else if ( s_sExpandedEntry == pSel->Id() )
					{
						s_sExpandedEntry.clear();
						s_nInlineFocus = -1;
					}
				}
			}
		}
	}

	// =====================================================================
	//  Public surface
	// =====================================================================
	void RequestPalette()
	{
		s_bPaletteRequested.store( true, std::memory_order_release );
	}

	void RequestLauncher()
	{
		s_bLauncherRequested.store( true, std::memory_order_release );
	}

	bool LauncherOnlyActive()
	{
		return s_bLauncherOnlyPublished.load( std::memory_order_acquire );
	}

	void NotifyOverlayHidden()
	{
		s_bOverlayHiddenNotice.store( true, std::memory_order_release );
		// Published immediately as well as through the frame-consumed notice.
		// wlserver reads this to decide what Left+Right Ctrl means, and the
		// overlay can be hidden and re-opened between two frames -- leaving
		// the mirror true until the next Draw() would make the reopen a
		// launcher the user never asked for.
		s_bLauncherOnlyPublished.store( false, std::memory_order_release );
	}

	void Draw()
	{
		// Issue #79's fix for this path -- see Palette.h. Without it the
		// shell would render at display_scale 1.0 for the life of the
		// process, because the lazy loader used to hang off the legacy
		// dock, which E2 never draws.
		gamescope::palette::EnsureThemeLoaded();

		// The kit's ONE scale input (Tokens.h). Pushed once, here, per
		// frame; nothing downstream reads palette:: directly.
		SetScale( gamescope::palette::DisplayScale() );

		const ImGuiIO &io = ImGui::GetIO();

		// Publish the surface size for every reader that has no ImGui
		// context -- the console thread's, above all. See FormatLadder().
		s_flSurfaceW.store( io.DisplaySize.x, std::memory_order_relaxed );
		s_flSurfaceH.store( io.DisplaySize.y, std::memory_order_relaxed );

		// D25: the overlay was hidden from outside the shell (a Right Ctrl
		// tap, Ctrl+Shift+O, gamescopectl). Consumed FIRST, before either
		// request below, so a hide and a re-open landing between two frames
		// resolves in the order they actually happened.
		if ( s_bOverlayHiddenNotice.exchange( false, std::memory_order_acq_rel ) )
		{
			s_bLauncherOnly = false;
			s_bPaletteOpen  = false;
		}

		// D22: consume a palette request from wlserver's hotkey path. Done
		// here, before anything draws, so the palette opens on the SAME frame
		// the request is seen rather than a frame later -- the shortcut has
		// to feel like the key opened it.
		//
		// D25: two requests now, and the launcher's is consumed second so
		// that if both somehow arrive in one frame the launcher wins -- it is
		// the more specific ask, and it is the one with a keybind on it.
		if ( s_bPaletteRequested.exchange( false, std::memory_order_acq_rel ) )
		{
			OpenPalette();
			s_bLauncherOnly = false;
		}
		if ( s_bLauncherRequested.exchange( false, std::memory_order_acq_rel ) )
		{
			OpenPalette();
			s_bLauncherOnly = true;
		}
		s_bLauncherOnlyPublished.store( s_bLauncherOnly, std::memory_order_release );

		const Slab slab = Slab::For( io.DisplaySize.x, io.DisplaySize.y, Scale() );
		if ( slab.flWidthPx <= 0.0f || slab.flHeightPx <= 0.0f )
			return;

		// =================================================================
		//  D25: the launcher -- the palette ALONE, over the game
		// =================================================================
		// The whole of what the standalone launcher draws is below, and the
		// entire rest of Draw() is skipped: no slab, no rail, no sheet, no
		// inspector, no drawer, no dropdown, no spine.
		//
		// This is an EARLY RETURN rather than a set of `if ( !s_bLauncherOnly )`
		// guards threaded through the shell's 200 lines of region drawing.
		// The guards would be the version where a future region gets added
		// without one and quietly reappears behind the launcher; a return
		// cannot be forgotten, and it makes "the keybind does not pull the
		// shell in" a property of the control flow rather than of an
		// invariant somebody has to keep re-checking.
		//
		// What is deliberately kept from the full path: the theme load and
		// the scale push above (the launcher is the same product), ImGui's
		// nav disable (D18 -- nav would eat the arrow keys that adjust in
		// place, and its NavDisableMouseHover would suppress the pointer this
		// commit just gave the list), and SyncDynamicAreas() (the launcher
		// searches the same live registry, so a dynamic area must resync
		// before anything reads an Entry out of it).
		if ( s_bLauncherOnly )
		{
			ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
			Reg().SyncDynamicAreas();

			std::vector<PaletteItem> launcherItems = Build( Reg(), s_sPaletteQuery );
			RunPaletteKeyboard( launcherItems );

			// Esc (or a click off the panel) while the launcher is up gives
			// the GAME back -- it does not uncover a shell, because there is
			// no shell to uncover and opening one would be the exact
			// behaviour this change exists to remove. RunPaletteKeyboard()
			// only clears the open bit; closing the overlay is ours.
			//
			// PaletteJump() takes the other exit: it clears s_bLauncherOnly
			// itself, so by the time we get here the shell is what should be
			// drawn and this branch is not taken. That frame draws nothing --
			// the launcher is gone and the shell has not started -- and the
			// next one is the full overlay at the chosen row.
			if ( !s_bPaletteOpen )
			{
				if ( s_bLauncherOnly )
				{
					s_bLauncherOnly = false;
					s_bLauncherOnlyPublished.store( false, std::memory_order_release );
					SettingsOverlay_SetVisible( false );
				}
				return;
			}

			// One top-level window over the whole surface. The launcher has
			// no slab to sit inside, so the surface IS its frame -- which is
			// also what centres it on screen rather than inside a rectangle
			// that is not being drawn.
			ImGui::SetNextWindowPos( ImVec2( 0.0f, 0.0f ) );
			ImGui::SetNextWindowSize( io.DisplaySize );
			ImGui::SetNextWindowFocus();
			ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
			ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );

			const ImGuiWindowFlags launchFlags =
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
				ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoBackground;

			if ( ImGui::Begin( "##e2launcher", nullptr, launchFlags ) )
			{
				DrawPalette( { 0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y },
				             launcherItems, /* bLauncher = */ true );
			}
			ImGui::End();
			ImGui::PopStyleVar( 2 );
			return;
		}

		// D18: E2 implements SPEC §8.2's keyboard IN FULL -- regions, rows,
		// adjustment, the palette, the Inspector, the mode strip. ImGui's own
		// keyboard navigation is a SECOND model over the same keys, and the
		// two visibly fight: Tab moved the shell's region and ImGui's nav
		// cursor independently, leaving a focus rectangle around whichever
		// child window ImGui had picked and the shell's own focus somewhere
		// else. Arrows are worse -- nav would consume the very keys SPEC §8.2
		// gives to "adjust the highlighted entry's value in place".
		//
		// This is the same call D16.3 made about InputText, for the same
		// reason and with the same scope: the legacy overlay keeps its nav
		// (cv_settings_overlay_keyboard_nav still governs it), and only the
		// frames E2 draws turn it off.
		ImGuiIO &ioMutable = ImGui::GetIO();
		ioMutable.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

		// D18: hand the just-finished frame's dropdown record to the
		// keyboard, and start a fresh one for the frame about to be drawn.
		// Here, immediately before RunKeyboard(), because this is the one
		// point where the previous frame's record is complete and the next
		// one has not begun.
		s_DropdownRows.swap( s_DropdownRowsBuild );
		s_DropdownRowsBuild.clear();

		// The open dropdown's borrowed options pointer lives for exactly one
		// frame: cleared here, set by the row painter if that row is drawn,
		// read by DrawDropdownList at the end. If the row is gone, the
		// pointer stays null and DrawDropdownList closes the dropdown.
		s_pDropdownOptions = nullptr;

		RunKeyboard();

		// The palette's own keys, and its results, are computed here rather
		// than inside RunKeyboard() because both the key handler and the
		// painter need the SAME ranked list -- building it twice would let
		// the row Enter jumps to differ from the row that was highlighted.
		std::vector<PaletteItem> paletteItems;
		if ( s_bPaletteOpen )
		{
			paletteItems = Build( Reg(), s_sPaletteQuery );
			RunPaletteKeyboard( paletteItems );
		}

		// Dynamic areas resynchronise HERE, at the top of the frame, before
		// anything reads a row out of one. A rebuild frees every Entry the
		// area held, so no Entry pointer may be held across it -- doing it
		// once, first, is what guarantees nothing does. Selection is by id
		// string and survives (Registry.h's Rebuilds()).
		Reg().SyncDynamicAreas();

		// An armed destructive action disarms itself. Left armed, it would
		// be one press from deleting a file for as long as the overlay
		// stayed open -- which is exactly the "never delete a config
		// automatically" rule, one step removed.
		if ( !s_sArmedAction.empty() &&
		     ( (float)ImGui::GetTime() - s_flArmedAt > kArmTimeout ||
		       !Reg().FindEntry( s_sArmedAction ) ) )
			s_sArmedAction.clear();

		const Area *pArea = SelectedArea();
		LadderResult ladder = Solve( slab, Host(), pArea ? (int)pArea->EntryCount() : 0,
		                             AreaIsUnsplittable( pArea ) );

		// SPEC §8.4's 160 ms region duration, applied to the ONE region
		// dimension that changes without the surface changing. Note the
		// other prohibition it respects: "regions never move" -- the rail
		// changes width, nothing slides.
		s_flRailAnim = Approach( s_flRailAnim, ladder.flRailBase, tok::kDurRegion, io.DeltaTime );
		// Snap once the remainder is under a physical pixel. An exponential
		// approach never arrives, and a rail that is forever 0.3 base units
		// off its target keeps re-uploading the same vertex buffer and keeps
		// the region boundary on a fractional pixel.
		if ( std::abs( s_flRailAnim - ladder.flRailBase ) < 1.0f / std::max( Scale(), 0.01f ) )
			s_flRailAnim = ladder.flRailBase;
		const LadderResult ladderDrawn = [ & ] {
			LadderResult L = ladder;
			L.flRailBase = s_flRailAnim;
			return L;
		}();

		const Regions regions = Regions::For( slab, ladderDrawn );

		// The slab: centred, fixed, unmovable, unresizable, unsaved. Every
		// one of those flags is load-bearing -- together they are what make
		// "no window management" a property of the code rather than a
		// promise in a doc.
		ImGui::SetNextWindowPos( ImVec2( ( io.DisplaySize.x - slab.flWidthPx ) * 0.5f,
		                                 ( io.DisplaySize.y - slab.flHeightPx ) * 0.5f ) );
		ImGui::SetNextWindowSize( ImVec2( slab.flWidthPx, slab.flHeightPx ) );
		ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
		ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, Hairline() );
		ImGui::PushStyleColor( ImGuiCol_WindowBg, Col( Role::Surface ) );
		ImGui::PushStyleColor( ImGuiCol_Border, Accent( 0.42f ) );

		const ImGuiWindowFlags flags =
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoBringToFrontOnFocus;

		if ( ImGui::Begin( "##e2slab", nullptr, flags ) )
		{
			// Everything below works in SCREEN space, offset by the slab's
			// own origin. Layout.h computes rects relative to the slab, so
			// this is the single translation between the two -- there is no
			// second place a coordinate system is chosen.
			const ImVec2 origin = ImGui::GetWindowPos();
			const auto Off = [ & ]( const Rect &r ) {
				return Rect{ r.x0 + origin.x, r.y0 + origin.y, r.x1 + origin.x, r.y1 + origin.y };
			};

			DrawSlabBar( Off( regions.rcSlabBar ) );
			DrawRail( Off( regions.rcRail ), ladder.RailIsIcons() );

			DrawSheetHead( Off( regions.rcSheetHead ), pArea,
			               ladderDrawn.eHost == InspectorHost::Hidden );

			// D17. A drawer overlays the sheet instead of taking width from
			// it, so at 2.0x it covered the sheet's entire control column. The
			// regions stay exactly as the ladder computed them -- the drawer
			// still floats, still costs no relayout -- and the sheet's LANE is
			// what gives way. Both rects are slab-space here, so the
			// difference needs no Off().
			const float flDrawerOverlapPx =
				ladderDrawn.eHost == InspectorHost::Drawer
					? std::max( 0.0f, regions.rcSheetBody.x1 - regions.rcInspector.x0 )
					: 0.0f;
			// D18: the explain page REPLACES the sheet body for as long as it
			// is open (SPEC §6.3), which is why it is a swap here rather than
			// an overlay: "replacing the sheet content" is what the spec
			// says, and an overlay would leave the rows underneath reachable
			// by pointer while the keyboard was somewhere else.
			if ( s_bExplainPage && SelectedEntry() )
				DrawExplainPage( Off( regions.rcSheetBody ), *SelectedEntry() );
			else
				DrawSheetBody( Off( regions.rcSheetBody ), pArea, flDrawerOverlapPx,
				               ladderDrawn.nColumns );
			DrawSheetFoot( Off( regions.rcSheetFoot ) );

			// The rail/sheet boundary. Drawn from the sheet's own left
			// edge, so the animated rail carries it along.
			VLine( Off( regions.rcSheet ).x0, Off( regions.rcBody ).y0, Off( regions.rcBody ).y1,
			       Col( Role::LineRegion ) );

			if ( ladderDrawn.eHost == InspectorHost::Hidden )
			{
				DrawSpine( Off( regions.rcSpine ) );
			}
			else
			{
				Regions screen = regions;
				screen.rcInspector     = Off( regions.rcInspector );
				screen.rcModeStrip     = Off( regions.rcModeStrip );
				screen.rcInspectorBody = Off( regions.rcInspectorBody );
				DrawInspector( screen, ladderDrawn );
			}
		}
		ImGui::End();

		ImGui::PopStyleColor( 2 );
		ImGui::PopStyleVar( 2 );

		// D18: the open dropdown's list, above the slab and below the
		// palette. Same sibling-window reason as the palette below, plus the
		// popup-focus one -- see DrawDropdownList.
		{
			const ImVec2 vSlab( ( io.DisplaySize.x - slab.flWidthPx ) * 0.5f,
			                    ( io.DisplaySize.y - slab.flHeightPx ) * 0.5f );
			DrawDropdownList( { vSlab.x, vSlab.y,
			                    vSlab.x + slab.flWidthPx, vSlab.y + slab.flHeightPx } );
		}

		// The palette gets its OWN top-level window, opened after the slab's
		// has closed.
		//
		// WHY NOT SIMPLY DRAW IT LAST INSIDE THE SLAB. The sheet's rows and
		// the Inspector's body are ImGui CHILD windows (`##sheetrows`,
		// `##inspbody`), and a child's draw list is emitted after its
		// parent's regardless of the order the two were filled in. Drawing
		// the palette into the slab's own list therefore put it UNDERNEATH
		// the very rows it is meant to cover -- the segmented controls and
		// switches painted straight through it. A sibling window ordered
		// after the slab is above both the parent and its children, which is
		// the only arrangement that holds no matter what the sheet does with
		// child windows later.
		//
		// The palette is the one surface in the shell allowed to cover
		// another: it is transient, keyboard-only, and dismisses on Esc.
		// SPEC §8.4's "regions never move" is about regions, not about a
		// modal index.
		if ( s_bPaletteOpen )
		{
			ImGui::SetNextWindowPos( ImVec2( ( io.DisplaySize.x - slab.flWidthPx ) * 0.5f,
			                                 ( io.DisplaySize.y - slab.flHeightPx ) * 0.5f ) );
			ImGui::SetNextWindowSize( ImVec2( slab.flWidthPx, slab.flHeightPx ) );
			ImGui::SetNextWindowFocus();
			ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
			ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );

			// Deliberately NOT `flags | NoBackground`: the slab carries
			// ImGuiWindowFlags_NoBringToFrontOnFocus, which pins a window to
			// the BACK of the draw order and made SetNextWindowFocus() above
			// a no-op -- the palette rendered behind the sheet it is supposed
			// to cover. The palette is the one window here that must come
			// forward, so it is the one window that does not carry that flag.
			const ImGuiWindowFlags palFlags =
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
				ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoBackground;
			if ( ImGui::Begin( "##e2palette", nullptr, palFlags ) )
			{
				const ImVec2 origin = ImGui::GetWindowPos();
				const Rect rcBody = {
					regions.rcBody.x0 + origin.x, regions.rcBody.y0 + origin.y,
					regions.rcBody.x1 + origin.x, regions.rcBody.y1 + origin.y };
				DrawPalette( rcBody, paletteItems, /* bLauncher = */ false );
			}
			ImGui::End();
			ImGui::PopStyleVar( 2 );
		}
	}
}

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
// ---------------------------------------------------------------------------
// WHAT P2 DOES NOT DO
// ---------------------------------------------------------------------------
// The five legacy panels still draw their own bodies, verbatim, through
// ui::Area::Escape(). They are not migrated; the sheet is only proving it can
// hold them. P3 rewrites them area by area against the Row grammar, and the
// escape hatch dies with the last one.
#include "Shell.h"

#include "Colors.h"
#include "Controls.h"
#include "Layout.h"
#include "Lane.h"
#include "Registry.h"
#include "Row.h"
#include "Tokens.h"

#include "Overlay/Chrome.h"
#include "Overlay/Fonts.h"
#include "Overlay/Palette.h"
#include "Overlay/PanelAudio.h"
#include "Overlay/PanelConfig.h"
#include "Overlay/PanelDisplay.h"
#include "Overlay/PanelLog.h"
#include "Overlay/PanelShaders.h"
#include "Overlay/FpsDisplay.h"

#include "convar.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstdio>
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
		// Runtime-only and deliberately NOT a config field: P2 must not add
		// an on-disk key, and "off by default, opt in per session" is what
		// makes the phase safe to land.
		ConVar<bool> cv_overlay_e2(
			"overlay_e2", false,
			"Use the E2 settings shell (rail / sheet / inspector) instead of the legacy dock and floating panels." );

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
		InspectorHost s_ePreferredHost   = InspectorHost::Column;
		bool          s_bModeOverridden  = false;
		InspectorMode s_eMode            = InspectorMode::Configure;

		// The one animated quantity: SPEC §8.4's 160 ms region duration,
		// used for the rail's collapse so the icon rail does not snap.
		float s_flRailAnim = shelltok::kRailFull;

		enum class Region : unsigned char { Rail, Sheet, Inspector };
		Region s_eFocusRegion = Region::Sheet;

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
			reg.Add( "display.gamescope", "Gamescope", Section::Display )
				.Escape( []{ PanelDisplay_DrawBody(); } );
			reg.Add( "image.shaders", "Shaders", Section::Display )
				.Escape( []{ PanelShaders_DrawBody(); } );

			// ---- SYSTEM --------------------------------------------------
			// Mixer leads SYSTEM, ahead of Monitor and Log -- SPEC §8.1's
			// amendment is explicit that the fold keeps the original
			// relative order.
			reg.Add( "audio.mixer", "Mixer", Section::System )
				.Escape( []{ PanelAudio_DrawBody(); } );
			reg.Add( "system.monitor", "Monitor", Section::System )
				// FpsDisplay.cpp does two jobs; this is the SETTINGS half
				// only. The HUD over the game is drawn from its own,
				// separate context and is not in P2's scope at all.
				.Escape( []{ FpsDisplay_DrawSettingsPanel(); } );
			reg.Add( "system.log", "Log", Section::System )
				.Escape( []{ PanelLog_DrawBody(); } );

			// ---- SETUP ---------------------------------------------------
			reg.Add( "setup.config", "Config", Section::Setup )
				.Escape( []{ PanelConfig_DrawBody(); } );

			Area &shell = reg.Add( "setup.shell", "Shell", Section::Setup );
			shell.Summary( []{ return std::string( "How the settings surface itself is laid out." ); } );

			shell.Choice( "shell.inspector_host", "Inspector",
				AnyBind::Of<int>(
					[]{ return (int)s_ePreferredHost; },
					[]( int n ) { s_ePreferredHost = (InspectorHost)std::clamp( n, 0, 2 ); } ),
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
					const Slab slab = Slab::For( ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y, Scale() );
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

			shell.Action( "shell.classic", "Classic UI", "switch back",
				[]{ cv_overlay_e2.SetValue( false ); } )
				.Help( "Turn the E2 shell off for this session and return to the dock and its "
				       "floating panels. The same as `overlay_e2 0` from the console." )
				.Keywords( "legacy dock classic old panels windows" );
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

		std::string FormatLadder()
		{
			const ImGuiIO &io = ImGui::GetIO();
			const Slab slab = Slab::For( io.DisplaySize.x, io.DisplaySize.y, Scale() );
			const Area *pArea = SelectedArea();
			const LadderResult L = Solve( slab, s_ePreferredHost, pArea ? (int)pArea->EntryCount() : 0 );

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

		void DrawRail( const Rect &rc, const LadderResult &ladder )
		{
			Fill( rc, Col( Role::SurfaceRail ) );
			VLine( rc.x1 - Hairline(), rc.y0, rc.y1, Col( Role::LineRegion ) );

			const bool  bIcons  = ladder.RailIsIcons();
			const float flItemH = Px( 40.0f );        // index.html's .ri
			const float flPadX  = Px( 16.0f );
			float       y       = rc.y0 + Px( tok::kS );

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
					if ( bIcons )
					{
						// The icon rail keeps the section BREAK but drops
						// the word: a divider rule, not a heading. SPEC
						// §8.0's collapse is about width, and a heading is
						// the one thing that cannot survive it.
						y += Px( 10.0f );
						HLine( rc.x0 + Px( tok::kM ), rc.x1 - Px( tok::kM ), y, Col( Role::Line ) );
						y += Px( 10.0f );
					}
					else
					{
						const float flSecH = Px( 26.0f );
						Label( { rc.x0 + flPadX, y + Px( tok::kM ), rc.x1, y + flSecH },
						       TypeRole::Section, Col( Role::TextMeta ), SectionName( area.GetSection() ) );
						y += flSecH + Px( tok::kXS );
					}
				}

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

				// P2 draws the rail item's initial where P3 draws SPEC
				// §8.0's icon set. Eleven stroked 24-unit glyphs are a
				// self-contained piece of work with its own acceptance
				// criteria (one silhouette at 12px, no new detail at 48px)
				// and they belong with the area rewrites that name them,
				// not wedged into the phase that builds the regions.
				const char szInitial[ 2 ] = { area.Title().empty() ? '?' : area.Title()[ 0 ], '\0' };
				const ImU32 colIcon = bActive ? Col( Role::AccentIcon ) : Col( Role::TextLabel );
				const float flIcon  = Px( tok::kIconBox );

				if ( bIcons )
				{
					Label( { rcItem.x0, rcItem.y0, rcItem.x1, rcItem.y1 },
					       TypeRole::Title, colIcon, szInitial, TextAlign::Center );
				}
				else
				{
					Label( { rcItem.x0 + flPadX, rcItem.y0, rcItem.x0 + flPadX + flIcon, rcItem.y1 },
					       TypeRole::Title, colIcon, szInitial, TextAlign::Center );
					Label( { rcItem.x0 + flPadX + flIcon + Px( tok::kM ), rcItem.y0, rcItem.x1 - Px( tok::kM ), rcItem.y1 },
					       TypeRole::Label, bActive ? Col( Role::TextPrimary ) : Col( Role::TextLabel ),
					       area.Title().c_str() );
				}

				y += flItemH;
			}
		}

		// =================================================================
		//  Region 2 -- the sheet (SPEC §8.1)
		// =================================================================
		void DrawSheetHead( const Rect &rc, const Area *pArea )
		{
			HLine( rc.x0, rc.x1, rc.y1 - Hairline(), Col( Role::LineRegion ) );

			char szCrumb[ 128 ];
			snprintf( szCrumb, sizeof( szCrumb ), "%s  /  %s",
				pArea ? SectionName( pArea->GetSection() ) : "",
				pArea ? pArea->Title().c_str() : "" );
			Label( { rc.x0 + Px( tok::kSheetPad ), rc.y0, rc.x1 - Px( tok::kSheetPad ), rc.y1 },
			       TypeRole::Section, Col( Role::TextPrimary ), szCrumb );

			if ( pArea && pArea->IsEscaped() )
			{
				// The migration seam is visible in the UI, not only in the
				// source. An un-migrated category should look un-migrated:
				// that is the pressure that gets P3 finished.
				Label( { rc.x0, rc.y0, rc.x1 - Px( tok::kSheetPad ), rc.y1 },
				       TypeRole::Meta, Col( Role::WarnText ), "legacy body", TextAlign::Right );
			}
		}

		void DrawSheetFoot( const Rect &rc )
		{
			HLine( rc.x0, rc.x1, rc.y0, Col( Role::LineRegion ) );
			Label( { rc.x0 + Px( tok::kSheetPad ), rc.y0, rc.x1 - Px( tok::kSheetPad ), rc.y1 },
			       TypeRole::Meta, Col( Role::TextMeta ),
			       "^I  inspector      Tab  region      Esc  close" );
		}

		// One Inspector/sheet row of the Row grammar, for the real area.
		// P3 generalises this; P2 needs exactly the kinds setup.shell uses.
		bool DrawEntryRow( const Entry &entry, const Lane &laneBase, float flOriginPx, float flTopPx,
		                   bool bSelected )
		{
			const RowCtx row = RowCtx::ForRow( laneBase, flOriginPx, flTopPx );
			const ImRect rcRow = row.Bounds();

			ImGui::SetCursorScreenPos( rcRow.Min );
			ImGui::PushID( entry.Id().c_str() );
			const bool bClicked = ImGui::InvisibleButton( "##row", rcRow.GetSize() );
			const bool bHovered = ImGui::IsItemHovered();

			if ( bSelected )
			{
				Fill( { rcRow.Min.x, rcRow.Min.y, rcRow.Max.x, rcRow.Max.y }, Accent( 0.08f ) );
				const ImRect rcEdge = row.StateEdge();
				Fill( { rcEdge.Min.x, rcEdge.Min.y, rcEdge.Max.x, rcEdge.Max.y }, Col( Role::AccentBase ) );
			}
			else if ( bHovered )
			{
				Fill( { rcRow.Min.x, rcRow.Min.y, rcRow.Max.x, rcRow.Max.y }, IM_COL32( 255, 255, 255, 10 ) );
			}
			HLine( rcRow.Min.x, rcRow.Max.x, rcRow.Max.y - Hairline(), Col( Role::Line ) );

			// The value string. Facts summarise themselves; everything else
			// prints its bound value.
			std::string sValue;
			if ( entry.GetKind() == Kind::Facts )
				sValue = entry.SummaryText();
			else if ( entry.Binding().IsBound() )
				sValue = ValueToString( entry.Binding().Get() );

			ImRect rcLabel, rcValue;
			const float flValueW = entry.UsesValue() && !sValue.empty()
				? MeasureText( TypeRole::Value, sValue.c_str() ).x : 0.0f;
			row.SplitLabelZone( flValueW, &rcLabel, &rcValue );

			Label( { rcLabel.Min.x, rcLabel.Min.y, rcLabel.Max.x, rcLabel.Max.y },
			       TypeRole::Label, bSelected ? Col( Role::TextPrimary ) : Col( Role::TextLabel ),
			       entry.Title().c_str() );
			if ( flValueW > 0.0f )
				Label( { rcValue.Min.x, rcValue.Min.y, rcValue.Max.x, rcValue.Max.y },
				       TypeRole::Value, Col( Role::TextPrimary ), sValue.c_str(), TextAlign::Right );

			// The control. Every atom is right-bound by construction --
			// RowCtx has no other kind of allocator (see Row.h).
			switch ( entry.GetKind() )
			{
				case Kind::Switch:
				{
					const Value v = entry.Binding().Get();
					bool b = std::holds_alternative<bool>( v ) && std::get<bool>( v );
					if ( controls::Switch( row, "sw", &b ) )
						entry.Binding().Set( Value{ b } );
					break;
				}
				case Kind::Choice:
				{
					const Value v = entry.Binding().Get();
					int n = std::holds_alternative<int>( v ) ? std::get<int>( v ) : 0;
					const controls::ChoiceResult res =
						controls::Choice( row, "ch", &n, entry.Options().data(), entry.Options().size() );
					if ( res.bChanged )
						entry.Binding().Set( Value{ n } );
					break;
				}
				case Kind::Action:
					if ( controls::Verb( row, "verb", entry.Verb().c_str() ) )
						entry.Invoke();
					break;
				default:
					break;   // Facts and the rest carry no control
			}

			ImGui::PopID();
			return bClicked;
		}

		void DrawSheetBody( const Rect &rc, const Area *pArea )
		{
			if ( !pArea )
				return;

			if ( pArea->IsEscaped() )
			{
				// ---------------------------------------------------------
				// THE MIGRATION SEAM, at its one call site.
				// ---------------------------------------------------------
				// API.md §13: "Escape() pushes the old ImGuiStyle, runs the
				// old code inside the sheet's child, pops." Concretely, the
				// legacy panels assume ImGui's own cursor-driven layout --
				// SameLine, item spacing, indent, a window content region
				// that starts at a padded origin. The kit assumes none of
				// that and pushes zero padding for its own rows. So the
				// hatch restores the padding the legacy code expects,
				// scoped to the child, and puts it back afterwards.
				//
				// It looks wrong on purpose. There is no styling here that
				// tries to make a legacy body blend into an E2 sheet: a
				// half-convincing blend is how a migration stops being
				// urgent.
				ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding,
					ImVec2( Px( tok::kSheetPad ), Px( tok::kM ) ) );
				ImGui::SetCursorScreenPos( ImVec2( rc.x0, rc.y0 ) );
				if ( ImGui::BeginChild( "##escape", ImVec2( rc.Width(), rc.Height() ),
					ImGuiChildFlags_None, ImGuiWindowFlags_NoSavedSettings ) )
				{
					pArea->EscapeBody()();
				}
				ImGui::EndChild();
				ImGui::PopStyleVar();
				return;
			}

			ImGui::SetCursorScreenPos( ImVec2( rc.x0, rc.y0 ) );
			if ( ImGui::BeginChild( "##sheetrows", ImVec2( rc.Width(), rc.Height() ),
				ImGuiChildFlags_None, ImGuiWindowFlags_NoSavedSettings ) )
			{
				const float flPad   = Px( tok::kSheetPad );
				const float flColW  = rc.Width() - 2.0f * flPad;
				const Lane  lane    = Lane::ForColumn( flColW / Scale() );
				float       y       = rc.y0 + Px( tok::kM );

				for ( size_t i = 0; i < pArea->EntryCount(); ++i )
				{
					const Entry &entry = pArea->EntryAt( i );
					const bool bSel = ( SelectedEntry() == &entry );
					if ( DrawEntryRow( entry, lane, rc.x0 + flPad, y, bSel ) )
					{
						Select( &entry );
						s_eFocusRegion = Region::Sheet;
					}
					y += Px( tok::kRowH );
				}
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
				s_ePreferredHost = InspectorHost::Hidden;
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
		void DrawConfigure( const Rect &rc, const Entry &entry )
		{
			const float flPad = Px( tok::kInspectorPad );
			const Rect  rcIn  { rc.x0 + flPad, rc.y0 + flPad, rc.x1 - flPad, rc.y1 };
			float y = rcIn.y0;

			Label( { rcIn.x0, y, rcIn.x1, y + Px( 18.0f ) }, TypeRole::Title,
			       Col( Role::TextPrimary ), entry.Title().c_str() );
			y += Px( 24.0f );

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
				DrawWrapped( rcIn, TypeRole::Body, Col( Role::TextMeta ),
					"This row is a readout -- there is nothing here to set. Its live values are in DETAILS.", y );
				return;
			}

			// The values block: the row's own control as an Inspector row,
			// then its Params, all at the same 44 height and the same
			// grammar (SPEC §5.3's amendment -- 44, not 40, because the
			// Sheet is the host a promoted parameter ends up in).
			HLine( rcIn.x0, rcIn.x1, y, Col( Role::Line ) );
			y += Px( tok::kS );
			Label( { rcIn.x0, y, rcIn.x1, y + Px( 14.0f ) }, TypeRole::Section,
			       Col( Role::TextMeta ), "VALUES" );
			y += Px( 20.0f );

			const Lane lane = Lane::ForColumn( rcIn.Width() / Scale() );
			DrawEntryRow( entry, lane, rcIn.x0, y, false );
			y += Px( tok::kRowH );

			for ( size_t i = 0; i < entry.ParamCount(); ++i )
			{
				const Parameter &param = entry.ParamAt( i );
				const RowCtx row = RowCtx::ForRow( lane, rcIn.x0, y );
				HLine( row.Bounds().Min.x, row.Bounds().Max.x, row.Bounds().Max.y - Hairline(), Col( Role::Line ) );

				ImRect rcLabel, rcValue;
				std::string sValue = param.Binding().IsBound()
					? ValueToString( param.Binding().Get() ) : std::string();
				row.SplitLabelZone( sValue.empty() ? 0.0f : MeasureText( TypeRole::Value, sValue.c_str() ).x,
				                    &rcLabel, &rcValue );
				Label( { rcLabel.Min.x, rcLabel.Min.y, rcLabel.Max.x, rcLabel.Max.y },
				       TypeRole::Label, Col( Role::TextLabel ), param.Title().c_str() );

				ImGui::PushID( (int)i + 1000 );
				if ( param.GetKind() == Kind::Switch && param.Binding().IsBound() )
				{
					Value v = param.Binding().Get();
					bool b = std::holds_alternative<bool>( v ) ? std::get<bool>( v ) : false;
					if ( controls::Switch( row, "psw", &b ) )
						param.Binding().Set( Value{ b } );
				}
				ImGui::PopID();
				y += Px( tok::kRowH );
			}
		}

		// ---- DETAILS (SPEC §5.1, §5.4) -----------------------------------
		void DrawDetails( const Rect &rc, const Entry &entry )
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
				Label( { rcIn.x0 + flKeyW, y, rcIn.x1, y + Px( 16.0f ) },
				       TypeRole::Meta, Col( Role::TextPrimary ), sVal.c_str() );
				y += Px( 20.0f );
			};

			Grid( "NOW", entry.GetKind() == Kind::Facts
				? entry.SummaryText()
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
				y += Px( 20.0f );

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
		}

		// ---- OVERVIEW (SPEC §5.5) ----------------------------------------
		void DrawOverview( const Rect &rc, const Area *pArea )
		{
			const float flPad = Px( tok::kInspectorPad );
			const Rect  rcIn  { rc.x0 + flPad, rc.y0 + flPad, rc.x1 - flPad, rc.y1 };
			float y = rcIn.y0;

			if ( !pArea )
				return;

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

			if ( pArea->IsEscaped() )
			{
				// Overview is honest about what this category currently is.
				// SPEC §5.5 calls the Overview "the most useful screen in
				// the product" precisely because it answers "what state am
				// I in" before you have clicked anything -- and for a
				// legacy body the true answer is that the shell cannot see
				// inside it yet.
				y = DrawWrapped( rcIn, TypeRole::Body, Col( Role::WarnText ),
					"This category still draws its original panel body, hosted verbatim in the sheet. "
					"It has no registered rows yet, so the Inspector has nothing to derive and no "
					"selection is possible here.", y );
				y += Px( tok::kM );
				DrawWrapped( rcIn, TypeRole::Meta, Col( Role::TextMeta ),
					"sheet: legacy escape  ·  inspector 0 params  ·  migration pending", y );
				return;
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
			DrawWrapped( rcIn, TypeRole::Meta, Col( Role::TextMeta ), szBudget, y );
		}

		void DrawInspector( const Regions &regions, const LadderResult &ladder )
		{
			const Entry *pEntry = SelectedEntry();

			Fill( regions.rcInspector, ladder.eHost == InspectorHost::Drawer
				? IM_COL32( 12, 14, 17, 251 )       // index.html's .insp.drawer
				: Col( Role::SurfaceInspector ) );
			VLine( regions.rcInspector.x0, regions.rcInspector.y0, regions.rcInspector.y1,
			       ladder.eHost == InspectorHost::Drawer ? Col( Role::AccentBase ) : Col( Role::LineRegion ) );

			DrawModeStrip( regions.rcModeStrip, pEntry );

			ImGui::SetCursorScreenPos( ImVec2( regions.rcInspectorBody.x0, regions.rcInspectorBody.y0 ) );
			if ( ImGui::BeginChild( "##inspbody",
				ImVec2( regions.rcInspectorBody.Width(), regions.rcInspectorBody.Height() ),
				ImGuiChildFlags_None, ImGuiWindowFlags_NoSavedSettings ) )
			{
				if ( !pEntry )
					DrawOverview( regions.rcInspectorBody, SelectedArea() );
				else if ( CurrentMode( pEntry ) == InspectorMode::Configure )
					DrawConfigure( regions.rcInspectorBody, *pEntry );
				else
					DrawDetails( regions.rcInspectorBody, *pEntry );
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
				s_ePreferredHost = InspectorHost::Column;

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
			static const char *const kSpineText = "inspector ›";
			const ImU32 col = bHovered ? Col( Role::AccentSeg ) : Col( Role::TextMeta );
			ImGui::PushFont( FontFor( TypeRole::Meta ) );
			const float flLineH = ImGui::GetFontSize() * 0.92f;
			const float flTotal = flLineH * 11.0f;
			float y = rc.y0 + ( rc.Height() - flTotal ) * 0.5f;
			for ( const char *p = kSpineText; *p; )
			{
				// Step one UTF-8 code point at a time -- the trailing "›"
				// is multi-byte and drawing half of it draws nothing.
				const char *pNext = p + 1;
				while ( ( *pNext & 0xC0 ) == 0x80 ) pNext++;
				const ImVec2 size = ImGui::CalcTextSize( p, pNext );
				ImGui::GetWindowDrawList()->AddText(
					ImVec2( rc.x0 + ( rc.Width() - size.x ) * 0.5f, y ), col, p, pNext );
				y += flLineH;
				p = pNext;
			}
			ImGui::PopFont();
		}

		// =================================================================
		//  The slab bar (SPEC §8.1's 40-base title strip)
		// =================================================================
		void DrawSlabBar( const Rect &rc )
		{
			Fill( rc, Accent( 0.10f ) );
			HLine( rc.x0, rc.x1, rc.y1 - Hairline(), Col( Role::LineRegion ) );

			const float flDot = Px( 6.0f );
			Fill( { rc.x0 + Px( tok::kM ), rc.y0 + ( rc.Height() - flDot ) * 0.5f,
			        rc.x0 + Px( tok::kM ) + flDot, rc.y0 + ( rc.Height() + flDot ) * 0.5f },
			      Col( Role::AccentBase ) );

			Label( { rc.x0 + Px( tok::kM ) + flDot + Px( tok::kS ), rc.y0, rc.x1, rc.y1 },
			       TypeRole::Title, Col( Role::TextPrimary ), "GAMESCOPE-RITZ" );
			Label( { rc.x0, rc.y0, rc.x1 - Px( tok::kM ), rc.y1 },
			       TypeRole::Meta, Col( Role::TextMeta ), "E2 shell · overlay_e2", TextAlign::Right );
		}

		// =================================================================
		//  Keyboard (SPEC §8.2)
		// =================================================================
		void RunKeyboard()
		{
			const ImGuiIO &io = ImGui::GetIO();

			// Ctrl+I: "cycle Inspector host: column -> drawer -> hidden".
			if ( io.KeyCtrl && ImGui::IsKeyPressed( ImGuiKey_I, false ) )
			{
				s_ePreferredHost =
					s_ePreferredHost == InspectorHost::Column ? InspectorHost::Drawer :
					s_ePreferredHost == InspectorHost::Drawer ? InspectorHost::Hidden :
					                                            InspectorHost::Column;
			}

			// Tab / Shift+Tab: cycle region Rail -> Sheet -> Inspector.
			// Only when no item is active, so Tab inside a text field still
			// belongs to the text field.
			if ( !io.KeyCtrl && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed( ImGuiKey_Tab, false ) )
			{
				const int n = (int)s_eFocusRegion + ( io.KeyShift ? 2 : 1 );
				s_eFocusRegion = (Region)( n % 3 );
			}

			// Esc: palette -> drawer -> inline expansion -> overlay. P2 has
			// no palette and no inline expansion, so the ladder it can
			// actually walk is: a floating drawer closes first, and only
			// then does Esc reach the overlay itself (which
			// SettingsOverlay.cpp owns).
			if ( ImGui::IsKeyPressed( ImGuiKey_Escape, false ) &&
			     s_ePreferredHost == InspectorHost::Drawer )
			{
				s_ePreferredHost = InspectorHost::Hidden;
			}
		}
	}

	// =====================================================================
	//  Public surface
	// =====================================================================
	bool Enabled()
	{
		return cv_overlay_e2.Get();
	}

	void Draw()
	{
		// Issue #79's fix for this path -- see Chrome.h. Without it the
		// shell would render at display_scale 1.0 for the life of the
		// process, because the lazy loader used to hang off the legacy
		// dock, which E2 never draws.
		gamescope::chrome::EnsureThemeLoaded();

		// The kit's ONE scale input (Tokens.h). Pushed once, here, per
		// frame; nothing downstream reads palette:: directly.
		SetScale( gamescope::palette::DisplayScale() );

		const ImGuiIO &io = ImGui::GetIO();
		const Slab slab = Slab::For( io.DisplaySize.x, io.DisplaySize.y, Scale() );
		if ( slab.flWidthPx <= 0.0f || slab.flHeightPx <= 0.0f )
			return;

		RunKeyboard();

		const Area *pArea = SelectedArea();
		LadderResult ladder = Solve( slab, s_ePreferredHost, pArea ? (int)pArea->EntryCount() : 0 );

		// SPEC §8.4's 160 ms region duration, applied to the ONE region
		// dimension that changes without the surface changing. Note the
		// other prohibition it respects: "regions never move" -- the rail
		// changes width, nothing slides.
		s_flRailAnim = Approach( s_flRailAnim, ladder.flRailBase, tok::kDurRegion, io.DeltaTime );
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
			DrawRail( Off( regions.rcRail ), ladderDrawn );

			DrawSheetHead( Off( regions.rcSheetHead ), pArea );
			DrawSheetBody( Off( regions.rcSheetBody ), pArea );
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
	}
}

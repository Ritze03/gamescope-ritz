#include "Controls.h"
#include "Colors.h"

#include "../Fonts.h"
#include "../Palette.h"

// imgui_internal.h is what makes these atoms behave like stock widgets:
// ItemAdd(), ButtonBehavior(), SliderBehavior(), RenderNavCursor() and
// MarkItemEdited() are the same primitives ImGui's own Checkbox and
// SliderScalar are built from, which is what "identical behaviour" means here
// -- the same hit-testing, the same keyboard/nav handling, the same disabled
// semantics, the same ID scoping. Same route Widgets.cpp already takes.
#include "imgui.h"
#include "imgui_internal.h"

#include "convar.h"

#include <cstdio>
#include <cstring>

namespace gamescope::ui
{
	// D22. Draw the rect every atom actually REGISTERED with ImGui, on top of
	// the atom it was registered for.
	//
	// This exists because "renders correctly, does nothing" is this kit's
	// recurring defect (issues #25, #68, and the whole reason D22 happened),
	// and every previous hunt for it read code. A hit box is a rectangle in
	// screen space; the cheapest true statement about it is a picture. With
	// this on, a control whose outline does not sit exactly on the control is
	// the bug, visible in one screenshot -- and a control with NO outline was
	// never registered at all, which is the other half of the same failure.
	//
	// Foreground draw list, so the outline is never clipped by the child
	// window or covered by anything drawn later.
	ConVar<bool> cv_overlay_e2_debug_hitboxes(
		"overlay_e2_debug_hitboxes", false,
		"Outline the rect each E2 control atom registered with ImGui for hit-testing, over the "
		"atom itself. An outline that does not match the painted control, or a painted control "
		"with no outline at all, is the drawn-vs-hit-tested divergence this kit keeps shipping." );

	// =====================================================================
	//  Text
	// =====================================================================
	namespace
	{
		// A type role names a FAMILY and a WEIGHT; the size comes from the
		// token, drawn at that size rather than at whatever the atlas happens
		// to be baked at. That is what stops a caller putting a number in Sans
		// (SPEC §7.6) without this file having to own a second font set.
		fonts::Style FaceFor( TypeRole eRole )
		{
			switch ( eRole )
			{
				case TypeRole::Title:   return fonts::Style::Title;         // Mono 600
				case TypeRole::Section: return fonts::Style::SegmentLabel;  // Mono 500
				case TypeRole::Label:   return fonts::Style::Label;         // Sans 400
				case TypeRole::Body:    return fonts::Style::Label;         // Sans 400
				case TypeRole::Value:   return fonts::Style::Value;         // Mono 500
				case TypeRole::Meta:    return fonts::Style::Meta;          // Mono 400
				default: break;
			}
			return fonts::Style::Label;
		}
	}

	ImVec2 MeasureText( TypeRole eRole, const char *pszText, const char *pszEnd )
	{
		if ( !pszText || !*pszText )
			return ImVec2( 0.0f, 0.0f );

		ImFont *pFont = fonts::Get( FaceFor( eRole ) );
		const float flSize = TypeSizePx( eRole );
		return pFont->CalcTextSizeA( flSize, FLT_MAX, 0.0f, pszText, pszEnd );
	}

	void DrawText( const ImRect &rcClip, TypeRole eRole, ImU32 col, const char *pszText, TextAlign eAlign )
	{
		if ( !pszText || !*pszText || rcClip.GetWidth() <= 0.0f )
			return;

		ImFont *pFont = fonts::Get( FaceFor( eRole ) );
		const float flSize = TypeSizePx( eRole );
		const ImVec2 size  = pFont->CalcTextSizeA( flSize, FLT_MAX, 0.0f, pszText );

		// Alignment only has meaning while the text FITS. Once it is wider
		// than the rect it gets left-aligned regardless of what was asked
		// for, so the truncation takes the tail rather than the head.
		//
		// Right-aligning an overlong string puts its start off the left edge
		// and clips there, which reads as garbage rather than as truncation:
		// SPEC §2.3 caps a value at 60% of the label zone, so a long value in
		// the narrow Inspector lane rendered `bottom-right · 64 / 32` as
		// `.ght`. Losing the end of a string is legible; losing the
		// beginning is not.
		float flX = rcClip.Min.x;
		if ( size.x <= rcClip.GetWidth() )
		{
			if ( eAlign == TextAlign::Right )
				flX = rcClip.Max.x - size.x;
			else if ( eAlign == TextAlign::Center )
				flX = rcClip.Min.x + ( rcClip.GetWidth() - size.x ) * 0.5f;
		}

		const ImVec2 pos( flX, rcClip.Min.y + ( rcClip.GetHeight() - size.y ) * 0.5f );

		// Text that does not fit is never allowed to overrun into the next
		// column -- that is what makes the four column lines unbroken from
		// the top of a sheet to the bottom. The draw is still hard-clipped to
		// the rect as a backstop, but the string is TRUNCATED WITH AN ELLIPSIS
		// first, so overflow reads as "there is more" instead of stopping
		// mid-word.
		//
		// D27, from the conformance audit's divergence 10. Hard clipping alone
		// rendered the Log Inspector's Buffer facts row as `51 lines · 2 er:`
		// in the narrow lane -- a truncation the user has objected to before
		// (#46) because it is indistinguishable from a value that genuinely
		// ends there. Raising the type ladder makes more strings overflow more
		// often, so the marker had to land in the same change rather than
		// after it.
		//
		// "..." and not U+2026: Fonts.cpp bakes Basic Latin + Latin-1 only,
		// and the bundled Geist faces do not carry the ellipsis glyph -- the
		// same constraint that made the kit DRAW its chevron and magnifier
		// (D18) rather than type them. Three periods are in every face.
		const ImVec4 clip( rcClip.Min.x, rcClip.Min.y, rcClip.Max.x, rcClip.Max.y );
		ImDrawList *pDrawList = ImGui::GetCurrentWindow()->DrawList;

		// ONE PHYSICAL PIXEL OF SLACK before a string counts as overflowing.
		// Several rects in the kit are sized FROM this very measurement --
		// RowCtx::SplitLabelZone() builds the value rect as `Lw - measured ..
		// Lw`, and `a - (a - b)` is not exactly `b` in float at screen-sized
		// coordinates -- so a rect cut to fit its text exactly can come back
		// a few ten-thousandths of a pixel too narrow. Hard clipping never
		// noticed; an ellipsis does, and the first build of this turned the
		// Monitor's `18 px` into `1...` because a 6e-5 px overflow cost it
		// three characters to the marker. Below one pixel, clipping is
		// invisible and a marker would be a lie.
		//
		// The alignment test above stays strict on purpose: it must only
		// right- or centre-align text that genuinely fits, or a string
		// overflowing by half a pixel would be pushed off the left edge.
		if ( size.x - rcClip.GetWidth() > 1.0f )
		{
			static const char * const kEllipsis = "...";
			const float flEllipsisW = pFont->CalcTextSizeA( flSize, FLT_MAX, 0.0f, kEllipsis ).x;
			const float flHeadW     = rcClip.GetWidth() - flEllipsisW;

			// Too narrow to hold even the marker plus one glyph: fall back to
			// the plain hard clip. An ellipsis alone tells the reader nothing
			// the empty space did not, and a marker wider than its own lane
			// would be the very overrun this guards against.
			if ( flHeadW > 0.0f )
			{
				// max_width (the 2nd argument), NOT wrap_width (the 3rd):
				// wrapping would break at a WORD boundary and return a
				// multi-line box, which is not what a one-line lane wants.
				// max_width stops at the last glyph that fits, hands back the
				// cut point in out_remaining, and returns that head's exact
				// width -- so the marker is positioned by measurement rather
				// than by a second CalcTextSize.
				const char *pszCut = nullptr;
				const ImVec2 head = pFont->CalcTextSizeA( flSize, flHeadW, 0.0f,
					pszText, nullptr, &pszCut );

				if ( pszCut && pszCut > pszText )
				{
					pDrawList->AddText( pFont, flSize, pos, col, pszText, pszCut, 0.0f, &clip );
					const ImVec2 posEllipsis( pos.x + head.x, pos.y );
					pDrawList->AddText( pFont, flSize, posEllipsis, col, kEllipsis, nullptr, 0.0f, &clip );
					return;
				}
			}
		}

		pDrawList->AddText( pFont, flSize, pos, col, pszText, nullptr, 0.0f, &clip );
	}

	// =====================================================================
	//  Drawn glyphs (D18) -- see Controls.h for why these are not characters
	// =====================================================================
	namespace glyph
	{
		namespace
		{
			// One stroke width for every glyph here, derived from the box so
			// a mark drawn at 9 base and one drawn at 13 read as the same
			// pen. Floored at one physical pixel: below that a stroke stops
			// being antialiased and starts disappearing, which is exactly
			// what the 0.5x ladder step would do to it.
			float StrokePx( float flSizePx )
			{
				return std::max( 1.0f, flSizePx * 0.11f );
			}

			// The atoms' own Dl() is declared further down this file; these
			// glyphs sit above them because DrawText() is what they belong
			// next to, so they take the same draw list by the same route.
			ImDrawList *Dl() { return ImGui::GetCurrentWindow()->DrawList; }
		}

		void Chevron( ImVec2 vCenterPx, float flSizePx, Dir eDir, ImU32 col )
		{
			// Authored once, on a unit square, pointing RIGHT -- then rotated
			// into the other three. One shape and one rotation beats four
			// hand-placed point triples that drift apart the first time the
			// proportions are tuned.
			//
			// 0.26 half-width against 0.5 half-height is a ~62 degree
			// included angle: the same wedge the mockup's `›` cuts, and open
			// enough that the two strokes stay distinguishable at 0.5x.
			const float w = flSizePx * 0.26f;
			const float h = flSizePx * 0.50f;

			ImVec2 pts[ 3 ] = { { -w, -h }, { +w, 0.0f }, { -w, +h } };

			for ( ImVec2 &p : pts )
			{
				const ImVec2 v = p;
				switch ( eDir )
				{
					case Dir::Right: p = v;                            break;
					case Dir::Left:  p = ImVec2( -v.x, v.y );          break;
					case Dir::Down:  p = ImVec2( -v.y, v.x );          break;
					case Dir::Up:    p = ImVec2(  v.y, -v.x );         break;
				}
				p = ImVec2( vCenterPx.x + p.x, vCenterPx.y + p.y );
			}

			// Open polyline, not a closed triangle: a chevron is two strokes
			// meeting at a point, and closing it would draw a third edge the
			// mark has never had.
			Dl()->AddPolyline( pts, 3, col, ImDrawFlags_None, StrokePx( flSizePx ) );
		}

		void Magnifier( ImVec2 vCenterPx, float flSizePx, ImU32 col )
		{
			const float flStroke = StrokePx( flSizePx );

			// The lens sits up-left of centre so the handle has room inside
			// the same box -- otherwise the glyph's optical centre drifts
			// right of the rect it was asked to centre on.
			const float flR = flSizePx * 0.32f;
			const ImVec2 c( vCenterPx.x - flSizePx * 0.09f, vCenterPx.y - flSizePx * 0.09f );

			// Segment count from the radius: a fixed count is chunky at 2.0x
			// and wasteful at 0.5x. ImGui's own auto-tessellation does this
			// when num_segments is 0.
			Dl()->AddCircle( c, flR, col, 0, flStroke );

			// The handle leaves the lens on the 45 degree diagonal and runs
			// to the box corner, so it never crosses back inside the circle.
			const float k = 0.70710678f;   // cos(45)
			Dl()->AddLine( ImVec2( c.x + flR * k, c.y + flR * k ),
			               ImVec2( vCenterPx.x + flSizePx * 0.46f, vCenterPx.y + flSizePx * 0.46f ),
			               col, flStroke );
		}

		void Lock( ImVec2 vCenterPx, float flSizePx, ImU32 col )
		{
			const float flStroke = StrokePx( flSizePx );

			// A body and a shackle, on the same unit square the other two
			// glyphs use. The body is FILLED because a lock's silhouette is
			// what identifies it at 0.5x -- a hollow rectangle at 12 physical
			// px reads as an empty box, which is exactly the fallback glyph
			// this whole set exists to avoid.
			const float w = flSizePx * 0.34f;
			const float yTop = vCenterPx.y - flSizePx * 0.04f;
			const float yBot = vCenterPx.y + flSizePx * 0.42f;
			Dl()->AddRectFilled( ImVec2( vCenterPx.x - w, yTop ), ImVec2( vCenterPx.x + w, yBot ),
			                     col, flSizePx * 0.06f );

			// The shackle is a half-circle sitting on the body's top edge.
			const float flR = flSizePx * 0.21f;
			Dl()->PathArcTo( ImVec2( vCenterPx.x, yTop ), flR, IM_PI, IM_PI * 2.0f );
			Dl()->PathStroke( col, ImDrawFlags_None, flStroke );
		}

		// ---- SPEC §8.0's rail icon set ------------------------------------
		// The eleven glyphs' GEOMETRY lives in Icons.h, which links no ImGui
		// on purpose (see that file). This is the only place it becomes
		// pixels, and it holds no coordinates of its own -- so a glyph can be
		// re-drawn, re-proportioned or replaced without touching a draw call,
		// and the draw calls can change without touching a glyph.
		void RailIcon( const Icon &icon, ImVec2 vCenterPx, float flBoxPx, ImU32 col )
		{
			// SPEC §8.0: "stroke 1.7" on the 24-unit grid, so the pen scales
			// with the box and the set reads as one family at every step of
			// the ladder. Floored at one physical pixel for the same reason
			// StrokePx() above is: below that a stroke stops being
			// antialiased and starts disappearing, which is precisely what
			// 0.5x would do to it.
			const float k = flBoxPx / kIconGrid;
			const float flStroke = std::max( 1.0f, kIconStroke * k );

			// Grid -> screen. The 24-unit box is centred on vCenterPx, so
			// unit (12,12) lands exactly on the centre the caller asked for.
			const auto P = [ & ]( IconPt p ) {
				return ImVec2( vCenterPx.x + ( p.x - kIconGrid * 0.5f ) * k,
				               vCenterPx.y + ( p.y - kIconGrid * 0.5f ) * k );
			};

			for ( size_t i = 0; i < icon.nShapes; ++i )
			{
				const IconShape &s = icon.shapes[ i ];

				ImVec2 pts[ kIconMaxPts ];
				for ( size_t j = 0; j < s.nPoints && j < kIconMaxPts; ++j )
					pts[ j ] = P( s.pts[ j ] );

				switch ( s.eOp )
				{
					case IconOp::Polyline:
						Dl()->AddPolyline( pts, (int)s.nPoints, col, ImDrawFlags_None, flStroke );
						break;

					case IconOp::Loop:
						Dl()->AddPolyline( pts, (int)s.nPoints, col, ImDrawFlags_Closed, flStroke );
						break;

					case IconOp::Circle:
						// Segment count 0 = ImGui's own auto-tessellation
						// from the radius, which is what keeps the circle
						// round at 48 px without being wasteful at 12.
						Dl()->AddCircle( pts[ 0 ], s.flRadius * k, col, 0, flStroke );
						break;

					case IconOp::FillRect:
						Dl()->AddRectFilled( pts[ 0 ], pts[ 1 ], col );
						break;

					case IconOp::FillPoly:
						Dl()->AddConvexPolyFilled( pts, (int)s.nPoints, col );
						break;

					case IconOp::HalfDisc:
					{
						// HDR's half-filled disc. Drawn as a filled arc from
						// -90 to +90 degrees -- the RIGHT half -- so it sits
						// inside the stroked circle the glyph also carries
						// rather than replacing it.
						const float r = s.flRadius * k;
						Dl()->PathArcTo( pts[ 0 ], r, -IM_PI * 0.5f, IM_PI * 0.5f );
						Dl()->PathFillConvex( col );
						break;
					}

					case IconOp::Teardrop:
					{
						// A droplet: the two tangent lines from the apex to
						// the circle, plus the arc between their feet.
						//
						// Constructed rather than transcribed because the
						// tangent points are where the straight and the
						// curved parts must meet EXACTLY -- a hand-placed
						// pair leaves a visible kink at 48 px and a visible
						// gap at 12. With `a = acos(r/d)` the joint is exact
						// at every size, which is the whole reason this op
						// exists instead of a polyline approximation.
						const ImVec2 vApex = pts[ 0 ];
						const ImVec2 vC    = pts[ 1 ];
						const float  r     = s.flRadius * k;

						const float dx = vApex.x - vC.x, dy = vApex.y - vC.y;
						const float d  = std::sqrt( dx * dx + dy * dy );
						if ( d <= r )
						{
							// Degenerate: the apex is inside the circle, so
							// there is no tangent and the honest drawing is
							// the circle itself.
							Dl()->AddCircle( vC, r, col, 0, flStroke );
							break;
						}

						const float th = std::atan2( dy, dx );
						const float a  = std::acos( r / d );

						Dl()->PathLineTo( vApex );
						// From one tangent foot, the long way round the
						// circle, to the other -- then closed back to the
						// apex by PathStroke's Closed flag.
						Dl()->PathArcTo( vC, r, th + a, th - a + IM_PI * 2.0f );
						Dl()->PathStroke( col, ImDrawFlags_Closed, flStroke );
						break;
					}
				}
			}
		}
	}

	// =====================================================================
	//  Shared atom plumbing
	// =====================================================================
	namespace
	{
		struct Atom
		{
			ImRect  rc;                  // THE rect. Registered and drawn; there is no second one.
			ImGuiID id       = 0;
			bool    bHovered = false;
			bool    bHeld    = false;
			bool    bPressed = false;
			bool    bValid   = false;

			explicit operator bool() const { return bValid; }
		};

		// Registers `rc` with ImGui and runs stock ButtonBehavior on it. The
		// caller draws into `out.rc` -- the same object -- so a drawn atom
		// cannot disagree with its own hit box.
		//
		// ItemSize() is deliberately NOT called: the kit lays rows out
		// absolutely from RowCtx, so advancing ImGui's cursor would be a
		// second, competing layout system. ItemAdd() alone is what registers
		// hit-testing and keyboard navigation.
		// The one write that may be waiting for a drag to end. Frame-scoped
		// and single-threaded: only ever touched from the atom prologue
		// below, which runs on the render thread inside a live frame.
		//
		// The drag flag itself lives in Registry.cpp
		// (ui::SetPointerDragActive) rather than here, because its readers are
		// registrations, and a registration must be able to ask the question
		// from the console thread where there is no ImGui context (D19.1).
		std::function<void()> s_DeferredApply;

		// The shared atom prologue. See Controls.h's DeferToRelease() comment
		// for why the flush lives here rather than in the slider: every atom
		// on screen runs this every frame, so a drag that ends anywhere still
		// gets its deferred write applied on the very next frame -- and a
		// deferred write can never run while something is being held.
		//
		// The state published here is the FRAME-START view: ImGui's ActiveId
		// still belongs to whatever was held last frame. That is deliberately
		// not the whole answer -- on the frame a press first lands, ActiveId
		// is taken later, by the atom's own behaviour call, so this reads
		// false while the pointer is very much down on a control. NoteDragOn()
		// below is what closes that gap; it runs after the behaviour, which is
		// where the truth is. Without it the first frame of a click-and-drag
		// applies immediately and the deferral only starts on frame two.
		void PublishDragStateAndFlush()
		{
			const bool bHeld = ImGui::IsAnyItemActive()
			                && ImGui::IsMouseDown( ImGuiMouseButton_Left );
			SetPointerDragActive( bHeld );
			if ( bHeld || !s_DeferredApply )
				return;

			// Moved out before the call: the callable is free to queue
			// another deferral (it will not, today) without this clearing it
			// again afterwards.
			std::function<void()> fn = std::move( s_DeferredApply );
			s_DeferredApply = nullptr;
			fn();
		}

		// Called by an atom immediately AFTER its behaviour ran, while the id
		// it just registered is still the "last item". This is the only place
		// that can see the press-frame drag, so it can only ever raise the
		// flag, never clear it -- clearing is the prologue's job, once per
		// frame, before any atom has run.
		void NoteDragOnLastItem()
		{
			if ( ImGui::IsItemActive() && ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
				SetPointerDragActive( true );
		}

		Atom Begin( const ImRect &rc, const char *pszId, ImGuiButtonFlags nFlags = 0 )
		{
			PublishDragStateAndFlush();

			Atom a;
			a.rc = rc;

			ImGuiWindow *pWindow = ImGui::GetCurrentWindow();
			if ( !pWindow || pWindow->SkipItems )
				return a;

			a.id = pWindow->GetID( pszId );
			if ( !ImGui::ItemAdd( a.rc, a.id ) )
			{
				// D22: an atom REJECTED by ItemAdd (clipped out, or inside a
				// window with SkipItems) is drawn-but-dead. Mark it in a
				// different colour so the screenshot distinguishes "hit box
				// in the wrong place" from "hit box refused".
				if ( cv_overlay_e2_debug_hitboxes )
					ImGui::GetForegroundDrawList()->AddRect( rc.Min, rc.Max, IM_COL32( 255, 40, 40, 255 ) );
				return a;
			}

			a.bPressed = ImGui::ButtonBehavior( a.rc, a.id, &a.bHovered, &a.bHeld, nFlags );
			a.bValid   = true;
			ImGui::RenderNavCursor( a.rc, a.id );

			if ( cv_overlay_e2_debug_hitboxes )
			{
				ImGui::GetForegroundDrawList()->AddRect( a.rc.Min, a.rc.Max,
					a.bHovered ? IM_COL32( 80, 255, 80, 255 ) : IM_COL32( 60, 200, 255, 200 ) );

				// Only the PRESS is logged, not hover or held: hover is
				// already on screen as a green outline, and logging it every
				// frame buried the one line that matters under thousands.
				// A press is the event that answers "did the click convert",
				// and it happens once.
				if ( a.bPressed || a.bHeld )
				{
					const ImVec2 mouse = ImGui::GetIO().MousePos;
					console_log.infof( "hitbox %s %s rc=%.0f,%.0f-%.0f,%.0f mouse=%.0f,%.0f",
						a.bPressed ? "press" : "held", pszId,
						a.rc.Min.x, a.rc.Min.y, a.rc.Max.x, a.rc.Max.y, mouse.x, mouse.y );
				}
			}
			return a;
		}

		ImDrawList *Dl() { return ImGui::GetCurrentWindow()->DrawList; }

		// A 1px control boundary at the current scale (SPEC §8.3).
		void Boundary( const ImRect &rc, ImU32 col, float flRounding = 0.0f )
		{
			Dl()->AddRect( rc.Min, rc.Max, col, flRounding, 0, Hairline() );
		}

		// ---- one measurement, two consumers ---------------------------
		// Segmented cells and chip-bank cells are CONTENT-SIZED (B's design,
		// SPEC §3.2/§3.12), so their width has to be measured. This is the
		// only function that measures them. `SegmentedFits` is that measure
		// compared with the lane, and the layout below walks the same widths
		// -- a control can never be laid out to a width its own fit test did
		// not see.
		struct CellRun
		{
			std::vector<float> widths;
			float flTotal = 0.0f;
		};

		CellRun MeasureCells( const Option *pOptions, size_t nOptions, TypeRole eRole,
		                      float flPadXBase, float flGapBase )
		{
			CellRun run;
			run.widths.reserve( nOptions );

			const float flPad = Px( flPadXBase );
			const float flGap = Px( flGapBase );

			for ( size_t i = 0; i < nOptions; ++i )
			{
				const float flW = MeasureText( eRole, pOptions[ i ].pszLabel ).x + flPad * 2.0f;
				run.widths.push_back( flW );
				run.flTotal += flW + ( i + 1 < nOptions ? flGap : 0.0f );
			}
			return run;
		}
	}

	// =====================================================================
	//  Switch -- SPEC §3.1
	// =====================================================================
	namespace controls
	{
		void DeferToRelease( std::function<void()> fn ) { s_DeferredApply = std::move( fn ); }

		bool Switch( const RowCtx &row, const char *pszId, bool *pbValue )
		{
			// The hit box is the full kControlH-tall rect (SPEC §3.0); the
			// 40 x 20 graphic is centred in it. Two rects, but only one of
			// them is ever hit-tested and the other is derived from it, so
			// they cannot disagree about where the control is.
			const Atom a = Begin( row.Place( tok::kSwitchW ), pszId );
			if ( !a )
				return false;

			bool bChanged = false;
			if ( a.bPressed )
			{
				*pbValue = !*pbValue;
				bChanged = true;
				ImGui::MarkItemEdited( a.id );
			}

			const float flTrackH = Px( tok::kSwitchH );
			const ImRect track(
				a.rc.Min.x, a.rc.GetCenter().y - flTrackH * 0.5f,
				a.rc.Max.x, a.rc.GetCenter().y + flTrackH * 0.5f );

			// B's colours verbatim, except the off-border, which B draws at
			// 18% (1.69:1 -- below the 3:1 floor for an interactive boundary)
			// and SPEC §3.1 raises to LineControl.
			const ImU32 colTrack  = *pbValue ? Accent( 0.30f ) : palette::White( 0.07f );
			const ImU32 colBorder = *pbValue ? Accent( 0.65f ) : Col( Role::LineControl );
			const ImU32 colKnob   = *pbValue ? Col( Role::AccentKnob ) : Col( Role::TextKnobOff );

			Dl()->AddRectFilled( track.Min, track.Max, colTrack );
			Boundary( track, colBorder );
			if ( a.bHovered )
				Dl()->AddRectFilled( track.Min, track.Max, palette::White( 0.05f ) );

			// The knob's travel is a token derived from the track and the knob
			// (Tokens.h), never a second literal.
			const float flKnob  = Px( tok::kSwitchKnob );
			const float flInset = Px( tok::kSwitchInset ) * 0.5f;   // 1 unit: the border
			const float flX = track.Min.x + flInset + ( *pbValue ? Px( tok::kSwitchTrvl ) : 0.0f );
			Dl()->AddRectFilled( ImVec2( flX, track.GetCenter().y - flKnob * 0.5f ),
			                     ImVec2( flX + flKnob, track.GetCenter().y + flKnob * 0.5f ), colKnob );

			return bChanged;
		}

		// =================================================================
		//  Slider -- SPEC §3.4
		// =================================================================
		namespace
		{
			// THE ONE PLACE A SLIDER GRAB IS SIZED.
			//
			// SliderBehavior() derives the draggable grab from
			// style.GrabMinSize and hands the resulting rect back. This pushes
			// the token into GrabMinSize, calls it, pops, and returns that
			// rect. The painter below draws *that rect* -- it is never told
			// how wide a handle is supposed to be.
			//
			// That is what makes issue #23's bug class unrepresentable rather
			// than fixed: there is one number, and the code that draws cannot
			// see it, so a future edit to the token moves the drawn handle and
			// the hit target together or not at all.
			bool SliderGrab( const ImRect &rcTrackHit, ImGuiID id, ImGuiDataType eType,
			                 void *pValue, const void *pMin, const void *pMax,
			                 const char *pszFormat, ImRect *pOutGrab )
			{
				ImGui::PushStyleVar( ImGuiStyleVar_GrabMinSize, Px( tok::kHandleW ) );
				const bool bChanged = ImGui::SliderBehavior(
					rcTrackHit, id, eType, pValue, pMin, pMax, pszFormat,
					ImGuiSliderFlags_AlwaysClamp, pOutGrab );
				ImGui::PopStyleVar();
				return bChanged;
			}

			// The shared paint. `rcGrab` is SliderBehavior()'s own output.
			void PaintSlider( const ImRect &rcHit, const ImRect &rcGrab,
			                  float flFraction, float flDefaultFraction, bool bHasDefault )
			{
				const float flTrackH = Px( tok::kTrack );
				const float flRound  = Px( tok::kTrackRound );
				const float flCy     = rcHit.GetCenter().y;

				const ImVec2 trackMin( rcHit.Min.x, flCy - flTrackH * 0.5f );
				const ImVec2 trackMax( rcHit.Max.x, flCy + flTrackH * 0.5f );

				// B's 16% unfilled track measures 1.57:1; SPEC §3.4 moves it to
				// TrackOff (34%, 3.07:1) -- "the rail is the part of the
				// control that tells you where the range ends".
				Dl()->AddRectFilled( trackMin, trackMax, Col( Role::TrackOff ), flRound );

				const float flFillMax = rcHit.Min.x + rcHit.GetWidth() * ImClamp( flFraction, 0.0f, 1.0f );
				if ( flFillMax > trackMin.x )
				{
					// B's left-to-right gradient, accent@50% -> AccentGradHi.
					Dl()->AddRectFilledMultiColor(
						trackMin, ImVec2( flFillMax, trackMax.y ),
						Accent( 0.50f ), Col( Role::AccentGradHi ),
						Col( Role::AccentGradHi ), Accent( 0.50f ) );
				}

				if ( bHasDefault )
				{
					// SPEC §3.4's 1px default tick, at 52%.
					const float flTickX = rcHit.Min.x + rcHit.GetWidth() * ImClamp( flDefaultFraction, 0.0f, 1.0f );
					Dl()->AddRectFilled( ImVec2( flTickX, trackMin.y ),
					                     ImVec2( flTickX + Hairline(), trackMax.y ),
					                     palette::White( 0.52f ) );
				}

				// The handle: x from SliderBehavior's grab, height from the
				// token. Nothing here restates the grab's width.
				const float flHandleH = Px( tok::kHandleH );
				const ImVec2 hMin( rcGrab.Min.x, flCy - flHandleH * 0.5f );
				const ImVec2 hMax( rcGrab.Max.x, flCy + flHandleH * 0.5f );
				const float flHalo = Px( tok::kHandleHalo );
				Dl()->AddRectFilled( ImVec2( hMin.x - flHalo, hMin.y - flHalo ),
				                     ImVec2( hMax.x + flHalo, hMax.y + flHalo ), Accent( 0.18f ) );
				Dl()->AddRectFilled( hMin, hMax, Col( Role::AccentHandle ), Px( tok::kHandleRound ) );
			}
		}

		bool Slider( const RowCtx &row, const char *pszId, float *pflValue,
		             float flMin, float flMax, float flDefault, bool bHasDefault )
		{
			// PlaceFull: the track IS the range (SPEC §2.2's table).
			const Atom a = Begin( row.PlaceFull(), pszId, ImGuiButtonFlags_None );
			if ( !a )
				return false;

			ImRect grab;
			const bool bChanged = SliderGrab( a.rc, a.id, ImGuiDataType_Float, pflValue,
			                                  &flMin, &flMax, "%.3f", &grab );
			if ( bChanged )
				ImGui::MarkItemEdited( a.id );
			NoteDragOnLastItem();

			const float flSpan = ( flMax - flMin );
			const float flFrac = flSpan != 0.0f ? ( *pflValue - flMin ) / flSpan : 0.0f;
			const float flDef  = flSpan != 0.0f ? ( flDefault - flMin ) / flSpan : 0.0f;
			PaintSlider( a.rc, grab, flFrac, flDef, bHasDefault );
			return bChanged;
		}

		bool SliderInt( const RowCtx &row, const char *pszId, int *pnValue,
		                int nMin, int nMax, int nDefault, bool bHasDefault )
		{
			const Atom a = Begin( row.PlaceFull(), pszId, ImGuiButtonFlags_None );
			if ( !a )
				return false;

			ImRect grab;
			const bool bChanged = SliderGrab( a.rc, a.id, ImGuiDataType_S32, pnValue,
			                                  &nMin, &nMax, "%d", &grab );
			if ( bChanged )
				ImGui::MarkItemEdited( a.id );
			NoteDragOnLastItem();

			const float flSpan = (float)( nMax - nMin );
			const float flFrac = flSpan != 0.0f ? (float)( *pnValue - nMin ) / flSpan : 0.0f;
			const float flDef  = flSpan != 0.0f ? (float)( nDefault - nMin ) / flSpan : 0.0f;
			PaintSlider( a.rc, grab, flFrac, flDef, bHasDefault );
			return bChanged;
		}

		// =================================================================
		//  Stepper -- SPEC §3.5
		// =================================================================
		bool Stepper( const RowCtx &row, const char *pszId, int *pnValue,
		              int nMin, int nMax, int nStep )
		{
			// B's borderless "- +": two 18-wide glyph hit boxes, 8 apart. The
			// number is NOT here -- it lives in the value column, which is
			// what SPEC §2.3's amendment is about.
			const ImRect rcGroup = row.Place( tok::kStepperW );
			const float flGlyphW = Px( tok::kStepperGlyphW );

			ImGui::PushID( pszId );
			bool bChanged = false;

			const ImRect rcMinus( rcGroup.Min.x, rcGroup.Min.y, rcGroup.Min.x + flGlyphW, rcGroup.Max.y );
			const ImRect rcPlus ( rcGroup.Max.x - flGlyphW, rcGroup.Min.y, rcGroup.Max.x, rcGroup.Max.y );

			// SPEC §3.5's "Step() accelerates after 400 ms" comes from ImGui's
			// own held-button repeat (ImGuiItemFlags_ButtonRepeat, timed by
			// io.KeyRepeatDelay/Rate) rather than a hand-rolled timer -- the
			// same reason every atom here is built on stock behaviours.
			ImGui::PushItemFlag( ImGuiItemFlags_ButtonRepeat, true );
			const Atom aMinus = Begin( rcMinus, "-" );
			if ( aMinus && aMinus.bPressed )
			{
				*pnValue = ImMax( nMin, *pnValue - nStep );
				bChanged = true;
				ImGui::MarkItemEdited( aMinus.id );
			}

			const Atom aPlus = Begin( rcPlus, "+" );
			ImGui::PopItemFlag();
			if ( aPlus && aPlus.bPressed )
			{
				*pnValue = ImMin( nMax, *pnValue + nStep );
				bChanged = true;
				ImGui::MarkItemEdited( aPlus.id );
			}

			DrawText( rcMinus, TypeRole::Value,
				aMinus.bHovered ? Col( Role::AccentSeg ) : Col( Role::TextStepGlyph ), "-", TextAlign::Center );
			DrawText( rcPlus, TypeRole::Value,
				aPlus.bHovered ? Col( Role::AccentSeg ) : Col( Role::TextStepGlyph ), "+", TextAlign::Center );

			ImGui::PopID();
			return bChanged;
		}

		// =================================================================
		//  Choice -- segmented (SPEC §3.2) or dropdown (§3.3)
		// =================================================================
		ChoiceResult Choice( const RowCtx &row, const char *pszId, int *pnValue,
		                     const Option *pOptions, size_t nOptions, bool bPopupOpen )
		{
			ChoiceResult res;
			if ( !pOptions || nOptions == 0 )
				return res;

			// SPEC §3.2: mutually exclusive, <= 5 options, <= 8 chars each,
			// static set -- "and the helper measures and auto-downgrades to a
			// dropdown if any of the three conditions fails OR if the measured
			// group does not fit the lane". ONE predicate, every host.
			constexpr size_t kSegMaxOptions = 5;
			constexpr size_t kSegMaxChars   = 8;

			bool bSeg = nOptions <= kSegMaxOptions;
			for ( size_t i = 0; bSeg && i < nOptions; ++i )
				bSeg = pOptions[ i ].pszLabel && strlen( pOptions[ i ].pszLabel ) <= kSegMaxChars;

			CellRun run;
			if ( bSeg )
			{
				run  = MeasureCells( pOptions, nOptions, TypeRole::Meta, tok::kSegPadX, tok::kGapSeg );
				bSeg = run.flTotal <= row.CtlWidthPx();
			}
			res.bSegmented = bSeg;

			ImGui::PushID( pszId );

			if ( bSeg )
			{
				// The group is right-bound; its cells are content-sized and
				// deliberately NOT stretched to fill the lane -- a stretched
				// cell set reads as a tab bar, and there is no tab bar in this
				// product (SPEC §3.2).
				const ImRect rcGroup = row.PlacePx( run.flTotal );
				const float  flGap   = Px( tok::kGapSeg );

				float flX = rcGroup.Min.x;
				for ( size_t i = 0; i < nOptions; ++i )
				{
					const ImRect rcCell( flX, rcGroup.Min.y, flX + run.widths[ i ], rcGroup.Max.y );
					flX += run.widths[ i ] + flGap;

					const bool bOn = ( *pnValue == pOptions[ i ].nValue );
					const Atom a = Begin( rcCell, pOptions[ i ].pszLabel );
					if ( a && a.bPressed && !bOn )
					{
						*pnValue = pOptions[ i ].nValue;
						res.bChanged = true;
						ImGui::MarkItemEdited( a.id );
					}

					const ImU32 colFill = bOn ? Accent( 0.24f )
					                          : ( a.bHovered ? palette::White( 0.11f ) : palette::White( 0.04f ) );
					Dl()->AddRectFilled( rcCell.Min, rcCell.Max, colFill );
					Boundary( rcCell, bOn ? Accent( 0.60f ) : Col( Role::LineControl ) );

					// B's inactive text is 50% (4.96:1) and its active cell is
					// Mono 600; the weight change is carried by the face, so
					// the active cell asks for a different role.
					DrawText( rcCell, bOn ? TypeRole::Section : TypeRole::Meta,
						bOn ? Col( Role::AccentSeg ) : Col( Role::TextSegInactive ),
						pOptions[ i ].pszLabel, TextAlign::Center );
				}
			}
			else
			{
				// B's dropdown is NOT a box: the resolved value in Mono 500 16
				// followed by a caret in Meta, with a hairline appearing on
				// hover and focus. Full lane, so the caret's right edge stays
				// on the control line.
				const Atom a = Begin( row.PlaceFull(), "dd" );
				if ( a && a.bPressed )
					res.bWantsPopup = true;

				const char *pszLabel = "";
				for ( size_t i = 0; i < nOptions; ++i )
					if ( pOptions[ i ].nValue == *pnValue )
						pszLabel = pOptions[ i ].pszLabel;

				if ( bPopupOpen )
				{
					Dl()->AddRectFilled( a.rc.Min, a.rc.Max, Accent( 0.14f ) );
					Boundary( a.rc, Col( Role::AccentBase ) );
				}
				else if ( a.bHovered )
				{
					Dl()->AddRectFilled( a.rc.Min, a.rc.Max, palette::White( 0.06f ) );
					Boundary( a.rc, Col( Role::LineControl ) );
				}

				const float flPad   = Px( tok::kSelfPadX );
				const float flGap   = Px( tok::kS );

				// D18: the caret was a lowercase "v" -- a letter standing in
				// for a triangle, hinted and weighted like a letter, and
				// sized by whatever the Meta face happened to make it. It is
				// a drawn chevron now, so its width is a token rather than a
				// text measurement.
				const float flCaret = Px( tok::kGlyphChevron );
				const ImRect rcCaret( a.rc.Max.x - flPad - flCaret, a.rc.Min.y, a.rc.Max.x - flPad, a.rc.Max.y );
				const ImRect rcValue( a.rc.Min.x + flPad, a.rc.Min.y, rcCaret.Min.x - flGap, a.rc.Max.y );

				// The value ellipsizes from the left of the group so the
				// caret's right edge stays on the lane (SPEC §3.3); DrawText's
				// clip rect is what implements that.
				DrawText( rcValue, TypeRole::Value,
					bPopupOpen ? Col( Role::AccentSeg ) : Col( Role::TextPrimary ),
					pszLabel, TextAlign::Right );
				glyph::Chevron( rcCaret.GetCenter(), flCaret, glyph::Dir::Down,
					Col( Role::TextMeta ) );
			}

			ImGui::PopID();
			return res;
		}

		// =================================================================
		//  Text -- SPEC §3.6
		// =================================================================
		bool Text( const RowCtx &row, const char *pszId, std::string *psValue,
		           bool *pbEditing, const char *pszPlaceholder, const char *pszError )
		{
			ImGui::PushID( pszId );
			const ImRect rc = row.PlaceFull();
			bool bCommitted = false;

			if ( pbEditing && *pbEditing )
			{
				// A real input, swapped in: caret Accent, 1px Accent bottom
				// edge, raised fill. Enter commits, Esc reverts, an outside
				// click commits.
				Dl()->AddRectFilled( rc.Min, rc.Max, Col( Role::SurfaceRaised ) );
				Dl()->AddRectFilled( ImVec2( rc.Min.x, rc.Max.y - Hairline() ), rc.Max,
					pszError ? Col( Role::Danger ) : Col( Role::AccentBase ) );

				char szBuf[ 256 ];
				snprintf( szBuf, sizeof( szBuf ), "%s", psValue->c_str() );

				ImGui::SetCursorScreenPos( ImVec2( rc.Min.x + Px( tok::kSelfPadX ), rc.Min.y ) );
				ImGui::SetNextItemWidth( rc.GetWidth() - Px( tok::kSelfPadX ) * 2.0f );
				ImGui::PushStyleColor( ImGuiCol_FrameBg, IM_COL32_BLACK_TRANS );
				ImGui::PushStyleColor( ImGuiCol_Text, Col( Role::TextPrimary ) );
				if ( ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive() )
					ImGui::SetKeyboardFocusHere();
				if ( ImGui::InputText( "##edit", szBuf, sizeof( szBuf ), ImGuiInputTextFlags_EnterReturnsTrue ) )
				{
					*psValue = szBuf;
					*pbEditing = false;
					bCommitted = true;
				}
				else if ( ImGui::IsItemDeactivated() )
				{
					if ( !ImGui::IsKeyPressed( ImGuiKey_Escape ) )
					{
						*psValue = szBuf;
						bCommitted = true;
					}
					*pbEditing = false;
				}
				ImGui::PopStyleColor( 2 );
			}
			else
			{
				// Closed state: B's value + pencil, same grammar as the
				// dropdown -- hairline on hover, no box at rest.
				const Atom a = Begin( rc, "tin" );
				if ( a && a.bPressed && pbEditing )
					*pbEditing = true;

				if ( pszError )
					Boundary( rc, Col( Role::Danger ) );
				else if ( a.bHovered )
				{
					Dl()->AddRectFilled( rc.Min, rc.Max, palette::White( 0.06f ) );
					Boundary( rc, Col( Role::LineControl ) );
				}

				const bool bEmpty = psValue->empty();
				const float flPad = Px( tok::kSelfPadX );
				const float flGap = Px( tok::kS );
				const ImVec2 pen  = MeasureText( TypeRole::Meta, "*" );
				const ImRect rcPen( rc.Max.x - flPad - pen.x, rc.Min.y, rc.Max.x - flPad, rc.Max.y );
				const ImRect rcVal( rc.Min.x + flPad, rc.Min.y, rcPen.Min.x - flGap, rc.Max.y );

				// A placeholder is Meta, not a deleted TextFaint role (§7.1).
				DrawText( rcVal, TypeRole::Value,
					bEmpty ? Col( Role::TextMeta ) : Col( Role::TextPrimary ),
					bEmpty ? ( pszPlaceholder ? pszPlaceholder : "" ) : psValue->c_str(),
					TextAlign::Right );
				DrawText( rcPen, TypeRole::Meta, Col( Role::TextMeta ), "*", TextAlign::Right );
			}

			ImGui::PopID();
			return bCommitted;
		}

		// =================================================================
		//  Chip bank -- SPEC §3.12
		// =================================================================
		bool Bank( const RowCtx &row, const char *pszId, uint32_t *pnMask,
		           const Option *pOptions, size_t nOptions, int nFocusChip )
		{
			if ( !pOptions || nOptions == 0 )
				return false;

			// Same single measurement path the segmented control uses, at the
			// bank's own padding and text role.
			CellRun run = MeasureCells( pOptions, nOptions, TypeRole::Meta, tok::kBankPadX, tok::kGapSeg );

			// A bank has no dropdown to fall back to the way a Choice does
			// (Choice() above tests exactly this and downgrades), so an
			// over-wide run used to be laid out at full size from the lane's
			// LEFT edge and ran straight out of the lane -- past the sheet's
			// right edge and under the drawer at 2.0x, where the last chips
			// were invisible and unclickable. SPEC §2.2's right-bound law is
			// not optional, so the run is scaled to the lane instead: every
			// chip stays inside it, stays hit-testable, and keeps its share
			// of the width. At 1.0x nothing is scaled and nothing moves.
			float flGap = Px( tok::kGapSeg );
			if ( run.flTotal > row.CtlWidthPx() && run.flTotal > 0.0f )
			{
				const float flShrink = row.CtlWidthPx() / run.flTotal;
				for ( float &flW : run.widths )
					flW *= flShrink;
				flGap *= flShrink;
				run.flTotal = row.CtlWidthPx();
			}
			const ImRect rcGroup = row.PlacePx( run.flTotal );

			ImGui::PushID( pszId );
			bool bChanged = false;
			float flX = rcGroup.Min.x;

			for ( size_t i = 0; i < nOptions; ++i )
			{
				const ImRect rcCell( flX, rcGroup.Min.y, flX + run.widths[ i ], rcGroup.Max.y );
				flX += run.widths[ i ] + flGap;

				const uint32_t nBit = 1u << (uint32_t)pOptions[ i ].nValue;
				const bool bOn = ( *pnMask & nBit ) != 0;

				const Atom a = Begin( rcCell, pOptions[ i ].pszLabel );
				if ( a && a.bPressed )
				{
					*pnMask ^= nBit;
					bChanged = true;
					ImGui::MarkItemEdited( a.id );
				}

				const ImU32 colFill = bOn ? Accent( 0.22f )
				                          : ( a.bHovered ? palette::White( 0.11f ) : palette::White( 0.05f ) );
				Dl()->AddRectFilled( rcCell.Min, rcCell.Max, colFill );
				Boundary( rcCell, bOn ? Accent( 0.55f ) : Col( Role::LineControl ) );
				DrawText( rcCell, TypeRole::Meta,
					bOn ? Col( Role::AccentSeg ) : Col( Role::TextMeta ),
					pOptions[ i ].pszLabel, TextAlign::Center );

				// SPEC §7.3's focus ring, at its measured Accent @ 85%. Drawn
				// OUTSIDE the cell so it cannot be confused with the on-state
				// border it sits next to.
				if ( (int)i == nFocusChip )
				{
					const float flO = Px( 2.0f );
					Dl()->AddRect( ImVec2( rcCell.Min.x - flO, rcCell.Min.y - flO ),
					               ImVec2( rcCell.Max.x + flO, rcCell.Max.y + flO ),
					               Accent( 0.85f ), 0.0f, 0, std::max( 1.0f, Px( 1.5f ) ) );
				}
			}

			ImGui::PopID();
			return bChanged;
		}

		// =================================================================
		//  Meter -- SPEC §3.8, read-only
		// =================================================================
		void Meter( const RowCtx &row, float flValue, float flMin, float flMax )
		{
			// No ItemAdd: a meter is read-only, takes no input and must not
			// enter the keyboard nav order.
			const ImRect rc = row.PlaceFull();
			const float flSpan = flMax - flMin;
			const float flFrac = flSpan != 0.0f ? ImClamp( ( flValue - flMin ) / flSpan, 0.0f, 1.0f ) : 0.0f;

			const int   nSegs = (int)tok::kMeterSegs;
			const float flGap = Px( tok::kMeterGap );
			const float flSegW = ( rc.GetWidth() - flGap * (float)( nSegs - 1 ) ) / (float)nSegs;
			const float flH    = Px( tok::kTrack );
			const float flCy   = rc.GetCenter().y;
			const int   nLit   = (int)( flFrac * (float)nSegs + 0.5f );

			for ( int i = 0; i < nSegs; ++i )
			{
				const float flX = rc.Min.x + (float)i * ( flSegW + flGap );
				Dl()->AddRectFilled( ImVec2( flX, flCy - flH * 0.5f ),
				                     ImVec2( flX + flSegW, flCy + flH * 0.5f ),
				                     i < nLit ? Accent( 0.85f ) : palette::White( 0.14f ) );
			}
		}

		// =================================================================
		//  Verb chip -- SPEC §3.9
		// =================================================================
		bool Verb( const RowCtx &row, const char *pszId, const char *pszVerb,
		           Intent eIntent, bool bEnabled )
		{
			const float flW = MeasureText( TypeRole::Meta, pszVerb ).x + Px( tok::kVerbPadX ) * 2.0f;
			const Atom a = Begin( row.PlacePx( flW ), pszId );
			if ( !a )
				return false;

			ImU32 colFill, colText;
			switch ( eIntent )
			{
				case Intent::Danger:
					// SPEC §3.9: fill Danger@14%, text DangerText. Danger is
					// hue-fixed and outside the accent family on purpose.
					colFill = Dim( Col( Role::Danger ), a.bHovered ? 0.26f : 0.14f );
					colText = Col( Role::DangerText );
					break;
				case Intent::Neutral:
					colFill = palette::White( 0.05f );
					colText = Col( Role::TextBody );
					break;
				default:
					colFill = Accent( a.bHovered ? 0.26f : 0.16f );
					colText = a.bHovered ? Col( Role::AccentSeg ) : Col( Role::AccentText );
					break;
			}

			if ( !bEnabled )
			{
				colFill = Dim( colFill, 0.45f );
				colText = Dim( colText, 0.45f );
			}

			Dl()->AddRectFilled( a.rc.Min, a.rc.Max, colFill );
			// The one place a verb gets a border: `neutral`, whose text is
			// dimmer than an accent verb's, so the fill alone would not
			// identify it (SPEC §3.9).
			if ( eIntent == Intent::Neutral )
				Boundary( a.rc, Col( Role::LineControl ) );

			DrawText( a.rc, TypeRole::Meta, colText, pszVerb, TextAlign::Center );
			return bEnabled && a.bPressed;
		}

		// =================================================================
		//  Anchor grid -- SPEC §4.3, the composite body that fix #3 names
		// =================================================================
		bool AnchorGrid( const ImRect &rcBody, const char *pszId, int *pnVert, int *pnHoriz )
		{
			ImGui::PushID( pszId );
			bool bChanged = false;

			const float flGap  = Px( tok::kGapSeg );
			const float flCell = ( rcBody.GetWidth() - flGap * 2.0f ) / 3.0f;

			for ( int nRow = 0; nRow < 3; ++nRow )
			{
				for ( int nCol = 0; nCol < 3; ++nCol )
				{
					const ImRect rcCell(
						rcBody.Min.x + (float)nCol * ( flCell + flGap ),
						rcBody.Min.y + (float)nRow * ( flCell + flGap ),
						rcBody.Min.x + (float)nCol * ( flCell + flGap ) + flCell,
						rcBody.Min.y + (float)nRow * ( flCell + flGap ) + flCell );

					char szCell[ 8 ];
					snprintf( szCell, sizeof( szCell ), "%d%d", nRow, nCol );

					const bool bOn = ( *pnVert == nRow && *pnHoriz == nCol );
					const Atom a = Begin( rcCell, szCell );
					if ( a && a.bPressed && !bOn )
					{
						*pnVert = nRow;
						*pnHoriz = nCol;
						bChanged = true;
						ImGui::MarkItemEdited( a.id );
					}

					Dl()->AddRectFilled( rcCell.Min, rcCell.Max,
						bOn ? Accent( 0.30f ) : ( a.bHovered ? palette::White( 0.14f ) : palette::White( 0.05f ) ) );
					Boundary( rcCell, bOn ? Accent( 0.65f ) : Col( Role::LineControl ) );
				}
			}

			ImGui::PopID();
			return bChanged;
		}

		// =================================================================
		//  Composite bodies -- SPEC §4.4
		// =================================================================
		bool Rail( const ImRect &rcRail, const char *pszId, float *pflValue,
		           float flMin, float flMax, RailColorFn fnColorAt, void *pUser )
		{
			if ( rcRail.GetWidth() <= 0.0f || flMax <= flMin )
				return false;

			const Atom a = Begin( rcRail, pszId );

			// The gradient is drawn as kStops quads, each interpolating
			// between two REAL samples of fnColorAt rather than between two
			// endpoint colours -- so the strip cannot disagree with what the
			// value it sets actually produces (issue #37's own reasoning).
			constexpr int kStops = 24;
			const float flRound = Px( 2.0f );
			for ( int i = 0; i < kStops; ++i )
			{
				const float t0 = (float)i / (float)kStops;
				const float t1 = (float)( i + 1 ) / (float)kStops;
				const ImU32 c0 = fnColorAt( t0, pUser );
				const ImU32 c1 = fnColorAt( t1, pUser );
				const float x0 = rcRail.Min.x + rcRail.GetWidth() * t0;
				const float x1 = rcRail.Min.x + rcRail.GetWidth() * t1;
				Dl()->AddRectFilledMultiColor( ImVec2( x0, rcRail.Min.y ), ImVec2( x1, rcRail.Max.y ),
					c0, c1, c1, c0 );
			}
			Boundary( rcRail, Col( Role::LineControl ), flRound );

			// The marker. Drawn from the SAME normalised t the hit test
			// converts back from, so the thing you see is the thing you grab
			// (Controls.h's drawn-vs-hit-tested rule).
			const float flT = ImClamp( ( *pflValue - flMin ) / ( flMax - flMin ), 0.0f, 1.0f );
			const float flX = rcRail.Min.x + rcRail.GetWidth() * flT;
			const float flR = ImMax( Px( 4.0f ), rcRail.GetHeight() * 0.5f );
			const ImVec2 c( flX, rcRail.GetCenter().y );
			Dl()->AddCircleFilled( c, flR + Px( 1.0f ), palette::Black( 0.55f ) );
			Dl()->AddCircleFilled( c, flR, fnColorAt( flT, pUser ) );
			Dl()->AddCircle( c, flR, palette::White( a.bHovered || a.bHeld ? 0.95f : 0.75f ),
				0, ImMax( 1.0f, Px( 1.5f ) ) );

			if ( a && a.bHeld )
			{
				const float flNew = flMin + ( flMax - flMin ) *
					ImClamp( ( ImGui::GetIO().MousePos.x - rcRail.Min.x ) / rcRail.GetWidth(), 0.0f, 1.0f );
				if ( flNew != *pflValue )
				{
					*pflValue = flNew;
					ImGui::MarkItemEdited( a.id );
					return true;
				}
			}
			return false;
		}

		namespace
		{
			// The accent family's own base L/C (Palette.h's kAccent token),
			// so the hue rail shows the accent it is actually choosing.
			constexpr float kAccentL = 0.74f;
			constexpr float kAccentC = 0.12f;

			ImU32 HueStop( float flT, void * )
			{
				return palette::OklchToImU32( kAccentL, kAccentC, flT * 360.0f );
			}

			struct LchUser { float flL, flC, flH; };

			ImU32 LStop( float flT, void *pUser )
			{
				const LchUser *u = (const LchUser *)pUser;
				return palette::OklchToImU32( flT, u->flC, u->flH );
			}
			ImU32 CStop( float flT, void *pUser )
			{
				const LchUser *u = (const LchUser *)pUser;
				return palette::OklchToImU32( u->flL, flT * 0.37f, u->flH );
			}
			ImU32 HStop( float flT, void *pUser )
			{
				const LchUser *u = (const LchUser *)pUser;
				return palette::OklchToImU32( u->flL, u->flC, flT * 360.0f );
			}
		}

		bool HueBody( const ImRect &rcBody, const char *pszId, float *pflHue )
		{
			ImGui::PushID( pszId );
			bool bChanged = false;

			// Two stacked rows inside the band's own body rect: the rail on
			// top, the eight preset swatches beneath it. Both are sized from
			// rcBody alone -- see Controls.h on why a body never measures
			// itself.
			const float flGap     = Px( tok::kS );
			const float flSwatchH = ImMin( Px( tok::kControlH ) * 0.5f,
			                               ( rcBody.GetHeight() - flGap ) * 0.45f );
			const float flRailH   = rcBody.GetHeight() - flGap - flSwatchH;

			const ImRect rcRail( rcBody.Min.x, rcBody.Min.y,
			                     rcBody.Max.x, rcBody.Min.y + flRailH );
			bChanged |= Rail( rcRail, "hue", pflHue, 0.0f, 360.0f, HueStop );

			// Eight 45-degree presets. They set the SAME value the rail does
			// -- a swatch is a shortcut, never a second setting, which is
			// what keeps this one row of the sheet rather than nine.
			constexpr int kSwatches = 8;
			const float flCellGap = Px( tok::kGapSeg );
			const float flCellW   = ( rcBody.GetWidth() - flCellGap * (float)( kSwatches - 1 ) ) / (float)kSwatches;
			const float flTop     = rcBody.Max.y - flSwatchH;

			for ( int i = 0; i < kSwatches; ++i )
			{
				const float flHue = ( 360.0f * (float)i ) / (float)kSwatches;
				const ImRect rcCell( rcBody.Min.x + (float)i * ( flCellW + flCellGap ), flTop,
				                     rcBody.Min.x + (float)i * ( flCellW + flCellGap ) + flCellW,
				                     rcBody.Max.y );

				char szId[ 8 ];
				snprintf( szId, sizeof( szId ), "s%d", i );
				const Atom a = Begin( rcCell, szId );

				// "Selected" is a proximity test, not equality: the rail can
				// leave the hue anywhere between two stops, and a swatch bank
				// that lights up only on an exact 45.000 would essentially
				// never light up at all.
				float flDelta = fabsf( *pflHue - flHue );
				if ( flDelta > 180.0f )
					flDelta = 360.0f - flDelta;
				const bool bOn = flDelta < ( 360.0f / (float)kSwatches ) * 0.5f;

				Dl()->AddRectFilled( rcCell.Min, rcCell.Max,
					palette::OklchToImU32( kAccentL, kAccentC, flHue ), Px( 2.0f ) );
				Boundary( rcCell, bOn ? palette::White( 0.95f ) : ( a.bHovered
					? palette::White( 0.55f ) : Col( Role::LineControl ) ), Px( 2.0f ) );

				if ( a && a.bPressed && *pflHue != flHue )
				{
					*pflHue = flHue;
					ImGui::MarkItemEdited( a.id );
					bChanged = true;
				}
			}

			ImGui::PopID();
			return bChanged;
		}

		bool ColorBody( const ImRect &rcBody, const char *pszId,
		                float *pflL, float *pflC, float *pflH )
		{
			ImGui::PushID( pszId );
			bool bChanged = false;

			// A square swatch on the left showing the resolved colour, three
			// stacked rails to its right. The swatch is the only part that
			// answers "what did I actually pick", which is why it is drawn
			// from the same OklchToImU32() the rails sample.
			const float flGap     = Px( tok::kS );
			const float flSwatchW = ImMin( rcBody.GetHeight(), rcBody.GetWidth() * 0.25f );
			const ImRect rcSwatch( rcBody.Min.x, rcBody.Min.y,
			                       rcBody.Min.x + flSwatchW, rcBody.Max.y );

			Dl()->AddRectFilled( rcSwatch.Min, rcSwatch.Max,
				palette::OklchToImU32( *pflL, *pflC, *pflH ), Px( 3.0f ) );
			Boundary( rcSwatch, Col( Role::LineControl ), Px( 3.0f ) );

			const float flX0     = rcSwatch.Max.x + flGap;
			const float flRailGap = Px( 4.0f );
			const float flRailH  = ( rcBody.GetHeight() - flRailGap * 2.0f ) / 3.0f;

			LchUser u{ *pflL, *pflC, *pflH };
			struct { float *pf; float flLo, flHi; RailColorFn fn; const char *pszId; } kRails[] = {
				{ pflL, 0.0f, 1.0f,   LStop, "l" },
				{ pflC, 0.0f, 0.37f,  CStop, "c" },
				{ pflH, 0.0f, 360.0f, HStop, "h" },
			};

			for ( int i = 0; i < 3; ++i )
			{
				const ImRect rcRail( flX0, rcBody.Min.y + (float)i * ( flRailH + flRailGap ),
				                     rcBody.Max.x, rcBody.Min.y + (float)i * ( flRailH + flRailGap ) + flRailH );
				if ( Rail( rcRail, kRails[ i ].pszId, kRails[ i ].pf,
					kRails[ i ].flLo, kRails[ i ].flHi, kRails[ i ].fn, &u ) )
				{
					// Re-seed the shared user data so the two rails BELOW
					// this one gradient against the value just set, not the
					// one it replaced. Without this the C and H strips would
					// lag a frame behind the L they are supposed to show.
					u = LchUser{ *pflL, *pflC, *pflH };
					bChanged = true;
				}
			}

			ImGui::PopID();
			return bChanged;
		}

		void GraphBody( const ImRect &rcBody, const float *pflSamples, size_t nSamples,
		                float flCeiling, float flOutlierMs, size_t nAxisSlots )
		{
			Dl()->AddRectFilled( rcBody.Min, rcBody.Max, Col( Role::SurfaceRaised ), Px( 2.0f ) );
			Boundary( rcBody, Col( Role::LineControl ), Px( 2.0f ) );

			if ( !pflSamples || nSamples == 0 || flCeiling <= 0.0f || rcBody.GetWidth() <= 0.0f )
				return;

			auto Bar = [ & ]( float flX, float flW, float flValue )
			{
				const float flH = ImClamp( flValue / flCeiling, 0.0f, 1.0f ) * rcBody.GetHeight();
				const bool  bOut = flOutlierMs > 0.0f && flValue >= flOutlierMs;
				Dl()->AddRectFilled( ImVec2( flX, rcBody.Max.y - flH ),
				                     ImVec2( flX + ImMax( 1.0f, flW - Px( 0.5f ) ), rcBody.Max.y ),
				                     bOut ? Col( Role::Warn ) : Accent( 0.85f ) );
			};

			if ( nAxisSlots > 0 )
			{
				// FIXED AXIS, filled from the left. The slot pitch is
				// computed from the FULL axis, never from how many samples
				// happen to have arrived -- that is the whole point (issue
				// #40): a warm-up must visibly occupy the left of the axis
				// and leave the rest blank, not stretch to fill it.
				const float flPitch = rcBody.GetWidth() / (float)nAxisSlots;
				const size_t nDraw  = ImMin( nSamples, nAxisSlots );
				for ( size_t i = 0; i < nDraw; ++i )
					Bar( rcBody.Min.x + (float)i * flPitch, flPitch, pflSamples[ i ] );
				return;
			}

			// ROLLING sparkline: one bar per column of available width, taken
			// from the TAIL of the buffer -- the newest samples -- so a narrow
			// band shows "right now" rather than a stale prefix.
			const float flBarW = ImMax( 1.0f, Px( 2.0f ) );
			const int   nBars  = ImMin( (int)nSamples, ImMax( 1, (int)( rcBody.GetWidth() / flBarW ) ) );
			const size_t nFirst = nSamples - (size_t)nBars;

			for ( int i = 0; i < nBars; ++i )
				Bar( rcBody.Max.x - (float)( nBars - i ) * flBarW, flBarW, pflSamples[ nFirst + (size_t)i ] );
		}
	}
}

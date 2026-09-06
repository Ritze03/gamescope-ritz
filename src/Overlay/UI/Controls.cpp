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

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
		// Issue #99: RasterSize(), never TypeSizePx() raw. The explicit
		// -size AddText()/CalcTextSizeA() overloads below hand ImGui the
		// float verbatim, and ImGui only ever BAKES at whole pixels -- a
		// fractional request is served by the nearest integer bake,
		// resampled. See Fonts.h's RasterSize() for the measurement and
		// for why display_scale 1.0 was the worst-hit scale of all.
		// Measuring and drawing MUST use the identical value, or the
		// ellipsis/alignment arithmetic below is done against a width the
		// draw does not produce.
		const float flSize = fonts::RasterSize( TypeSizePx( eRole ) );
		return pFont->CalcTextSizeA( flSize, FLT_MAX, 0.0f, pszText, pszEnd );
	}

	void DrawText( const ImRect &rcClip, TypeRole eRole, ImU32 col, const char *pszText, TextAlign eAlign )
	{
		if ( !pszText || !*pszText || rcClip.GetWidth() <= 0.0f )
			return;

		ImFont *pFont = fonts::Get( FaceFor( eRole ) );
		// Issue #99: RasterSize(), never TypeSizePx() raw. The explicit
		// -size AddText()/CalcTextSizeA() overloads below hand ImGui the
		// float verbatim, and ImGui only ever BAKES at whole pixels -- a
		// fractional request is served by the nearest integer bake,
		// resampled. See Fonts.h's RasterSize() for the measurement and
		// for why display_scale 1.0 was the worst-hit scale of all.
		// Measuring and drawing MUST use the identical value, or the
		// ellipsis/alignment arithmetic below is done against a width the
		// draw does not produce.
		const float flSize = fonts::RasterSize( TypeSizePx( eRole ) );
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

		// The verb chip's visuals, at a caller-given rect rather than a row's
		// own PlacePx() -- shared by controls::Verb() (SPEC §3.9, row-hosted)
		// and DrawModal()'s Cancel/primary footer buttons (screen-hosted, no
		// row at all). One function decides what a verb chip looks like; both
		// hosts draw the same thing rather than the footer growing a second,
		// almost-identical button style.
		bool VerbAt( const ImRect &rc, const char *pszId, const char *pszVerb,
		            controls::Intent eIntent, bool bEnabled )
		{
			const Atom a = Begin( rc, pszId );
			if ( !a )
				return false;

			ImU32 colFill, colText;
			switch ( eIntent )
			{
				case controls::Intent::Danger:
					// SPEC §3.9: fill Danger@14%, text DangerText. Danger is
					// hue-fixed and outside the accent family on purpose.
					colFill = Dim( Col( Role::Danger ), a.bHovered ? 0.26f : 0.14f );
					colText = Col( Role::DangerText );
					break;
				case controls::Intent::Neutral:
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
			if ( eIntent == controls::Intent::Neutral )
				Boundary( a.rc, Col( Role::LineControl ) );

			DrawText( a.rc, TypeRole::Meta, colText, pszVerb, TextAlign::Center );
			return bEnabled && a.bPressed;
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

		// B's dropdown chrome: the resolved value in Mono 500 right-aligned,
		// followed by a caret in Meta, a hairline on hover/open. Shared by
		// Choice()'s own auto-downgrade dropdown branch and controls::
		// Dropdown() below -- ONE definition of what this control looks like,
		// regardless of which of the two decided to draw it.
		Atom DrawDropdownChrome( const RowCtx &row, int nValue, const Option *pOptions,
		                        size_t nOptions, bool bOpenNow )
		{
			const Atom a = Begin( row.PlaceFull(), "dd" );

			const char *pszLabel = "";
			for ( size_t i = 0; i < nOptions; ++i )
				if ( pOptions[ i ].nValue == nValue )
					pszLabel = pOptions[ i ].pszLabel;

			if ( bOpenNow )
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

			// D18: the caret was a lowercase "v" -- a letter standing in for a
			// triangle. It is a drawn chevron now, so its width is a token
			// rather than a text measurement.
			const float flCaret = Px( tok::kGlyphChevron );
			const ImRect rcCaret( a.rc.Max.x - flPad - flCaret, a.rc.Min.y, a.rc.Max.x - flPad, a.rc.Max.y );
			const ImRect rcValue( a.rc.Min.x + flPad, a.rc.Min.y, rcCaret.Min.x - flGap, a.rc.Max.y );

			// The value ellipsizes from the left of the group so the caret's
			// right edge stays on the lane (SPEC §3.3); DrawText's clip rect
			// is what implements that.
			DrawText( rcValue, TypeRole::Value,
				bOpenNow ? Col( Role::AccentSeg ) : Col( Role::TextPrimary ),
				pszLabel, TextAlign::Right );
			glyph::Chevron( rcCaret.GetCenter(), flCaret, glyph::Dir::Down, Col( Role::TextMeta ) );

			return a;
		}

		// ---- controls::Dropdown()'s self-owned popup state -----------------
		// Keyed by the caller's FULL ImGuiID (window stack included), not by
		// the bare pszId string -- Shell.cpp's Sheet and Inspector both call
		// this with the SAME "profiles.inherits" string for the SAME
		// selected row, once each, in the same frame, from two different
		// child windows. A string key cannot tell those two calls apart, and
		// the collision is not theoretical: the first version of this code
		// kept it string-keyed, and the Inspector's copy -- drawn after the
		// Sheet's in Shell.cpp's own Draw() order -- stomped the anchor the
		// Sheet's own click had just set, so the popup opened at the
		// INSPECTOR's box while the click landed on the SHEET's (caught by
		// this task's own mandatory capture, not by inspection). ImGuiID
		// already disambiguates this for free: two Begin()s with the same
		// pszId in two different windows hash to two different ids, exactly
		// the property ItemAdd()'s own hit-testing already relies on.
		struct DropdownPopupState
		{
			ImGuiID       idOpen    = 0;           // the open popup's ImGuiID; 0 = none
			const Option *pOptions  = nullptr;    // borrowed for this frame's draw only
			size_t        nOptions  = 0;
			ImRect        rcAnchor;                // the closed box's own rect, screen space
			int           nSelected = -1;          // ListBox's index cursor into pOptions

			// A pick made in the popup, waiting for its owning Dropdown() call
			// to apply it -- see Controls.h's "ONE-FRAME-LATE COMMIT" comment.
			bool    bHasCommit   = false;
			ImGuiID idCommit     = 0;
			int     nCommitValue = 0;
		};
		DropdownPopupState s_DropdownPopup;

		// The popup's minimum content width: wide enough for every option's
		// label, plus the same padding a segmented cell gets. One scan,
		// shared by DismissDropdownOnOutsideClick() and DrawDropdownPopup()
		// so the two can never compute a different rect for the same click.
		float DropdownContentWidthPx( const DropdownPopupState &st )
		{
			float flW = Px( 180.0f );
			for ( size_t i = 0; i < st.nOptions; ++i )
				flW = std::max( flW, MeasureText( TypeRole::Value,
					st.pOptions[ i ].pszLabel ? st.pOptions[ i ].pszLabel : "" ).x + Px( tok::kXL ) * 2.0f );
			return flW;
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
			const float flInset = Px( tok::kSwitchInset );   // issue #84: the full inset, not half
			const float flX = track.Min.x + flInset + ( *pbValue ? Px( tok::kSwitchTrvl ) : 0.0f );
			Dl()->AddRectFilled( ImVec2( flX, track.GetCenter().y - flKnob * 0.5f ),
			                     ImVec2( flX + flKnob, track.GetCenter().y + flKnob * 0.5f ), colKnob );

			return bChanged;
		}

		// =================================================================
		//  Slider -- SPEC §3.4
		// =================================================================
		// See Controls.h's own comment on ConstantWidthGrab() for the bug
		// and the fix; this is just the arithmetic, kept free of ImGui so
		// test_overlay_ui.cpp's ImGui-free binary can pin it directly.
		ImRect ConstantWidthGrab( const ImRect &grab, float flConstantW, float flHitW )
		{
			const float flCenterX = grab.GetCenter().x;
			const float flHalfW   = std::min( flConstantW, flHitW ) * 0.5f;
			return ImRect( flCenterX - flHalfW, grab.Min.y, flCenterX + flHalfW, grab.Max.y );
		}

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
				// issue #86: SliderBehaviorT hardcodes `grab_padding = 2.0f`
				// (imgui_widgets.cpp, its own comment: "FIXME: Should be part
				// of style.") and insets the grab's travel by it at BOTH ends
				//   [bb.Min + pad + gs/2 .. bb.Max - pad - gs/2]
				// so the handle halts 2px short of each cap and the track juts
				// out past it -- the "~2px sticking out on the outer edge" in
				// the report. Measured off the user's 3x screenshot: handle
				// 192..215 against a track starting at 186, and 1461..1484
				// against a track ending at 1490. 6 image px == 2 real px at
				// both ends.
				//
				// The padding is a raw literal, NOT scale-aware, so the
				// cancellation is raw too -- Px() here would over-correct at
				// every scale but 1.0. Growing the rect handed to
				// SliderBehavior by exactly that padding makes its inset land
				// back on the track: at 0 the grab's left edge sits on
				// trackMin, at 1 its right edge on trackMax.
				//
				// Widening this rect rather than nudging the returned grab is
				// deliberate: the same rect drives `clicked_t`, so the drag
				// mapping moves with the drawn geometry instead of drifting
				// 2px away from it.
				constexpr float kImGuiGrabPadding = 2.0f;
				ImRect rcBehaviour( rcTrackHit );
				rcBehaviour.Min.x -= kImGuiGrabPadding;
				rcBehaviour.Max.x += kImGuiGrabPadding;

				ImGui::PushStyleVar( ImGuiStyleVar_GrabMinSize, Px( tok::kHandleW ) );
				const bool bChanged = ImGui::SliderBehavior(
					rcBehaviour, id, eType, pValue, pMin, pMax, pszFormat,
					ImGuiSliderFlags_AlwaysClamp, pOutGrab );
				ImGui::PopStyleVar();

				// requests-2026-09-06.md item 4: "Outline Width", "Dot >
				// Size" and "Line > Width" all drew a grab far wider than
				// every other slider's. GrabMinSize is a MINIMUM, and for a
				// non-decimal data type (every SliderInt) ImGui's own
				// SliderBehaviorT widens the grab past it on purpose --
				// "if possible have the grab size represent 1 unit"
				// (imgui_widgets.cpp) -- so a coarse, small-range int
				// slider gets a grab spanning a large fraction of the
				// track. No style var caps that growth, only a floor.
				//
				// Recentring pOutGrab here, AFTER SliderBehavior has
				// already used its own (possibly wider) grab rect to
				// resolve this frame's click/drag and step the value, is
				// what keeps the keyboard step and drag math untouched --
				// only the PAINTED width changes. Clamped to the hit
				// rect's own width so a pathologically narrow track still
				// cannot overflow it.
				if ( pOutGrab )
					*pOutGrab = ConstantWidthGrab( *pOutGrab, Px( tok::kHandleW ), rcTrackHit.GetWidth() );
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

				// issue #86: the fill spans the whole track, not the grab's
				// inset travel range. Deriving it from rcGrab.GetCenter() --
				// an earlier "one source of truth" attempt -- is wrong at the
				// ends: the grab centre can only reach half a handle in from
				// each cap (grab_sz/2 == 4px at 1x, once SliderGrab has
				// cancelled grab_padding), so 100% left a 4px unfilled sliver
				// of TrackOff at the right cap and 0% painted a 4px stub of
				// fill at the left one. Away from the
				// ends the two maps differ by at most half a handle width and
				// the 8px-wide opaque handle sits over the seam, so the linear
				// map is exact at 0/1 and invisible in between.
				const float flFillMax =
					rcHit.Min.x + rcHit.GetWidth() * ImClamp( flFraction, 0.0f, 1.0f );
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
				// B's "0 0 0 2px accent@18%" is a spread-only box-shadow, so
				// its corners follow the handle's radius grown by the spread.
				// Drawn square, a 24px-tall ring around an 8px-tall track reads
				// as a hard box sitting on the slider rather than a halo.
				Dl()->AddRectFilled( ImVec2( hMin.x - flHalo, hMin.y - flHalo ),
				                     ImVec2( hMax.x + flHalo, hMax.y + flHalo ),
				                     Accent( 0.18f ), Px( tok::kHandleRound ) + flHalo );
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
		//  The one inline editor -- Text's edit state, and the Stepper's
		// =================================================================
		// SPEC §3.6's field: raised fill, 1px Accent bottom edge, Accent
		// caret, Enter commits, Esc reverts, an outside click commits. When
		// request #14 (2026-09-05) gave the Stepper typed entry, the choice
		// was between calling this or writing a second InputText with its
		// own commit rules -- and "what does blur do" is exactly the kind of
		// question two copies answer differently within a month. So there is
		// one editor; Text() and Stepper() both draw it, and the only things
		// they decide are the rect, the flags and what to do with the text.
		//
		// Returns true on commit, with szBuf holding the text to commit.
		// *pbEditing is cleared on EVERY exit, commit or cancel, so the
		// caller's one bit of state can never outlive the field.
		bool EditField( const ImRect &rc, char *szBuf, size_t nBuf, bool *pbEditing,
		                ImGuiInputTextFlags nFlags, bool bError )
		{
			Dl()->AddRectFilled( rc.Min, rc.Max, Col( Role::SurfaceRaised ) );
			Dl()->AddRectFilled( ImVec2( rc.Min.x, rc.Max.y - Hairline() ), rc.Max,
				bError ? Col( Role::Danger ) : Col( Role::AccentBase ) );

			ImGui::SetCursorScreenPos( ImVec2( rc.Min.x + Px( tok::kSelfPadX ), rc.Min.y ) );
			ImGui::SetNextItemWidth( ImMax( rc.GetWidth() - Px( tok::kSelfPadX ) * 2.0f, 1.0f ) );
			ImGui::PushStyleColor( ImGuiCol_FrameBg, IM_COL32_BLACK_TRANS );
			ImGui::PushStyleColor( ImGuiCol_Text, Col( Role::TextPrimary ) );
			if ( ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive() )
				ImGui::SetKeyboardFocusHere();

			bool bCommitted = false;
			if ( ImGui::InputText( "##edit", szBuf, nBuf, nFlags | ImGuiInputTextFlags_EnterReturnsTrue ) )
			{
				bCommitted = true;
				*pbEditing = false;
			}
			else if ( ImGui::IsItemDeactivated() )
			{
				// ImGui itself reverts the buffer on Esc; the shell's Esc
				// handler also drops the caller's bit, so this branch is
				// reached for an outside click, a Tab away, or an Esc the
				// shell did not see first. Either way the field closes.
				bCommitted = !ImGui::IsKeyPressed( ImGuiKey_Escape );
				*pbEditing = false;
			}
			ImGui::PopStyleColor( 2 );
			return bCommitted;
		}

		// =================================================================
		//  Stepper -- SPEC §3.5
		// =================================================================
		// The typed field's width, in base units. Six digits of Mono Value
		// text plus the caret: wide enough for every range a Stepper in the
		// product declares (7680, 500, 480) with room to type past it, and
		// the parse clamps whatever lands. A constant here, not in Tokens.h,
		// because it is this atom's alone (Controls.h: "per-control widths
		// are constants in Controls.cpp").
		constexpr float kStepperEditW = 72.0f;

		float StepperEditWidthPx( const char *pszUnit )
		{
			float flW = Px( kStepperEditW );
			if ( pszUnit && *pszUnit )
				flW += Px( tok::kS ) + MeasureText( TypeRole::Value, pszUnit ).x;
			return flW;
		}

		bool ParseClampedInt( const char *pszText, int nMin, int nMax, int *pnOut )
		{
			if ( !pszText || !pnOut )
				return false;
			while ( *pszText == ' ' || *pszText == '\t' )
				++pszText;
			if ( !*pszText )
				return false;

			char *pEnd = nullptr;
			errno = 0;
			const long lParsed = strtol( pszText, &pEnd, 10 );
			if ( pEnd == pszText || errno == ERANGE )
				return false;
			while ( *pEnd == ' ' || *pEnd == '\t' )
				++pEnd;
			// "12.5", "1e3", "1600px": not a whole number -> not a value.
			// The field's CharsDecimal filter already keeps letters out; this
			// is the rule for what gets past it, and for tests.
			if ( *pEnd )
				return false;

			if ( nMin > nMax )
				std::swap( nMin, nMax );
			// Clamped to the range and nothing else -- deliberately NOT
			// snapped to the step; see Controls.h for why (the step is the
			// buttons' increment, not a validity grid).
			*pnOut = (int)std::clamp( lParsed, (long)nMin, (long)nMax );
			return true;
		}

		bool Stepper( const RowCtx &row, const char *pszId, int *pnValue,
		              int nMin, int nMax, int nStep, const StepperEdit *pEdit )
		{
			// B's borderless "- +": two 18-wide glyph hit boxes, 8 apart. The
			// number is NOT here -- it lives in the value column, which is
			// what SPEC §2.3's amendment is about.
			const ImRect rcGroup = row.Place( tok::kStepperW );
			const float flGlyphW = Px( tok::kStepperGlyphW );

			ImGui::PushID( pszId );
			bool bChanged = false;

			// ---- typed entry (request #14) -------------------------------
			// The value column belongs to the row, which drew the number
			// there before calling this; the atom is handed that rect and
			// makes it a target. Vertically it takes the control's own hit
			// box, not the row's full height, so the field is exactly as
			// tall as Text()'s and the row does not grow.
			if ( pEdit && pEdit->pbEditing )
			{
				const ImRect rcVal( pEdit->rcValue.Min.x, rcGroup.Min.y,
				                    pEdit->rcValue.Max.x, rcGroup.Max.y );
				const bool bUnit = pEdit->pszUnit && *pEdit->pszUnit;

				if ( *pEdit->pbEditing )
				{
					// [ field ][ gap ][ unit ]. The unit stays a label
					// outside the field so the user types only the number;
					// the field takes whatever the split left after it.
					const float flUnitW = bUnit
						? MeasureText( TypeRole::Value, pEdit->pszUnit ).x + Px( tok::kS ) : 0.0f;
					const ImRect rcField( rcVal.Min.x, rcVal.Min.y,
					                      ImMax( rcVal.Min.x, rcVal.Max.x - flUnitW ), rcVal.Max.y );

					char szBuf[ 32 ];
					snprintf( szBuf, sizeof( szBuf ), "%d", *pnValue );

					// Submitted BEFORE the buttons: a click on "-" while the
					// field is open blurs the field (commit) and then steps,
					// in that order, within this one frame.
					if ( EditField( rcField, szBuf, sizeof( szBuf ), pEdit->pbEditing,
						ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsDecimal, false ) )
					{
						int nTyped = *pnValue;
						if ( ParseClampedInt( szBuf, nMin, nMax, &nTyped ) && nTyped != *pnValue )
						{
							*pnValue = nTyped;
							bChanged = true;
						}
					}
					if ( bUnit )
						DrawText( ImRect( rcField.Max.x, rcVal.Min.y, rcVal.Max.x, rcVal.Max.y ),
							TypeRole::Value, Col( Role::TextPrimary ), pEdit->pszUnit, TextAlign::Right );
				}
				else if ( rcVal.GetWidth() > 0.0f )
				{
					// Closed: the number is a click target with Text()'s
					// closed-state grammar -- hairline on hover, nothing at
					// rest. The number itself was already drawn by the row.
					const Atom aVal = Begin( rcVal, "val" );
					if ( aVal && aVal.bPressed )
						*pEdit->pbEditing = true;
					if ( aVal.bHovered )
					{
						Dl()->AddRectFilled( rcVal.Min, rcVal.Max, palette::White( 0.06f ) );
						Boundary( rcVal, Col( Role::LineControl ) );
					}
				}
			}

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
				// on the control line. DrawDropdownChrome() is the one place
				// that draws this -- controls::Dropdown() below shares it.
				const Atom a = DrawDropdownChrome( row, *pnValue, pOptions, nOptions, bPopupOpen );
				if ( a && a.bPressed )
					res.bWantsPopup = true;
			}

			ImGui::PopID();
			return res;
		}

		// =================================================================
		//  Dropdown -- see Controls.h
		// =================================================================
		DropdownResult Dropdown( const RowCtx &row, const char *pszId, int *pnValue,
		                        const Option *pOptions, size_t nOptions, bool bRowSelected )
		{
			DropdownResult out;
			if ( !pOptions || nOptions == 0 || !pnValue )
				return out;

			ImGui::PushID( pszId );

			// The disambiguating key -- see DropdownPopupState's own comment
			// for why this is an ImGuiID and not pszId itself. "dd" is
			// exactly the id string DrawDropdownChrome()'s Begin() call
			// hashes next, from the same window/PushID stack this GetID()
			// reads right now, so the two are guaranteed to agree.
			const ImGuiID idScope = ImGui::GetID( "dd" );

			// Apply a pick the popup made on an earlier frame, if it belongs
			// to THIS control -- see Controls.h's "ONE-FRAME-LATE COMMIT".
			if ( s_DropdownPopup.bHasCommit && s_DropdownPopup.idCommit == idScope )
			{
				if ( *pnValue != s_DropdownPopup.nCommitValue )
				{
					*pnValue = s_DropdownPopup.nCommitValue;
					out.bChanged = true;
				}
				s_DropdownPopup.bHasCommit = false;
				s_DropdownPopup.idCommit = 0;
			}

			// This frame's chrome mirrors LAST frame's open state -- exactly
			// the lag Shell.cpp's own bPopupOpen already has (it is computed
			// from the previous frame's s_sOpenDropdown before this same
			// Choice/Dropdown call runs), so a freshly-opened box does not
			// show the accent tint until the frame after. Accepted there
			// already; accepted here for the same reason.
			const bool bWasOpen = ( s_DropdownPopup.idOpen == idScope );
			const Atom a = DrawDropdownChrome( row, *pnValue, pOptions, nOptions, bWasOpen );

			bool bWantsOpen = ( a && a.bPressed );
			if ( !bWantsOpen && bRowSelected &&
			     ( ImGui::IsKeyPressed( ImGuiKey_Enter, false ) ||
			       ImGui::IsKeyPressed( ImGuiKey_KeypadEnter, false ) ||
			       ImGui::IsKeyPressed( ImGuiKey_Space, false ) ) )
				bWantsOpen = true;

			if ( bWantsOpen )
			{
				if ( bWasOpen )
				{
					// A second press on the OWNING control while its own list
					// is open toggles it closed -- native dropdown behaviour.
					s_DropdownPopup = DropdownPopupState{};
				}
				else if ( !IsModalOpen() )
				{
					s_DropdownPopup             = DropdownPopupState{};
					s_DropdownPopup.idOpen      = idScope;
					s_DropdownPopup.pOptions    = pOptions;
					s_DropdownPopup.nOptions    = nOptions;
					s_DropdownPopup.rcAnchor    = a.rc;
					for ( size_t i = 0; i < nOptions; ++i )
						if ( pOptions[ i ].nValue == *pnValue )
							s_DropdownPopup.nSelected = (int)i;
				}
			}
			else if ( bWasOpen )
			{
				// Keep the anchor/options fresh across frames the popup stays
				// open without a click here -- the row this box belongs to
				// can move (a scroll, a rebuilt area above it).
				s_DropdownPopup.rcAnchor = a.rc;
				s_DropdownPopup.pOptions = pOptions;
				s_DropdownPopup.nOptions = nOptions;
			}

			ImGui::PopID();
			return out;
		}

		ImRect DropdownPopupRect( const ImRect &rcAnchor, const ImRect &rcSlab,
		                         float flMinContentWidthPx, int nItemCount,
		                         int nMaxVisibleRows )
		{
			// ListBox()'s own row height -- see this function's header
			// comment for why the two must never disagree.
			const float flRowH = Px( tok::kControlH );
			const int   nVisible = std::max( 1, std::min( std::max( 1, nMaxVisibleRows ), std::max( 1, nItemCount ) ) );
			const float flH = flRowH * (float)nVisible;

			// Never narrower than the box it drops from, never wider than
			// the slab (minus a hairline margin either side).
			const float flMarginPx = Px( tok::kS );
			float flW = std::max( rcAnchor.GetWidth(), flMinContentWidthPx );
			flW = std::min( flW, std::max( 1.0f, rcSlab.GetWidth() - flMarginPx * 2.0f ) );

			// Right-aligned under the box, exactly as Choice's own dropdown
			// value column is right-aligned in its lane -- a list dropping
			// from the LEFT edge would not line up with the control it came
			// from. Clamped so it never runs past either slab edge.
			float x1 = std::min( rcAnchor.Max.x, rcSlab.Max.x - flMarginPx );
			x1 = std::max( x1, rcSlab.Min.x + flMarginPx + flW );
			const float x0 = x1 - flW;

			// Flip above the anchor when there is no room below -- a list
			// that runs off the bottom of the slab is "the control is there
			// but you cannot reach it", the same failure a scroll fixes.
			float y0 = rcAnchor.Max.y;
			if ( y0 + flH > rcSlab.Max.y - flMarginPx )
				y0 = std::max( rcSlab.Min.y, rcAnchor.Min.y - flH );

			return ImRect( x0, y0, x0 + flW, y0 + flH );
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
				// A real input, swapped in -- EditField() above, shared with
				// the Stepper's typed entry. Enter commits, Esc reverts, an
				// outside click commits.
				char szBuf[ 256 ];
				snprintf( szBuf, sizeof( szBuf ), "%s", psValue->c_str() );
				if ( EditField( rc, szBuf, sizeof( szBuf ), pbEditing, 0, pszError != nullptr ) )
				{
					*psValue = szBuf;
					bCommitted = true;
				}
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
			return VerbAt( row.PlacePx( flW ), pszId, pszVerb, eIntent, bEnabled );
		}

		int VerbStrip( const ImRect &rcStrip, const char *pszId, const VerbSpec *pVerbs, size_t nVerbs )
		{
			if ( !pVerbs || nVerbs == 0 || rcStrip.GetWidth() <= 0.0f )
				return -1;
			ImGui::PushID( pszId );
			const float flGap  = Px( tok::kGapSeg );
			const float flCell = ( rcStrip.GetWidth() - flGap * (float)( nVerbs - 1 ) ) / (float)nVerbs;
			int nPressed = -1;
			for ( size_t i = 0; i < nVerbs; ++i )
			{
				const float x0 = rcStrip.Min.x + (float)i * ( flCell + flGap );
				const ImRect rc( x0, rcStrip.Min.y, x0 + flCell, rcStrip.Max.y );
				char szId[ 16 ];
				snprintf( szId, sizeof( szId ), "verb%d", (int)i );
				if ( VerbAt( rc, szId, pVerbs[ i ].pszLabel ? pVerbs[ i ].pszLabel : "",
				             pVerbs[ i ].eIntent, pVerbs[ i ].bEnabled ) )
					nPressed = (int)i;
			}
			ImGui::PopID();
			return nPressed;
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

			// request #5 (2026-09-04): the full-colour picker's three rails
			// are plain sRGB now, not OKLCH -- RgbUser carries the OTHER two
			// channels (each already in the control's own 0-255 range) so a
			// rail can gradient itself against what the other two are
			// currently set to, same shape as LchUser did for L/C/H.
			struct RgbUser { float flR, flG, flB; };

			ImU32 RStop( float flT, void *pUser )
			{
				const RgbUser *u = (const RgbUser *)pUser;
				return IM_COL32( (int)( flT * 255.0f + 0.5f ), (int)( u->flG + 0.5f ), (int)( u->flB + 0.5f ), 255 );
			}
			ImU32 GStop( float flT, void *pUser )
			{
				const RgbUser *u = (const RgbUser *)pUser;
				return IM_COL32( (int)( u->flR + 0.5f ), (int)( flT * 255.0f + 0.5f ), (int)( u->flB + 0.5f ), 255 );
			}
			ImU32 BStop( float flT, void *pUser )
			{
				const RgbUser *u = (const RgbUser *)pUser;
				return IM_COL32( (int)( u->flR + 0.5f ), (int)( u->flG + 0.5f ), (int)( flT * 255.0f + 0.5f ), 255 );
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
		                float *pflR, float *pflG, float *pflB )
		{
			ImGui::PushID( pszId );
			bool bChanged = false;

			// A square swatch on the left showing the resolved colour, three
			// stacked rails to its right -- same layout as before request #5
			// (2026-09-04), which only changed what the three rails MEAN
			// (R/G/B, not OKLCH L/C/H). The swatch is the only part that
			// answers "what did I actually pick", which is why it is drawn
			// from the exact same 0-255 components the rails edit.
			const float flGap     = Px( tok::kS );
			const float flSwatchW = ImMin( rcBody.GetHeight(), rcBody.GetWidth() * 0.25f );
			const ImRect rcSwatch( rcBody.Min.x, rcBody.Min.y,
			                       rcBody.Min.x + flSwatchW, rcBody.Max.y );

			Dl()->AddRectFilled( rcSwatch.Min, rcSwatch.Max,
				IM_COL32( (int)( *pflR + 0.5f ), (int)( *pflG + 0.5f ), (int)( *pflB + 0.5f ), 255 ), Px( 3.0f ) );
			Boundary( rcSwatch, Col( Role::LineControl ), Px( 3.0f ) );

			// A one-letter channel label sits left of each rail -- Meta role,
			// same register as every other "unit / mark" in this UI (a
			// segmented cell's suffix, the Text atom's trailing pencil).
			// Mono at this size means R/G/B are all the same width, so one
			// measurement sizes the column for all three; the rails then
			// start flGap further right than before, which is the only
			// geometry change -- their hit rects still run flush to
			// rcBody.Max.x, so nothing about grabbing or dragging a rail
			// moves except its left edge.
			const float flLabelW = MeasureText( TypeRole::Meta, "R" ).x;
			const float flX0     = rcSwatch.Max.x + flGap + flLabelW + flGap;
			const float flRailGap = Px( 4.0f );
			const float flRailH  = ( rcBody.GetHeight() - flRailGap * 2.0f ) / 3.0f;

			RgbUser u{ *pflR, *pflG, *pflB };
			struct { float *pf; float flLo, flHi; RailColorFn fn; const char *pszId; const char *pszLabel; } kRails[] = {
				{ pflR, 0.0f, 255.0f, RStop, "r", "R" },
				{ pflG, 0.0f, 255.0f, GStop, "g", "G" },
				{ pflB, 0.0f, 255.0f, BStop, "b", "B" },
			};

			for ( int i = 0; i < 3; ++i )
			{
				const float flY0 = rcBody.Min.y + (float)i * ( flRailH + flRailGap );
				const float flY1 = flY0 + flRailH;

				const ImRect rcLabel( rcSwatch.Max.x + flGap, flY0, flX0 - flGap, flY1 );
				DrawText( rcLabel, TypeRole::Meta, Col( Role::TextMeta ), kRails[ i ].pszLabel, TextAlign::Center );

				const ImRect rcRail( flX0, flY0, rcBody.Max.x, flY1 );
				if ( Rail( rcRail, kRails[ i ].pszId, kRails[ i ].pf,
					kRails[ i ].flLo, kRails[ i ].flHi, kRails[ i ].fn, &u ) )
				{
					// Re-seed the shared user data so the two rails BELOW
					// this one gradient against the value just set, not the
					// one it replaced. Without this the G and B strips would
					// lag a frame behind the R they are supposed to show.
					u = RgbUser{ *pflR, *pflG, *pflB };
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

		// =================================================================
		//  ListBox -- see Controls.h
		// =================================================================
		int ListBoxStep( int nSelected, int nCount, ListBoxNav eNav )
		{
			if ( nCount <= 0 )
				return -1;
			switch ( eNav )
			{
				case ListBoxNav::Home: return 0;
				case ListBoxNav::End:  return nCount - 1;
				case ListBoxNav::Up:   return nSelected <= 0 ? 0 : nSelected - 1;
				case ListBoxNav::Down: return nSelected < 0 ? 0 : std::min( nSelected + 1, nCount - 1 );
			}
			return std::clamp( nSelected, 0, nCount - 1 );
		}

		int ListBoxScrollForSelection( int nScrollTop, int nSelected, int nVisibleRows, int nCount )
		{
			const int nMaxTop = std::max( 0, nCount - std::max( 0, nVisibleRows ) );
			nScrollTop = std::clamp( nScrollTop, 0, nMaxTop );
			if ( nSelected < 0 || nVisibleRows <= 0 )
				return nScrollTop;
			if ( nSelected < nScrollTop )
				nScrollTop = nSelected;
			else if ( nSelected >= nScrollTop + nVisibleRows )
				nScrollTop = nSelected - nVisibleRows + 1;
			return std::clamp( nScrollTop, 0, nMaxTop );
		}

		ListBoxItemLayout LayoutListBoxItem( const ImRect &rcItem, float flTagWidthPx,
		                                     float flSecondaryWidthPx, float flPadPx )
		{
			ListBoxItemLayout out;

			float flX = rcItem.Min.x;
			if ( flTagWidthPx > 0.0f )
			{
				out.rcTag = ImRect( flX, rcItem.Min.y, flX + flTagWidthPx, rcItem.Max.y );
				flX = out.rcTag.Max.x + flPadPx;
			}
			else
			{
				out.rcTag = ImRect( flX, rcItem.Min.y, flX, rcItem.Max.y );
			}

			// The secondary column is reserved only when doing so still
			// leaves the label at least one padding's worth of width --
			// SPEC states no minimum label width, so kGapLabel (the row
			// grammar's own label<->value gap) is reused as "the smallest
			// gap that still reads as two columns" rather than inventing a
			// second constant nobody asked for.
			float flLabelMax = rcItem.Max.x;
			if ( flSecondaryWidthPx > 0.0f &&
			     ( rcItem.Max.x - flSecondaryWidthPx - flPadPx - flX ) >= flPadPx )
			{
				out.rcSecondary = ImRect( rcItem.Max.x - flSecondaryWidthPx, rcItem.Min.y,
				                         rcItem.Max.x, rcItem.Max.y );
				flLabelMax = out.rcSecondary.Min.x - flPadPx;
				out.bSecondaryShown = true;
			}
			else
			{
				out.rcSecondary = ImRect( rcItem.Max.x, rcItem.Min.y, rcItem.Max.x, rcItem.Max.y );
			}

			out.rcLabel = ImRect( flX, rcItem.Min.y, std::max( flX, flLabelMax ), rcItem.Max.y );
			return out;
		}

		ListBoxResult ListBox( const ImRect &rcBody, const char *pszId, int *pnSelected,
		                      const ListBoxItem *pItems, size_t nItems, int nMaxVisibleRows )
		{
			ListBoxResult out;
			if ( !pnSelected )
				return out;

			ImGui::PushID( pszId );
			const ImGuiID id = ImGui::GetID( "##scroll" );
			ImGuiStorage *pStorage = ImGui::GetStateStorage();

			const int   nCount  = (int)nItems;
			const float flRowH  = Px( tok::kControlH );
			const int   nFit    = ( rcBody.GetHeight() > 0.0f && flRowH > 0.0f )
			                     ? (int)( rcBody.GetHeight() / flRowH ) : 0;
			const int   nVisible = std::max( 1, std::min( std::max( 1, nMaxVisibleRows ), std::max( 1, nFit ) ) );

			// KEYBOARD: only while the pointer hovers the list. See
			// Controls.h's ListBox() comment for why this widget has no
			// ID-based focus of its own.
			const bool bHovered = ImGui::IsMouseHoveringRect( rcBody.Min, rcBody.Max );
			if ( bHovered && nCount > 0 )
			{
				bool       bNav = true;
				ListBoxNav eNav = ListBoxNav::Down;
				if      ( ImGui::IsKeyPressed( ImGuiKey_DownArrow, true ) ) eNav = ListBoxNav::Down;
				else if ( ImGui::IsKeyPressed( ImGuiKey_UpArrow,   true ) ) eNav = ListBoxNav::Up;
				else if ( ImGui::IsKeyPressed( ImGuiKey_Home,      false ) ) eNav = ListBoxNav::Home;
				else if ( ImGui::IsKeyPressed( ImGuiKey_End,       false ) ) eNav = ListBoxNav::End;
				else bNav = false;

				if ( bNav )
				{
					const int nNext = ListBoxStep( *pnSelected, nCount, eNav );
					if ( nNext != *pnSelected )
					{
						*pnSelected = nNext;
						out.bChanged = true;
					}
				}

				if ( *pnSelected >= 0 &&
				     ( ImGui::IsKeyPressed( ImGuiKey_Enter, false ) ||
				       ImGui::IsKeyPressed( ImGuiKey_KeypadEnter, false ) ) )
					out.bActivated = true;
			}

			int nScroll = pStorage->GetInt( id, 0 );
			if ( bHovered && nCount > nVisible )
			{
				const float flWheel = ImGui::GetIO().MouseWheel;
				if ( flWheel != 0.0f )
					nScroll -= (int)flWheel;
			}
			nScroll = ListBoxScrollForSelection( nScroll, *pnSelected, nVisible, nCount );

			// ---- chrome: wire lines only, no fill, no rounding (the sketch) --
			Dl()->AddRectFilled( rcBody.Min, rcBody.Max, palette::White( 0.03f ) );
			Boundary( rcBody, Col( Role::LineControl ) );

			const bool  bScrollbar   = nCount > nVisible;
			const float flScrollbarW = bScrollbar ? Px( tok::kXS ) : 0.0f;
			const float flPad        = Px( tok::kS );

			for ( int nRow = 0; nRow < nVisible; ++nRow )
			{
				const int i = nScroll + nRow;
				if ( i < 0 || i >= nCount )
					break;

				const ImRect rcItem( rcBody.Min.x, rcBody.Min.y + flRowH * (float)nRow,
				                     rcBody.Max.x - flScrollbarW, rcBody.Min.y + flRowH * (float)( nRow + 1 ) );

				char szRowId[ 16 ];
				snprintf( szRowId, sizeof( szRowId ), "row%d", i );
				const Atom a = Begin( rcItem, szRowId );

				const bool bSelected = ( i == *pnSelected );
				if ( a && a.bPressed )
				{
					if ( *pnSelected != i )
					{
						*pnSelected = i;
						out.bChanged = true;
					}
					// Click == activate (Controls.h: "Enter = activate = same
					// as click"), whether or not the selection actually moved.
					out.bActivated = true;
				}

				// requests-2026-09-06.md item 5: an outline alone (below)
				// read as not-selected-enough against the Profiles list's
				// busy rows. Filled now with the same accent-soft backdrop
				// DrawRail() paints behind the selected rail item
				// (Shell.cpp: `Fill( rcItem, Accent( 0.10f ) )`) -- the
				// nearest sibling in this kit's own language, a single
				// selected row in a vertical list, rather than a
				// segmented Choice cell's stronger 24% (that fill marks
				// which of several buttons is "on", not which row of many
				// is selected).
				if ( bSelected )
					Dl()->AddRectFilled( rcItem.Min, rcItem.Max, Accent( 0.10f ) );
				else if ( a.bHovered )
					Dl()->AddRectFilled( rcItem.Min, rcItem.Max, palette::White( 0.05f ) );

				const ListBoxItem &item  = pItems[ i ];
				const bool  bHasTag      = item.pszTag && *item.pszTag;
				const bool  bHasSecond   = item.pszSecondary && *item.pszSecondary;
				const float flTagW       = bHasTag    ? MeasureText( TypeRole::Meta, item.pszTag ).x + Px( tok::kS ) : 0.0f;
				const float flSecW       = bHasSecond ? MeasureText( TypeRole::Meta, item.pszSecondary ).x : 0.0f;

				const ImRect rcInner( rcItem.Min.x + flPad, rcItem.Min.y, rcItem.Max.x - flPad, rcItem.Max.y );
				const ListBoxItemLayout lay = LayoutListBoxItem( rcInner, flTagW, flSecW, Px( tok::kGapLabel ) );

				if ( bHasTag )
					DrawText( lay.rcTag, TypeRole::Meta, Col( Role::TextMeta ), item.pszTag );
				if ( item.pszLabel )
					DrawText( lay.rcLabel, TypeRole::Label,
					         bSelected ? Col( Role::TextPrimary ) : Col( Role::TextLabel ), item.pszLabel );
				if ( lay.bSecondaryShown && bHasSecond )
					DrawText( lay.rcSecondary, TypeRole::Meta, Col( Role::TextMeta ), item.pszSecondary, TextAlign::Right );

				// Selected: a 1px accent outline, no fill -- exactly the sketch.
				if ( bSelected )
					Boundary( rcItem, Accent( 1.0f ) );
				if ( nRow + 1 < nVisible )
					Dl()->AddLine( ImVec2( rcItem.Min.x, rcItem.Max.y ), ImVec2( rcItem.Max.x, rcItem.Max.y ),
					              Col( Role::Line ), Hairline() );
			}

			if ( bScrollbar )
			{
				const float flTrackX = rcBody.Max.x - flScrollbarW;
				Dl()->AddRectFilled( ImVec2( flTrackX, rcBody.Min.y ), ImVec2( rcBody.Max.x, rcBody.Max.y ),
				                    palette::White( 0.05f ) );

				const float flThumbH = std::max( flRowH, rcBody.GetHeight() * ( (float)nVisible / (float)nCount ) );
				const float flRange  = std::max( 1, nCount - nVisible );
				const float flThumbY = rcBody.Min.y +
					( (float)nScroll / (float)flRange ) * ( rcBody.GetHeight() - flThumbH );
				Dl()->AddRectFilled( ImVec2( flTrackX, flThumbY ), ImVec2( rcBody.Max.x, flThumbY + flThumbH ),
				                    Accent( 0.55f ) );
			}

			pStorage->SetInt( id, nScroll );
			ImGui::PopID();
			return out;
		}
	}

	// =========================================================================
	//  Modal -- see Controls.h
	// =========================================================================
	namespace
	{
		struct ModalState
		{
			bool       bOpen = false;
			ModalSpec  spec;
			// The previous frame's measured body height, so the panel can be
			// sized and centred BEFORE fnBody runs this frame -- the standard
			// immediate-mode "auto-size from last frame" trick (the same one
			// ImGuiWindowFlags_AlwaysAutoResize itself relies on). The one
			// visible cost: a body whose row count changes on its own (the
			// sketch's Switch-gated GameID/name fields) grows or shrinks the
			// dialog one frame after the toggle, not the same frame. Accepted
			// rather than running fnBody twice a frame to measure it first --
			// fnBody may itself have side effects (EditField's own commit-on-
			// Enter), and running a control atom's Begin()/ItemAdd() twice in
			// one frame against the same ids is not a thing this kit's atoms
			// are built to tolerate.
			float      flLastBodyHeightPx = 0.0f;
		};
		ModalState s_Modal;
	}

	RowCtx ModalNextRow( ModalBodyCtx &ctx )
	{
		const RowCtx row = RowCtx::ForRow( ctx.lane, ctx.rcBody.Min.x, ctx.flCursorY );
		ctx.flCursorY += Px( tok::kRowH );
		return row;
	}

	ImRect ModalNextBlock( ModalBodyCtx &ctx, float flHeightPx )
	{
		const ImRect rc( ctx.rcBody.Min.x, ctx.flCursorY, ctx.rcBody.Max.x, ctx.flCursorY + flHeightPx );
		ctx.flCursorY += flHeightPx;
		return rc;
	}

	void OpenModal( ModalSpec spec )
	{
		// SPEC gap, resolved per this task's brief: "opening one while
		// another is open is a programming error" -- IM_ASSERT() in a debug/
		// assertions build (imgui.h's own default: assert() from <cassert>,
		// compiled out under NDEBUG), the open modal left untouched either
		// way so a release build degrades to "ignored" rather than a crash
		// or a clobbered dialog.
		IM_ASSERT( !s_Modal.bOpen && "OpenModal() called while a modal is already open" );
		if ( s_Modal.bOpen )
			return;

		s_Modal.bOpen = true;
		// One row tall, or `flMinBodyRows` rows if the caller stated one,
		// until DrawModal() has measured a real body -- see ModalSpec's own
		// comment on flMinBodyRows for why this is a hint and not a
		// guarantee (a body whose row count changes at runtime still grows
		// or shrinks a frame late either way).
		s_Modal.flLastBodyHeightPx = Px( tok::kRowH ) * std::max( 1.0f, spec.flMinBodyRows );
		s_Modal.spec  = std::move( spec );
	}

	void CloseModal()
	{
		s_Modal = ModalState{};
	}

	bool IsModalOpen() { return s_Modal.bOpen; }

	void DrawModal( const ImRect &rcSlab )
	{
		if ( !s_Modal.bOpen )
			return;

		// ITS OWN TOP-LEVEL WINDOW, exactly Shell.cpp's DrawPalette()/
		// DrawDropdownList() reasoning: `Dl()` below is `GetCurrentWindow()->
		// DrawList`, and a caller that draws this without an enclosing
		// Begin()/End() of its own would post into WHATEVER window ImGui
		// currently considers current -- the implicit debug window, most
		// likely, with that window's own (unrelated, and much smaller) clip
		// rect silently truncating every rect computed above. `SetNextWindow
		// Focus()` and no `NoBringToFrontOnFocus` bring it above the slab,
		// the same way the palette's own window does.
		ImGui::SetNextWindowPos( rcSlab.Min );
		ImGui::SetNextWindowSize( rcSlab.GetSize() );
		ImGui::SetNextWindowFocus();
		ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
		ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );
		const ImGuiWindowFlags eHostFlags =
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoBackground;
		if ( !ImGui::Begin( "##e2modalhost", nullptr, eHostFlags ) )
		{
			ImGui::End();
			ImGui::PopStyleVar( 2 );
			return;
		}

		ImGui::PushID( "e2modal" );

		const float flW      = std::clamp( rcSlab.GetWidth() - Px( tok::kXL ) * 2.0f, Px( 240.0f ), Px( 420.0f ) );
		const float flTitleH = Px( tok::kRowH );
		const float flFootH  = Px( tok::kRowH );
		const float flBodyH  = std::max( s_Modal.flLastBodyHeightPx, Px( tok::kRowH ) );
		const float flH      = flTitleH + flBodyH + flFootH;

		const float x0 = rcSlab.Min.x + ( rcSlab.GetWidth()  - flW ) * 0.5f;
		const float y0 = rcSlab.Min.y + ( rcSlab.GetHeight() - flH ) * 0.5f;
		const ImRect rc( x0, y0, x0 + flW, y0 + flH );

		// The scrim -- the exact fill Shell.cpp's DrawPalette() dims the slab
		// with, repeated rather than reinvented, so the shell never grows a
		// second "surface behind me is dimmed" look.
		Dl()->AddRectFilled( rcSlab.Min, rcSlab.Max, IM_COL32( 0, 0, 0, 150 ) );

		// Two coats, same reason DrawPalette() gives: Role::Surface's own 88%
		// alpha reads a sheet's controls straight through a panel sitting on
		// top of them.
		Dl()->AddRectFilled( rc.Min, rc.Max, Col( Role::Surface ) );
		Dl()->AddRectFilled( rc.Min, rc.Max, Col( Role::Surface ) );
		Boundary( rc, Accent( 0.42f ) );

		const float flPad = Px( tok::kL );
		const ImRect rcTitle( rc.Min.x + flPad, rc.Min.y, rc.Max.x - flPad, rc.Min.y + flTitleH );
		DrawText( rcTitle, TypeRole::Title, Col( Role::TextPrimary ), s_Modal.spec.sTitle.c_str() );
		Dl()->AddLine( ImVec2( rc.Min.x, rcTitle.Max.y ), ImVec2( rc.Max.x, rcTitle.Max.y ),
		              Col( Role::Line ), Hairline() );

		const ImRect rcFoot( rc.Min.x, rc.Max.y - flFootH, rc.Max.x, rc.Max.y );
		Dl()->AddLine( ImVec2( rc.Min.x, rcFoot.Min.y ), ImVec2( rc.Max.x, rcFoot.Min.y ),
		              Col( Role::Line ), Hairline() );

		// ---- body: ordinary rows, the sheet's own grammar ------------------
		ModalBodyCtx ctx;
		ctx.rcBody    = ImRect( rc.Min.x + flPad, rcTitle.Max.y, rc.Max.x - flPad, rcFoot.Min.y );
		ctx.lane      = Lane::ForColumn( ImMax( 0.0f, ctx.rcBody.GetWidth() ) / std::max( Scale(), 0.01f ) );
		ctx.flCursorY = ctx.rcBody.Min.y;
		if ( s_Modal.spec.fnBody )
			s_Modal.spec.fnBody( ctx );
		s_Modal.flLastBodyHeightPx = std::max( ctx.flCursorY - ctx.rcBody.Min.y, Px( tok::kRowH ) );

		// ---- footer: Cancel, then the caller's primary ---------------------
		const float flBtnH     = Px( tok::kControlH );
		const float flBtnGap   = Px( tok::kM );
		const char *pszPrimary = s_Modal.spec.sPrimaryLabel.empty() ? "OK" : s_Modal.spec.sPrimaryLabel.c_str();
		const float flPrimaryW = MeasureText( TypeRole::Meta, pszPrimary ).x + Px( tok::kVerbPadX ) * 2.0f;
		const float flCancelW  = MeasureText( TypeRole::Meta, "Cancel" ).x + Px( tok::kVerbPadX ) * 2.0f;
		const float flBtnY     = rcFoot.Min.y + ( rcFoot.GetHeight() - flBtnH ) * 0.5f;

		const ImRect rcPrimary( rc.Max.x - flPad - flPrimaryW, flBtnY, rc.Max.x - flPad, flBtnY + flBtnH );
		const ImRect rcCancel( rcPrimary.Min.x - flBtnGap - flCancelW, flBtnY,
		                       rcPrimary.Min.x - flBtnGap, flBtnY + flBtnH );

		const bool bCancelClicked  = VerbAt( rcCancel, "cancel", "Cancel", controls::Intent::Neutral, true );
		const bool bPrimaryClicked = VerbAt( rcPrimary, "primary", pszPrimary,
		                                    s_Modal.spec.bPrimaryDanger ? controls::Intent::Danger
		                                                                : controls::Intent::Accent,
		                                    true );

		// ---- keyboard: Esc cancels; Enter confirms when no field is being
		// edited. Controls.h's ModalSpec::fnPrimary comment records why "the
		// LAST Text field" specifically could not be told apart from any
		// other field -- there is no cross-field tab order in this kit, so
		// "nothing is currently active" is the closest honest approximation,
		// and it is also true on the very frame a field's own Enter has just
		// committed it.
		const bool bEsc   = ImGui::IsKeyPressed( ImGuiKey_Escape, false );
		const bool bEnter = ( ImGui::IsKeyPressed( ImGuiKey_Enter, false ) ||
		                     ImGui::IsKeyPressed( ImGuiKey_KeypadEnter, false ) ) &&
		                   !ImGui::IsAnyItemActive();

		if ( bCancelClicked || bEsc )
		{
			std::function<void()> fnCancel = std::move( s_Modal.spec.fnCancel );
			CloseModal();
			if ( fnCancel )
				fnCancel();
		}
		else if ( ( bPrimaryClicked || bEnter ) &&
		          ( !s_Modal.spec.fnValidate || s_Modal.spec.fnValidate() ) )
		{
			std::function<void()> fnPrimary = std::move( s_Modal.spec.fnPrimary );
			CloseModal();
			if ( fnPrimary )
				fnPrimary();
		}

		ImGui::PopID();
		ImGui::End();
		ImGui::PopStyleVar( 2 );
	}

	// =========================================================================
	//  Dropdown popup -- see Controls.h
	// =========================================================================
	void DismissDropdownOnOutsideClick( const ImRect &rcSlab )
	{
		if ( s_DropdownPopup.idOpen == 0 )
			return;
		if ( !ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
			return;
		if ( s_DropdownPopup.pOptions == nullptr || s_DropdownPopup.nOptions == 0 )
			return;

		const float flMinW = DropdownContentWidthPx( s_DropdownPopup );
		const ImRect rcList = controls::DropdownPopupRect( s_DropdownPopup.rcAnchor, rcSlab,
			flMinW, (int)s_DropdownPopup.nOptions, 8 );

		const ImVec2 vMouse = ImGui::GetIO().MousePos;
		if ( rcList.Contains( vMouse ) || s_DropdownPopup.rcAnchor.Contains( vMouse ) )
			return;   // inside the list, or the box that owns it -- its own click handles this

		s_DropdownPopup = DropdownPopupState{};

		// Swallow, exactly Shell.cpp's own former DismissOpenDropdownOnOutsideClick()'s
		// reasoning: without this, a control under the cursor still sees
		// MouseClicked this same frame and can arm itself.
		ImGui::GetIO().MouseClicked[ ImGuiMouseButton_Left ] = false;
	}

	void DrawDropdownPopup( const ImRect &rcSlab )
	{
		if ( s_DropdownPopup.idOpen == 0 )
			return;

		// A Modal opened on top wins outright -- it owns the whole surface,
		// and controls::Dropdown() already refuses to OPEN a new popup while
		// one is up, so the only way to reach this state is a Modal opened
		// from elsewhere while a Dropdown was already open. Self-heal the
		// same way DrawDropdownList() used to: close rather than draw two
		// surfaces that both claim the input.
		//
		// Also self-heals the row-not-drawn-this-frame case: the options
		// pointer is borrowed for one frame only, so a null/empty one means
		// the owning row did not run (the user navigated away, or a dynamic
		// area rebuilt underneath it).
		if ( IsModalOpen() || s_DropdownPopup.pOptions == nullptr || s_DropdownPopup.nOptions == 0 )
		{
			s_DropdownPopup = DropdownPopupState{};
			return;
		}

		if ( ImGui::IsKeyPressed( ImGuiKey_Escape, false ) )
		{
			s_DropdownPopup = DropdownPopupState{};
			return;
		}

		const float flMinW = DropdownContentWidthPx( s_DropdownPopup );
		const ImRect rc = controls::DropdownPopupRect( s_DropdownPopup.rcAnchor, rcSlab,
			flMinW, (int)s_DropdownPopup.nOptions, 8 );

		ImGui::SetNextWindowPos( rcSlab.Min );
		ImGui::SetNextWindowSize( rcSlab.GetSize() );
		ImGui::SetNextWindowFocus();
		ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
		ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );
		const ImGuiWindowFlags eFlags =
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoBackground;

		if ( !ImGui::Begin( "##e2dropdownpopup", nullptr, eFlags ) )
		{
			ImGui::End();
			ImGui::PopStyleVar( 2 );
			return;
		}

		ImGui::PushID( "e2dropdown" );

		// Two coats, same reason DrawModal()/DrawPalette() give: Role::
		// Surface's own translucency reads a sheet's controls straight
		// through a single coat.
		Dl()->AddRectFilled( rc.Min, rc.Max, Col( Role::Surface ) );
		Dl()->AddRectFilled( rc.Min, rc.Max, Col( Role::Surface ) );
		Boundary( rc, Col( Role::AccentBase ) );

		std::vector<controls::ListBoxItem> items;
		items.reserve( s_DropdownPopup.nOptions );
		for ( size_t i = 0; i < s_DropdownPopup.nOptions; ++i )
			items.push_back( controls::ListBoxItem{ s_DropdownPopup.pOptions[ i ].pszLabel, nullptr, nullptr } );

		// ListBox() itself is the entire Up/Down/Home/End/Enter/wheel/click
		// story (Controls.h's own ListBox() comment) -- reused rather than a
		// second, parallel list widget just for this popup.
		const controls::ListBoxResult res = controls::ListBox( rc, "##items",
			&s_DropdownPopup.nSelected, items.data(), items.size(), 8 );

		if ( res.bActivated && s_DropdownPopup.nSelected >= 0 &&
		     (size_t)s_DropdownPopup.nSelected < s_DropdownPopup.nOptions )
		{
			const ImGuiID idOwner = s_DropdownPopup.idOpen;
			const int nValue = s_DropdownPopup.pOptions[ s_DropdownPopup.nSelected ].nValue;
			s_DropdownPopup = DropdownPopupState{};
			s_DropdownPopup.bHasCommit   = true;
			s_DropdownPopup.idCommit     = idOwner;
			s_DropdownPopup.nCommitValue = nValue;
		}

		ImGui::PopID();
		ImGui::End();
		ImGui::PopStyleVar( 2 );
	}

	bool IsDropdownPopupOpen() { return s_DropdownPopup.idOpen != 0; }

	void CloseDropdownPopup() { s_DropdownPopup = DropdownPopupState{}; }
}

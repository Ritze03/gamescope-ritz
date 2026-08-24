// The registry -- one declaration per setting, yielding the sheet row, the
// palette entry and the Inspector's content.
//
// API.md is the contract; SPEC.md §5.2 is the reasoning. The whole point of
// this file is that four laws are enforced HERE, at registration, rather than
// in a style guide:
//
//   1. THE PREFIX LAW      a Param's id is synthesised as "<parent>.<leaf>",
//                          so it cannot name an unrelated setting.
//   2. THE SIX BUDGET      a row owns at most 6 Params; a 7th aborts.
//   3. ID UNIQUENESS       every Entry and every Param id is unique registry-wide.
//   4. HELP IS REQUIRED    .Help() must be called with non-empty text.
//
// Two of the design's guarantees are enforced by the TYPE SYSTEM instead and
// therefore have no runtime check at all, which is stronger:
//
//   * ONE LEVEL. `Param` is a distinct type from `Entry`. Its `Param()` is a
//     *sibling* factory -- it forwards to the parent Entry and returns another
//     child of that same Entry (this is what makes API.md §7's two chained
//     `.Param()` calls read the way they do). There is no operation anywhere
//     that produces a Param owned by a Param, so the One-Level Rule is not
//     checked; it is unsayable.
//   * DETAILS IS READ-ONLY. `.Live()` takes std::function<Fact()> and has no
//     Bind overload, so a control cannot be constructed in Details. Putting a
//     setting in the diagnostics panel is a compile error, not a review catch.
//
// There is deliberately NO Inspector authoring API here -- no ui::Panel, no
// PaneCtx, no Inspect(lambda). SPEC.md §5.2 clause 0. Adding a fifth generator
// means editing this header, which shows up in a diff.
//
// Free of ImGui on purpose: registration is data, and the laws are the part of
// the design most worth testing (tests/test_overlay_ui.cpp).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace gamescope::ui
{
	// =====================================================================
	//  Law violations
	// =====================================================================
	enum class Law : uint8_t
	{
		Prefix,          // a Param id that is not "<parent>.<leaf>"
		OneLevel,        // a Param leaf containing a dot
		SixBudget,       // the 7th Param on one row
		UniqueId,        // an id already registered
		HelpRequired,    // .Help() missing or empty
		ReasonRequired,  // .DisabledUnless() with an empty reason
		Dynamic,         // P3b: a dynamic area rebuilt with no builder
	};

	const char *LawName( Law eLaw );

	struct Violation
	{
		Law         eLaw;
		std::string sId;
		std::string sMessage;
	};

	// A law violation is a boot failure: RegisterAll() runs once at startup,
	// and a registry that breaks a law is a programming error that must not
	// reach a user. The default handler prints and aborts.
	//
	// LawRecorder is the ONE seam that changes what happens after a violation
	// fires -- never whether it fires. While one is alive, violations are
	// collected instead of aborting, which is what lets tests assert that a
	// law caught the thing it is supposed to catch without terminating the
	// test binary. Nothing in the shipping build constructs one.
	class LawRecorder
	{
	public:
		LawRecorder();
		~LawRecorder();
		LawRecorder( const LawRecorder & ) = delete;
		LawRecorder &operator=( const LawRecorder & ) = delete;

		const std::vector<Violation> &Violations() const { return m_Violations; }
		bool Caught( Law eLaw ) const;
		size_t Count() const { return m_Violations.size(); }

	private:
		friend void ReportViolation( Law, const std::string &, const std::string & );
		std::vector<Violation> m_Violations;
		LawRecorder           *m_pPrev = nullptr;
	};

	void ReportViolation( Law eLaw, const std::string &sId, const std::string &sMessage );

	// =====================================================================
	//  Values and bindings
	// =====================================================================
	using Value = std::variant<std::monostate, bool, int, float, std::string>;

	std::string ValueToString( const Value &v );

	// When a setting is applied. Derived from the binding, never typed into a
	// row (API.md §9).
	enum class Applies : uint8_t { Live, NextFrame, NeedsRestart };

	// ---- is a pointer dragging a control right now? -----------------------
	// Published once a frame by the control atoms (Controls.cpp's shared
	// prologue) and read here. It lives in this ImGui-free file on purpose:
	// D19.1's rule is that a registration must never have to ask ImGui a
	// question, because a registration's getters and setters are also reached
	// from the console thread, where there is no context at all. A plain bool
	// is answerable from anywhere, and answers "no" off the render thread,
	// which is the correct answer there.
	//
	// Two things consume it: `overlay.display_scale`, whose apply waits for
	// the drag to end, and Step()'s quantisation below.
	void SetPointerDragActive( bool bActive );
	bool IsPointerDragActive();

	// A type-erased binding. `Cfg()` (schema-backed, knows its destination
	// file) arrives in P2 with the config seam; `Bind()` covers the transient
	// and getter/setter cases the atoms need today.
	class AnyBind
	{
	public:
		AnyBind() = default;

		bool  IsBound() const { return (bool)m_Get; }
		Value Get() const;
		void  Set( const Value &v ) const;

		// Wraps this binding's setter so that a write made DURING A POINTER
		// DRAG is rounded to the nearest multiple of `flStep`. Applied by
		// Entry::Step()/Parameter::Step() on slider kinds; see Registry.cpp
		// for why only the drag is quantised.
		AnyBind &SnapDragsTo( float flStep );

		template <typename T>
		static AnyBind Of( T *p )
		{
			AnyBind b;
			b.m_Get = [ p ] { return Value( *p ); };
			b.m_Set = [ p ]( const Value &v ) { if ( const T *q = std::get_if<T>( &v ) ) *p = *q; };
			return b;
		}

		template <typename T>
		static AnyBind Of( std::function<T()> get, std::function<void( T )> set )
		{
			AnyBind b;
			b.m_Get = [ get ] { return Value( get() ); };
			b.m_Set = [ set ]( const Value &v ) { if ( const T *q = std::get_if<T>( &v ) ) set( *q ); };
			return b;
		}

	private:
		std::function<Value()>             m_Get;
		std::function<void( const Value &)> m_Set;
	};

	template <typename T> AnyBind Bind( T *p ) { return AnyBind::Of<T>( p ); }

	// =====================================================================
	//  The taxonomy
	// =====================================================================
	// SPEC.md §3's eleven kinds. Segmented and Dropdown are one Kind (Choice)
	// because the helper measures and picks between them -- "a caller cannot
	// pick wrong because a caller does not pick" (API.md §12.6).
	enum class Kind : uint8_t
	{
		Switch, Slider, Stepper, Choice, Text, Bank, Action, Meter, Facts, Composite,
	};

	enum class CompositeKind : uint8_t { Anchor, Hue, Strip, Graph, Color };

	// The kind's own name, for Details' binding grid. A lookup rather than a
	// string a call site passes, so the grid cannot be told a kind that is
	// not the one the registration actually has.
	const char *KindName( Kind eKind );

	// SPEC §2.3's table, as a function, "which the helper decides from the
	// control kind, never the caller". A control that can display its own
	// value must never duplicate it in the value column.
	constexpr bool UsesValueColumn( Kind eKind )
	{
		switch ( eKind )
		{
			case Kind::Switch: case Kind::Slider: case Kind::Stepper:
			case Kind::Meter:  case Kind::Composite:
				return true;
			// Choice (segmented cell / dropdown value), Text (the field), Bank
			// (the lit chips) all self-display. Facts' summary is a string in
			// the control zone, not a value. Action has no value.
			case Kind::Choice: case Kind::Text: case Kind::Bank:
			case Kind::Action: case Kind::Facts:
				return false;
		}
		return false;
	}

	// SPEC §5.1: a read-only kind opens the Inspector in Details and its
	// CONFIGURE cell reads `ro`. Not a flag anyone sets.
	constexpr bool IsReadOnly( Kind eKind )
	{
		return eKind == Kind::Meter || eKind == Kind::Facts;
	}

	struct Option
	{
		int         nValue = 0;
		const char *pszLabel = nullptr;
	};

	// What a Composite(Graph) draws: a window of samples, newest last, plus
	// the scale to draw them against. Borrowed, never owned -- the provider
	// hands out a pointer into its own ring buffer for the duration of one
	// frame, exactly as the HUD's own graph already reads it.
	struct SampleWindow
	{
		const float *pflSamples = nullptr;
		size_t       nCount     = 0;
		float        flCeiling  = 0.0f;   // value mapped to the band's full height
		float        flOutlier  = 0.0f;   // at or above this draws in the warn colour; 0 marks none

		// 0 rolls (newest right-aligned); >0 is a fixed axis of that many
		// slots filled from the left, so a warm-up cannot read as a full
		// window. See controls::GraphBody().
		size_t       nAxisSlots = 0;
	};

	// A Details readout. Returned by .Live(); deliberately has no setter.
	struct Fact
	{
		std::string sLabel;
		std::string sValue;
	};

	class Entry;
	class Area;
	class Registry;

	// =====================================================================
	//  Parameter -- inspector-preferred depth attached to an Entry
	// =====================================================================
	// API.md calls this type `Param`; it is spelled `Parameter` here for one
	// mechanical reason: C++ forbids a member function whose name matches its
	// enclosing class, and this type needs a `.Param()` member for API.md §7's
	// chained declarations to compile. The concept, the fluent surface and the
	// laws are exactly API.md's.
	//
	// Its fluent surface is a strict subset of Entry's (API.md §4.2). It has
	// no .Live() that binds anything, and its .Param() is a SIBLING factory on
	// the parent -- never a nesting one.
	class Parameter
	{
	public:
		Parameter &Default( Value v );
		Parameter &Help( const char *pszHelp );
		Parameter &Range( float flLo, float flHi );
		Parameter &Step( float flStep );
		Parameter &Unit( const char *pszUnit );
		Parameter &ZeroMeans( const char *pszWord );
		Parameter &Keywords( const char *pszKeywords );
		Parameter &DisabledUnless( std::function<bool()> pred, const char *pszReason );

		// Sibling factory -- forwards to the owning Entry and returns another
		// child of that same Entry. This is what keeps API.md §7's chained
		// .Param() calls legal while leaving "a Param owning a Param"
		// unsayable: there is no operation, on any type, that nests.
		Parameter &Param( const char *pszLeaf, const char *pszTitle, AnyBind bind );
		Parameter &Param( const char *pszLeaf, const char *pszTitle, AnyBind bind,
		                  const Option *pOptions, size_t nOptions );
		Entry &Live( const char *pszLeaf, std::function<Fact()> fn );

		const std::string &Id() const    { return m_sId; }
		const std::string &Title() const { return m_sTitle; }
		const std::string &HelpText() const { return m_sHelp; }
		// The blob `.Keywords()` recorded. Read by the command palette, which
		// is the reason the setter has existed since P1 -- until now nothing
		// consumed it, so a keyword list was a declaration with no reader.
		const std::string &KeywordText() const { return m_sKeywords; }
		Kind  GetKind() const            { return m_eKind; }
		const AnyBind &Binding() const   { return m_Bind; }
		const Value &DefaultValue() const { return m_Default; }
		const Entry *Owner() const       { return m_pOwner; }
		const std::vector<Option> &Options() const { return m_Options; }

		// ---- P3 read side ------------------------------------------------
		// SPEC §5.3: a Param is drawn "at the same 44 height and the same
		// grammar" as its parent, and §5.2's promotion path turns one into an
		// Entry outright. So the renderer must be able to ask a Param exactly
		// what it asks an Entry -- these are Entry's accessors, name for name,
		// and deliberately not one more. A Param still cannot SAY anything an
		// Entry cannot; it can now only be READ the same way.
		bool  HasRange() const           { return m_bHasRange; }
		float Lo() const                 { return m_flLo; }
		float Hi() const                 { return m_flHi; }
		float StepSize() const           { return m_flStep; }
		const std::string &Unit() const  { return m_sUnit; }
		const std::string &ZeroWord() const { return m_sZeroMeans; }
		bool  UsesValue() const          { return UsesValueColumn( m_eKind ); }

		// SPEC §3.13: "A parameter inherits its parent's reason, EXCEPT when
		// it is the cause of it." That exception is why this reads the Param's
		// OWN predicate and never walks to Owner() -- a param that gates its
		// parent must stay reachable while the parent is greyed, which was a
		// real bug in the first version.
		std::string DisabledReason() const;

		// ---- reset (P3b) -------------------------------------------------
		bool HasDefault() const;
		bool IsAtDefault() const;
		void ResetToDefault() const;

	private:
		friend class Entry;

		std::string m_sId, m_sTitle, m_sHelp, m_sUnit, m_sZeroMeans, m_sKeywords, m_sReason;
		Kind        m_eKind = Kind::Switch;
		AnyBind     m_Bind;
		Value       m_Default;
		float       m_flLo = 0.0f, m_flHi = 0.0f, m_flStep = 0.0f;
		bool        m_bHasRange = false;
		std::vector<Option> m_Options;
		std::function<bool()> m_Enabled;
		Entry      *m_pOwner = nullptr;
	};

	// =====================================================================
	//  Entry -- one setting: row + palette entry + inspector content
	// =====================================================================
	class Entry
	{
	public:
		Entry &Default( Value v );
		Entry &Default( Value vA, Value vB );          // composites with two axes (Anchor)
		Entry &Help( const char *pszHelp );            // REQUIRED -- law 4
		Entry &Range( float flLo, float flHi );
		Entry &Step( float flStep );
		Entry &Unit( const char *pszUnit );
		Entry &ZeroMeans( const char *pszWord );
		Entry &Keywords( const char *pszKeywords );

		// One disabled mechanism, and there is no overload without a reason
		// (API.md §5). The reason renders in Configure; the row draws at 0.55.
		Entry &DisabledUnless( std::function<bool()> pred, const char *pszReason );

		Entry &Validate( std::function<std::string( const std::string & )> fn );

		// ---- destructive actions (P3b) -----------------------------------
		// Arms an Action so that ONE press cannot perform it: the first press
		// swaps the verb for `pszPrompt` and reddens the chip, and only a
		// second, deliberate press invokes. Disarms on a timeout, on
		// selecting something else, or on the Inspector closing.
		//
		// WHY THIS IS A REGISTRY FEATURE RATHER THAN A MODAL THE CALLER
		// OPENS. This user has been explicit, after an agent wiped a config:
		// "There can be a button for it, but never delete configs
		// automatically." A confirmation that a call site has to remember to
		// build is a confirmation the next call site forgets -- and the
		// registry has no way to host a modal anyway, because a category
		// file cannot place a pixel (SPEC §5.2 clause 0). Declaring the
		// prompt makes the two-press flow a property of the DECLARATION, so
		// an action that destroys something is armed by construction.
		//
		// Only meaningful on Kind::Action; it is the verb chip that arms.
		Entry &Confirm( const char *pszPrompt );
		const std::string &ConfirmPrompt() const { return m_sConfirm; }
		bool  NeedsConfirm() const { return !m_sConfirm.empty(); }

		// ---- generator 3: Configure rows ---------------------------------
		// Takes a LEAF, never an id. The full id is synthesised from the
		// parent, which is what makes the Prefix Law unrepresentable to break
		// rather than merely checked (API.md §4.2).
		Parameter &Param( const char *pszLeaf, const char *pszTitle, AnyBind bind );
		Parameter &Param( const char *pszLeaf, const char *pszTitle, AnyBind bind,
		                  const Option *pOptions, size_t nOptions );

		// ---- generator 4: Details readouts -------------------------------
		// std::function<Fact()> and nothing else. There is no Bind overload,
		// so Details cannot contain a control (SPEC §5.2 clause 4).
		Entry &Live( const char *pszLeaf, std::function<Fact()> fn );

		// ---- Composite(Graph)'s sample window ----------------------------
		// Exactly Meter's shape -- std::function returning a READ, with no
		// setter anywhere -- and for the same reason: a graph is read-only
		// by construction (ReadOnly() already returns true for Graph), so
		// there must be no route through which one could be given a
		// binding. This is not a fifth generator; it is the graph's value,
		// the way Meter() takes its scalar and Facts() its summary.
		Entry &Samples( std::function<SampleWindow()> fn );
		SampleWindow SampleData() const { return m_Samples ? m_Samples() : SampleWindow{}; }

		const std::string &Id() const       { return m_sId; }
		const std::string &Title() const    { return m_sTitle; }
		const std::string &HelpText() const { return m_sHelp; }
		// See Parameter::KeywordText().
		const std::string &KeywordText() const { return m_sKeywords; }
		const std::string &Unit() const     { return m_sUnit; }
		const std::string &ZeroWord() const { return m_sZeroMeans; }
		Kind  GetKind() const               { return m_eKind; }
		CompositeKind GetCompositeKind() const { return m_eComposite; }
		const AnyBind &Binding() const      { return m_Bind; }
		const Value &DefaultValue() const   { return m_Default; }

		// ---- the second axis (composites only) ---------------------------
		// Area::Composite() is the ONLY factory that takes two bindings, and
		// Default( vA, vB ) the only setter that fills both. A composite
		// whose value is genuinely two numbers -- the anchor's row and
		// column -- would otherwise have to smuggle the pair through one
		// binding as a packed int or a string, which is exactly how the two
		// legacy Position Grid call sites drifted apart (SPEC §4.4).
		//
		// There is no third axis and no way to ask for one: a call site
		// passes bindA and bindB by position and cannot name a bindC.
		const AnyBind &BindingB() const     { return m_BindB; }
		const Value &DefaultValueB() const  { return m_DefaultB; }
		bool  HasRange() const              { return m_bHasRange; }
		float Lo() const                    { return m_flLo; }
		float Hi() const                    { return m_flHi; }
		float StepSize() const              { return m_flStep; }
		const std::vector<Option> &Options() const { return m_Options; }

		size_t ParamCount() const { return m_Params.size(); }
		const Parameter &ParamAt( size_t i ) const { return *m_Params[ i ]; }
		size_t LiveCount() const { return m_Lives.size(); }
		const std::pair<std::string, std::function<Fact()>> &LiveAt( size_t i ) const { return m_Lives[ i ]; }

		bool ReadOnly() const;
		bool UsesValue() const { return UsesValueColumn( m_eKind ); }

		// ---- read side, for the shell -----------------------------------
		// The shell renders a registration; these are how it reads one.
		// Deliberately all const and all trivial: there is no way to reach
		// the stored std::functions themselves, only to ask them for their
		// current answer, so a renderer cannot rebind anything it draws.
		const std::string &Verb() const  { return m_sVerb; }
		void Invoke() const              { if ( m_Action ) m_Action(); }
		std::string SummaryText() const  { return m_Summary ? m_Summary() : std::string(); }
		double Scalar() const            { return m_Scalar ? m_Scalar() : 0.0; }

		// The disabled predicate's reason, or "" when enabled.
		std::string DisabledReason() const;

		// ---- reset (P3b) -------------------------------------------------
		// D6 decided that "differs from default" is shown by the accent left
		// edge and that the RESET ACTION moves into the Inspector -- but no
		// phase had implemented either half, so until now the E2 shell had
		// no way to reset anything at all. The legacy Config panel's four
		// per-group reset links (issue #43) would have been silently lost by
		// migrating that panel, which is exactly the kind of quiet feature
		// loss this project has suffered before.
		//
		// Reset is per-ROW and includes the row's PARAMETERS, because that
		// is what makes it the successor to a group link rather than a
		// weaker thing: the legacy "UI Scale" group is, in E2, the `UI scale`
		// row with dock and notification scale as its parameters, so one
		// reset there restores exactly what the old link did.
		//
		// A row that never declared a Default has nothing to reset TO, and
		// says so by having no affordance rather than by resetting to zero.
		bool HasDefault() const;
		bool IsAtDefault() const;
		void ResetToDefault() const;

	private:
		friend class Area;
		friend class Registry;
		friend class Parameter;

		Parameter &AddParam( const char *pszLeaf, const char *pszTitle, AnyBind bind,
		                     const Option *pOptions, size_t nOptions );

		std::string m_sId, m_sTitle, m_sHelp, m_sUnit, m_sZeroMeans, m_sKeywords, m_sReason;
		std::string m_sConfirm;   // Confirm() -- a destructive Action's second-press prompt
		Kind          m_eKind      = Kind::Switch;
		CompositeKind m_eComposite = CompositeKind::Anchor;
		AnyBind     m_Bind, m_BindB;
		Value       m_Default, m_DefaultB;
		float       m_flLo = 0.0f, m_flHi = 0.0f, m_flStep = 0.0f;
		bool        m_bHasRange = false;
		std::vector<Option> m_Options;
		std::function<bool()> m_Enabled;
		std::function<std::string( const std::string & )> m_Validate;
		std::function<double()>      m_Scalar;    // Meter
		std::function<std::string()> m_Summary;   // Facts
		std::function<void()>        m_Action;    // Action
		std::function<SampleWindow()> m_Samples;  // Composite(Graph)
		std::string m_sVerb;

		std::vector<std::unique_ptr<Parameter>> m_Params;
		std::vector<std::pair<std::string, std::function<Fact()>>> m_Lives;

		Area     *m_pArea = nullptr;
		Registry *m_pRegistry = nullptr;
		size_t    m_nGroup = 0;
	};

	// One line of an area's content body. See Area::Content().
	struct ContentLine
	{
		int         nSeverity = 0;   // 0 info, 1 debug, 2 warn, 3 error
		std::string sScope;          // subsystem tag, drawn as a dim prefix; may be empty
		std::string sText;
	};

	// =====================================================================
	//  Area -- one rail item / one sheet
	// =====================================================================
	enum class Section : uint8_t { Display, System, Setup };   // SPEC §8.1, after D8

	class Area
	{
	public:
		Area &Keywords( const char *pszKeywords );
		Area &Summary( std::function<std::string()> fn );
		Area &AvailableWhen( std::function<bool()> fn );

		// ---- the layer badge (P3b) ---------------------------------------
		// A short tag drawn right-aligned in the sheet header. It exists for
		// issue #43's question, which a settings UI must never leave
		// ambiguous: WHERE DOES WHAT I CHANGE HERE GET WRITTEN? For the
		// config areas that is "global", "app <id>" or "global only", and
		// the answer differs per area -- Appearance always writes
		// global.json even when a per-game override is active, which is a
		// routing rule the session state alone cannot express.
		//
		// It is an AREA property, not a row one, because it describes the
		// file a whole sheet routes to. A row-level badge would repeat the
		// same word down the sheet and still not be visible from Overview.
		Area &Badge( std::function<std::string()> fn );
		std::string BadgeText() const { return m_Badge ? m_Badge() : std::string(); }

		void Group( const char *pszName );
		void GroupCount( const char *pszName );

		// ---- the complete taxonomy. There are no other factories. --------
		Entry &Switch ( const char *pszId, const char *pszTitle, AnyBind bind );
		Entry &Slider ( const char *pszId, const char *pszTitle, AnyBind bind );
		Entry &Stepper( const char *pszId, const char *pszTitle, AnyBind bind );
		Entry &Choice ( const char *pszId, const char *pszTitle, AnyBind bind,
		                const Option *pOptions, size_t nOptions );
		Entry &Text   ( const char *pszId, const char *pszTitle, AnyBind bind );
		Entry &Bank   ( const char *pszId, const char *pszTitle, AnyBind bind,
		                const Option *pOptions, size_t nOptions );
		Entry &Action ( const char *pszId, const char *pszTitle, const char *pszVerb,
		                std::function<void()> fn );
		Entry &Meter  ( const char *pszId, const char *pszTitle,
		                std::function<double()> fn, double flLo, double flHi );
		Entry &Facts  ( const char *pszId, const char *pszTitle,
		                std::function<std::string()> fnSummary );
		Entry &Composite( const char *pszId, const char *pszTitle, CompositeKind eKind,
		                  AnyBind bindA, AnyBind bindB = {} );

		// ================================================================
		//  CONTENT AREAS -- P3c
		// ================================================================
		// An area whose body is CONTENT rather than a list of settings.
		// Log is the only one: a settings sheet made of log lines is not a
		// settings sheet, and pretending each line is a row would put
		// thousands of them through the Six Budget and the Prefix Law for
		// no reason.
		//
		// Content() HANDS THE SHELL DATA AND NOTHING ELSE: a function
		// returning lines. The call site cannot place a pixel (SPEC §5.2
		// clause 0), cannot choose a font, a colour or a width, and cannot
		// lay anything out. The shell draws the view, exactly as it draws
		// every control.
		//
		// That distinction is why this outlived the migration seam it was
		// written to be contrasted with. P5 deleted Area::Escape(), which
		// handed a call site the sheet's own child window and let it run
		// arbitrary ImGui with every law suspended; it was temporary from
		// the day it was written and reached zero call sites in P3c. The
		// difference was never the amount of code -- it is that an escape
		// hatch takes the DRAWING away from the shell, and this does not.
		// Keep it that way: a Content() overload that took a draw callback
		// would be Escape() again under a third name.
		//
		// A content area STILL DECLARES ROWS, and they are still ordinary
		// rows: Log's filter bank, its text filter and its buffer facts all
		// go through the same grammar, the same Inspector and the same
		// help. They are drawn above the content. So an area is never
		// "rows or content" -- it is rows AND, optionally, a content body
		// beneath them.
		Area &Content( std::function<std::vector<ContentLine>()> fn );
		bool  HasContent() const { return (bool)m_Content; }
		std::vector<ContentLine> ContentLines() const
		{
			return m_Content ? m_Content() : std::vector<ContentLine>{};
		}

		// Whether the content view sticks to its newest line. Read by the
		// shell; declared by the area so the behaviour is a property of the
		// registration rather than shell state a category cannot see.
		Area &FollowsTail( std::function<bool()> fn );
		bool  FollowTail() const { return !m_FollowTail || m_FollowTail(); }

		// ================================================================
		//  DYNAMIC AREAS -- P3b
		// ================================================================
		// An area whose ROW SET is discovered at runtime instead of being
		// declared at startup. Audio is the first and, so far, the only
		// one: PipeWire streams appear and disappear while the overlay is
		// open, so "one row per active stream" cannot be written down in
		// RegisterAll().
		//
		// WHY THIS IS A REGISTRY FEATURE AND NOT A TRICK IN THE AUDIO
		// FILE. The obvious workaround is a fixed pool of N slots, each
		// bound to "whichever stream is currently at index i" and greyed
		// when there are fewer than i streams. That fits the existing laws
		// with no changes at all -- and it is wrong, for a reason worth
		// recording: slot identity would be POSITIONAL. A stream ending
		// shifts every stream after it down one slot, so the slider under
		// the pointer silently changes which application it controls. That
		// is not a cosmetic problem; it is the volume of the wrong program
		// moving. Identity has to come from the stream, so the row's id
		// has to come from the stream, so the row set has to be rebuilt.
		//
		// HOW THE FOUR LAWS SURVIVE IT:
		//
		//   * ID UNIQUENESS holds because a rebuild RELEASES the ids the
		//     previous build claimed before the builder runs. Ids are also
		//     derived from the PipeWire node id, which the server does not
		//     reuse while a node lives, so two live streams cannot collide.
		//   * THE PREFIX LAW is untouched -- a rebuilt Entry mints its
		//     Params through the same synthesis every other Entry uses.
		//   * THE SIX BUDGET is untouched, and is checked per Entry as it
		//     is built, exactly as at startup.
		//   * HELP IS REQUIRED is the one that genuinely changes, and it
		//     is stated plainly rather than hidden: Registry::SelfTest()
		//     runs once after RegisterAll(), so a row built later has
		//     never been through it. SyncIfStale() therefore re-runs the
		//     help and prefix checks over THIS AREA after every rebuild.
		//
		// THE CONSEQUENCE, STATED. A law violation aborts (D11.6), and a
		// rebuild happens mid-session, so a malformed dynamic row is a
		// mid-session abort rather than a boot failure. That is a real
		// widening of when the guillotine can fall. It is acceptable only
		// because a dynamic row is GENERATED -- no human types one -- so
		// the failure is in one code path that a unit test can run against
		// a fabricated stream list without a compositor. tests/
		// test_overlay_ui.cpp does exactly that.
		//
		// `fnGeneration` is polled every frame and must be cheap; the
		// builder runs only when its answer changes. Make it a function of
		// everything the ROWS depend on (the set of streams AND their
		// names), never of the values those rows read -- a generation that
		// changed with the volume would rebuild the area mid-drag.
		Area &Rebuilds( std::function<uint64_t()> fnGeneration,
		                std::function<void( Area & )> fnBuild );
		bool  IsDynamic() const { return (bool)m_Build; }

		// Rebuilds if the generation moved. Returns true if it rebuilt.
		// The shell calls this once per frame, BEFORE it reads anything
		// out of the area -- a rebuild frees every Entry the area held, so
		// no Entry pointer may be held across it. Selection is by id
		// string and therefore survives.
		bool SyncIfStale();

		const std::string &Id() const    { return m_sId; }
		const std::string &Title() const { return m_sTitle; }
		Section GetSection() const       { return m_eSection; }
		size_t  EntryCount() const       { return m_Entries.size(); }
		const Entry &EntryAt( size_t i ) const { return *m_Entries[ i ]; }

		// Whether this area is currently offerable at all (SPEC's rail hides
		// what a machine cannot do). No predicate means always available.
		bool Available() const { return !m_Available || m_Available(); }

		// The area's own one-line summary, for the Overview card. Empty when
		// none was registered.
		std::string SummaryText() const { return m_Summary ? m_Summary() : std::string(); }

		struct GroupBand { std::string sName; bool bCounted = false; size_t nFirstEntry = 0; };
		const std::vector<GroupBand> &Groups() const { return m_Groups; }

		// Which band entry `i` sits under. Recorded on the Entry when it was
		// emitted -- i.e. the band that was open at the moment of declaration
		// -- rather than re-derived from nFirstEntry ranges at draw time. The
		// two would agree today; only one of them stays right if a band is
		// ever declared with no entries under it.
		size_t GroupOf( size_t i ) const { return m_Entries[ i ]->m_nGroup; }

	private:
		friend class Registry;

		Entry &Emit( const char *pszId, const char *pszTitle, Kind eKind, AnyBind bind );

		std::string m_sId, m_sTitle, m_sKeywords;
		Section     m_eSection = Section::Display;
		std::function<std::string()> m_Summary;
		std::function<std::string()> m_Badge;
		std::function<bool()>        m_Available;
		std::function<std::vector<ContentLine>()> m_Content;  // content areas -- see Content()
		std::function<bool()>        m_FollowTail;
		std::function<uint64_t()>    m_Generation;  // dynamic areas -- see Rebuilds()
		std::function<void( Area & )> m_Build;
		uint64_t                     m_ulGeneration = 0;
		bool                         m_bBuilt = false;
		std::vector<std::unique_ptr<Entry>> m_Entries;
		std::vector<GroupBand>       m_Groups;
		Registry   *m_pRegistry = nullptr;
	};

	// =====================================================================
	//  Registry
	// =====================================================================
	class Registry
	{
	public:
		Area &Add( const char *pszId, const char *pszTitle, Section eSection );

		size_t AreaCount() const { return m_Areas.size(); }
		const Area &AreaAt( size_t i ) const { return *m_Areas[ i ]; }
		const Area *FindArea( const std::string &sId ) const;

		// Every registered id, Params included. Lookup for the palette, and
		// the uniqueness law's own bookkeeping.
		const Entry *FindEntry( const std::string &sId ) const;
		const Parameter *FindParam( const std::string &sId ) const;

		// Runs after RegisterAll(). Checks the two laws that cannot be decided
		// at the moment of the call that breaks them:
		//   * HELP IS REQUIRED -- an Entry or Param may legally be built one
		//     fluent call at a time, so "no Help() yet" is only a violation
		//     once registration is over.
		//   * THE PREFIX LAW, re-checked. Param ids are synthesised, so this
		//     can only fail if someone reaches past the synthesis; checking it
		//     anyway costs nothing and makes the law visible in a test.
		// Returns the number of violations reported.
		size_t SelfTest();

		// The same two checks SelfTest() makes, over ONE area. A dynamic
		// area's rows are built after SelfTest() has already run, so this
		// is how they are still held to the laws -- see Area::Rebuilds().
		size_t SelfTestArea( const Area &area );

		// Rebuilds every dynamic area whose generation moved. The shell
		// calls this once per frame, before it reads anything out of an
		// area. Returns how many rebuilt. See Area::Rebuilds().
		size_t SyncDynamicAreas();

	private:
		friend class Area;
		friend class Entry;

		// Returns false (and reports Law::UniqueId) if the id is taken.
		bool ClaimId( const std::string &sId );

		// Gives an id back, so a dynamic area's rebuild does not collide
		// with the build it replaces. Only Area::SyncIfStale() calls this,
		// and only for ids that area itself claimed.
		void ReleaseId( const std::string &sId );

		std::vector<std::unique_ptr<Area>> m_Areas;
		std::vector<std::string>           m_ClaimedIds;
	};

	// =====================================================================
	//  Adjustable -- "step this declaration's value by one"
	// =====================================================================
	// SPEC §8.2 gives the same meaning to the arrow keys in three places: on
	// a focused Sheet row, on an Inspector row, and on the highlighted
	// palette result ("adjust the highlighted entry's value IN PLACE,
	// without leaving the list").
	//
	// WHY THIS IS ONE FUNCTION AND NOT THREE. Those three hosts having their
	// own stepping code is precisely how the two legacy Position Grid call
	// sites drifted apart (D15.1's argument, applied to behaviour rather than
	// geometry). A slider that steps by 1 in the Sheet and by its full range
	// in the palette is the same class of defect as a control that renders
	// and does nothing (#25, #68), and it would not be visible in either host
	// alone.
	//
	// It is a VIEW over an Entry or a Parameter -- both expose the same six
	// accessors this needs -- so neither type has to grow a method and the
	// function stays free of the registry's ownership rules.
	struct Adjustable
	{
		Kind                        eKind    = Kind::Switch;
		const AnyBind              *pBind    = nullptr;
		bool                        bHasRange = false;
		float                       flLo = 0.0f, flHi = 0.0f, flStep = 0.0f;
		const std::vector<Option>  *pOptions = nullptr;

		// Which composite, when eKind is Composite. Carried because not
		// every composite has an ordering an arrow key can follow: an
		// Anchor's axes and a Hue's degrees do, a packed 0xRRGGBB colour
		// does not. Anchor for a Parameter, which is never a composite.
		CompositeKind               eComposite = CompositeKind::Anchor;

		static Adjustable Of( const Entry &e );
		static Adjustable Of( const Parameter &p );
	};

	// Steps the bound value by `nDir` (-1 / +1). `bFine` is SPEC §3.4's
	// Shift modifier: x0.1 of the normal step on a continuous range.
	//
	// Returns false and writes nothing when the declaration cannot be
	// stepped -- a read-only kind, an unbound binding, or a kind whose value
	// is not ordered (Text, Bank, Action, Facts, Meter). A caller therefore
	// never has to know the taxonomy to decide whether an arrow key applies.
	bool AdjustValue( const Adjustable &adj, int nDir, bool bFine );

	// D25: whether an arrow key means ANYTHING on this declaration -- i.e.
	// whether the launcher can offer it in place, or has to send the user to
	// the full overlay for it.
	//
	// WHY THIS IS NOT "CALL AdjustValue AND SEE". AdjustValue() answers a
	// different question: "did this particular press change the value". It
	// returns false for a Choice already on its last option and for a slider
	// already at its ceiling, and both of those rows ARE adjustable -- the
	// other arrow moves them. A launcher that decided adjustability by trying
	// the step would mislabel every row sitting at an end stop, which is the
	// half of the registry a user is most likely to be looking at (a maxed
	// sharpness, a switch already on).
	//
	// So this is the TAXONOMY question only: kind, read-only-ness, and
	// whether the binding exists. It deliberately reads nothing out of the
	// binding, so it is stable frame to frame and does not flicker a row's
	// hint as its value moves. test_overlay_palette.cpp pins it against
	// AdjustValue() in both directions so the two cannot drift.
	bool CanAdjust( const Adjustable &adj );
}

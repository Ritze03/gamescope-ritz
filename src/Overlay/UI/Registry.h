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
		Kind  GetKind() const            { return m_eKind; }
		const AnyBind &Binding() const   { return m_Bind; }
		const Value &DefaultValue() const { return m_Default; }
		const Entry *Owner() const       { return m_pOwner; }
		const std::vector<Option> &Options() const { return m_Options; }

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

		const std::string &Id() const       { return m_sId; }
		const std::string &Title() const    { return m_sTitle; }
		const std::string &HelpText() const { return m_sHelp; }
		const std::string &Unit() const     { return m_sUnit; }
		Kind  GetKind() const               { return m_eKind; }
		CompositeKind GetCompositeKind() const { return m_eComposite; }
		const AnyBind &Binding() const      { return m_Bind; }
		const Value &DefaultValue() const   { return m_Default; }
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

		// The disabled predicate's reason, or "" when enabled.
		std::string DisabledReason() const;

	private:
		friend class Area;
		friend class Registry;
		friend class Parameter;

		Parameter &AddParam( const char *pszLeaf, const char *pszTitle, AnyBind bind,
		                     const Option *pOptions, size_t nOptions );

		std::string m_sId, m_sTitle, m_sHelp, m_sUnit, m_sZeroMeans, m_sKeywords, m_sReason;
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
		std::string m_sVerb;

		std::vector<std::unique_ptr<Parameter>> m_Params;
		std::vector<std::pair<std::string, std::function<Fact()>>> m_Lives;

		Area     *m_pArea = nullptr;
		Registry *m_pRegistry = nullptr;
		size_t    m_nGroup = 0;
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

		const std::string &Id() const    { return m_sId; }
		const std::string &Title() const { return m_sTitle; }
		Section GetSection() const       { return m_eSection; }
		size_t  EntryCount() const       { return m_Entries.size(); }
		const Entry &EntryAt( size_t i ) const { return *m_Entries[ i ]; }

		struct GroupBand { std::string sName; bool bCounted = false; size_t nFirstEntry = 0; };
		const std::vector<GroupBand> &Groups() const { return m_Groups; }

	private:
		friend class Registry;

		Entry &Emit( const char *pszId, const char *pszTitle, Kind eKind, AnyBind bind );

		std::string m_sId, m_sTitle, m_sKeywords;
		Section     m_eSection = Section::Display;
		std::function<std::string()> m_Summary;
		std::function<bool()>        m_Available;
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

	private:
		friend class Area;
		friend class Entry;

		// Returns false (and reports Law::UniqueId) if the id is taken.
		bool ClaimId( const std::string &sId );

		std::vector<std::unique_ptr<Area>> m_Areas;
		std::vector<std::string>           m_ClaimedIds;
	};
}

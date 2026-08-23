#include "Registry.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gamescope::ui
{
	// =====================================================================
	//  Law reporting
	// =====================================================================
	namespace
	{
		LawRecorder *s_pRecorder = nullptr;

		// Handed back to a caller whose chain broke a law, so the rest of the
		// fluent chain writes into a discard target instead of dereferencing
		// nothing. Only ever reached when a violation has already been
		// reported -- in a shipping build ReportViolation() has aborted by
		// then and these are unreachable.
		Entry     &SinkEntry();
		Parameter &SinkParam();

		// The Six Budget, SPEC §5.2 clause 3.
		constexpr size_t kParamBudget = 6;
	}

	const char *LawName( Law eLaw )
	{
		switch ( eLaw )
		{
			case Law::Prefix:         return "Prefix Law";
			case Law::OneLevel:       return "One-Level Rule";
			case Law::SixBudget:      return "Six Budget";
			case Law::UniqueId:       return "id uniqueness";
			case Law::HelpRequired:   return "Help() is required";
			case Law::ReasonRequired: return "DisabledUnless() requires a reason";
			case Law::Escaped:        return "an escaped area cannot also be populated";
		}
		return "unknown law";
	}

	const char *KindName( Kind eKind )
	{
		switch ( eKind )
		{
			case Kind::Switch:    return "switch";
			case Kind::Slider:    return "slider";
			case Kind::Stepper:   return "stepper";
			case Kind::Choice:    return "choice";
			case Kind::Text:      return "text";
			case Kind::Bank:      return "bank";
			case Kind::Action:    return "action";
			case Kind::Meter:     return "meter";
			case Kind::Facts:     return "facts";
			case Kind::Composite: return "composite";
		}
		return "unknown";
	}

	LawRecorder::LawRecorder()
	{
		m_pPrev = s_pRecorder;
		s_pRecorder = this;
	}

	LawRecorder::~LawRecorder()
	{
		s_pRecorder = m_pPrev;
	}

	bool LawRecorder::Caught( Law eLaw ) const
	{
		return std::any_of( m_Violations.begin(), m_Violations.end(),
			[ eLaw ]( const Violation &v ) { return v.eLaw == eLaw; } );
	}

	void ReportViolation( Law eLaw, const std::string &sId, const std::string &sMessage )
	{
		if ( s_pRecorder )
		{
			s_pRecorder->m_Violations.push_back( { eLaw, sId, sMessage } );
			return;
		}

		// No recorder: this is a real build, RegisterAll() is running at
		// startup, and the registry is malformed. Abort loudly rather than
		// ship a UI whose laws do not hold -- a law that only warns is a
		// convention, which is the exact thing SPEC.md §5.2 exists to replace.
		fprintf( stderr, "ui::Registry: %s violated by '%s': %s\n",
			LawName( eLaw ), sId.c_str(), sMessage.c_str() );
		abort();
	}

	// =====================================================================
	//  Values / bindings
	// =====================================================================
	std::string ValueToString( const Value &v )
	{
		if ( const bool *p = std::get_if<bool>( &v ) )
			return *p ? "on" : "off";
		if ( const int *p = std::get_if<int>( &v ) )
			return std::to_string( *p );
		if ( const float *p = std::get_if<float>( &v ) )
		{
			char sz[ 32 ];
			snprintf( sz, sizeof( sz ), "%.4g", (double)*p );
			return sz;
		}
		if ( const std::string *p = std::get_if<std::string>( &v ) )
			return *p;
		return {};
	}

	Value AnyBind::Get() const { return m_Get ? m_Get() : Value{}; }
	void  AnyBind::Set( const Value &v ) const { if ( m_Set ) m_Set( v ); }

	// =====================================================================
	//  Parameter
	// =====================================================================
	Parameter &Parameter::Default( Value v )        { m_Default = std::move( v ); return *this; }
	Parameter &Parameter::Step( float s )           { m_flStep = s; return *this; }

	// A param never names its own control (AddParam()'s comment): options
	// present means Choice, a range means Slider, neither means Switch. So
	// Range() is the declaration that resolves the kind -- the caller states a
	// FACT about the value ("it is bounded"), and the kind follows from it.
	// Guarded against clobbering a Choice, because "a bounded set of options"
	// is still a Choice.
	Parameter &Parameter::Range( float lo, float hi )
	{
		m_flLo = lo;
		m_flHi = hi;
		m_bHasRange = true;
		if ( m_eKind != Kind::Choice )
			m_eKind = Kind::Slider;
		return *this;
	}
	Parameter &Parameter::Unit( const char *psz )   { m_sUnit = psz ? psz : ""; return *this; }
	Parameter &Parameter::ZeroMeans( const char *psz ) { m_sZeroMeans = psz ? psz : ""; return *this; }
	Parameter &Parameter::Keywords( const char *psz )  { m_sKeywords = psz ? psz : ""; return *this; }

	Parameter &Parameter::Help( const char *pszHelp )
	{
		// LAW 4, at the point of the call. The other half -- "never called at
		// all" -- can only be decided once registration is over, and lives in
		// Registry::SelfTest().
		if ( !pszHelp || !*pszHelp )
		{
			ReportViolation( Law::HelpRequired, m_sId,
				"Help() was called with empty text. Every entry and every "
				"parameter needs one short description of what it does "
				"(SPEC.md 5.2 clause 1)." );
			return *this;
		}
		m_sHelp = pszHelp;
		return *this;
	}

	Parameter &Parameter::DisabledUnless( std::function<bool()> pred, const char *pszReason )
	{
		if ( !pszReason || !*pszReason )
		{
			ReportViolation( Law::ReasonRequired, m_sId,
				"DisabledUnless() needs a reason string -- a control that greys "
				"out without saying why is the most common inconsistency in the "
				"old code (API.md 3.2)." );
			return *this;
		}
		m_Enabled = std::move( pred );
		m_sReason = pszReason;
		return *this;
	}

	// Deliberately does NOT walk to Owner(): see the header. A param is
	// disabled only by its own predicate; the renderer greys a param under a
	// disabled parent by drawing the whole block dim, which is the inherit
	// half, and a param that IS the cause stays live because nothing here
	// asks the parent.
	std::string Parameter::DisabledReason() const
	{
		if ( !m_Enabled || m_Enabled() )
			return {};
		return m_sReason;
	}

	Parameter &Parameter::Param( const char *pszLeaf, const char *pszTitle, AnyBind bind )
	{
		return Param( pszLeaf, pszTitle, std::move( bind ), nullptr, 0 );
	}

	Parameter &Parameter::Param( const char *pszLeaf, const char *pszTitle, AnyBind bind,
	                             const Option *pOptions, size_t nOptions )
	{
		// Sibling, not child: the new Parameter is added to the SAME Entry.
		if ( !m_pOwner )
			return SinkParam();
		return m_pOwner->AddParam( pszLeaf, pszTitle, std::move( bind ), pOptions, nOptions );
	}

	Entry &Parameter::Live( const char *pszLeaf, std::function<Fact()> fn )
	{
		if ( !m_pOwner )
			return SinkEntry();
		return m_pOwner->Live( pszLeaf, std::move( fn ) );
	}

	// =====================================================================
	//  Entry
	// =====================================================================
	Entry &Entry::Default( Value v )                 { m_Default = std::move( v ); return *this; }
	Entry &Entry::Default( Value a, Value b )        { m_Default = std::move( a ); m_DefaultB = std::move( b ); return *this; }
	Entry &Entry::Range( float lo, float hi )        { m_flLo = lo; m_flHi = hi; m_bHasRange = true; return *this; }
	Entry &Entry::Step( float s )                    { m_flStep = s; return *this; }
	Entry &Entry::Unit( const char *psz )            { m_sUnit = psz ? psz : ""; return *this; }
	Entry &Entry::ZeroMeans( const char *psz )       { m_sZeroMeans = psz ? psz : ""; return *this; }
	Entry &Entry::Keywords( const char *psz )        { m_sKeywords = psz ? psz : ""; return *this; }
	Entry &Entry::Validate( std::function<std::string( const std::string & )> fn ) { m_Validate = std::move( fn ); return *this; }

	Entry &Entry::Help( const char *pszHelp )
	{
		if ( !pszHelp || !*pszHelp )
		{
			ReportViolation( Law::HelpRequired, m_sId,
				"Help() was called with empty text. Every entry and every "
				"parameter needs one short description of what it does "
				"(SPEC.md 5.2 clause 1)." );
			return *this;
		}
		m_sHelp = pszHelp;
		return *this;
	}

	Entry &Entry::DisabledUnless( std::function<bool()> pred, const char *pszReason )
	{
		if ( !pszReason || !*pszReason )
		{
			ReportViolation( Law::ReasonRequired, m_sId,
				"DisabledUnless() needs a reason string -- a control that greys "
				"out without saying why is the most common inconsistency in the "
				"old code (API.md 3.2)." );
			return *this;
		}
		m_Enabled = std::move( pred );
		m_sReason = pszReason;
		return *this;
	}

	bool Entry::ReadOnly() const
	{
		// SPEC §5.1 also lists the frametime Graph composite as read-only.
		if ( m_eKind == Kind::Composite )
			return m_eComposite == CompositeKind::Graph;
		return IsReadOnly( m_eKind );
	}

	std::string Entry::DisabledReason() const
	{
		if ( !m_Enabled || m_Enabled() )
			return {};
		return m_sReason;
	}

	Parameter &Entry::Param( const char *pszLeaf, const char *pszTitle, AnyBind bind )
	{
		return AddParam( pszLeaf, pszTitle, std::move( bind ), nullptr, 0 );
	}

	Parameter &Entry::Param( const char *pszLeaf, const char *pszTitle, AnyBind bind,
	                         const Option *pOptions, size_t nOptions )
	{
		return AddParam( pszLeaf, pszTitle, std::move( bind ), pOptions, nOptions );
	}

	Parameter &Entry::AddParam( const char *pszLeaf, const char *pszTitle, AnyBind bind,
	                            const Option *pOptions, size_t nOptions )
	{
		// ---- LAW 2, the One-Level Rule's runtime half ---------------------
		// The type system already makes a Param-owning-a-Param unsayable; a
		// dot in the leaf is the other way someone could try to express one,
		// by smuggling the nesting into the id.
		if ( !pszLeaf || !*pszLeaf )
		{
			ReportViolation( Law::Prefix, m_sId,
				"Param() needs a leaf name; the id is synthesised as "
				"'<parent>.<leaf>' (SPEC.md 5.2 clause 2)." );
			return SinkParam();
		}
		if ( strchr( pszLeaf, '.' ) )
		{
			ReportViolation( Law::OneLevel, m_sId,
				std::string( "Param leaf '" ) + pszLeaf + "' contains a dot. A Param is one "
				"level deep; it cannot own Params (SPEC.md 5.2 clause 3)." );
			return SinkParam();
		}

		// ---- LAW 3, the Six Budget ----------------------------------------
		if ( m_Params.size() >= kParamBudget )
		{
			ReportViolation( Law::SixBudget, m_sId,
				"'" + m_sId + "' has 7 parameters. A row may own at most 6. Promote '"
				+ m_sId + "' to a category (SPEC.md 5.2 clause 3)." );
			return SinkParam();
		}

		// ---- LAW 1, the Prefix Law ----------------------------------------
		// Not checked -- CONSTRUCTED. The caller supplies a leaf and never an
		// id, so a Param's config key is a child of its parent's by the only
		// route that exists.
		const std::string sId = m_sId + "." + pszLeaf;

		// ---- LAW 4, id uniqueness -----------------------------------------
		if ( m_pRegistry && !m_pRegistry->ClaimId( sId ) )
			return SinkParam();

		auto up = std::make_unique<Parameter>();
		Parameter &p = *up;
		p.m_sId    = sId;
		p.m_sTitle = pszTitle ? pszTitle : "";
		p.m_Bind   = std::move( bind );
		p.m_pOwner = this;
		// A param's kind is inferred the same way an entry's Choice is: options
		// present means a choice, absent means a binary. Sliders and steppers
		// declare themselves by calling Range()/Step() and are resolved by
		// Kind() below -- a param never names its own control, which is what
		// keeps it identical to its parent in every host (SPEC 5.3).
		if ( pOptions && nOptions )
		{
			p.m_eKind = Kind::Choice;
			p.m_Options.assign( pOptions, pOptions + nOptions );
		}
		m_Params.push_back( std::move( up ) );
		return p;
	}

	Entry &Entry::Live( const char *pszLeaf, std::function<Fact()> fn )
	{
		m_Lives.emplace_back( pszLeaf ? pszLeaf : "", std::move( fn ) );
		return *this;
	}

	// =====================================================================
	//  Area
	// =====================================================================
	Area &Area::Keywords( const char *psz )                 { m_sKeywords = psz ? psz : ""; return *this; }
	Area &Area::Summary( std::function<std::string()> fn )  { m_Summary = std::move( fn ); return *this; }
	Area &Area::AvailableWhen( std::function<bool()> fn )    { m_Available = std::move( fn ); return *this; }

	// MIGRATION SEAM -- see Registry.h's Escape() comment. P3 deletes this.
	//
	// The one rule that keeps it from becoming permanent: an area is legacy
	// or it is E2, never both. A half-migrated area -- three real rows plus
	// an escaped tail -- is the shape that would survive P3 indefinitely,
	// because "it mostly works" is the strongest argument against finishing
	// anything. So the mixture is a registration violation in BOTH
	// directions: escaping a populated area, and populating an escaped one
	// (Emit() re-checks below).
	Area &Area::Escape( std::function<void()> fn )
	{
		if ( !fn )
		{
			ReportViolation( Law::Escaped, m_sId, "Escape() was given an empty function." );
			return *this;
		}
		if ( !m_Entries.empty() )
		{
			ReportViolation( Law::Escaped, m_sId,
				"an area with registered entries cannot also be escaped -- migrate it fully or not at all." );
			return *this;
		}
		m_Escape = std::move( fn );
		return *this;
	}

	void Area::Group( const char *pszName )
	{
		m_Groups.push_back( { pszName ? pszName : "", false, m_Entries.size() } );
	}

	void Area::GroupCount( const char *pszName )
	{
		m_Groups.push_back( { pszName ? pszName : "", true, m_Entries.size() } );
	}

	Entry &Area::Emit( const char *pszId, const char *pszTitle, Kind eKind, AnyBind bind )
	{
		const std::string sId = pszId ? pszId : "";

		if ( sId.empty() )
		{
			ReportViolation( Law::UniqueId, m_sId, "an entry was registered with an empty id." );
			return SinkEntry();
		}
		// The other half of the escape hatch's exclusivity -- see Escape().
		if ( m_Escape )
		{
			ReportViolation( Law::Escaped, m_sId,
				"an escaped area cannot register entries -- migrate it fully or not at all." );
			return SinkEntry();
		}
		if ( m_pRegistry && !m_pRegistry->ClaimId( sId ) )
			return SinkEntry();

		auto up = std::make_unique<Entry>();
		Entry &e = *up;
		e.m_sId        = sId;
		e.m_sTitle     = pszTitle ? pszTitle : "";
		e.m_eKind      = eKind;
		e.m_Bind       = std::move( bind );
		e.m_pArea      = this;
		e.m_pRegistry  = m_pRegistry;
		e.m_nGroup     = m_Groups.empty() ? 0 : m_Groups.size() - 1;
		m_Entries.push_back( std::move( up ) );
		return e;
	}

	Entry &Area::Switch ( const char *pszId, const char *pszTitle, AnyBind b ) { return Emit( pszId, pszTitle, Kind::Switch,  std::move( b ) ); }
	Entry &Area::Slider ( const char *pszId, const char *pszTitle, AnyBind b ) { return Emit( pszId, pszTitle, Kind::Slider,  std::move( b ) ); }
	Entry &Area::Stepper( const char *pszId, const char *pszTitle, AnyBind b ) { return Emit( pszId, pszTitle, Kind::Stepper, std::move( b ) ); }
	Entry &Area::Text   ( const char *pszId, const char *pszTitle, AnyBind b ) { return Emit( pszId, pszTitle, Kind::Text,    std::move( b ) ); }

	Entry &Area::Choice( const char *pszId, const char *pszTitle, AnyBind b,
	                     const Option *pOptions, size_t nOptions )
	{
		Entry &e = Emit( pszId, pszTitle, Kind::Choice, std::move( b ) );
		e.m_Options.assign( pOptions, pOptions + nOptions );
		return e;
	}

	Entry &Area::Bank( const char *pszId, const char *pszTitle, AnyBind b,
	                   const Option *pOptions, size_t nOptions )
	{
		Entry &e = Emit( pszId, pszTitle, Kind::Bank, std::move( b ) );
		e.m_Options.assign( pOptions, pOptions + nOptions );
		return e;
	}

	Entry &Area::Action( const char *pszId, const char *pszTitle, const char *pszVerb,
	                     std::function<void()> fn )
	{
		Entry &e = Emit( pszId, pszTitle, Kind::Action, {} );
		e.m_sVerb  = pszVerb ? pszVerb : "";
		e.m_Action = std::move( fn );
		return e;
	}

	Entry &Area::Meter( const char *pszId, const char *pszTitle,
	                    std::function<double()> fn, double flLo, double flHi )
	{
		Entry &e = Emit( pszId, pszTitle, Kind::Meter, {} );
		e.m_Scalar    = std::move( fn );
		e.m_flLo      = (float)flLo;
		e.m_flHi      = (float)flHi;
		e.m_bHasRange = true;
		return e;
	}

	Entry &Area::Facts( const char *pszId, const char *pszTitle, std::function<std::string()> fnSummary )
	{
		Entry &e = Emit( pszId, pszTitle, Kind::Facts, {} );
		e.m_Summary = std::move( fnSummary );
		return e;
	}

	Entry &Area::Composite( const char *pszId, const char *pszTitle, CompositeKind eKind,
	                        AnyBind bindA, AnyBind bindB )
	{
		Entry &e = Emit( pszId, pszTitle, Kind::Composite, std::move( bindA ) );
		e.m_eComposite = eKind;
		e.m_BindB      = std::move( bindB );
		return e;
	}

	// =====================================================================
	//  Registry
	// =====================================================================
	Area &Registry::Add( const char *pszId, const char *pszTitle, Section eSection )
	{
		const std::string sId = pszId ? pszId : "";
		ClaimId( sId );

		auto up = std::make_unique<Area>();
		Area &a = *up;
		a.m_sId       = sId;
		a.m_sTitle    = pszTitle ? pszTitle : "";
		a.m_eSection  = eSection;
		a.m_pRegistry = this;
		m_Areas.push_back( std::move( up ) );
		return a;
	}

	bool Registry::ClaimId( const std::string &sId )
	{
		if ( std::find( m_ClaimedIds.begin(), m_ClaimedIds.end(), sId ) != m_ClaimedIds.end() )
		{
			ReportViolation( Law::UniqueId, sId,
				"'" + sId + "' is already registered. Every area, entry and param id is "
				"unique registry-wide -- the palette, the config key and ui_snapshot all "
				"address a setting by it." );
			return false;
		}
		m_ClaimedIds.push_back( sId );
		return true;
	}

	const Area *Registry::FindArea( const std::string &sId ) const
	{
		for ( const auto &pArea : m_Areas )
			if ( pArea->Id() == sId )
				return pArea.get();
		return nullptr;
	}

	size_t Registry::EscapeCount() const
	{
		size_t n = 0;
		for ( const auto &pArea : m_Areas )
			n += pArea->IsEscaped() ? 1u : 0u;
		return n;
	}

	const Entry *Registry::FindEntry( const std::string &sId ) const
	{
		for ( const auto &pArea : m_Areas )
			for ( size_t i = 0; i < pArea->EntryCount(); ++i )
				if ( pArea->EntryAt( i ).Id() == sId )
					return &pArea->EntryAt( i );
		return nullptr;
	}

	const Parameter *Registry::FindParam( const std::string &sId ) const
	{
		for ( const auto &pArea : m_Areas )
			for ( size_t i = 0; i < pArea->EntryCount(); ++i )
			{
				const Entry &e = pArea->EntryAt( i );
				for ( size_t j = 0; j < e.ParamCount(); ++j )
					if ( e.ParamAt( j ).Id() == sId )
						return &e.ParamAt( j );
			}
		return nullptr;
	}

	size_t Registry::SelfTest()
	{
		size_t nFound = 0;

		auto RequireHelp = [ & ]( const std::string &sId, const std::string &sHelp, const char *pszWhat )
		{
			if ( !sHelp.empty() )
				return;
			++nFound;
			ReportViolation( Law::HelpRequired, sId,
				std::string( pszWhat ) + " '" + sId + "' never called Help(). Every "
				"registration must say, in at most three sentences, what the setting "
				"does -- it is the Inspector's Configure description and there is no "
				"other way to author one (SPEC.md 5.2 clause 1)." );
		};

		for ( const auto &pArea : m_Areas )
		{
			for ( size_t i = 0; i < pArea->EntryCount(); ++i )
			{
				const Entry &e = pArea->EntryAt( i );
				RequireHelp( e.Id(), e.HelpText(), "entry" );

				for ( size_t j = 0; j < e.ParamCount(); ++j )
				{
					const Parameter &p = e.ParamAt( j );
					RequireHelp( p.Id(), p.HelpText(), "param" );

					// The Prefix Law, re-checked. Synthesis already guarantees
					// it; verifying it anyway is what turns "unrepresentable"
					// into something a test can actually observe.
					const std::string sWant = e.Id() + ".";
					if ( p.Id().compare( 0, sWant.size(), sWant ) != 0 )
					{
						++nFound;
						ReportViolation( Law::Prefix, p.Id(),
							"'" + p.Id() + "' is not a child of '" + e.Id() + "'. A Param's id "
							"must be '<parent>.<leaf>' (SPEC.md 5.2 clause 2)." );
					}
				}
			}
		}

		return nFound;
	}

	// =====================================================================
	//  Sinks
	// =====================================================================
	namespace
	{
		Entry &SinkEntry()
		{
			static Entry s_Sink;
			return s_Sink;
		}

		Parameter &SinkParam()
		{
			static Parameter s_Sink;
			return s_Sink;
		}
	}
}

#include "Registry.h"

#include <algorithm>
#include <cmath>
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
			case Law::Dynamic:        return "a dynamic area is malformed";
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

	// ---- the pointer-drag flag (see Registry.h) --------------------------
	namespace
	{
		bool s_bPointerDragActive = false;
	}
	void SetPointerDragActive( bool bActive ) { s_bPointerDragActive = bActive; }
	bool IsPointerDragActive()                { return s_bPointerDragActive; }

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

	// ---- reset (P3b) ----------------------------------------------------
	namespace
	{
		// Two Values are "the same setting value" if they agree. Floats get a
		// tolerance because a default declared as 0.9f and a value that has
		// been through a JSON round-trip are the same setting to a user, and
		// a reset chip that never switches off would be worse than none.
		bool ValueEquals( const Value &a, const Value &b )
		{
			if ( const float *pA = std::get_if<float>( &a ) )
			{
				if ( const float *pB = std::get_if<float>( &b ) )
					return std::fabs( *pA - *pB ) < 1e-4f;
			}
			return a == b;
		}
	}

	bool Parameter::HasDefault() const
	{
		return !std::holds_alternative<std::monostate>( m_Default );
	}

	bool Parameter::IsAtDefault() const
	{
		if ( !HasDefault() || !m_Bind.IsBound() )
			return true;
		return ValueEquals( m_Bind.Get(), m_Default );
	}

	void Parameter::ResetToDefault() const
	{
		if ( HasDefault() && m_Bind.IsBound() )
			m_Bind.Set( m_Default );
	}

	// All three of these treat the SECOND AXIS exactly like the first.
	// A composite bound to two values (the anchor's row and column) is one
	// setting, so "differs from default" and "reset" have to mean the pair
	// -- an anchor sitting at the right column but the wrong row is not at
	// its default, and resetting it must move both. Reading only m_Bind
	// would leave the grid able to show an un-resettable half.
	bool Entry::HasDefault() const
	{
		if ( !std::holds_alternative<std::monostate>( m_Default ) )
			return true;
		if ( !std::holds_alternative<std::monostate>( m_DefaultB ) )
			return true;
		for ( const auto &pParam : m_Params )
			if ( pParam->HasDefault() )
				return true;
		return false;
	}

	bool Entry::IsAtDefault() const
	{
		if ( !std::holds_alternative<std::monostate>( m_Default ) && m_Bind.IsBound() )
			if ( !ValueEquals( m_Bind.Get(), m_Default ) )
				return false;
		if ( !std::holds_alternative<std::monostate>( m_DefaultB ) && m_BindB.IsBound() )
			if ( !ValueEquals( m_BindB.Get(), m_DefaultB ) )
				return false;

		// The row's parameters count as part of the row -- that is what
		// makes one reset the successor to a whole group link.
		for ( const auto &pParam : m_Params )
			if ( !pParam->IsAtDefault() )
				return false;
		return true;
	}

	void Entry::ResetToDefault() const
	{
		if ( !std::holds_alternative<std::monostate>( m_Default ) && m_Bind.IsBound() )
			m_Bind.Set( m_Default );
		if ( !std::holds_alternative<std::monostate>( m_DefaultB ) && m_BindB.IsBound() )
			m_BindB.Set( m_DefaultB );
		for ( const auto &pParam : m_Params )
			pParam->ResetToDefault();
	}

	Entry &Entry::Samples( std::function<SampleWindow()> fn )
	{
		m_Samples = std::move( fn );
		return *this;
	}

	Entry &Entry::Confirm( const char *pszPrompt )
	{
		m_sConfirm = pszPrompt ? pszPrompt : "";
		return *this;
	}

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
	Area &Area::Badge( std::function<std::string()> fn )     { m_Badge = std::move( fn ); return *this; }

	// A content body and registered rows are the CORRECT shape together --
	// Log's filter bank and text filter are what make its lines usable, so
	// an area is never "rows or content", it is rows AND, optionally, a
	// content body beneath them. See Registry.h's Content() comment for the
	// distinction this has to keep from the escape hatch P5 deleted.
	Area &Area::Content( std::function<std::vector<ContentLine>()> fn )
	{
		m_Content = std::move( fn );
		return *this;
	}

	Area &Area::FollowsTail( std::function<bool()> fn )
	{
		m_FollowTail = std::move( fn );
		return *this;
	}

	// ---- dynamic areas (P3b) --------------------------------------------
	// See Registry.h's Rebuilds() comment for why this exists and what it
	// costs.
	Area &Area::Rebuilds( std::function<uint64_t()> fnGeneration,
	                      std::function<void( Area & )> fnBuild )
	{
		if ( !fnGeneration || !fnBuild )
		{
			ReportViolation( Law::Dynamic, m_sId,
				"Rebuilds() needs both a generation function and a builder." );
			return *this;
		}
		m_Generation = std::move( fnGeneration );
		m_Build      = std::move( fnBuild );
		return *this;
	}

	bool Area::SyncIfStale()
	{
		if ( !m_Build )
			return false;

		const uint64_t ulGen = m_Generation();
		if ( m_bBuilt && ulGen == m_ulGeneration )
			return false;

		// Give back every id the previous build claimed, params included,
		// so the rebuild does not collide with the build it replaces. This
		// is the whole reason ID UNIQUENESS survives a dynamic area.
		if ( m_pRegistry )
		{
			for ( const auto &pEntry : m_Entries )
			{
				for ( size_t j = 0; j < pEntry->ParamCount(); ++j )
					m_pRegistry->ReleaseId( pEntry->ParamAt( j ).Id() );
				m_pRegistry->ReleaseId( pEntry->Id() );
			}
		}

		m_Entries.clear();
		m_Groups.clear();

		m_Build( *this );
		m_ulGeneration = ulGen;
		m_bBuilt = true;

		// HELP IS REQUIRED, and the Prefix Law, over the rows that were
		// just built. Registry::SelfTest() ran once after RegisterAll() and
		// has no way to see these -- without this line a dynamic row could
		// ship with no Help() at all, which is the one law a rebuild would
		// otherwise slip past.
		if ( m_pRegistry )
			m_pRegistry->SelfTestArea( *this );

		return true;
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

	void Registry::ReleaseId( const std::string &sId )
	{
		auto it = std::find( m_ClaimedIds.begin(), m_ClaimedIds.end(), sId );
		if ( it != m_ClaimedIds.end() )
			m_ClaimedIds.erase( it );
	}

	const Area *Registry::FindArea( const std::string &sId ) const
	{
		for ( const auto &pArea : m_Areas )
			if ( pArea->Id() == sId )
				return pArea.get();
		return nullptr;
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
		for ( const auto &pArea : m_Areas )
			nFound += SelfTestArea( *pArea );
		return nFound;
	}

	size_t Registry::SyncDynamicAreas()
	{
		size_t nRebuilt = 0;
		for ( auto &pArea : m_Areas )
			nRebuilt += pArea->SyncIfStale() ? 1u : 0u;
		return nRebuilt;
	}

	// One area's worth of SelfTest(). Split out so a dynamic area can be
	// re-checked after every rebuild (Area::SyncIfStale) against exactly
	// the same two laws, rather than against a second, drifting copy of
	// them.
	size_t Registry::SelfTestArea( const Area &area )
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

		for ( size_t i = 0; i < area.EntryCount(); ++i )
		{
			const Entry &e = area.EntryAt( i );
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

		return nFound;
	}

	// =====================================================================
	//  Adjustable (see Registry.h)
	// =====================================================================
	Adjustable Adjustable::Of( const Entry &e )
	{
		return Adjustable{ e.GetKind(), &e.Binding(), e.HasRange(),
		                   e.Lo(), e.Hi(), e.StepSize(), &e.Options(),
		                   e.GetCompositeKind() };
	}

	Adjustable Adjustable::Of( const Parameter &p )
	{
		return Adjustable{ p.GetKind(), &p.Binding(), p.HasRange(),
		                   p.Lo(), p.Hi(), p.StepSize(), &p.Options() };
	}

	bool AdjustValue( const Adjustable &adj, int nDir, bool bFine )
	{
		if ( nDir == 0 || adj.pBind == nullptr || !adj.pBind->IsBound() )
			return false;
		if ( IsReadOnly( adj.eKind ) )
			return false;

		const Value vNow = adj.pBind->Get();

		switch ( adj.eKind )
		{
			// A switch is ordered even though it is binary: right is on, left
			// is off. Toggling on either arrow would make the key's meaning
			// depend on the current value, which is the one thing an
			// adjust-in-place list must not do -- holding Right down a list
			// of switches should end with them all on, not oscillating.
			case Kind::Switch:
			{
				if ( const bool *p = std::get_if<bool>( &vNow ) )
				{
					const bool bNew = nDir > 0;
					if ( bNew == *p )
						return false;
					adj.pBind->Set( Value{ bNew } );
					return true;
				}
				return false;
			}

			// A choice steps through its own option list, in declaration
			// order, and STOPS at both ends rather than wrapping. Wrapping
			// would mean a held arrow key never settles, and it would make
			// "press Right until it is what I want" unreliable.
			case Kind::Choice:
			{
				if ( adj.pOptions == nullptr || adj.pOptions->empty() )
					return false;
				const int *p = std::get_if<int>( &vNow );
				if ( p == nullptr )
					return false;

				int nAt = -1;
				for ( size_t i = 0; i < adj.pOptions->size(); i++ )
				{
					if ( ( *adj.pOptions )[ i ].nValue == *p )
					{
						nAt = (int)i;
						break;
					}
				}
				// A value not in the list (a config holding something the
				// build no longer offers) steps to the first option rather
				// than refusing -- otherwise that row is unfixable from the
				// keyboard, which is exactly the unreachable state the
				// Reachability Law forbids.
				const int nNext = nAt < 0 ? 0
					: std::clamp( nAt + nDir, 0, (int)adj.pOptions->size() - 1 );
				if ( nNext == nAt )
					return false;
				adj.pBind->Set( Value{ ( *adj.pOptions )[ (size_t)nNext ].nValue } );
				return true;
			}

			// A Color composite's value is a PACKED 0xRRGGBB int, so the
			// generic int step below moved it by one -- i.e. it nudged the
			// blue channel's least significant bit. Harmless, imperceptible
			// and meaningless: a colour is not an ordered scalar, and there
			// is no direction "right" could sensibly mean. Refused here for
			// the same reason a Bank is (SPEC 3.12), and like the Bank it
			// keeps its own route -- the Inspector's OKLCH body, which is
			// the control the row actually offers.
			//
			// Anchor and Hue are NOT refused: an anchor's axes and a hue's
			// degrees are both genuinely ordered.
			case Kind::Composite:
				if ( adj.eComposite == CompositeKind::Color )
					return false;
				[[fallthrough]];

			case Kind::Slider:
			case Kind::Stepper:
			{
				if ( const int *p = std::get_if<int>( &vNow ) )
				{
					// An int step is never fractional, so Shift cannot mean
					// x0.1 here; it stays one unit rather than rounding to
					// zero and looking like a dead key.
					const int nStep = adj.flStep > 0.0f ? std::max( 1, (int)adj.flStep ) : 1;
					int nNew = *p + nDir * nStep;
					if ( adj.bHasRange )
						nNew = std::clamp( nNew, (int)adj.flLo, (int)adj.flHi );
					if ( nNew == *p )
						return false;
					adj.pBind->Set( Value{ nNew } );
					return true;
				}
				if ( const float *p = std::get_if<float>( &vNow ) )
				{
					// No declared step means 100 notches across the range --
					// the same resolution a drag gives, so the keyboard and
					// the pointer land on the same set of values.
					float flStep = adj.flStep;
					if ( flStep <= 0.0f )
						flStep = adj.bHasRange ? ( adj.flHi - adj.flLo ) / 100.0f : 1.0f;
					if ( bFine )
						flStep *= 0.1f;

					float flNew = *p + (float)nDir * flStep;
					if ( adj.bHasRange )
						flNew = std::clamp( flNew, adj.flLo, adj.flHi );
					if ( flNew == *p )
						return false;
					adj.pBind->Set( Value{ flNew } );
					return true;
				}
				return false;
			}

			// Text, Bank and Action have no ordering an arrow key could
			// follow: a Bank's value is a SET (SPEC §3.12), text is not
			// ordered, and an action has no value at all. Refusing here is
			// what lets a caller pass any selection without a kind check.
			default:
				return false;
		}
	}

	// D25. The same taxonomy AdjustValue()'s switch encodes, asked as a
	// question instead of performed. Kept immediately below it, and pinned to
	// it by test, because two copies of a taxonomy in different files is
	// exactly how the Position Grid's two call sites drifted (D15.1).
	bool CanAdjust( const Adjustable &adj )
	{
		if ( adj.pBind == nullptr || !adj.pBind->IsBound() )
			return false;
		if ( IsReadOnly( adj.eKind ) )
			return false;

		switch ( adj.eKind )
		{
			case Kind::Switch:
			case Kind::Slider:
			case Kind::Stepper:
				return true;

			// An empty option list has nothing to step through, and the
			// arrow would be a dead key rather than a refused one.
			case Kind::Choice:
				return adj.pOptions != nullptr && !adj.pOptions->empty();

			// A packed colour is not an ordered scalar -- see the Composite
			// case above for why. Anchor, Hue, Strip and Graph are.
			case Kind::Composite:
				return adj.eComposite != CompositeKind::Color;

			// Text, Bank, Action: no ordering an arrow could follow.
			default:
				return false;
		}
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

#include "LaunchOptions.h"

#include <array>
#include <string>

namespace gamescope::LaunchOptions
{
	namespace
	{
		struct OptState
		{
			bool        bGiven = false;
			std::string sSpelling;
		};

		// File-static, exactly like Keybinds.h's own state: written once by
		// the startup getopt loops (a single thread, before anything else
		// runs), read from anywhere afterwards.
		std::array<OptState, (size_t)Opt::Count> s_State{};
	}

	void MarkGiven( Opt eOpt, const char *pszSpelling )
	{
		OptState &state = s_State[ (size_t)eOpt ];
		state.bGiven = true;
		state.sSpelling = pszSpelling ? pszSpelling : "";
	}

	bool Given( Opt eOpt )
	{
		return s_State[ (size_t)eOpt ].bGiven;
	}

	const char *Spelling( Opt eOpt )
	{
		return s_State[ (size_t)eOpt ].sSpelling.c_str();
	}
}

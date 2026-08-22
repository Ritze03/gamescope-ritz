#pragma once

#include <cstdarg>
#include <cstdint>

#include <memory>
#include <functional>
#include <string_view>
#include <unordered_map>

#ifdef __GNUC__
#define ATTRIB_PRINTF(start, end) __attribute__((format(printf, start, end)))
#else
#define ATTRIB_PRINTF(start, end)
#endif

enum LogPriority
{
	LOG_SILENT,
	LOG_ERROR,
	LOG_WARNING,
	LOG_INFO,
	LOG_DEBUG,
};

struct LogConVar_t;

class LogScope
{
public:
	LogScope( std::string_view psvName, LogPriority eMaxPriority = LOG_INFO );
	LogScope( std::string_view psvName, std::string_view psvPrefix, LogPriority eMaxPriority = LOG_INFO );
	~LogScope();

	bool Enabled( LogPriority ePriority ) const;
	void SetPriority( LogPriority ePriority ) { m_eMaxPriority = ePriority; }

	void vlogf(enum LogPriority priority, const char *fmt, va_list args) ATTRIB_PRINTF(3, 0);
	void log(enum LogPriority priority, std::string_view psvText);

	void warnf(const char *fmt, ...) ATTRIB_PRINTF(2, 3);
	void errorf(const char *fmt, ...) ATTRIB_PRINTF(2, 3);
	void infof(const char *fmt, ...) ATTRIB_PRINTF(2, 3);
	void debugf(const char *fmt, ...) ATTRIB_PRINTF(2, 3);

	void errorf_errno(const char *fmt, ...) ATTRIB_PRINTF(2, 3);

	bool bPrefixEnabled = true;

	using LoggingListenerFunc = std::function<void( LogPriority ePriority, std::string_view psvScope, std::string_view psvText )>;
	std::unordered_map<uintptr_t, LoggingListenerFunc> m_LoggingListeners;

	// Issue #39: logging in this codebase is fully decentralized -- ~39
	// independent `static LogScope` instances, one per subsystem file, each
	// with its own m_LoggingListeners map above (already used by exactly one
	// consumer today: wlserver.cpp's gamescope_private_bind(), which listens
	// on console_log alone to stream that one scope's lines to a connected
	// Wayland client). There is no single "the gamescope log" to tap.
	//
	// This is the fix: every LogScope pushes/erases itself into a process-
	// wide registry in its own constructor/destructor (log.cpp), guarded by
	// a mutex since scopes are static globals across many translation units
	// and can be constructed from more than one thread. A "whole log"
	// listener registered here (AddGlobalLoggingListener) is copied into
	// every scope that already exists *and* into every scope constructed
	// afterward, so a consumer only has to attach once. This is what the
	// settings overlay's LOG panel's Gamescope tab uses (see
	// Overlay/LogCapture.cpp) instead of hand-listing every scope.
	static uintptr_t AddGlobalLoggingListener( LoggingListenerFunc func );
	static void RemoveGlobalLoggingListener( uintptr_t ulListenerId );

private:
	void vprintf(enum LogPriority priority, const char *fmt, va_list args) ATTRIB_PRINTF(3, 0);
	void logf(enum LogPriority priority, const char *fmt, ...) ATTRIB_PRINTF(3, 4);

	std::string_view m_psvName;
	std::string_view m_psvPrefix;

	LogPriority m_eMaxPriority = LOG_INFO;
	
	std::unique_ptr<LogConVar_t> m_pEnableConVar;
};

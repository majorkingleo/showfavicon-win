#pragma once

#include <string>

namespace sf {

// Start the debug logging session.
//
// `logFile`  path of the log file; empty disables the file backend.
// `console`  mirrors every message to stdout. ShowFavicon is a WIN32 GUI
//            program with no console of its own, so one is attached to the
//            process that started it when there is one, and allocated
//            otherwise. That is what `-d` on the command line enables.
//
// Call once, before anything can emit a CPPDEBUG message, and call
// sf::logShutdown() before returning from wWinMain so the last lines drain.
void logInit(const std::wstring& logFile, bool console);
void logShutdown();

}  // namespace sf

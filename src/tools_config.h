#pragma once

// Build configuration for the vendored cpputils (src/cpputils).
//
// cpputils/io/DetectLocale.h includes this file as "../../tools_config.h",
// i.e. src/cpputils/io/../../tools_config.h == src/tools_config.h. It has to
// exist: an empty file keeps DetectLocale - and with it Tools::OutDebug, the
// console side of the debug logging - compiled in. Defining
// DISABLE_CPPUTILS_READFILE here would disable both and silence -d.
//
// The options below are the ones the upstream Makefile.am documents; none of
// them are needed by ShowFavicon.

// #define TOOLS_USE_THREADS
// #define TOOLS_USE_GUI
// #define TOOLS_USE_MYSQL
// #define TOOLS_USE_ODBC
// #define TOOLS_USE_DB
// #define TOOLS_USE_ORACLE

// #define TOOLS_USE_GLOBAL_COUNT

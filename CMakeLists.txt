cmake_minimum_required(VERSION 3.20)
project(ShowFavicon LANGUAGES CXX)

enable_language(RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CPU/timing diagnostics in src/diag.h (per-thread heartbeats, fetch/parse
# timings). Off by default, so the shipped binary carries none of it; switch it
# on (-DLOG_TIMING=ON) to hunt a process that burns CPU.
option(LOG_TIMING "Enable the CPU/timing diagnostics in src/diag.h" OFF)

# cpputils' CPPDEBUG is compiled out when NDEBUG is defined. Autotools - the
# reference build - never sets it, and a tray app's log matters most in the
# shipped Release binary, so drop the flag CMake adds for Release.
string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

find_package(Threads REQUIRED)

set(RESOURCES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/resources)
set(SETTINGS_RES_OBJ ${CMAKE_CURRENT_BINARY_DIR}/settings_res.o)
set(CPPUTILS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/cpputils)

# ---------------------------------------------------------------------------
# Vendored cpputils (git submodule, src/cpputils): the debug plumbing behind
# CPPDEBUG / Tools::x_debug, the synchronous OutDebug frontend, the console
# colour helper, the locale conversion the output paths need, and Tools::format.
# Source list taken from the reference project's Makefile.am; the async file /
# console backends live in src/ (AsyncOutDebug.*, AsyncFileLogger.*) and are
# listed below in this library, so the program's -Wall/-Wextra does not apply.
# ---------------------------------------------------------------------------
add_library(cpputils STATIC
    ${CPPUTILS_DIR}/io/ColoredOutput.cc
    ${CPPUTILS_DIR}/io/CpputilsDebug.cc
    ${CPPUTILS_DIR}/io/DetectLocale.cc
    ${CPPUTILS_DIR}/io/OutDebug.cc
    ${CPPUTILS_DIR}/io/read_file.cc
    ${CPPUTILS_DIR}/cpputilsshared/stderr_exception.cc
    ${CPPUTILS_DIR}/cpputilsshared/string_utils.cc
    ${CPPUTILS_DIR}/cpputilsshared/utf8_util.cc
    ${CPPUTILS_DIR}/cpputilsshared/xml.cc
    ${CPPUTILS_DIR}/cpputilsshared/cpputilsformat/format1.cc
    ${CPPUTILS_DIR}/cpputilsshared/cpputilsformat/format2.cc
    ${CPPUTILS_DIR}/cpputilsshared/cpputilsformat/cformat.cc
    ${CPPUTILS_DIR}/cpputilsshared/cpputilsformat/wformat2.cc
    ${CPPUTILS_DIR}/cpputilsshared/cpputilsformat/cwformat.cc
    # vendored from the reference project's libcommon: the async frontend and
    # the file/console backends built on top of cpputils' OutDebug.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AsyncOutDebug.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AsyncFileLogger.cc
)

# SYSTEM so that -Wall -Wextra on the app sources does not report the vendored
# cpputilsformat templates (they carry a long list of -Wunused-parameter).
target_include_directories(cpputils SYSTEM PUBLIC
    ${CPPUTILS_DIR}/io
    ${CPPUTILS_DIR}/cpputilsshared
    ${CPPUTILS_DIR}/cpputilsshared/cpputilsformat
)

# DetectLocale.cc picks setlocale(LC_ALL, "") under WIN32; with -std=c++23
# __STRICT_ANSI__ is set, so the macro has to be defined explicitly (the
# reference does the same for its mingw build).
target_compile_definitions(cpputils PRIVATE WIN32)

target_compile_options(cpputils PRIVATE
    -fno-declone-ctor-dtor
    -ffunction-sections
    -fdata-sections
    $<$<CONFIG:Release>:-Os>
    $<$<CONFIG:Debug>:-g -O0>
)

target_link_libraries(cpputils PUBLIC Threads::Threads)

add_custom_command(
    OUTPUT ${SETTINGS_RES_OBJ}
    COMMAND ${CMAKE_RC_COMPILER} -I${RESOURCES_DIR} -O coff
            ${RESOURCES_DIR}/settings.rc -o ${SETTINGS_RES_OBJ}
    DEPENDS ${RESOURCES_DIR}/settings.rc ${RESOURCES_DIR}/resource.h
    VERBATIM
    COMMENT "Compiling dialog resource (windres)"
)

add_executable(showfavicon WIN32
    src/main.cpp
    src/util.cpp
    src/http.cpp
    src/icon.cpp
    src/settings.cpp
    src/logging.cpp
    ${SETTINGS_RES_OBJ}
)

target_include_directories(showfavicon PRIVATE
    third_party/nanosvg
    src/resources
)

target_compile_definitions(showfavicon PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(LOG_TIMING)
    target_compile_definitions(showfavicon PRIVATE LOG_TIMING)
endif()

target_compile_options(showfavicon PRIVATE
    -fno-declone-ctor-dtor
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-Os>
    $<$<CONFIG:Debug>:-g -O0>
)

target_link_options(showfavicon PRIVATE
    -municode
    -static
    $<$<CONFIG:Release>:-s>
    -Wl,--gc-sections
)

target_link_libraries(showfavicon PRIVATE
    cpputils
    # cpputils/io/read_file.cc uses iconv; DetectLocale pulls it in.
    iconv
    gdiplus
    winhttp
    windowscodecs
    wininet
    iphlpapi
    shell32
    user32
    gdi32
    ole32
    shlwapi
)

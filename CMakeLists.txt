cmake_minimum_required(VERSION 3.20)
project(ShowFavicon LANGUAGES CXX)

enable_language(RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RESOURCES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/resources)
set(SETTINGS_RES_OBJ ${CMAKE_CURRENT_BINARY_DIR}/settings_res.o)

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

target_compile_options(showfavicon PRIVATE
    -Os
    -fno-declone-ctor-dtor
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
)

target_link_options(showfavicon PRIVATE
    -municode
    -static
    -s
    -Wl,--gc-sections
)

target_link_libraries(showfavicon PRIVATE
    gdiplus
    winhttp
    windowscodecs
    shell32
    user32
    gdi32
    ole32
    shlwapi
)

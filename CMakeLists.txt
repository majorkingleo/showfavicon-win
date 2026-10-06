cmake_minimum_required(VERSION 3.20)
project(ShowFavicon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(showfavicon WIN32
    src/main.cpp
    src/util.cpp
    src/http.cpp
    src/icon.cpp
)

target_include_directories(showfavicon PRIVATE
    third_party/nanosvg
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

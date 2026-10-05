cmake_minimum_required(VERSION 3.20)
project(ShowFavicon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(showfavicon WIN32
    src/main.cpp
)

target_compile_definitions(showfavicon PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(showfavicon PRIVATE
    -Os
    -flto
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
)

target_link_options(showfavicon PRIVATE
    -municode
    -static
    -s
    -flto
    -Wl,--gc-sections
)

target_link_libraries(showfavicon PRIVATE
    gdiplus
    winhttp
    shell32
    user32
    gdi32
    ole32
    shlwapi
)

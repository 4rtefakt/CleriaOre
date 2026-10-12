# CleriaOre — a mod loader for the retail Ys Origin (Steam, yso_win.exe): a 32-bit dinput8.dll proxy
# that hosts CleriaCore's mod layer (external/CleriaCore, cmake/modkit.cmake) inside the game.
#
#   git submodule update --init
#   python external/CleriaCore/tools/fetch_deps.py --only lua,miniz
#   cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
#   cmake --build build --config Release        -> build/Release/dinput8.dll
#   ctest --test-dir build -C Release
cmake_minimum_required(VERSION 3.24)
project(CleriaOre C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Configure a 32-bit Windows build (cmake -A Win32): the game is 32-bit.")
endif()
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)   # as CleriaCore builds the same sources
endif()

include(external/CleriaCore/cmake/modkit.cmake)

# MinHook (function detours). The v1.3.3 tag predates its CMake support: it is fetched, and its C files built here.
include(FetchContent)
FetchContent_Declare(minhook GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git GIT_TAG v1.3.3)
FetchContent_MakeAvailable(minhook)
add_library(minhook STATIC
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde32.c)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)
target_compile_options(minhook PRIVATE /W0)

add_library(dinput8 SHARED
  src/dllmain.cpp
  src/proxy_dinput8.cpp
  src/ore.cpp
  src/files.cpp
  src/hooks.cpp
  src/exports.def)
target_link_libraries(dinput8 PRIVATE cleria-modkit miniz minhook)
set_target_properties(dinput8 PROPERTIES PREFIX "" OUTPUT_NAME "dinput8")

# ore-loadtest: loads the built dinput8.dll as the game would, checks that DirectInput8Create still
# reaches the system DLL and that a sample Lua mod of CleriaCore starts. No game needed.
enable_testing()
add_executable(ore-loadtest tests/loadtest.cpp)
target_link_libraries(ore-loadtest PRIVATE dxguid)
add_dependencies(ore-loadtest dinput8)
add_test(NAME loadtest COMMAND ore-loadtest "${CMAKE_SOURCE_DIR}/external/CleriaCore/samples/mods/hello")

# ore-filestest: the file overrides as the game's archive reader would ask for them (src/files), on a
# scratch mod folder under %TEMP%: a plain read, a ".z" packed on demand, a path no mod provides.
add_executable(ore-filestest tests/filestest.cpp src/files.cpp src/ore.cpp)
target_include_directories(ore-filestest PRIVATE src)
target_link_libraries(ore-filestest PRIVATE cleria-modkit miniz)
add_test(NAME filestest COMMAND ore-filestest)

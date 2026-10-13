# CleriaOre — a mod loader for the retail Ys Origin (Steam, yso_win.exe): a 32-bit dinput8.dll proxy
# that hosts CleriaCore's mod layer (external/CleriaCore, cmake/modkit.cmake) inside the game.
#
#   git submodule update --init
#   python external/CleriaCore/tools/fetch_deps.py --only lua,miniz,fonts
#   cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
#   cmake --build build --config Release        -> build/Release/CleriaOre.exe (the loader is inside it)
#   ctest --test-dir build -C Release
cmake_minimum_required(VERSION 3.24)
project(CleriaOre C CXX)

set(CMAKE_CXX_STANDARD 20)
# The C runtime is linked in: a player needs no Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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

# Dear ImGui 1.92.9b (ocornut/imgui, MIT), the version CleriaCore builds: its UI kit (apps/cleria-view/view_ui.inl)
# is used as it is, on the DirectX 9 and Win32 backends here.
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.92.9b)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_options(imgui PRIVATE /W0)

add_library(dinput8 SHARED
  src/dllmain.cpp
  src/proxy_dinput8.cpp
  src/ore.cpp
  src/log.cpp
  src/files.cpp
  src/hooks.cpp
  src/host.cpp
  src/ui.cpp
  src/exports.def)
target_link_libraries(dinput8 PRIVATE cleria-modkit miniz minhook imgui d3d9)
target_compile_definitions(dinput8 PRIVATE NOMINMAX)   # CleriaCore's sources use std::min / std::max
set_target_properties(dinput8 PROPERTIES PREFIX "" OUTPUT_NAME "dinput8")

# CleriaOre.exe: the mods and the loader managed outside the game (src/manager), and the installer: the one
# file a player downloads. What it installs is inside it as resources (res/cleriaore.rc): the built
# dinput8.dll and the panel's fonts. The .rc is compiled in the `payload` folder, beside a copy of the DLL,
# and again whenever the DLL changes.
set(PAYLOAD "${CMAKE_BINARY_DIR}/payload")
add_custom_command(OUTPUT "${PAYLOAD}/cleriaore.rc" "${PAYLOAD}/dinput8.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:dinput8>" "${PAYLOAD}/dinput8.dll"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/res/cleriaore.rc" "${PAYLOAD}/cleriaore.rc"
  DEPENDS dinput8 "$<TARGET_FILE:dinput8>" res/cleriaore.rc)
add_executable(CleriaOre WIN32 src/manager/main.cpp "${PAYLOAD}/cleriaore.rc")
target_include_directories(CleriaOre PRIVATE src res "${PAYLOAD}" external/CleriaCore/third_party/fonts)   # the .rc's files too
target_link_libraries(CleriaOre PRIVATE cleria-modkit imgui d3d9 comdlg32 shell32 advapi32 ole32 uuid)
target_compile_definitions(CleriaOre PRIVATE NOMINMAX)

# ore-loadtest: loads the built dinput8.dll as the game would, checks that DirectInput8Create still
# reaches the system DLL and that a sample Lua mod of CleriaCore starts. No game needed.
enable_testing()
add_executable(ore-loadtest tests/loadtest.cpp)
target_link_libraries(ore-loadtest PRIVATE dxguid)
add_dependencies(ore-loadtest dinput8)
add_test(NAME loadtest COMMAND ore-loadtest "${CMAKE_SOURCE_DIR}/external/CleriaCore/samples/mods/hello")

# ore-filestest: the file overrides as the game's archive reader would ask for them (src/files), on a
# scratch mod folder under %TEMP%: a plain read, a ".z" packed on demand, a path no mod provides.
add_executable(ore-filestest tests/filestest.cpp src/files.cpp src/log.cpp)
target_include_directories(ore-filestest PRIVATE src)
target_link_libraries(ore-filestest PRIVATE cleria-modkit miniz)
add_test(NAME filestest COMMAND ore-filestest)

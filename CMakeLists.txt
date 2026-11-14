cmake_minimum_required(VERSION 3.20)
project(SWROTS_PC LANGUAGES C CXX)

# The game's code is 32-bit x86 machine code and runs natively in-process,
# so the port must be a 32-bit Windows program.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "SWROTS-PC must be configured as 32-bit: cmake -A Win32")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "SWROTS-PC requires MSVC (inline x86 asm, __declspec(naked)).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Game code shares our threads and stacks; runtime stack checks and /GS canaries
# in hook entry points would misfire on the game's calling patterns.
string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")

foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${CMAKE_BINARY_DIR}/bin)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg} ${CMAKE_BINARY_DIR}/bin)
endforeach()

# ---------------------------------------------------------------------------
# swrots.exe -- tiny loader. Linked at 0x10000 with a large uninitialized
# section so the game's fixed address range belongs to this image before
# Windows can place anything else there. It loads core.dll and never
# returns (its own code is overwritten by the game image).
# ---------------------------------------------------------------------------
add_executable(SWROTS WIN32 src/loader/loader.cpp src/loader/loader.rc)
target_compile_options(SWROTS PRIVATE /GS- /Oi)
set_target_properties(SWROTS PROPERTIES LINK_FLAGS
  "/BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE /NODEFAULTLIB /ENTRY:LoaderEntry /SAFESEH:NO")
target_link_libraries(SWROTS PRIVATE kernel32 user32)

# ---------------------------------------------------------------------------
# core.dll -- the native runtime: XBE loader, Xbox kernel services on
# Win32, and native replacements for the XDK libraries.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
  src/core/*.cpp src/kernel/*.cpp src/xapi/*.cpp src/game/*.cpp src/input/*.cpp src/d3d/*.cpp src/audio/*.cpp
  src/debug/*.cpp src/net/*.cpp)
file(GLOB_RECURSE CORE_HEADERS CONFIGURE_DEPENDS
  src/core/*.h src/kernel/*.h src/xapi/*.h src/game/*.h src/game/*.inc src/input/*.h src/d3d/*.h src/debug/*.h
  src/net/*.h)

# Dear ImGui (third_party/imgui, MIT): the debug menu.
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
set(IMGUI_SOURCES
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)

# Lua (third_party/lua, MIT): the scripting of missions and modes (src/game/missions.cpp).
file(GLOB LUA_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/*.c)
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE C COMPILE_OPTIONS "/W0")
add_library(SWROTSCore SHARED ${CORE_SOURCES} ${CORE_HEADERS} ${IMGUI_SOURCES} ${LUA_SOURCES})
source_group(lua FILES ${LUA_SOURCES})
source_group(imgui FILES ${IMGUI_SOURCES})
target_include_directories(SWROTSCore PRIVATE src ${IMGUI_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua ${CMAKE_BINARY_DIR}/generated)
# The version for the log (generated/version.h, from git describe at every build).
add_custom_target(SWROTSVersion
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_SOURCE_DIR} -DOUTPUT=${CMAKE_BINARY_DIR}/generated/version.h
          -P ${CMAKE_SOURCE_DIR}/tools/version.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/version.h)
add_dependencies(SWROTSCore SWROTSVersion)
target_compile_definitions(SWROTSCore PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_compile_options(SWROTSCore PRIVATE /W3 /GS- /EHsc /Zi /utf-8)
# A fixed base (netplay): the port's own state that the game holds pointers to (sound objects, the jumps patched into
# its libraries) must be at the same addresses on every PC. Relocations are kept, should the address be taken.
set_target_properties(SWROTSCore PROPERTIES LINK_FLAGS "/LARGEADDRESSAWARE /SAFESEH:NO /DEBUG /BASE:0x60000000 /DYNAMICBASE:NO /MAP")
target_link_libraries(SWROTSCore PRIVATE ws2_32 winhttp ntdll bcrypt winmm user32 dbghelp xinput d3d9 d3dcompiler xaudio2 windowscodecs ole32 shell32 comdlg32 uuid hid setupapi propsys)

add_dependencies(SWROTS SWROTSCore)
set_target_properties(SWROTS PROPERTIES OUTPUT_NAME swrots)
set_target_properties(SWROTSCore PROPERTIES OUTPUT_NAME core)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src FILES ${CORE_SOURCES} ${CORE_HEADERS})

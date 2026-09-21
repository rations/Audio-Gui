cmake_minimum_required(VERSION 3.16)
project(AudioGui LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Position-independent executables for all targets.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# All binaries land in the build root so the GUI finds the bridge executables
# next to itself (BridgeManager resolves them via /proc/self/exe).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(JACK IMPORTED_TARGET jack)  # optional

# The GUI is hand-drawn with Cairo and FreeType on a plain X11 window. There is no
# widget toolkit: no Qt, no GTK, no GLib.
#
# CAIRO/CAIRO-FT ARE SEPARATE FROM CAIRO-XLIB ON PURPOSE. The drawing layer
# (src/gfx, src/panel.cpp) links the first set and never the second, which is what
# lets tools/uirender compose and audit the real panel with no X server running.
# An #include of Xlib.h under src/gfx costs that, so keep the split.
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo cairo-ft freetype2)
pkg_check_modules(CAIROX11 REQUIRED IMPORTED_TARGET cairo-xlib x11)

# Where the bundled fonts are installed. respath.cpp falls back to a build tree and
# to $AUDIOGUI_RESOURCE_DIR, so an uninstalled binary still finds them.
include(GNUInstallDirs)
set(AUDIOGUI_RESOURCE_DIR "${CMAKE_INSTALL_FULL_DATADIR}/audio-gui")

# Shared hardening flags (applied per-target below).
set(HARDEN_FLAGS
  -Wall -Wextra -O2
  -fstack-protector-strong
  -D_FORTIFY_SOURCE=2
  -Wformat -Wformat-security)
add_link_options(-pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)

# ---- pa-alsa-bridge (C) ----
add_executable(pa-alsa-bridge pulse_alsa_bridge.c)
target_include_directories(pa-alsa-bridge PRIVATE include)
target_compile_options(pa-alsa-bridge PRIVATE ${HARDEN_FLAGS})
target_link_libraries(pa-alsa-bridge PRIVATE PkgConfig::ALSA pthread m rt)

# ---- pulse-jack-bridge (C) — only if libjack is available ----
if(JACK_FOUND)
  add_executable(pulse-jack-bridge pulse_jack_bridge.c)
  target_include_directories(pulse-jack-bridge PRIVATE include)
  target_compile_options(pulse-jack-bridge PRIVATE ${HARDEN_FLAGS})
  target_link_libraries(pulse-jack-bridge PRIVATE PkgConfig::JACK rt)
else()
  message(STATUS "libjack not found: skipping pulse-jack-bridge "
                 "(GUI keeps JACK routing disabled)")
endif()

# ---- the drawing layer: cairo + freetype, NO X11 ----
# Shared by the GUI and by tools/uirender, which is the point: the audit draws the
# same code the window does.
add_library(audiogui-gfx STATIC
  src/gfx/canvas.cpp
  src/gfx/fontstack.cpp
  src/gfx/palette.cpp
  src/gfx/widgets.cpp
  src/gfx/combo.cpp
  src/gfx/levelmeter.cpp
  src/panel.cpp
  src/platform/respath.cpp)
target_include_directories(audiogui-gfx PUBLIC src)
target_compile_definitions(audiogui-gfx PUBLIC
  AUDIOGUI_RESOURCE_DIR_DEFAULT="${AUDIOGUI_RESOURCE_DIR}")
target_compile_options(audiogui-gfx PRIVATE ${HARDEN_FLAGS})
target_link_libraries(audiogui-gfx PUBLIC PkgConfig::CAIRO)

# ---- audio-gui ----
add_executable(audio-gui
  src/main.cpp
  src/app.cpp
  src/config.cpp
  src/peaksource.cpp
  src/AlsaMixer.cpp
  src/AlsaDevices.cpp
  src/BridgeManager.cpp
  src/platform/fs.cpp
  src/platform/spawn.cpp
  src/platform/x11window.cpp
  src/platform/xerror.cpp)
target_include_directories(audio-gui PRIVATE include src)
target_compile_options(audio-gui PRIVATE ${HARDEN_FLAGS})
# JACK is detected at runtime via dlopen, so the GUI links libdl + librt only
# (no hard libjack dependency). rt is for shm_open, which the level meter needs.
target_link_libraries(audio-gui PRIVATE
  audiogui-gfx PkgConfig::CAIROX11 PkgConfig::ALSA rt ${CMAKE_DL_LIBS})

# ---- uirender: the offline layout audit ----
# Draws the real panel to PNG with no X server and fails if any string overflows
# its slot or uses a glyph the bundled fonts do not have.
add_executable(uirender tools/uirender.cpp)
target_include_directories(uirender PRIVATE src)
target_compile_options(uirender PRIVATE ${HARDEN_FLAGS})
target_link_libraries(uirender PRIVATE audiogui-gfx)

# ---- install ----
install(TARGETS audio-gui pa-alsa-bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(JACK_FOUND)
  install(TARGETS pulse-jack-bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY resources/fonts DESTINATION ${AUDIOGUI_RESOURCE_DIR})
install(DIRECTORY packaging/icons/hicolor
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons)

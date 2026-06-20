cmake_minimum_required(VERSION 3.16)
project(AudioGui LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Position-independent executables for all targets.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# All binaries land in the build root so the GUI finds the bridge executables
# next to itself (BridgeManager resolves them via applicationDirPath()).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(JACK IMPORTED_TARGET jack)  # optional

find_package(Qt6 REQUIRED COMPONENTS Widgets)
set(CMAKE_AUTOMOC ON)

# Shared hardening flags (applied per-target below).
set(HARDEN_FLAGS
  -Wall -Wextra -O2
  -fstack-protector-strong
  -D_FORTIFY_SOURCE=2
  -Wformat -Wformat-security)
add_link_options(-pie)

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

# ---- audio-gui (C++/Qt6) ----
add_executable(audio-gui
  src/main.cpp
  src/MainWindow.cpp
  src/AlsaMixer.cpp
  src/MixerStripWidget.cpp
  src/LevelMeter.cpp
  src/BridgeManager.cpp
  src/Theme.cpp)
target_include_directories(audio-gui PRIVATE include src)
target_compile_options(audio-gui PRIVATE ${HARDEN_FLAGS})
# JACK is detected at runtime via dlopen, so the GUI links libdl + librt only
# (no hard libjack dependency).
target_link_libraries(audio-gui PRIVATE Qt6::Widgets PkgConfig::ALSA rt ${CMAKE_DL_LIBS})

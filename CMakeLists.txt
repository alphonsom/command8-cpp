# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(command8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  set(C8_WARNINGS /W4)
else()
  set(C8_WARNINGS -Wall -Wextra)
endif()

# Core library: protocol/feedback/controller are platform-neutral; the MIDI
# transport (Surface + MidiPort) is ALSA on Linux and RtMidi on Windows.
add_library(command8 STATIC
  src/protocol.cpp
  src/feedback.cpp
  src/controller.cpp)
target_include_directories(command8 PUBLIC src)
target_compile_options(command8 PRIVATE ${C8_WARNINGS})

if(WIN32)
  find_package(RtMidi CONFIG REQUIRED)
  target_sources(command8 PRIVATE
    src/rtmidi/rtmidi_surface.cpp
    src/rtmidi/rtmidi_midi_port.cpp)
  target_link_libraries(command8 PUBLIC RtMidi::rtmidi)
  target_compile_definitions(command8 PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(ALSA REQUIRED alsa)
  find_package(Threads REQUIRED)
  target_sources(command8 PRIVATE
    src/alsa/alsa_surface.cpp
    src/alsa/alsa_midi_port.cpp)
  target_include_directories(command8 PUBLIC ${ALSA_INCLUDE_DIRS})
  target_link_libraries(command8 PUBLIC ${ALSA_LIBRARIES} Threads::Threads)
endif()

add_executable(command8-monitor src/main.cpp)
target_link_libraries(command8-monitor PRIVATE command8)
target_compile_options(command8-monitor PRIVATE ${C8_WARNINGS})

# Reaper front-end (OSC via liblo). Optional: only built if liblo is present
# (vcpkg on Windows, pkg-config elsewhere).
set(C8_LO_LIBS "")
if(WIN32)
  find_package(liblo CONFIG QUIET)
  foreach(t liblo::liblo liblo::liblo_static liblo::lo liblo::lo_static)
    if(NOT C8_LO_LIBS AND TARGET ${t})
      set(C8_LO_LIBS ${t})
    endif()
  endforeach()
else()
  pkg_check_modules(LIBLO liblo)
  if(LIBLO_FOUND)
    set(C8_LO_LIBS ${LIBLO_LIBRARIES})
  endif()
endif()
if(C8_LO_LIBS)
  add_executable(command8-reaper
    src/reaper/reaper_main.cpp
    src/reaper/reaper_backend.cpp)
  if(NOT WIN32)
    target_include_directories(command8-reaper PRIVATE ${LIBLO_INCLUDE_DIRS})
  endif()
  target_link_libraries(command8-reaper PRIVATE command8 ${C8_LO_LIBS})
  target_compile_options(command8-reaper PRIVATE ${C8_WARNINGS})
else()
  message(STATUS "liblo not found - skipping command8-reaper")
endif()

# Mackie Control (MCU) front-end (no extra deps beyond the MIDI backend).
add_executable(command8-mackie
  src/mackie/mackie_main.cpp
  src/mackie/mackie_backend.cpp)
target_link_libraries(command8-mackie PRIVATE command8)
target_compile_options(command8-mackie PRIVATE ${C8_WARNINGS})

# --- tests (CTest) ---
enable_testing()
add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE command8)
target_compile_options(test_protocol PRIVATE ${C8_WARNINGS})
add_test(NAME protocol COMMAND test_protocol)

# --- install ---
include(GNUInstallDirs)

# Ship files/dirs with the conventional 0644/0755 perms regardless of the
# builder's umask (lintian: non-standard-{file,dir}-perm).
set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE)

# Executables. command8-reaper is only defined when liblo was found.
set(_command8_bins command8-monitor command8-mackie)
set(_command8_mans man/command8-monitor.1 man/command8-mackie.1)
if(LIBLO_FOUND)
  list(APPEND _command8_bins command8-reaper)
  list(APPEND _command8_mans man/command8-reaper.1)
endif()
install(TARGETS ${_command8_bins} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# Debian-style copyright (satisfies lintian's copyright-file requirement; on
# Debian it references /usr/share/common-licenses/GPL-3 rather than duplicating
# the ~35 KB license text in every package's doc dir).
install(FILES debian/copyright
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# Full GPL-3.0 text, shipped in every package + a plain `cmake --install` so
# consumers off Debian (the .tar.gz) get the license even without a system
# /usr/share/common-licenses/GPL-3 to reference.
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

# Compress the changelog and man pages with `gzip -9n` (deterministic, no
# timestamp) at configure time, then install the .gz. These paths don't depend
# on the install prefix, so configure-time generation is fine.
find_program(GZIP_EXECUTABLE gzip REQUIRED)
function(command8_install_gz src destdir renamed)
  get_filename_component(_name "${src}" NAME)
  set(_gz "${CMAKE_CURRENT_BINARY_DIR}/${renamed}")
  add_custom_command(
    OUTPUT "${_gz}"
    COMMAND ${GZIP_EXECUTABLE} -9nc "${CMAKE_CURRENT_SOURCE_DIR}/${src}" > "${_gz}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${src}"
    COMMENT "Compressing ${_name}")
  string(MAKE_C_IDENTIFIER "gz_${renamed}" _tgt)
  add_custom_target(${_tgt} ALL DEPENDS "${_gz}")
  install(FILES "${_gz}" DESTINATION "${destdir}")
endfunction()

# Native-package changelog -> <docdir>/changelog.gz
command8_install_gz(debian/changelog "${CMAKE_INSTALL_DOCDIR}" "changelog.gz")

# Man pages -> <mandir>/man1/*.1.gz
foreach(_man IN LISTS _command8_mans)
  get_filename_component(_mn "${_man}" NAME)
  command8_install_gz("${_man}" "${CMAKE_INSTALL_MANDIR}/man1" "${_mn}.gz")
endforeach()

# systemd *user* unit for the Reaper bridge. Generated at INSTALL time (not
# configure time) so ExecStart tracks the actual install prefix / DESTDIR --
# e.g. /usr for the .deb, /usr/local for a plain `cmake --install`. Only
# meaningful when command8-reaper is built.
if(LIBLO_FOUND)
  # Pre-create the unit dir with the default 0755 perms; otherwise the
  # configure_file below creates it implicitly under the builder's umask (0775).
  install(DIRECTORY DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user)
  # Carry configure-time values into the install script's scope.
  install(CODE "set(COMMAND8_SERVICE_IN \"${CMAKE_CURRENT_SOURCE_DIR}/systemd/command8-reaper.service.in\")")
  install(CODE "set(COMMAND8_BINDIR_REL \"${CMAKE_INSTALL_BINDIR}\")")
  install(CODE "set(COMMAND8_UNITDIR_REL \"${CMAKE_INSTALL_LIBDIR}/systemd/user\")")
  install(CODE [[
    set(COMMAND8_BINDIR "${CMAKE_INSTALL_PREFIX}/${COMMAND8_BINDIR_REL}")
    set(_out "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${COMMAND8_UNITDIR_REL}/command8-reaper.service")
    configure_file("${COMMAND8_SERVICE_IN}" "${_out}" @ONLY)
    # configure_file honours the builder's umask; force 0644 (lintian).
    execute_process(COMMAND chmod 644 "${_out}")
    message(STATUS "Installing: ${_out}")
  ]])
endif()

# --- packaging (CPack: .deb + .tar.gz) ---
set(CPACK_PACKAGE_NAME command8)
# Overridable from the release tag: cmake -B build -DCOMMAND8_VERSION=1.2.3
set(COMMAND8_VERSION "0.1.0" CACHE STRING "Package version (x.y.z)")
set(CPACK_PACKAGE_VERSION "${COMMAND8_VERSION}")
set(CPACK_PACKAGE_CONTACT "Alphonso Mango <alphonso.mango@outlook.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "native C++ engine for the Digidesign Command|8 control surface")
# Hand-written extended description: <=80 col lines, "." = paragraph break, no
# leading spaces (verbatim) -- keeps lintian happy (unlike feeding it README).
set(CPACK_PACKAGE_DESCRIPTION
"command8 is a native C++ userspace engine for the Digidesign Command|8
control surface on Linux. It performs the USB-MIDI wake/keepalive handshake,
decodes buttons, faders, encoders and fader-touch, and drives all device
feedback: LEDs, motor faders, meters, encoder rings and the LCD.
.
It ships a DAW-agnostic loopback demo (command8-monitor), a Reaper OSC bridge
(command8-reaper) and a Mackie Control translator (command8-mackie), plus a
self-healing systemd user service for the Reaper bridge.")
set(CPACK_STRIP_FILES ON)                       # strip binaries (lintian)
set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)          # auto-detect libasound2/liblo deps
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Alphonso Mango <alphonso.mango@outlook.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/alphonsom/command8-linux")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
include(CPack)

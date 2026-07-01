# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(command8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)

add_library(command8 STATIC
  src/protocol.cpp
  src/surface.cpp
  src/feedback.cpp
  src/controller.cpp)
target_include_directories(command8 PUBLIC src ${ALSA_INCLUDE_DIRS})
target_link_libraries(command8 PUBLIC ${ALSA_LIBRARIES})
target_compile_options(command8 PRIVATE -Wall -Wextra)

add_executable(command8-monitor src/main.cpp)
target_link_libraries(command8-monitor PRIVATE command8)
target_compile_options(command8-monitor PRIVATE -Wall -Wextra)

# Reaper front-end (OSC via liblo). Optional: only built if liblo is present.
pkg_check_modules(LIBLO liblo)
if(LIBLO_FOUND)
  add_executable(command8-reaper
    src/reaper/reaper_main.cpp
    src/reaper/reaper_backend.cpp)
  target_include_directories(command8-reaper PRIVATE ${LIBLO_INCLUDE_DIRS})
  target_link_libraries(command8-reaper PRIVATE command8 ${LIBLO_LIBRARIES})
  target_compile_options(command8-reaper PRIVATE -Wall -Wextra)
else()
  message(STATUS "liblo not found - skipping command8-reaper")
endif()

# --- tests (CTest) ---
enable_testing()
add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE command8)
target_compile_options(test_protocol PRIVATE -Wall -Wextra)
add_test(NAME protocol COMMAND test_protocol)

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

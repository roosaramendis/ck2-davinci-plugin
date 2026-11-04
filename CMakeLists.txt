cmake_minimum_required(VERSION 3.20)
project(CorridorKey2OFX LANGUAGES C CXX)

# CorridorKey 2.0 OpenFX plugin for DaVinci Resolve (Free + Studio) on
# Linux, Windows and macOS. Produces CorridorKey2.ofx.bundle.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#   cmake --install build   # copies the bundle to the system OFX folder
#
# Dependencies are fetched at configure time (pinned commits). For offline
# builds pass -DFETCHCONTENT_SOURCE_DIR_OPENFX=... -DFETCHCONTENT_SOURCE_DIR_TINYEXR=...

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")
  if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
  endif()
endif()

include(FetchContent)
FetchContent_Declare(openfx
  GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openfx.git
  GIT_TAG d6aff2e7d4c80c437bb56aea081741c670d707ce)
FetchContent_Declare(tinyexr
  GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
  GIT_TAG 96dabe5a4abdba531c7bc71e891152898419fbd4)
FetchContent_GetProperties(openfx)
if(NOT openfx_POPULATED)
  FetchContent_Populate(openfx)
endif()
FetchContent_GetProperties(tinyexr)
if(NOT tinyexr_POPULATED)
  FetchContent_Populate(tinyexr)
endif()

# OpenFX C++ support library (only the sources, not its own CMake project)
file(GLOB OFX_SUPPORT_SOURCES ${openfx_SOURCE_DIR}/Support/Library/*.cpp)
add_library(OfxSupportLib STATIC ${OFX_SUPPORT_SOURCES})
target_include_directories(OfxSupportLib PUBLIC
  ${openfx_SOURCE_DIR}/include ${openfx_SOURCE_DIR}/Support/include)
if(MSVC)
  target_compile_definitions(OfxSupportLib PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# tinyexr + miniz
add_library(tinyexr_lib STATIC
  ${tinyexr_SOURCE_DIR}/tinyexr.cc
  ${tinyexr_SOURCE_DIR}/deps/miniz/miniz.c)
target_include_directories(tinyexr_lib PUBLIC ${tinyexr_SOURCE_DIR} ${tinyexr_SOURCE_DIR}/deps/miniz)

add_library(CorridorKey2 MODULE
  src/CorridorKeyOFX.cpp
  src/engine.cpp
  src/job.cpp
  src/exr_io.cpp)
target_link_libraries(CorridorKey2 PRIVATE OfxSupportLib tinyexr_lib)
set_target_properties(CorridorKey2 PROPERTIES PREFIX "" SUFFIX ".ofx")
if(UNIX AND NOT APPLE)
  target_link_options(CorridorKey2 PRIVATE -static-libstdc++ -static-libgcc
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map)
endif()
if(MINGW)
  # No libstdc++/libgcc/winpthread DLLs next to the plugin: link them in.
  target_link_options(CorridorKey2 PRIVATE -static -static-libgcc -static-libstdc++)
endif()
if(MSVC)
  set_property(TARGET CorridorKey2 OfxSupportLib tinyexr_lib PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(APPLE)
  target_link_options(CorridorKey2 PRIVATE
    -Wl,-exported_symbol,_OfxGetPlugin -Wl,-exported_symbol,_OfxGetNumberOfPlugins)
endif()

# OFX bundle layout: CorridorKey2.ofx.bundle/Contents/<arch>/CorridorKey2.ofx
if(WIN32)
  set(OFX_ARCH "Win64")
elseif(APPLE)
  set(OFX_ARCH "MacOS")
else()
  set(OFX_ARCH "Linux-x86-64")
endif()
set(BUNDLE_DIR ${CMAKE_BINARY_DIR}/CorridorKey2.ofx.bundle)
add_custom_command(TARGET CorridorKey2 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUNDLE_DIR}/Contents/${OFX_ARCH}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:CorridorKey2> ${BUNDLE_DIR}/Contents/${OFX_ARCH}/
  COMMENT "Assembling CorridorKey2.ofx.bundle")
if(APPLE)
  add_custom_command(TARGET CorridorKey2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist ${BUNDLE_DIR}/Contents/Info.plist)
endif()

if(WIN32)
  set(OFX_INSTALL_DIR "C:/Program Files/Common Files/OFX/Plugins" CACHE PATH "OFX plugin folder")
elseif(APPLE)
  set(OFX_INSTALL_DIR "/Library/OFX/Plugins" CACHE PATH "OFX plugin folder")
else()
  set(OFX_INSTALL_DIR "/usr/OFX/Plugins" CACHE PATH "OFX plugin folder")
endif()
install(DIRECTORY ${BUNDLE_DIR} DESTINATION ${OFX_INSTALL_DIR})

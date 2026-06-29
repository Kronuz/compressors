cmake_minimum_required(VERSION 3.14)
project(compressors LANGUAGES CXX)

include(FetchContent)

# --- Dependency: zlib (deflate backend) ---
# zlib ships a CMake module finder with CMake itself, so find_package(ZLIB) is
# the canonical discovery. On macOS with Homebrew, point CMAKE_PREFIX_PATH at
# $(brew --prefix) so keg-only zlib is found.
find_package(ZLIB REQUIRED)

# --- Dependency: lz4 (lz4 backend) ---
# lz4 does not install a CMake package config in most distributions (Homebrew
# included), so there is no find_package(lz4). Discover the header and the
# library by hand; if neither is present, fall back to fetching the upstream
# source (lz4/lz4) and building its bundled CMake target. The result is exposed
# as an imported target `lz4::lz4` either way.
find_path(LZ4_INCLUDE_DIR lz4.h)
find_library(LZ4_LIBRARY NAMES lz4 liblz4)

if(LZ4_INCLUDE_DIR AND LZ4_LIBRARY)
  message(STATUS "compressors: found system lz4 (${LZ4_LIBRARY})")
  add_library(lz4::lz4 UNKNOWN IMPORTED)
  set_target_properties(lz4::lz4 PROPERTIES
    IMPORTED_LOCATION "${LZ4_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LZ4_INCLUDE_DIR}")
else()
  message(STATUS "compressors: system lz4 not found, fetching lz4/lz4")
  FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG        v1.10.0
    SOURCE_SUBDIR  build/cmake
  )
  FetchContent_MakeAvailable(lz4)
  # Upstream's CMake exports the static target as `lz4_static`; alias it to the
  # name the rest of this file uses.
  if(TARGET lz4_static AND NOT TARGET lz4::lz4)
    add_library(lz4::lz4 ALIAS lz4_static)
  endif()
endif()

# --- Dependency: zstd (zstd backend, the value-add) ---
# zstd does ship a CMake package config (zstdConfig.cmake), so prefer
# find_package. Homebrew installs it under $(brew --prefix zstd)/lib/cmake/zstd.
# Fall back to find_library + FetchContent of facebook/zstd if the config is
# absent. The imported target is normalized to `zstd::libzstd`.
find_package(zstd QUIET CONFIG)
if(zstd_FOUND)
  message(STATUS "compressors: found zstd via CMake config")
  if(TARGET zstd::libzstd_shared AND NOT TARGET zstd::libzstd)
    add_library(zstd::libzstd ALIAS zstd::libzstd_shared)
  elseif(TARGET zstd::libzstd_static AND NOT TARGET zstd::libzstd)
    add_library(zstd::libzstd ALIAS zstd::libzstd_static)
  endif()
else()
  find_path(ZSTD_INCLUDE_DIR zstd.h)
  find_library(ZSTD_LIBRARY NAMES zstd libzstd)
  if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
    message(STATUS "compressors: found system zstd (${ZSTD_LIBRARY})")
    add_library(zstd::libzstd UNKNOWN IMPORTED)
    set_target_properties(zstd::libzstd PROPERTIES
      IMPORTED_LOCATION "${ZSTD_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}")
  else()
    message(STATUS "compressors: system zstd not found, fetching facebook/zstd")
    FetchContent_Declare(
      zstd
      GIT_REPOSITORY https://github.com/facebook/zstd.git
      GIT_TAG        v1.5.7
      SOURCE_SUBDIR  build/cmake
    )
    FetchContent_MakeAvailable(zstd)
    if(TARGET libzstd_static AND NOT TARGET zstd::libzstd)
      add_library(zstd::libzstd ALIAS libzstd_static)
    elseif(TARGET libzstd_shared AND NOT TARGET zstd::libzstd)
      add_library(zstd::libzstd ALIAS libzstd_shared)
    endif()
  endif()
endif()

# --- Library target ---
# A small static library carrying the three backends' .cc files plus the public
# headers and vendored utils. The include dir is the repo root, so a consumer
# includes "compressor_deflate.h" / "compressor_lz4.h" / "compressor_zstd.h".
add_library(compressors STATIC
  compressor_deflate.cc
  compressor_lz4.cc
  compressor_zstd.cc)
target_include_directories(compressors PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(compressors PUBLIC cxx_std_20)
target_link_libraries(compressors PUBLIC ZLIB::ZLIB lz4::lz4 zstd::libzstd)

# Alias so consumers can link `compressors::compressors` whether they add this
# via add_subdirectory or FetchContent.
add_library(compressors::compressors ALIAS compressors)

# Only build the round-trip test when this repo is the top-level project, so
# consumers (e.g. via FetchContent) don't build our checks.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_executable(compressors_test test/test.cc)
  target_link_libraries(compressors_test PRIVATE compressors)
  add_test(NAME compressors COMMAND compressors_test)
endif()

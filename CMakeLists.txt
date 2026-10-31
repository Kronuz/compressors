cmake_minimum_required(VERSION 3.14)
project(compressors LANGUAGES CXX)

include(FetchContent)

# Each backend can be left out, with its dependency, by consumers that need only some.
option(COMPRESSORS_DEFLATE "Build the deflate backend (zlib)" ON)
option(COMPRESSORS_LZ4 "Build the LZ4 backend" ON)
option(COMPRESSORS_ZSTD "Build the Zstandard backend" ON)
# Different zstd versions may compress the same input to different (valid) bytes, so a
# consumer that needs byte-identical output on every platform builds the pinned release.
option(COMPRESSORS_ZSTD_PINNED "Build zstd from its pinned release, ignoring the system's" OFF)
# The same for LZ4, whose versions may also compress one input to different valid bytes.
option(COMPRESSORS_LZ4_PINNED "Build LZ4 from its pinned release, ignoring the system's" OFF)
if(NOT COMPRESSORS_DEFLATE AND NOT COMPRESSORS_LZ4 AND NOT COMPRESSORS_ZSTD)
  message(FATAL_ERROR "compressors: enable at least one of COMPRESSORS_DEFLATE, COMPRESSORS_LZ4, COMPRESSORS_ZSTD")
endif()

# --- Dependency: zlib (deflate backend) ---
# zlib ships a CMake module finder with CMake itself, so find_package(ZLIB) is
# the canonical discovery. On macOS with Homebrew, point CMAKE_PREFIX_PATH at
# $(brew --prefix) so keg-only zlib is found.
if(COMPRESSORS_DEFLATE)
  find_package(ZLIB REQUIRED)
endif()

# --- Dependency: lz4 (lz4 backend) ---
# lz4 does not install a CMake package config in most distributions (Homebrew
# included), so there is no find_package(lz4). Discover the header and the
# library by hand; if neither is present, fall back to fetching the upstream
# source (lz4/lz4) and building its bundled CMake target, as COMPRESSORS_LZ4_PINNED
# always does. The result is exposed as an imported target `lz4::lz4` either way.
if(COMPRESSORS_LZ4)
if(NOT COMPRESSORS_LZ4_PINNED)
  find_path(LZ4_INCLUDE_DIR lz4.h)
  find_library(LZ4_LIBRARY NAMES lz4 liblz4)
endif()

if(NOT COMPRESSORS_LZ4_PINNED AND LZ4_INCLUDE_DIR AND LZ4_LIBRARY)
  message(STATUS "compressors: found system lz4 (${LZ4_LIBRARY})")
  add_library(lz4::lz4 UNKNOWN IMPORTED)
  set_target_properties(lz4::lz4 PROPERTIES
    IMPORTED_LOCATION "${LZ4_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LZ4_INCLUDE_DIR}")
else()
  message(STATUS "compressors: fetching lz4/lz4 v1.10.0")
  # Only the static library: no command-line programs.
  set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
  set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG        ebb370ca83af193212df4dcbadcc5d87bc0de2f0  # v1.10.0, by commit: a moved tag cannot change the decoders
    SOURCE_SUBDIR  build/cmake
  )
  FetchContent_MakeAvailable(lz4)
  # Upstream's CMake exports the static target as `lz4_static`; alias it to the
  # name the rest of this file uses.
  if(TARGET lz4_static AND NOT TARGET lz4::lz4)
    add_library(lz4::lz4 ALIAS lz4_static)
  endif()
endif()
endif()

# --- Dependency: zstd (zstd backend, the value-add) ---
# zstd does ship a CMake package config (zstdConfig.cmake), so prefer
# find_package. Homebrew installs it under $(brew --prefix zstd)/lib/cmake/zstd.
# Fall back to find_library + FetchContent of facebook/zstd if the config is
# absent, and fetch it always under COMPRESSORS_ZSTD_PINNED. The imported target is
# normalized to `zstd::libzstd`.
if(COMPRESSORS_ZSTD)
if(NOT COMPRESSORS_ZSTD_PINNED)
  find_package(zstd QUIET CONFIG)
  if(NOT zstd_FOUND)
    find_path(ZSTD_INCLUDE_DIR zstd.h)
    find_library(ZSTD_LIBRARY NAMES zstd libzstd)
  endif()
endif()
if(zstd_FOUND)
  message(STATUS "compressors: found zstd via CMake config")
  if(TARGET zstd::libzstd_shared AND NOT TARGET zstd::libzstd)
    add_library(zstd::libzstd ALIAS zstd::libzstd_shared)
  elseif(TARGET zstd::libzstd_static AND NOT TARGET zstd::libzstd)
    add_library(zstd::libzstd ALIAS zstd::libzstd_static)
  endif()
else()
  if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
    message(STATUS "compressors: found system zstd (${ZSTD_LIBRARY})")
    add_library(zstd::libzstd UNKNOWN IMPORTED)
    set_target_properties(zstd::libzstd PROPERTIES
      IMPORTED_LOCATION "${ZSTD_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}")
  else()
    message(STATUS "compressors: fetching facebook/zstd v1.5.7")
    FetchContent_Declare(
      zstd
      GIT_REPOSITORY https://github.com/facebook/zstd.git
      GIT_TAG        f8745da6ff1ad1e7bab384bd1f9d742439278e99  # v1.5.7, by commit
      SOURCE_SUBDIR  build/cmake
    )
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zstd)
    if(TARGET libzstd_static AND NOT TARGET zstd::libzstd)
      add_library(zstd::libzstd ALIAS libzstd_static)
    elseif(TARGET libzstd_shared AND NOT TARGET zstd::libzstd)
      add_library(zstd::libzstd ALIAS libzstd_shared)
    endif()
  endif()
endif()
endif()

# --- Library target ---
# A small static library carrying the three backends' .cc files plus the public
# headers and vendored utils. The include dir is the repo root, so a consumer
# includes "compressor_deflate.h" / "compressor_lz4.h" / "compressor_zstd.h".
add_library(compressors STATIC)
if(COMPRESSORS_DEFLATE)
  target_sources(compressors PRIVATE compressor_deflate.cc)
  target_link_libraries(compressors PUBLIC ZLIB::ZLIB)
endif()
if(COMPRESSORS_LZ4)
  target_sources(compressors PRIVATE compressor_lz4.cc)
  target_link_libraries(compressors PUBLIC lz4::lz4)
endif()
if(COMPRESSORS_ZSTD)
  target_sources(compressors PRIVATE compressor_zstd.cc)
  target_link_libraries(compressors PUBLIC zstd::libzstd)
endif()
target_include_directories(compressors PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(compressors PUBLIC cxx_std_20)

# Alias so consumers can link `compressors::compressors` whether they add this
# via add_subdirectory or FetchContent.
add_library(compressors::compressors ALIAS compressors)

# Only build the checks when this repo is the top-level project, so consumers (e.g. via
# FetchContent) don't build them. The round-trip test covers whichever backends are built;
# the example dispatches over all three.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  find_package(Threads REQUIRED)
  add_executable(compressors_test test/test.cc)
  target_link_libraries(compressors_test PRIVATE compressors Threads::Threads)
  target_compile_definitions(compressors_test PRIVATE
    COMPRESSORS_TEST_DEFLATE=$<BOOL:${COMPRESSORS_DEFLATE}>
    COMPRESSORS_TEST_LZ4=$<BOOL:${COMPRESSORS_LZ4}>
    COMPRESSORS_TEST_ZSTD=$<BOOL:${COMPRESSORS_ZSTD}>)
  add_test(NAME compressors COMMAND compressors_test)

  # Damaged compressed input: every decoder decodes it or throws its own error.
  add_executable(compressors_corrupt_test test/test_corrupt.cc)
  target_link_libraries(compressors_corrupt_test PRIVATE compressors)
  target_compile_definitions(compressors_corrupt_test PRIVATE
    COMPRESSORS_TEST_DEFLATE=$<BOOL:${COMPRESSORS_DEFLATE}>
    COMPRESSORS_TEST_LZ4=$<BOOL:${COMPRESSORS_LZ4}>
    COMPRESSORS_TEST_ZSTD=$<BOOL:${COMPRESSORS_ZSTD}>)
  add_test(NAME compressors_corrupt COMMAND compressors_corrupt_test)

  # The one-shot lz4 helpers against the stream classes: identical bytes, identical verdicts.
  if(COMPRESSORS_LZ4)
    add_executable(compressors_lz4_oneshot_test test/test_lz4_oneshot.cc)
    target_link_libraries(compressors_lz4_oneshot_test PRIVATE compressors Threads::Threads)
    add_test(NAME compressors_lz4_oneshot COMMAND compressors_lz4_oneshot_test)
  endif()

  # Benchmarks (not built by default): cmake -DCOMPRESSORS_BENCH=ON. The XXH32 one compares with
  # upstream xxHash, fetched here only; the library never depends on it.
  option(COMPRESSORS_BENCH "Build the benchmarks" OFF)
  if(COMPRESSORS_BENCH)
    if(COMPRESSORS_LZ4)
      add_executable(compressors_bench_lz4 bench/bench_lz4.cc)
      target_link_libraries(compressors_bench_lz4 PRIVATE compressors)
    endif()
    FetchContent_Declare(xxhash_upstream GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git GIT_TAG v0.8.3)
    FetchContent_GetProperties(xxhash_upstream)
    if(NOT xxhash_upstream_POPULATED)
      FetchContent_Populate(xxhash_upstream)
    endif()
    add_executable(compressors_bench_xxh32 bench/bench_xxh32.cc)
    target_link_libraries(compressors_bench_xxh32 PRIVATE compressors)
    target_include_directories(compressors_bench_xxh32 PRIVATE "${xxhash_upstream_SOURCE_DIR}")
  endif()

  # libFuzzer targets, one per decoder (Clang): cmake -DCOMPRESSORS_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
  # Without it a replay driver runs the seed corpus and generated inputs through the same checks as a test.
  option(COMPRESSORS_FUZZ "Build the libFuzzer targets" OFF)
  foreach(codec lz4 zstd deflate)
    string(TOUPPER ${codec} CODEC_UPPER)
    if(COMPRESSORS_${CODEC_UPPER})
      if(COMPRESSORS_FUZZ)
        add_executable(compressors_fuzz_${codec} fuzz/fuzz_${codec}.cc)
        target_link_libraries(compressors_fuzz_${codec} PRIVATE compressors)
        target_compile_options(compressors_fuzz_${codec} PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
        target_link_options(compressors_fuzz_${codec} PRIVATE -fsanitize=fuzzer,address,undefined)
      else()
        add_executable(compressors_fuzz_${codec}_replay fuzz/fuzz_${codec}.cc fuzz/replay.cc)
        target_link_libraries(compressors_fuzz_${codec}_replay PRIVATE compressors)
        add_test(NAME compressors_fuzz_${codec}_replay
          COMMAND compressors_fuzz_${codec}_replay ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/${codec})
      endif()
    endif()
  endforeach()
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND COMPRESSORS_DEFLATE AND COMPRESSORS_LZ4 AND COMPRESSORS_ZSTD)

  # A realistic usage example that mirrors Xapiand's one-shot helpers, block
  # streaming classes, and LZ4-default codec dispatch. It self-checks and is
  # ALSO registered as a ctest test.
  add_executable(compressors_roundtrip examples/roundtrip.cc)
  target_link_libraries(compressors_roundtrip PRIVATE compressors)
  add_test(NAME compressors_roundtrip COMMAND compressors_roundtrip)
endif()

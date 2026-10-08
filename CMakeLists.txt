cmake_minimum_required(VERSION 3.20)
project(InputLine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(INPUTLINE_BUILD_HOST "Build inputline-host and inputline-sim" ON)
option(INPUTLINE_BUILD_TESTS "Build unit tests" ON)
option(INPUTLINE_FUZZ "Build the libFuzzer targets in fuzz/ (clang only)" OFF)
option(INPUTLINE_CROSS_CHECK "Also test the crypto against libsodium (a test only; needs libsodium)" OFF)
set(INPUTLINE_VERSION "${PROJECT_VERSION}" CACHE STRING "Version the programs report (release builds set it)")

if(MSVC)
  set(INPUTLINE_WARNINGS /W4 /permissive-)
else()
  set(INPUTLINE_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(INPUTLINE_BUILD_TESTS)
  enable_testing()
endif()

if(INPUTLINE_FUZZ)
  # Instrument everything, so the fuzzers see coverage inside InputLine's code.
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(core)
if(INPUTLINE_BUILD_HOST)
  add_subdirectory(host)
endif()

if(INPUTLINE_FUZZ)
  add_subdirectory(fuzz)
endif()

# InputLine itself never uses libsodium: only this test does, to cross-check core/'s crypto.
if(INPUTLINE_BUILD_TESTS AND INPUTLINE_CROSS_CHECK)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
  add_executable(inputline_crypto_cross_check tests/crypto_cross_check.cpp)
  target_link_libraries(inputline_crypto_cross_check PRIVATE inputline_core PkgConfig::SODIUM)
  target_compile_options(inputline_crypto_cross_check PRIVATE ${INPUTLINE_WARNINGS})
  add_test(NAME inputline_crypto_cross_check COMMAND inputline_crypto_cross_check)
endif()

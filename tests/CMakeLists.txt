# Standalone unit tests for the pure (OBS-free) engine modules.
# Independent of the plugin build: configure + build from this folder, e.g.
#   cmake -S tests -B tests/build
#   cmake --build tests/build --config Release
#   ./tests/build/Release/dse_tests
cmake_minimum_required(VERSION 3.16)
project(dse_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(dse_tests test_pure.cpp)

# stubs/ provides an empty <obs.h>; ../src for the module headers.
target_include_directories(dse_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src")

enable_testing()
add_test(NAME pure_modules COMMAND dse_tests)

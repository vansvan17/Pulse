# Pulse — epoll HTTP server build
#
# Targets:
#   pulse              — release binary, optimized
#   pulse_asan         — debug binary with ASan + UBSan for catching bugs early
#   tests              — unit tests via the simple harness in tests/
#
# Why CMake and not just Make:
#   CMake is what most C++ projects an LFX mentor will look at use. Also
#   makes it trivial to swap in libraries (picohttpparser, liburing) later.

cmake_minimum_required(VERSION 3.16)
project(pulse CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Warnings: be strict. Mentors will read every line; let the compiler help.
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Wcast-align
    -Wunused -Woverloaded-virtual -Wconversion
    -Wsign-conversion -Wnull-dereference
    -Wdouble-promotion -Wformat=2
)

# Release-only optimization knobs.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -fno-omit-frame-pointer)
endif()

include_directories(include)

# Core library — everything except main(). Lets tests link against it.
add_library(pulse_core STATIC
    src/core/event_loop.cpp
    src/http/http_parser.cpp
)
target_include_directories(pulse_core PUBLIC include)

# Release binary
add_executable(pulse src/main.cpp)
target_link_libraries(pulse PRIVATE pulse_core)

# ASan + UBSan build for development. Build with:
#   cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build-asan --target pulse_asan
add_executable(pulse_asan EXCLUDE_FROM_ALL src/main.cpp)
target_link_libraries(pulse_asan PRIVATE pulse_core)
target_compile_options(pulse_asan PRIVATE
    -O1 -g -fno-omit-frame-pointer
    -fsanitize=address,undefined
)
target_link_options(pulse_asan PRIVATE -fsanitize=address,undefined)

# Tests — light handwritten harness, no gtest dependency. Keeps the project
# self-contained for reviewers who don't want to install anything.
enable_testing()
add_executable(test_buffer       tests/test_buffer.cpp)
add_executable(test_http_parser  tests/test_http_parser.cpp)
add_executable(test_timer_queue  tests/test_timer_queue.cpp)
target_link_libraries(test_buffer       PRIVATE pulse_core)
target_link_libraries(test_http_parser  PRIVATE pulse_core)
target_link_libraries(test_timer_queue  PRIVATE pulse_core)
add_test(NAME buffer       COMMAND test_buffer)
add_test(NAME http_parser  COMMAND test_http_parser)
add_test(NAME timer_queue  COMMAND test_timer_queue)

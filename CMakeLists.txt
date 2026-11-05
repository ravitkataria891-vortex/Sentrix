cmake_minimum_required(VERSION 3.16)
project(Sentrix LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Debug, Release, RelWithDebInfo)" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT sentrix_ipo_supported OUTPUT sentrix_ipo_error)

add_library(sentrix_core
    src/hash_utils.cpp
)
target_include_directories(sentrix_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(sentrix src/main.cpp)
target_link_libraries(sentrix PRIVATE sentrix_core)

add_executable(sentrix_benchmark src/benchmark.cpp)
target_link_libraries(sentrix_benchmark PRIVATE sentrix_core)
if(WIN32)
    target_link_libraries(sentrix_benchmark PRIVATE psapi)
endif()

add_executable(sentrix_tests tests/test_sentrix.cpp)
target_link_libraries(sentrix_tests PRIVATE sentrix_core)

if(sentrix_ipo_supported AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET sentrix_core sentrix sentrix_benchmark sentrix_tests
                 PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

enable_testing()
add_test(NAME sentrix_tests COMMAND sentrix_tests)

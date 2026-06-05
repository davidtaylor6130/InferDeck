cmake_minimum_required(VERSION 3.27)
project(parity LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SOURCES
    src/compare.cpp
    src/prompts.cpp
    src/runner.cpp
)

add_library(parity STATIC ${SOURCES})

target_include_directories(parity PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(MSVC)
    target_compile_options(parity PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(parity PRIVATE -Wall -Wextra)
endif()

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

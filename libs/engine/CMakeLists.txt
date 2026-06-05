cmake_minimum_required(VERSION 3.27)
project(engine LANGUAGES CXX)

set(ENGINE_SOURCES
    src/slot_pool.cpp
)

add_library(engine STATIC ${ENGINE_SOURCES})

target_include_directories(engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(engine
    PUBLIC
        foundation
)

set_target_properties(engine PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(engine PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

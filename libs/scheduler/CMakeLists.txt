cmake_minimum_required(VERSION 3.27)
project(scheduler LANGUAGES CXX)

set(SCHEDULER_SOURCES
    src/scheduler.cpp
)

add_library(scheduler STATIC ${SCHEDULER_SOURCES})

target_include_directories(scheduler
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(scheduler
    PUBLIC
        foundation
        model
        engine
)

set_target_properties(scheduler PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(scheduler PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(scheduler PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

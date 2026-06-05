cmake_minimum_required(VERSION 3.27)
project(model LANGUAGES CXX)

set(MODEL_SOURCES
    src/model_registry.cpp
    src/backend_coordinator.cpp
)

add_library(model STATIC ${MODEL_SOURCES})

target_include_directories(model
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(model
    PUBLIC
        foundation
)

set_target_properties(model PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(model PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(model PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

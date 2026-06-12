cmake_minimum_required(VERSION 3.27)
project(gateway LANGUAGES CXX)

set(GATEWAY_SOURCES
    src/streaming_sanitizer.cpp
    src/routes.cpp
    src/anthropic_routes.cpp
    src/dashboard_routes.cpp
    src/metrics_builder.cpp
)

add_library(gateway STATIC ${GATEWAY_SOURCES})

target_include_directories(gateway
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(gateway
    PUBLIC
        foundation
        model
        observability
        nlohmann_json::nlohmann_json
        httplib::httplib
)

set_target_properties(gateway PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(gateway PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(gateway PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

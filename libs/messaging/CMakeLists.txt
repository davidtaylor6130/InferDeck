cmake_minimum_required(VERSION 3.27)
project(messaging LANGUAGES CXX)

add_library(messaging STATIC
    src/conversions.cpp
)

target_include_directories(messaging PUBLIC
    include
)

target_link_libraries(messaging PUBLIC
    foundation
    nlohmann_json::nlohmann_json
)

set_target_properties(messaging PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(messaging PRIVATE /W4 /utf-8 /permissive- /FS)
else()
    target_compile_options(messaging PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

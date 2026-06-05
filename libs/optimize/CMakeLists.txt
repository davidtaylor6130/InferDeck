cmake_minimum_required(VERSION 3.27)
project(optimize LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(yaml-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(optimize STATIC
    src/search_space.cpp
    src/study.cpp
    src/yaml_loader.cpp
)

target_include_directories(optimize PUBLIC include)
target_link_libraries(optimize
    PUBLIC
        nlohmann_json::nlohmann_json
        yaml-cpp::yaml-cpp
)

if(MSVC)
    target_compile_options(optimize PRIVATE /W4 /utf-8 /FS)
    target_compile_definitions(optimize PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(optimize PRIVATE -Wall -Wextra)
endif()

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

cmake_minimum_required(VERSION 3.27)
project(observability LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(observability STATIC
    src/logger.cpp
    src/metrics.cpp
    src/gpu_telemetry.cpp
    src/stats_db.cpp
)

target_include_directories(observability PUBLIC include)

find_package(Threads REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
target_link_libraries(observability PUBLIC Threads::Threads unofficial::sqlite3::sqlite3)

if(MSVC)
    target_compile_options(observability PRIVATE /W4 /utf-8)
    target_compile_definitions(observability PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(observability PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

cmake_minimum_required(VERSION 3.27)
project(gateway LANGUAGES CXX)

set(GATEWAY_SOURCES
    src/streaming_sanitizer.cpp
    src/generation_session.cpp
    src/routes.cpp
    src/openai_adapter.cpp
    src/responses_adapter.cpp
    src/openai_error.cpp
    src/openai_routes.cpp
    src/dashboard_routes.cpp
    src/metrics_builder.cpp
    src/model_store.cpp
    src/audio_decoder.cpp
    src/media_routes.cpp
    src/profile_benchmark.cpp
    src/profile_benchmark_scheduler.cpp
    src/config_repository.cpp
    src/config_secrets.cpp
)

add_library(gateway STATIC ${GATEWAY_SOURCES})

target_include_directories(gateway
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/libs/third_party/llama.cpp/vendor
)

target_link_libraries(gateway
    PUBLIC
        foundation
        model
        observability
        optimize
        nlohmann_json::nlohmann_json
        httplib::httplib
    PRIVATE
        yaml-cpp::yaml-cpp
)

if(WIN32)
    target_link_libraries(gateway PRIVATE winhttp bcrypt)
endif()

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

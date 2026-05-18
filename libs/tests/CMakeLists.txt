# libs/tests CMakeLists.txt
cmake_minimum_required(VERSION 3.27)
project(inferdeck-tests LANGUAGES CXX)

add_executable(inferdeck-tests
    test_logger.cpp
    test_metrics.cpp
    test_config.cpp
    test_llama_engine.cpp
    test_vulkan_device.cpp
    test_gguf_parser.cpp
    HealthTest.cpp
    ModelsTest.cpp
    MetricsTest.cpp
    ChatCompletionsTest.cpp
    CompletionsTest.cpp
    EmbeddingsTest.cpp
    benchmarks.cpp
)

target_include_directories(inferdeck-tests PRIVATE
    ../core/include
    ../llama_cpp_wrapper/include
    ../../apps/gateway-service/src
    ../../apps/gateway-service/src/routes
)

target_link_libraries(inferdeck-tests PRIVATE
    core
    llama_cpp_wrapper
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    cpp-httplib::httplib
    spdlog::spdlog
    fmt::fmt
)

target_compile_options(inferdeck-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    --coverage
)

# Add test discovery
include(CTest)
include(Catch2)
catch_discover_tests(inferdeck-tests)

# Coverage target
add_custom_target(coverage
    COMMAND lcov --capture --directory . --output-file coverage.info
    COMMAND genhtml coverage.info --output-directory coverage_html
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

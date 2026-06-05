# libs/tests CMakeLists.txt
cmake_minimum_required(VERSION 3.27)
project(inferdeck-tests LANGUAGES CXX)

add_executable(inferdeck-dashboard-tests
    DashboardRoutesTest.cpp
)

target_include_directories(inferdeck-dashboard-tests PRIVATE
    .
    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
)

target_link_libraries(inferdeck-dashboard-tests PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(inferdeck-dashboard-tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(inferdeck-dashboard-tests PRIVATE -Wall -Wextra -Wpedantic --coverage)
endif()

catch_discover_tests(inferdeck-dashboard-tests)

add_executable(inferdeck-whisper-tests
    AudioTranscriptionsTest.cpp
    ../../apps/gateway-service/src/WhisperRuntime.cpp
    ../../apps/gateway-service/src/routes/AudioTranscriptions.cpp
)

target_include_directories(inferdeck-whisper-tests PRIVATE
    .
    ../core/include
    ../../apps/gateway-service/src
    ../../apps/gateway-service/src/routes
    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
)

target_link_libraries(inferdeck-whisper-tests PRIVATE
    core
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    httplib::httplib
    spdlog::spdlog
    fmt::fmt
)

if(MSVC)
    target_compile_options(inferdeck-whisper-tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(inferdeck-whisper-tests PRIVATE -Wall -Wextra -Wpedantic --coverage)
endif()

catch_discover_tests(inferdeck-whisper-tests)

# Coverage target
add_custom_target(coverage
    COMMAND lcov --capture --directory . --output-file coverage.info
    COMMAND genhtml coverage.info --output-directory coverage_html
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

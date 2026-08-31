if(NOT DEFINED OUTPUT OR NOT DEFINED SOURCE_DIR OR NOT DEFINED VERSION)
    message(FATAL_ERROR "OUTPUT, SOURCE_DIR, and VERSION are required")
endif()

set(_revision "${REVISION_OVERRIDE}")
set(_dirty "false")
if(NOT _revision)
    find_program(_git_executable git)
    if(NOT _git_executable)
        message(FATAL_ERROR "Git is required unless INFERDECK_BUILD_REVISION is set")
    endif()
    execute_process(
        COMMAND "${_git_executable}" -C "${SOURCE_DIR}" rev-parse HEAD
        RESULT_VARIABLE _revision_result
        OUTPUT_VARIABLE _revision
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _revision_result EQUAL 0)
        message(FATAL_ERROR "Unable to resolve the InferDeck source revision")
    endif()
    execute_process(
        COMMAND "${_git_executable}" -C "${SOURCE_DIR}" status --porcelain --untracked-files=no
        RESULT_VARIABLE _status_result
        OUTPUT_VARIABLE _status
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _status_result EQUAL 0)
        message(FATAL_ERROR "Unable to determine whether the InferDeck source tree is dirty")
    endif()
    if(_status)
        set(_dirty "true")
    endif()
endif()

string(TOLOWER "${_revision}" _revision)
string(LENGTH "${_revision}" _revision_length)
if(NOT _revision MATCHES "^[0-9a-f]+$" OR NOT _revision_length EQUAL 40)
    message(FATAL_ERROR "InferDeck build revision must be a full 40-character Git SHA")
endif()

set(_content "#pragma once\n\n#include <string_view>\n\nnamespace inferdeck::app {\n\ninline constexpr std::string_view build_version = \"${VERSION}\";\ninline constexpr std::string_view build_revision = \"${_revision}\";\ninline constexpr bool build_dirty = ${_dirty};\n\n}\n")
get_filename_component(_output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_output_directory}")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" _existing)
endif()
if(NOT _existing STREQUAL _content)
    file(WRITE "${OUTPUT}" "${_content}")
endif()

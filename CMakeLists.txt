cmake_minimum_required(VERSION 3.16.0)

# The firmware version ends up in the app description, where it is reported in
# BLE telemetry and compared against during an OTA update. CI sets FW_VERSION
# to the release tag; otherwise it comes from git. esp_app_desc_t holds at most
# 31 characters.
if(DEFINED ENV{FW_VERSION} AND NOT "$ENV{FW_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{FW_VERSION}")
else()
    execute_process(
        COMMAND git describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE PROJECT_VER
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE git_result)
    if(NOT git_result EQUAL 0 OR PROJECT_VER STREQUAL "")
        set(PROJECT_VER "unknown")
    endif()
endif()
string(SUBSTRING "${PROJECT_VER}" 0 31 PROJECT_VER)
message(STATUS "Firmware version: ${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(temp)

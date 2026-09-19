# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/include
    REQUIRES bt nvs_flash esp_wifi mqtt driver esp_event json
             app_update esp_app_format esp_http_client esp_https_ota mbedtls
)

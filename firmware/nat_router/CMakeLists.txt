# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.4.17")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# NomadLink divergence from upstream: the IDF project name is what idf.py names the
# output binary after. Upstream calls this esp32_nat_router, which made the file the
# web installer flashes read "esp32_nat_router.bin" — the wrong product on our own
# installer. Renaming the project renames the artifact to nomadlink.bin. This is the
# only identity change made to the base; behaviour is untouched.
project(nomadlink)

# Force esp_app_desc.c to recompile on every build so that
# __DATE__/__TIME__ in the app descriptor stay current.
set(APP_DESC_OBJ "${CMAKE_BINARY_DIR}/esp-idf/esp_app_format/CMakeFiles/__idf_esp_app_format.dir/esp_app_desc.c.obj")
add_custom_target(force_app_desc_rebuild ALL
    COMMAND ${CMAKE_COMMAND} -E remove -f "${APP_DESC_OBJ}"
    COMMENT "Ensuring app descriptor timestamp is current"
)
add_dependencies(${PROJECT_NAME}.elf force_app_desc_rebuild)

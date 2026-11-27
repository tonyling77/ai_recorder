cmake_minimum_required(VERSION 3.16)
set(MOSAICO_UTILS_ROOT "${CMAKE_CURRENT_LIST_DIR}/submodule/esp-mosaico-utils")
set(MOSAICO_BSP_ROOT "${CMAKE_CURRENT_LIST_DIR}/submodule/esp-mosaico-bsp")
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()
if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.application.defaults")
endif()
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_compiler.cmake")
list(APPEND EXTRA_COMPONENT_DIRS
    "${MOSAICO_UTILS_ROOT}/esp-mosaico-vibe-mode/components/esp_mosaico_app_recovery"
    "${MOSAICO_UTILS_ROOT}/mosaico-tools/components/esp_mosaico_gsp_bundle"
    "${MOSAICO_UTILS_ROOT}/mosaico-tools/components/esp_mosaico_gsp_iris")
include("${MOSAICO_UTILS_ROOT}/esp-mosaico-vibe-mode/cmake/mosaico_idf_project.cmake")

# The path of the build machine does not belong in a released image. A log
# line that carries __FILE__ would otherwise put the home directory of
# whoever built it into the firmware that everybody else downloads, and the
# image would differ from one build directory to the next.
#
# The option is set before project() registers the components, and it is
# appended so that the options of the tools stay.
get_filename_component(MOSAICO_WORKSPACE_ROOT "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
idf_build_set_property(COMPILE_OPTIONS
    "-fmacro-prefix-map=${MOSAICO_WORKSPACE_ROOT}=."
    "-fmacro-prefix-map=${CMAKE_CURRENT_LIST_DIR}=." APPEND)

project(ai_recorder VERSION 1.0.0)

# The RainMaker Neo firmware is a sibling checkout rather than a registry
# component, so nothing pins it and a build would take whatever the checkout
# happens to hold. The commit decides what the board talks to, so the build
# refuses a checkout that is not the one this application was made against.
set(MOSAICO_NEO_FIRMWARE "${CMAKE_CURRENT_LIST_DIR}/submodule/esp-rainmaker-neo-firmware")
set(MOSAICO_NEO_FIRMWARE_COMMIT "e222db454bdbf0aeb630cd0374287de6df78efc7")
execute_process(
    COMMAND git -C "${MOSAICO_NEO_FIRMWARE}" rev-parse HEAD
    OUTPUT_VARIABLE mosaico_neo_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE mosaico_neo_result
    ERROR_QUIET)
if(NOT mosaico_neo_result EQUAL 0)
    message(FATAL_ERROR
        "The RainMaker Neo firmware is not at ${MOSAICO_NEO_FIRMWARE}. "
        "Clone it next to the workspace: "
        "git clone https://github.com/espressif/esp-rainmaker-neo-firmware.git "
        "${MOSAICO_NEO_FIRMWARE}")
endif()
if(NOT mosaico_neo_commit STREQUAL MOSAICO_NEO_FIRMWARE_COMMIT)
    message(FATAL_ERROR
        "The RainMaker Neo firmware is at ${mosaico_neo_commit}, but this "
        "application was made against ${MOSAICO_NEO_FIRMWARE_COMMIT}. Take the "
        "one it was made against:\n"
        "  git -C ${MOSAICO_NEO_FIRMWARE} fetch origin\n"
        "  git -C ${MOSAICO_NEO_FIRMWARE} checkout ${MOSAICO_NEO_FIRMWARE_COMMIT}")
endif()

include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/system_update.cmake")

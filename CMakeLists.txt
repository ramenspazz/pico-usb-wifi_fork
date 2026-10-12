# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(pico-usb-wifi C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(pico-usb-wifi
    src/main.c
    src/config.c
    src/config_proto.c
    src/serial_console.c
    src/wifi_scan.c
    src/debug_console.c
    src/usb_network.c
    src/usb_descriptors.c
)

# Firmware version: single source of truth is the git tag.
# CI passes -DFW_VERSION=$CI_COMMIT_TAG; local builds derive it from git describe;
# the fallback keeps a bare source tree (no .git, no tags) building.
if(NOT DEFINED FW_VERSION)
  execute_process(
    COMMAND git describe --tags --dirty --always
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE FW_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()
if(NOT FW_VERSION)
  set(FW_VERSION "0.0.0-dev")
endif()
message(STATUS "Firmware version: ${FW_VERSION}")
target_compile_definitions(pico-usb-wifi PRIVATE FW_VERSION="${FW_VERSION}")

pico_set_program_name(pico-usb-wifi "pico-usb-wifi")
pico_set_program_version(pico-usb-wifi "${FW_VERSION}")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(pico-usb-wifi 1)
pico_enable_stdio_usb(pico-usb-wifi 0)

# Add libraries to the build
target_link_libraries(pico-usb-wifi
    pico_stdlib
    tinyusb_device
    pico_cyw43_arch_lwip_threadsafe_background   # Wi-Fi serviced in background IRQ so USB (tud_task) never starves
    pico_unique_id
    pico_flash          # flash_safe_execute() for persisting runtime config
    hardware_flash      # flash_range_erase/program
    hardware_watchdog   # main-loop hang -> auto-reset + recovery telemetry
)

# Add include directories to the build
target_include_directories(pico-usb-wifi PRIVATE
    ${PICO_TINYUSB_PATH}/lib/networking # for rndis_protocol.h
    ${CMAKE_CURRENT_LIST_DIR}/src       # project headers, tusb_config.h, lwipopts.h, wifi_config.h
)

pico_add_extra_outputs(pico-usb-wifi)

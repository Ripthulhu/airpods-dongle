cmake_minimum_required(VERSION 3.17)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PICOTOOL_FORCE_FETCH_FROM_GIT 1)
include(pico_sdk_import.cmake)
project(airpods_dongle VERSION 0.1.0 LANGUAGES C CXX ASM)
pico_sdk_init()

if(NOT PICO_BOARD STREQUAL "pico2_w")
    message(FATAL_ERROR "Only Pico 2 W is supported; the audio buffers do not fit on RP2040")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/prepare_sdk.py"
    --sdk "${PICO_SDK_PATH}" --check RESULT_VARIABLE sdk_check)
if(NOT sdk_check EQUAL 0)
    message(FATAL_ERROR "Pinned SDK patches are required; see README.md")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static codec libraries" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "No desktop codec tools" FORCE)
add_subdirectory(3rd-party/fdk-aac)
target_compile_definitions(fdk-aac PUBLIC FDK_ARM_ARCH)

add_executable(airpods_dongle
    src/main.c
    src/pico_w_led.c
    src/btstack/btstack_avdtp_source.c
    src/btstack/btstack_hci.c
    src/btstack/btstack_aacp.c
    src/btstack/aacp_mic_dec.c
    src/tinyusb/uac.c
    src/tinyusb/usb_descriptors.c
    src/tinyusb/debug_cdc.c
    src/tinyusb/media_control.c
)
target_include_directories(airpods_dongle PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    3rd-party/fdk-aac
)
target_link_libraries(airpods_dongle PRIVATE
    pico_stdlib pico_btstack_ble pico_btstack_classic pico_btstack_cyw43
    pico_cyw43_arch_threadsafe_background pico_btstack_sbc_encoder
    pico_multicore tinyusb_device tinyusb_board fdk-aac
)
pico_enable_stdio_uart(airpods_dongle 0)
target_compile_definitions(airpods_dongle PRIVATE
    CYW43_LWIP=0 PICO_FLASH_ASSUME_CORE1_SAFE=1 PICO_MALLOC_PANIC=0
)
pico_set_program_name(airpods_dongle "AirPods Dongle")
pico_set_program_version(airpods_dongle "${PROJECT_VERSION}")
pico_add_extra_outputs(airpods_dongle)
add_custom_target(check_sdk ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/prepare_sdk.py"
        --sdk "${PICO_SDK_PATH}" --check
    VERBATIM)
add_dependencies(airpods_dongle check_sdk)

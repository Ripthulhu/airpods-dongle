cmake_minimum_required(VERSION 3.17)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PICOTOOL_FORCE_FETCH_FROM_GIT 1)
include(pico_sdk_import.cmake)
project(airpods_dongle VERSION 0.1.7 LANGUAGES C CXX ASM)
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
set(dongle_linker "${CMAKE_CURRENT_BINARY_DIR}/airpods.ld")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/make_linker.py"
    --sdk "${PICO_SDK_PATH}" --output "${dongle_linker}" RESULT_VARIABLE linker_check)
if(NOT linker_check EQUAL 0)
    message(FATAL_ERROR "The pinned SDK linker layout is required")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/tools/make_linker.py")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static codec libraries" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "No desktop codec tools" FORCE)
add_subdirectory(3rd-party/fdk-aac)
target_compile_definitions(fdk-aac PRIVATE FDK_AIRPODS_PROFILE=1)
set(codec_fingerprint "${CMAKE_CURRENT_BINARY_DIR}/codec_fingerprint.c")
add_custom_command(OUTPUT "${codec_fingerprint}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/stamp.py"
        --library "$<TARGET_FILE:fdk-aac>" --output "${codec_fingerprint}"
    DEPENDS fdk-aac "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/stamp.py"
    VERBATIM)
add_executable(airpods_dongle
    "${codec_fingerprint}"
    src/main.c
    src/pico_w_led.c
    src/slot_storage.c
    src/btstack/btstack_avdtp_source.c
    src/btstack/btstack_hci.c
    src/btstack/btstack_ble_scan.c
    src/btstack/btstack_aacp.c
    src/btstack/btstack_att.c
    src/btstack/aacp_mic_dec.c
    src/btstack/phone_sink.c
    src/btstack/phone_codec.c
    src/btstack/phone_worker.c
    3rd-party/libfreeaptx/freeaptx.c
    src/tinyusb/uac.c
    src/tinyusb/usb_descriptors.c
    src/tinyusb/debug_cdc.c
    src/tinyusb/media_control.c
    src/tinyusb/dongle_control.c
    src/tinyusb/dongle_rpc.c
)
target_include_directories(airpods_dongle PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    3rd-party/fdk-aac
    3rd-party/libfreeaptx
)
target_link_libraries(airpods_dongle PRIVATE
    pico_stdlib pico_multicore pico_btstack_ble pico_btstack_classic pico_btstack_cyw43
    pico_cyw43_arch_threadsafe_background pico_btstack_sbc_encoder pico_btstack_sbc_decoder
    tinyusb_device tinyusb_board fdk-aac
)
pico_enable_stdio_uart(airpods_dongle 0)
target_link_options(airpods_dongle PRIVATE "-Wl,--undefined=airpods_codec_fingerprint")
target_compile_definitions(airpods_dongle PRIVATE
    CYW43_LWIP=0 PICO_MALLOC_PANIC=0 PICO_USE_MALLOC_MUTEX=1
    PICO_CORE1_STACK_SIZE=0 PICO_STACK_SIZE=16384 PICO_USE_STACK_GUARDS=1
)
pico_set_linker_script(airpods_dongle "${dongle_linker}")
pico_set_program_name(airpods_dongle "AirPods Dongle")
pico_set_program_version(airpods_dongle "${PROJECT_VERSION}")
pico_add_extra_outputs(airpods_dongle)
set(codec_check_command
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/run.py"
    --firmware "$<TARGET_FILE:airpods_dongle>" --library "$<TARGET_FILE:fdk-aac>"
    --compiler "${CMAKE_C_COMPILER}" --nm "${CMAKE_NM}"
    --output "${CMAKE_CURRENT_BINARY_DIR}/codec-probe")
add_custom_command(TARGET airpods_dongle POST_BUILD
    COMMAND ${codec_check_command}
    COMMENT "Checking codec allocations against the linked firmware heap"
    VERBATIM)
set_property(TARGET airpods_dongle APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/run.py"
    "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/probe.c"
    "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/profile_checks.h"
    "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/startup.c"
    "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/probe.ld")
add_custom_target(check_codec_memory COMMAND ${codec_check_command}
    DEPENDS airpods_dongle fdk-aac VERBATIM)
add_custom_target(check_sdk ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/prepare_sdk.py"
        --sdk "${PICO_SDK_PATH}" --check
    VERBATIM)
add_dependencies(airpods_dongle check_sdk)

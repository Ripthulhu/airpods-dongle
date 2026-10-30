cmake_minimum_required(VERSION 3.17)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PICOTOOL_FORCE_FETCH_FROM_GIT 1)
set(PICOTOOL_GIT_BRANCH 2041936441b48a3cc53ae3da9e805229fe8f4e18)
include(pico_sdk_import.cmake)
project(airpods_dongle VERSION 0.1.54 LANGUAGES C CXX ASM)
option(AIRPODS_USB_FLOAT "Expose float and integer PCM32 USB input alongside S16 compatibility" ON)
set(AACELD_CBR_BANDWIDTH 20000 CACHE STRING "AAC-ELD CBR bandwidth in Hz; 0 uses FDK automatic selection")
set_property(CACHE AACELD_CBR_BANDWIDTH PROPERTY STRINGS 0 16000 18000 20000)
if(NOT AACELD_CBR_BANDWIDTH MATCHES "^(0|16000|18000|20000)$")
    message(FATAL_ERROR "AACELD_CBR_BANDWIDTH must be 0, 16000, 18000 or 20000")
endif()
pico_sdk_init()

# Bluedroid's product-packing macro deliberately wraps its signed low word.
set_source_files_properties("${PICO_BTSTACK_PATH}/3rd-party/bluedroid/encoder/srce/sbc_packing.c"
    PROPERTIES COMPILE_OPTIONS "-fwrapv")

if(NOT PICO_BOARD STREQUAL "pico2_w")
    message(FATAL_ERROR "Only Pico 2 W is supported; the audio buffers do not fit on RP2040")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/prepare_sdk.py"
    --sdk "${PICO_SDK_PATH}" --check RESULT_VARIABLE sdk_check)
if(NOT sdk_check EQUAL 0)
    message(FATAL_ERROR "Pinned SDK patches are required; see README.md")
endif()
set(dongle_linker "${CMAKE_CURRENT_BINARY_DIR}/airpods-linker")
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
target_compile_definitions(fdk-aac PUBLIC FDK_DONGLE_PCM32=1 PRIVATE FDK_AIRPODS_PROFILE=1)
file(GLOB lhdc_sources CONFIGURE_DEPENDS "3rd-party/lhdcv5/decoder/*.c")
add_library(lhdcv5 STATIC 3rd-party/lhdcv5/lhdc48.c ${lhdc_sources})
target_include_directories(lhdcv5 PUBLIC 3rd-party/lhdcv5
    PRIVATE 3rd-party/lhdcv5/decoder 3rd-party/lhdcv5/port)
target_compile_options(lhdcv5 PRIVATE -ffp-contract=off)
target_link_libraries(lhdcv5 PRIVATE pico_stdlib m)
set(codec_fingerprint "${CMAKE_CURRENT_BINARY_DIR}/codec_fingerprint.c")
set(lhdc_fingerprint "${CMAKE_CURRENT_BINARY_DIR}/lhdc_fingerprint.c")
add_custom_command(OUTPUT "${codec_fingerprint}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/stamp.py"
        --library "$<TARGET_FILE:fdk-aac>" --output "${codec_fingerprint}"
    DEPENDS fdk-aac "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/stamp.py"
    VERBATIM)
add_custom_command(OUTPUT "${lhdc_fingerprint}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/stamp.py"
        --label LHDC --library "$<TARGET_FILE:lhdcv5>" --output "${lhdc_fingerprint}"
    DEPENDS lhdcv5 "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/stamp.py"
    VERBATIM)
add_executable(airpods_dongle
    "${codec_fingerprint}"
    "${lhdc_fingerprint}"
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
    src/btstack/phone_call.c
    src/btstack/phone_call_audio.c
    src/btstack/phone_control.c
    src/btstack/dongle_finder.c
    src/btstack/dongle_case_battery.c
    src/btstack/phone_codec.c
    src/btstack/phone_worker.c
    src/btstack/phone_benchmark.c
    3rd-party/libfreeaptx/freeaptx.c
    src/tinyusb/uac.c
    src/tinyusb/usb_descriptors.c
    src/tinyusb/debug_cdc.c
    src/tinyusb/media_control.c
    src/tinyusb/dongle_control.c
    src/tinyusb/dongle_rpc.c
    src/tinyusb/dongle_features.c
)
target_include_directories(airpods_dongle PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    3rd-party/fdk-aac
    3rd-party/libfreeaptx
)
target_link_libraries(airpods_dongle PRIVATE
    pico_stdlib pico_multicore pico_btstack_ble pico_btstack_classic pico_btstack_cyw43
    pico_cyw43_arch_threadsafe_background pico_btstack_sbc_encoder pico_btstack_sbc_decoder
    tinyusb_device tinyusb_board fdk-aac lhdcv5
)
pico_enable_stdio_uart(airpods_dongle 0)
target_link_options(airpods_dongle PRIVATE "-Wl,--undefined=airpods_codec_fingerprint")
target_link_options(airpods_dongle PRIVATE "-Wl,--undefined=airpods_eld_cbr_bandwidth_hz")
target_link_options(airpods_dongle PRIVATE "-Wl,--undefined=airpods_lhdc_fingerprint"
    "-Wl,--undefined=airpods_phone_worker_storage_bytes")
target_compile_definitions(airpods_dongle PRIVATE
    AACELD_CBR_BANDWIDTH=${AACELD_CBR_BANDWIDTH}
    AIRPODS_USB_FLOAT=$<BOOL:${AIRPODS_USB_FLOAT}>
    CYW43_LWIP=0 CYW43_PIO_CLOCK_DIV_DYNAMIC=1 ENABLE_RIJNDAEL_DECRYPT=1 PICO_MALLOC_PANIC=0 PICO_USE_MALLOC_MUTEX=1
    PICO_CORE1_STACK_SIZE=0 PICO_STACK_SIZE=16384 PICO_USE_STACK_GUARDS=1
)
pico_add_linker_script_override_path(airpods_dongle "${dongle_linker}"
    FILES section_heap.incl sections_stack.incl section_end.incl
    default_text_excludes.incl default_rodata_excludes.incl)
pico_set_program_name(airpods_dongle "AirPods Dongle")
pico_set_program_version(airpods_dongle "${PROJECT_VERSION}")
pico_init_picotool()
include(tools/codec_probe/publish.cmake)
set(codec_check_command
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/run.py"
    --firmware "$<TARGET_FILE:airpods_dongle>" --library "$<TARGET_FILE:fdk-aac>"
    --lhdc-library "$<TARGET_FILE:lhdcv5>"
    --compiler "${CMAKE_C_COMPILER}" --nm "${CMAKE_NM}"
    --picotool "$<TARGET_FILE:picotool>" --uf2-family "${PICO_PLATFORM}"
    --eld-cbr-bandwidth "${AACELD_CBR_BANDWIDTH}"
    --output "${CMAKE_CURRENT_BINARY_DIR}/codec-probe")
if(AIRPODS_USB_FLOAT)
    list(APPEND codec_check_command --usb-audio float)
else()
    list(APPEND codec_check_command --usb-audio s16)
endif()
if(PICO_RP2350_A2_SUPPORTED)
    list(APPEND codec_check_command --uf2-abs-block)
endif()
airpods_add_checked_outputs(airpods_dongle ${codec_check_command})
set_property(TARGET airpods_dongle APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/run.py"
    "${CMAKE_CURRENT_LIST_DIR}/tools/check_usb_audio.py"
    "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/probe.c"
    "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/lhdc_quality.h"
    "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/publish.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/profile_checks.h"
    "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/startup.c"
    "${CMAKE_CURRENT_LIST_DIR}/tools/codec_probe/probe.ld"
    "${CMAKE_CURRENT_LIST_DIR}/3rd-party/lhdcv5/lhdc48_bench_fixture.h"
    "${CMAKE_CURRENT_LIST_DIR}/tests/lhdc/music_reference.h")
add_custom_target(check_codec_memory COMMAND ${codec_check_command}
    DEPENDS airpods_dongle fdk-aac lhdcv5 VERBATIM)
add_custom_target(check_sdk ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/prepare_sdk.py"
        --sdk "${PICO_SDK_PATH}" --check
    VERBATIM)
add_dependencies(airpods_dongle check_sdk)

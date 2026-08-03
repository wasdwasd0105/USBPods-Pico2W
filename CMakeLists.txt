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
set(sdkVersion 2.1.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.1.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# UNIVERSAL board: ONE binary for Pico 2 W and Waveshare RP2350B-Plus-W.
# src/boards/usbpods_universal.h compiles for the RP2350B GPIO superset and
# defines CYW43_PIN_WL_DYNAMIC, which enables the runtime radio-pin probe in
# main() (Pico pins 23/24/25/29, else Waveshare 36-39). Building with the stock
# `pico2_w` board instead compiles the probe OUT and as RP2350A, producing a
# binary whose radio can never start on the Waveshare — so these two lines are
# REQUIRED and must stay ahead of pico_sdk_import/pico_sdk_init.
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/src/boards)
set(PICO_BOARD usbpods_universal CACHE STRING "Board type")
set(FAMILY rp2040)
set(BOARD pico_sdk)



# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)


project(USBPods_Pico2W C CXX ASM)


# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# The firmware depends on LOCAL pico-sdk patches (sdk-patches/README.md).
# A fresh SDK clone compiles almost clean but silently changes wire behaviour
# — worst case, stock BTstack DISCONNECTS the ACL on every flush event.
# Fail loudly at configure time instead.
foreach(_usbpods_check
        "lib/btstack/src/hci.c|usbpods"
        "lib/btstack/src/l2cap.c|l2cap_set_flushable_local_cid"
        "lib/btstack/src/classic/avdtp_source.c|usbpods"
        "lib/tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c|// panic(\"ep")
    string(REPLACE "|" ";" _usbpods_pair "${_usbpods_check}")
    list(GET _usbpods_pair 0 _usbpods_file)
    list(GET _usbpods_pair 1 _usbpods_marker)
    file(READ "${PICO_SDK_PATH}/${_usbpods_file}" _usbpods_src)
    string(FIND "${_usbpods_src}" "${_usbpods_marker}" _usbpods_found)
    if(_usbpods_found EQUAL -1)
        message(FATAL_ERROR
            "pico-sdk at ${PICO_SDK_PATH} is missing the usbpods local patch "
            "for ${_usbpods_file} (marker '${_usbpods_marker}' not found). "
            "Apply sdk-patches/*.patch first — see sdk-patches/README.md.")
    endif()
    # re-run this check if the SDK file changes AFTER configuring (an SDK
    # revert/re-sync would otherwise rebuild stock behaviour silently)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${PICO_SDK_PATH}/${_usbpods_file}")
endforeach()


#LDAC https://github.com/EHfive/ldacBT
add_subdirectory(3rd-party/ldacBT)

# ---- LHDC V5 (Savitech) — Rust encoder core, built by cargo ---------------
# The crate (3rd-party/lhdcv5, AOSP Rust reimplementation, usbpods no_std
# port) produces a staticlib for the SOFT-FLOAT-ABI target: the pico-sdk
# compiles -mfloat-abi=softfp and an eabihf archive refuses to link.
find_program(CARGO_EXECUTABLE cargo PATHS $ENV{HOME}/.cargo/bin REQUIRED)
set(LHDCV5_DIR ${CMAKE_CURRENT_LIST_DIR}/3rd-party/lhdcv5)
set(LHDCV5_LIB ${LHDCV5_DIR}/target/thumbv8m.main-none-eabi/release/liblhdcv5.a)
file(GLOB_RECURSE LHDCV5_RS_SOURCES ${LHDCV5_DIR}/src/*.rs)
add_custom_command(
    OUTPUT ${LHDCV5_LIB}
    COMMAND ${CARGO_EXECUTABLE} build --release --lib --target thumbv8m.main-none-eabi
    WORKING_DIRECTORY ${LHDCV5_DIR}
    DEPENDS ${LHDCV5_RS_SOURCES} ${LHDCV5_DIR}/Cargo.toml
    COMMENT "cargo: liblhdcv5.a (LHDC V5 Rust encoder, thumbv8m soft-float ABI)")
add_custom_target(lhdcv5_rust DEPENDS ${LHDCV5_LIB})


# FDK-AAC (encoder only)
set(FDK_DIR ${CMAKE_CURRENT_LIST_DIR}/3rd-party/fdk-aac)
set(FDK_BUILD_DIR ${CMAKE_BINARY_DIR}/fdk-aac)


add_subdirectory(${FDK_DIR} ${FDK_BUILD_DIR})
target_compile_definitions(fdk-aac PUBLIC FDK_ARM_ARCH)


set(BTSTACK_3RD_PARTY_PATH ${PICO_SDK_PATH}/lib/btstack/3rd-party)
#set(BTSTACK_EXAMPLE_PATH $ENV{PICO_SDK_PATH}/lib/btstack/3rd-party)


# Add executable.
# The device always exposes TWO USB configurations (UAC2 + UAC1). Hosts pick
# the first by default; this option flips the order so legacy hosts (consoles,
# KVMs) get UAC1 as configuration 1: cmake -B build-uac1 -DUSB_UAC1_FIRST=ON
option(USB_UAC1_FIRST "UAC1 as the default (first) USB configuration" OFF)

# ---- build variant --------------------------------------------------------
# This tree builds the open-source firmware. Optional vendor add-ons may be
# dropped in at private/; when they are absent (the normal case) the build
# simply proceeds without them.
option(USBPODS_LITE "Build the open-source variant" OFF)
if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/private/full.cmake)
    set(USBPODS_LITE ON)
    message(STATUS "usbpods: building the open-source variant")
endif()
if(USBPODS_LITE)
    set(USBPODS_VARIANT_SOURCES      src/lite_stubs.c)
    set(USBPODS_VARIANT_INCLUDE_DIRS "")
    # target-wide: tusb_config.h and the descriptors read it (HAVE_AAC_FDK rule)
    set(USBPODS_VARIANT_DEFINES      USBPODS_LITE=1)
    set(USBPODS_OUT_NAME    USBPods_Pico2W_Lite)
    set(USBPODS_CHANNEL_DIR lite)   # never feeds the dev/production updaters
    message(STATUS "usbpods variant: LITE")
else()
    include(${CMAKE_CURRENT_LIST_DIR}/private/full.cmake)
    message(STATUS "usbpods variant: FULL")
endif()

add_executable(${PROJECT_NAME}
    src/main.c
    src/console.c
    src/settings.c
    src/resamp44.c
    src/pico_w_led.c
    src/btstack/btstack_avdtp_source.c
    src/btstack/codec/codec_sbc.c
    src/btstack/codec/codec_aac.c
    src/btstack/codec/codec_ldac.c
    src/btstack/codec/codec_aaceld.c
    src/btstack/codec/codec_registry.c
    src/btstack/btstack_hci.c
    src/tinyusb/uac.c
    src/tinyusb/uac1_device.c
    src/tinyusb/usb_serial.c
    src/tinyusb/usb_descriptors.c
    src/btstack/codec/codec_lhdc.c
    3rd-party/lhdcv5/src/lhdcv5BT_enc.c
    ${USBPODS_VARIANT_SOURCES}
)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME ${USBPODS_OUT_NAME})

if (USB_UAC1_FIRST)
    target_compile_definitions(${PROJECT_NAME} PRIVATE USB_UAC1_FIRST=1)
endif()
if(USBPODS_VARIANT_DEFINES)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ${USBPODS_VARIANT_DEFINES})
endif()
if(USBPODS_VARIANT_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${USBPODS_VARIANT_INCLUDE_DIRS})
endif()


# Including header files directly from project directory
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)


target_include_directories(${PROJECT_NAME} PRIVATE
        3rd-party/ldacBT/libldac/inc
        3rd-party/ldacBT/libldac/abr/inc
        3rd-party/fdk-aac
        3rd-party/lhdcv5/include
)


# Link the Project to extra libraries
target_link_libraries(${PROJECT_NAME}
pico_stdlib
pico_btstack_ble
pico_btstack_classic
pico_btstack_cyw43
pico_cyw43_arch_threadsafe_background
pico_btstack_sbc_encoder
pico_btstack_sbc_decoder
pico_multicore
tinyusb_device
tinyusb_board
pico_bootrom
pico_stdio_uart
fdk-aac
ldacBT_enc
ldacBT_abr
${LHDCV5_LIB}
)

add_dependencies(${PROJECT_NAME} lhdcv5_rust)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    # FDK-AAC available: gates the AAC-LC and AAC-ELD source paths. MUST be
    # target-wide — it used to be a #define inside btstack_avdtp_source.c,
    # and the codec split left src/btstack/codec/codec_aac.c blind to it, so
    # the AAC stream endpoint was never created and every AAC-LC headset
    # silently fell back to SBC.
    HAVE_AAC_FDK
    CYW43_LWIP=0
    PICO_FLASH_ASSUME_CORE1_SAFE=1
    # OOM = malloc returns NULL (FDK bails cleanly, relay prints an error)
    # instead of a device-rebooting panic mid-stream.
    PICO_MALLOC_PANIC=0
    # Route every SDK/TinyUSB panic() into the scratch-register forensics
    # (main.c usbpods_panic) — stock panic breakpoints into a mute hardfault.
    PICO_PANIC_FUNCTION=usbpods_panic
    # The whole BTstack run loop (incl. FDK ENCODE and the relay's inline
    # AAC DECODE) executes in the cyw43 background IRQ on the core-0 stack.
    # At the old 4 KB (SCRATCH_Y bank) the first LATM decode hardfaulted
    # deterministically (the reason the AAC sink SEP was parked): FDK's
    # CAacDecoder_DecodeFrame carried a ~34.5 KB entry frame from the CCE
    # parse branch's stack locals — now excised in our fdk (frame ~1 KB).
    # 24 KB covers IRQ-context decode (BTstack depth + decoder callees)
    # with generous margin while leaving the heap its ~80 KB config-time
    # headroom. Relocated to top of main RAM by memmap_bigstack.ld; main.c
    # arms MSPLIM so any overflow faults loudly instead of chewing heap.
    PICO_STACK_SIZE=0x6000
    # Core-1 stack defaults to PICO_STACK_SIZE and must still fit its 4 KB
    # SCRATCH_X bank (core 1 never launches — this only sizes the dummy).
    PICO_CORE1_STACK_SIZE=0x1000

    # must enable on rp2040 & 330kbps(MQ)
    #LDAC_SOFT_FLOAT=OFF
)



# Core-0 stack at top of main RAM instead of the 4 KB SCRATCH_Y bank
# (see PICO_STACK_SIZE above). Copy of the SDK's rp2350 memmap_default.ld
# with changes marked "usbpods:".
pico_set_linker_script(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/src/memmap_bigstack.ld)

# Export binaries like hex, bin, and uf2 files.
# Blocking UART stdio stalls the shared BT/audio core per printed char —
# console lives on the non-blocking USB CDC driver (usb_serial.c) instead.
pico_enable_stdio_uart(${PROJECT_NAME} 0)

# Version from src/version.h (single source of truth) -> picotool info
file(STRINGS src/version.h _V_NAME REGEX "USBPODS_VERSION_NAME")
file(STRINGS src/version.h _V_CODE REGEX "USBPODS_VERSION_CODE")
string(REGEX REPLACE ".*\"(.*)\".*" "\\1" _V_NAME "${_V_NAME}")
string(REGEX REPLACE ".*CODE +([0-9]+).*" "\\1" _V_CODE "${_V_CODE}")
if(USBPODS_LITE)
    set(_V_VARIANT "-lite")
else()
    set(_V_VARIANT "")
endif()
pico_set_program_version(${PROJECT_NAME} "${_V_NAME}${_V_VARIANT}+${_V_CODE}")

# web update channel: the page checks latest.json against the device's version
# code. Each channel is an INDEPENDENT update stream — opt/dev/ feeds
# dev-webui, opt/production/ feeds webui (see webui-usbpods/README.md). A build
# only ever writes its own channel, so a dev build can never become the
# production update.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/version.h)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/opt/dev
                    ${CMAKE_CURRENT_LIST_DIR}/opt/production
                    ${CMAKE_CURRENT_LIST_DIR}/opt/lite)
file(WRITE ${CMAKE_CURRENT_LIST_DIR}/opt/${USBPODS_CHANNEL_DIR}/latest.json
     "{\"name\":\"${_V_NAME}\",\"code\":${_V_CODE}}\n")

pico_add_extra_outputs(${PROJECT_NAME})

# Stage the UF2 for the web updater. MUST come AFTER pico_add_extra_outputs:
# POST_BUILD commands run in addition order, and the UF2 is itself produced
# by a POST_BUILD step — registering the copy earlier shipped every build's
# PREDECESSOR ("the fw version did not change" after a web update).
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_BINARY_DIR}/${USBPODS_OUT_NAME}.uf2
            ${CMAKE_CURRENT_LIST_DIR}/opt/${USBPODS_CHANNEL_DIR}/${USBPODS_OUT_NAME}.uf2
    COMMENT "staging UF2 in opt/${USBPODS_CHANNEL_DIR}/")

cmake_minimum_required(VERSION 3.13)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

set(PICO_BOARD pico_w CACHE STRING "Board type")

include(${USERHOME}/.pico-sdk/sdk/${sdkVersion}/external/pico_sdk_import.cmake)

project(sidetnfs C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

set(FS_BACKEND "TNFS" CACHE STRING "Filesystem backend: MEMORY or TNFS")

add_executable(sidetnfs
    src/main.c
    src/constants.c
    src/tprotocol.c
    src/romemul.c
    src/gemdrvemul.c
    src/net_wifi.c
    src/net_test.c
    src/net_udp_test.c
    src/tnfs_client.c
    src/tnfs_test.c
    src/firmware/firmware_gemdrvemul.c
)

if(FS_BACKEND STREQUAL "TNFS")
    target_sources(sidetnfs PRIVATE src/fs_tnfs.c)
    target_compile_definitions(sidetnfs PRIVATE FS_BACKEND_TNFS=1)
    message(STATUS "sidetnfs: filesystem backend = TNFS")
else()
    target_sources(sidetnfs PRIVATE src/fs_memory.c)
    target_compile_definitions(sidetnfs PRIVATE FS_BACKEND_MEMORY=1)
    message(STATUS "sidetnfs: filesystem backend = MEMORY")
endif()

pico_generate_pio_header(sidetnfs ${CMAKE_CURRENT_SOURCE_DIR}/src/romemul.pio)

target_include_directories(sidetnfs PRIVATE
    src
    src/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

# -DSIDETNFS_DEBUG=ON  → USB serial, all LOG()/DPRINTF() output, 3-s USB wait (default)
# -DSIDETNFS_DEBUG=OFF → no USB, no logging, ROM emulator starts immediately
option(SIDETNFS_DEBUG        "Enable USB serial, debug logging, USB enumeration wait" ON)
option(DIAGNOSTIC_PASSTHROUGH "Patch 68k handler to bypass exec_trapped_handler (pure pass-through)" OFF)

target_compile_definitions(sidetnfs PRIVATE
    $<$<BOOL:${SIDETNFS_DEBUG}>:SIDETNFS_DEBUG=1>
    $<$<BOOL:${DIAGNOSTIC_PASSTHROUGH}>:DIAGNOSTIC_PASSTHROUGH=1>
)

target_link_libraries(sidetnfs
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_vreg
    hardware_flash
    pico_cyw43_arch_lwip_threadsafe_background
)

pico_set_linker_script(sidetnfs ${CMAKE_CURRENT_SOURCE_DIR}/memmap_sidetnfs.ld)

if(SIDETNFS_DEBUG)
    pico_enable_stdio_usb(sidetnfs 1)
else()
    pico_enable_stdio_usb(sidetnfs 0)
endif()
pico_enable_stdio_uart(sidetnfs 0)

pico_add_extra_outputs(sidetnfs)

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

add_executable(sidetnfs
    src/main.c
    src/constants.c
    src/tprotocol.c
    src/romemul.c
    src/gemdrvemul.c
    src/fs_memory.c
    src/net_wifi.c
    src/firmware/firmware_gemdrvemul.c
)

pico_generate_pio_header(sidetnfs ${CMAKE_CURRENT_SOURCE_DIR}/src/romemul.pio)

target_include_directories(sidetnfs PRIVATE
    src
    src/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

option(DIAGNOSTIC_PASSTHROUGH "Patch 68k handler to bypass exec_trapped_handler (pure pass-through)" OFF)

target_compile_definitions(sidetnfs PRIVATE
    _DEBUG=1
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

pico_enable_stdio_usb(sidetnfs 1)
pico_enable_stdio_uart(sidetnfs 0)

pico_add_extra_outputs(sidetnfs)

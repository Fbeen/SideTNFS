#pragma once

#include <stdint.h>
#include "pico/stdlib.h"

#define SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE 16

#define SHARED_VARIABLE_HARDWARE_TYPE 0
#define SHARED_VARIABLE_SVERSION 1
#define SHARED_VARIABLE_BUFFER_TYPE 2

extern const uint32_t SELECT_GPIO;

extern const uint READ_ADDR_GPIO_BASE;
extern const uint READ_ADDR_PIN_COUNT;
extern const uint READ_SIGNAL_GPIO_BASE;
extern const uint READ_SIGNAL_PIN_COUNT;

extern const uint WRITE_DATA_GPIO_BASE;
extern const uint WRITE_DATA_PIN_COUNT;
extern const uint WRITE_SIGNAL_GPIO_BASE;
extern const uint WRITE_SIGNAL_PIN_COUNT;

#define SAMPLE_DIV_FREQ (1.f)
#define RP2040_CLOCK_FREQ_KHZ (125000 + 100000)
#define RP2040_VOLTAGE VREG_VOLTAGE_1_10

extern const uint8_t ROM_BANKS;
extern const uint32_t FLASH_ROM_LOAD_OFFSET;
extern const uint32_t FLASH_ROM4_LOAD_OFFSET;
extern const uint32_t FLASH_ROM3_LOAD_OFFSET;
extern const uint32_t ROM_IN_RAM_ADDRESS;
extern const uint32_t ROMS_START_ADDRESS;
extern const uint32_t ROM4_START_ADDRESS;
extern const uint32_t ROM3_START_ADDRESS;
extern const uint32_t ROM4_END_ADDRESS;
extern const uint32_t ROM3_END_ADDRESS;
extern const uint32_t ROM_SIZE_BYTES;
extern const uint32_t ROM_SIZE_WORDS;
extern const uint32_t ROM_SIZE_LONGWORDS;
extern const uint32_t CONFIG_FLASH_OFFSET;
extern const uint32_t CONFIG_FLASH_SIZE;

extern const uint32_t ATARI_ROM4_START_ADDRESS;
extern const uint32_t ATARI_ROM3_START_ADDRESS;

#define WATCHDOG_RESET_DELAY_MS 100
#define GET_CURRENT_TIME() (((uint64_t)timer_hw->timerawh) << 32u | timer_hw->timerawl)

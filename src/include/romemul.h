#ifndef ROMEMUL_H
#define ROMEMUL_H

#include "debug.h"
#include "constants.h"
#include "memfunc.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/vreg.h"
#include "hardware/structs/bus_ctrl.h"
#include "pico/cyw43_arch.h"

#include "romemul.pio.h"

typedef void (*IRQInterceptionCallback)();

extern int read_addr_rom_dma_channel;
extern int lookup_data_rom_dma_channel;

int init_romemul(IRQInterceptionCallback requestCallback, IRQInterceptionCallback responseCallback, bool copyFlashToRAM);

#endif // ROMEMUL_H

#include "include/romemul.h"
#include "include/gemdrvemul.h"
#include "hardware/clocks.h"

int main(void)
{
    // Overclock to 225 MHz at 1.10 V for reliable ROM bus timing
    set_sys_clock_khz(RP2040_CLOCK_FREQ_KHZ, true);
    vreg_set_voltage(RP2040_VOLTAGE);

    stdio_init_all();
    setvbuf(stdout, NULL, _IONBF, 1);
    // Give USB host up to 3 s to enumerate; proceed regardless
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++)
        sleep_ms(100);

    printf("SIDETNFS booting...\n");

    // Init CYW43 (required on Pico W even without WiFi to access board GPIO)
    if (cyw43_arch_init())
    {
        printf("cyw43_arch_init failed\n");
        return -1;
    }
    // No WiFi needed for M1 — deinit immediately
    cyw43_arch_deinit();

    // Copy the 68k GEMDRIVE driver firmware to ROM_IN_RAM (ROM4 bank)
    COPY_FIRMWARE_TO_RAM((uint16_t *)gemdrvemulROM, gemdrvemulROM_length);

    // Initialise the protocol parser (allocates payload buffer)
    init_protocol_parser();

    // Start ROM emulator: PIO + DMA chain + IRQ handler for ROM3 commands
    init_romemul(NULL, gemdrvemul_dma_irq_handler_lookup_callback, false);

    printf("ROM emulator running. Entering GEMDRIVE loop.\n");

    // Enter the GEMDRIVE command dispatch loop — never returns
    init_gemdrvemul();

    return 0;
}

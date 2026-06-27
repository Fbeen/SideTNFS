#include "include/romemul.h"

int read_addr_rom_dma_channel = -1;
int lookup_data_rom_dma_channel = -1;

PIO default_pio = pio0;

static int init_monitor_rom4(PIO pio)
{
    uint offsetMonitorROM4 = pio_add_program(pio, &monitor_rom4_program);
    uint smMonitorROM4 = pio_claim_unused_sm(pio, true);
    monitor_rom4_program_init(pio, smMonitorROM4, offsetMonitorROM4, SAMPLE_DIV_FREQ);
    pio_sm_set_enabled(pio, smMonitorROM4, true);
    DPRINTF("ROM4 signal monitor initialized.\n");
    return smMonitorROM4;
}

static int init_monitor_rom3(PIO pio)
{
    uint offsetMonitorROM3 = pio_add_program(pio, &monitor_rom3_program);
    uint smMonitorROM3 = pio_claim_unused_sm(pio, true);
    monitor_rom4_program_init(pio, smMonitorROM3, offsetMonitorROM3, SAMPLE_DIV_FREQ);
    pio_sm_set_enabled(pio, smMonitorROM3, true);
    DPRINTF("ROM3 signal monitor initialized.\n");
    return smMonitorROM3;
}

static int init_rom_emulator(PIO pio, IRQInterceptionCallback requestCallback, IRQInterceptionCallback responseCallback)
{
    read_addr_rom_dma_channel = dma_claim_unused_channel(true);
    DPRINTF("DMA channel for read_addr_rom_dma_channel: %d\n", read_addr_rom_dma_channel);
    if (read_addr_rom_dma_channel == -1)
    {
        DPRINTF("Failed to claim a DMA channel for read_addr_rom_dma_channel.\n");
        return -1;
    }

    lookup_data_rom_dma_channel = dma_claim_unused_channel(true);
    DPRINTF("DMA channel for lookup_data_rom_dma_channel: %d\n", lookup_data_rom_dma_channel);
    if (lookup_data_rom_dma_channel == -1)
    {
        DPRINTF("Failed to claim a DMA channel for lookup_data_rom_dma_channel.\n");
        return -1;
    }

    uint offsetReadROM = pio_add_program(pio, &romemul_read_program);
    uint smReadROM = pio_claim_unused_sm(pio, true);
    romemul_read_program_init(pio, smReadROM, offsetReadROM, READ_ADDR_GPIO_BASE, READ_ADDR_PIN_COUNT, READ_SIGNAL_GPIO_BASE, SAMPLE_DIV_FREQ);

    pio_sm_clear_fifos(pio, smReadROM);
    pio_sm_restart(pio, smReadROM);
    pio_sm_set_enabled(pio, smReadROM, true);

    // Lookup DMA: IRQ-driven, reads 16-bit data from ROM_IN_RAM and pushes to PIO TX FIFO
    dma_channel_config cdmaLookup = dma_channel_get_default_config(lookup_data_rom_dma_channel);
    channel_config_set_transfer_data_size(&cdmaLookup, DMA_SIZE_16);
    channel_config_set_read_increment(&cdmaLookup, false);
    channel_config_set_write_increment(&cdmaLookup, false);
    channel_config_set_dreq(&cdmaLookup, pio_get_dreq(pio, smReadROM, true));
    channel_config_set_chain_to(&cdmaLookup, read_addr_rom_dma_channel);
    dma_channel_configure(
        lookup_data_rom_dma_channel,
        &cdmaLookup,
        &pio->txf[smReadROM],
        NULL,
        1,
        false);

    // Address DMA: reads 32-bit address from PIO RX FIFO, injects into lookup DMA read addr
    dma_channel_config cdma = dma_channel_get_default_config(read_addr_rom_dma_channel);
    channel_config_set_transfer_data_size(&cdma, DMA_SIZE_32);
    channel_config_set_read_increment(&cdma, false);
    channel_config_set_write_increment(&cdma, false);
    channel_config_set_dreq(&cdma, pio_get_dreq(pio, smReadROM, false));
    dma_channel_configure(
        read_addr_rom_dma_channel,
        &cdma,
        &dma_hw->ch[lookup_data_rom_dma_channel].al3_read_addr_trig,
        &pio->rxf[smReadROM],
        1,
        true);

    if (requestCallback != NULL)
    {
        dma_channel_set_irq1_enabled(read_addr_rom_dma_channel, true);
        irq_set_exclusive_handler(DMA_IRQ_1, requestCallback);
        irq_set_enabled(DMA_IRQ_1, true);
    }
    if (responseCallback != NULL)
    {
        dma_channel_set_irq1_enabled(lookup_data_rom_dma_channel, true);
        irq_set_exclusive_handler(DMA_IRQ_1, responseCallback);
        irq_set_enabled(DMA_IRQ_1, true);
    }

    DPRINTF("ROM emulator initialized.\n");
    return smReadROM;
}

int init_romemul(IRQInterceptionCallback requestCallback, IRQInterceptionCallback responseCallback, bool copyFlashToRAM)
{
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    if (copyFlashToRAM)
    {
        const uint16_t *src_addr = (const uint16_t *)(XIP_BASE + FLASH_ROM_LOAD_OFFSET);
        COPY_FIRMWARE_TO_RAM(src_addr, ROM_SIZE_WORDS * ROM_BANKS);
    }

    int smMonitorROM4 = init_monitor_rom4(default_pio);
    if (smMonitorROM4 < 0)
    {
        DPRINTF("Error initializing ROM4 monitor.\n");
        return -1;
    }

    int smMonitorROM3 = init_monitor_rom3(default_pio);
    if (smMonitorROM3 < 0)
    {
        DPRINTF("Error initializing ROM3 monitor.\n");
        return -1;
    }

    int smReadROM = init_rom_emulator(default_pio, requestCallback, responseCallback);
    if (smReadROM < 0)
    {
        DPRINTF("Error initializing ROM emulator.\n");
        return -1;
    }

    // Push MSW of ROM_IN_RAM base address shifted right 17 bits (see PIO program comment)
    pio_sm_put_blocking(default_pio, smReadROM, (unsigned long int)ROMS_START_ADDRESS >> 17);

    pio_gpio_init(default_pio, READ_SIGNAL_GPIO_BASE);
    gpio_set_dir(READ_SIGNAL_GPIO_BASE, GPIO_OUT);
    gpio_set_pulls(READ_SIGNAL_GPIO_BASE, true, false);
    gpio_put(READ_SIGNAL_GPIO_BASE, 1);

    pio_gpio_init(default_pio, WRITE_SIGNAL_GPIO_BASE);
    gpio_set_dir(WRITE_SIGNAL_GPIO_BASE, GPIO_OUT);
    gpio_set_pulls(WRITE_SIGNAL_GPIO_BASE, true, false);
    gpio_put(WRITE_SIGNAL_GPIO_BASE, 1);

    pio_gpio_init(default_pio, ROM4_GPIO);
    gpio_set_dir(ROM4_GPIO, GPIO_IN);
    gpio_set_pulls(ROM4_GPIO, true, false);
    gpio_pull_up(ROM4_GPIO);

    pio_gpio_init(default_pio, ROM3_GPIO);
    gpio_set_dir(ROM3_GPIO, GPIO_IN);
    gpio_set_pulls(ROM3_GPIO, true, false);
    gpio_pull_up(ROM3_GPIO);

    for (int i = 0; i < WRITE_DATA_PIN_COUNT; i++)
    {
        pio_gpio_init(default_pio, WRITE_DATA_GPIO_BASE + i);
        gpio_set_dir(WRITE_DATA_GPIO_BASE + i, GPIO_OUT);
        gpio_set_pulls(WRITE_DATA_GPIO_BASE + i, false, true);
        gpio_put(WRITE_DATA_GPIO_BASE + i, 0);
    }

    return 0;
}

/***************************************************************************//**
 * @file    EFR32Port.c
 * @brief   AD5940 port layer for EFR32MG24
 *          Replaces ADICUP3029Port.c
 ******************************************************************************/

#include "ad5940.h"
#include "em_device.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_eusart.h"

#include "sl_udelay.h"

/* ── Pin definitions ── fill these in to match your hardware ────────────── */
#define AD5940_SPI_PERIPHERAL     EUSART1
#define AD5940_SPI_CLOCK          cmuClock_EUSART1

#define AD5940_CS_PORT            gpioPortC
#define AD5940_CS_PIN             0

#define AD5940_RST_PORT           gpioPortC
#define AD5940_RST_PIN            8

#define AD5940_INT_PORT           gpioPortC
#define AD5940_INT_PIN            5

/* ── ADD THESE — SPI bus pins ── */
#define AD5940_MOSI_PORT          gpioPortC
#define AD5940_MOSI_PIN           3

#define AD5940_MISO_PORT          gpioPortC
#define AD5940_MISO_PIN           2

#define AD5940_SCLK_PORT          gpioPortC
#define AD5940_SCLK_PIN           1

/* Bring-up tuning knobs — 4 MHz is more tolerant of wiring/loading than 8 MHz. */
#define AD5940_SPI_BITRATE_HZ     4000000u
#define AD5940_SPI_CLOCK_MODE     eusartClockMode0  /* Try eusartClockMode3 if needed */

/*
 * EUSART_SPI_MASTER_INIT_DEFAULT_HF sets advancedSettings to NULL. With no
 * advanced block, CFG0.MSBF stays at reset (0) = LSB first. AD5940 requires
 * MSB first (datasheet). Use advanced init with msbFirst true; autoCs off
 * because CS is GPIO (AD5940_CsClr/Set).
 */
static EUSART_SpiAdvancedInit_TypeDef ad5940_spi_advanced = {
  .csPolarity = eusartCsActiveLow,
  .invertIO = eusartInvertIODisable,
  .autoCsEnable = false,
  .msbFirst = true,
  .autoCsSetupTime = 0u,
  .autoCsHoldTime = 0u,
  .autoInterFrameTime = 0u,
  .autoTxEnable = false,
  .defaultTxData = 0x0000,
  .dmaWakeUpOnRx = false,
  .prsRxEnable = false,
  .prsRxChannel = (EUSART_PrsChannel_TypeDef)0u,
  .prsClockEnable = false,
  .prsClockChannel = (EUSART_PrsChannel_TypeDef)1u,
  .RxFifoWatermark = eusartRxFiFoWatermark1Frame,
  .TxFifoWatermark = eusartTxFiFoWatermark1Frame,
  .forceLoad = true,
  .setupWindow = 0x04u,
};


volatile static uint32_t ucInterrupted = 0;   /* mirrors the original flag */
volatile uint32_t g_ad5940_spi_timeouts = 0;
volatile uint32_t g_ad5940_spi_last_status = 0;
volatile uint32_t g_ad5940_spi_last_len = 0;


/* ── SPI transfer ───────────────────────────────────────────────────────── */
/*
 * The original polls SPI FIFOs directly via registers.
 * On EFR32MG24 we do the same thing but through EUSART registers.
 * EUSART_Tx/Rx are blocking single-byte calls — looping N times
 * gives us the same full-duplex behaviour the AD5940 library expects.
 */

void AD5940_ReadWriteNBytes(unsigned char *pSendBuffer,
                            unsigned char *pRecvBuff,
                            unsigned long  length)
{
    g_ad5940_spi_last_len = (uint32_t)length;
    for (unsigned long i = 0; i < length; i++) {
        uint32_t timeout = 1000000u;
        while (((AD5940_SPI_PERIPHERAL->STATUS & EUSART_STATUS_TXFL) == 0u) && (timeout-- > 0u)) {
        }
        if (timeout == 0u) {
            g_ad5940_spi_timeouts++;
            g_ad5940_spi_last_status = AD5940_SPI_PERIPHERAL->STATUS;
            pRecvBuff[i] = 0xFFu;
            continue;
        }

        AD5940_SPI_PERIPHERAL->TXDATA = pSendBuffer[i];

        timeout = 1000000u;
        while (((AD5940_SPI_PERIPHERAL->STATUS & EUSART_STATUS_RXFL) == 0u) && (timeout-- > 0u)) {
        }
        if (timeout == 0u) {
            g_ad5940_spi_timeouts++;
            g_ad5940_spi_last_status = AD5940_SPI_PERIPHERAL->STATUS;
            pRecvBuff[i] = 0xFFu;
            continue;
        }

        pRecvBuff[i] = (uint8_t)AD5940_SPI_PERIPHERAL->RXDATA;
    }
}


/* ── Chip select ────────────────────────────────────────────────────────── */
void AD5940_CsClr(void)
{
    GPIO_PinOutClear(AD5940_CS_PORT, AD5940_CS_PIN);
}

void AD5940_CsSet(void)
{
    GPIO_PinOutSet(AD5940_CS_PORT, AD5940_CS_PIN);
}


/* ── Reset ──────────────────────────────────────────────────────────────── */
void AD5940_RstSet(void)
{
    GPIO_PinOutSet(AD5940_RST_PORT, AD5940_RST_PIN);
}

void AD5940_RstClr(void)
{
    GPIO_PinOutClear(AD5940_RST_PORT, AD5940_RST_PIN);
}


/* ── Delay ──────────────────────────────────────────────────────────────── */
/*
 * The original uses SysTick manually. sl_udelay_wait() is a
 * Simplicity Studio primitive that gives us microsecond-accurate
 * busy-wait delay without touching SysTick configuration ourselves.
 * time is in units of 10us, so we multiply by 10.
 */
void AD5940_Delay10us(uint32_t time)
{
    if (time == 0) return;
    sl_udelay_wait(time * 10);
}


/* ── Interrupt flag ─────────────────────────────────────────────────────── */
uint32_t AD5940_GetMCUIntFlag(void)
{
    return ucInterrupted;
}

uint32_t AD5940_ClrMCUIntFlag(void)
{
    ucInterrupted = 0;
    return 1;
}
/*
 * NOTE: Unlike the original, we don't need to write to a hardware
 * interrupt-clear register here. On EFR32MG24, the GPIO interrupt
 * flag is cleared inside the ISR itself (see GPIO_EVEN/ODD handler
 * below). This function only clears the software flag that
 * AD5940Main.c polls.
 */


/* ── Resource init ──────────────────────────────────────────────────────── */
uint32_t AD5940_MCUResourceInit(void *pCfg)
{
    (void)pCfg;

    /* ── Step 1: GPIO clocks ── */
    CMU_ClockEnable(cmuClock_GPIO, true);

    /* ── Step 2: CS and RST pins as push-pull outputs ── */
    GPIO_PinModeSet(AD5940_CS_PORT,  AD5940_CS_PIN,  gpioModePushPull, 1); /* idle high */
    GPIO_PinModeSet(AD5940_RST_PORT, AD5940_RST_PIN, gpioModePushPull, 1); /* idle high */

    /* Configure SPI pins explicitly for EUSART master operation. */
    GPIO_PinModeSet(AD5940_MOSI_PORT, AD5940_MOSI_PIN, gpioModePushPull, 0);
    GPIO_PinModeSet(AD5940_MISO_PORT, AD5940_MISO_PIN, gpioModeInput,    0);
    GPIO_PinModeSet(AD5940_SCLK_PORT, AD5940_SCLK_PIN, gpioModePushPull, 0);

    /* ── Step 3: SPI peripheral ── */
    /* On this device family, EUSART clock source select uses cmuClock_EUSART0CLK enum. */
    CMU_ClockSelectSet(cmuClock_EUSART0CLK, cmuSelect_EM01GRPCCLK);
    CMU_ClockEnable(AD5940_SPI_CLOCK, true);

    EUSART_SpiInit_TypeDef spiInit = EUSART_SPI_MASTER_INIT_DEFAULT_HF;
    /*
     * AD5940 datasheet: SPI Mode 0 (CPOL=0, CPHA=0), MSB first.
     * EUSART_SPI_MASTER_INIT_DEFAULT_HF already sets mode 0 and MSB,
     * so we only need to set the bitrate.
     * Keep bitrate <= 10MHz for typical PCB layouts; 
     * you can push to 16MHz if your layout is clean and traces are short.
     */
    spiInit.bitRate = AD5940_SPI_BITRATE_HZ;
    spiInit.clockMode = AD5940_SPI_CLOCK_MODE;
    spiInit.advancedSettings = &ad5940_spi_advanced;

    EUSART_SpiInit(AD5940_SPI_PERIPHERAL, &spiInit);

    /*
     * Route EUSART signals to the GPIO pins you assigned in the Pin Tool.
     * These MODEM route register values are auto-generated by Simplicity
     * Studio when you configure the EUSART component — copy them from
     * your autogen/sl_event_handler.c or just hardcode the route here.
     * Example below assumes EUSART1 routed to PC0/PC1/PC2:
     */
    GPIO->EUSARTROUTE[1].TXROUTE  = (AD5940_MOSI_PORT << _GPIO_EUSART_TXROUTE_PORT_SHIFT)
                                   | (AD5940_MOSI_PIN  << _GPIO_EUSART_TXROUTE_PIN_SHIFT);
    GPIO->EUSARTROUTE[1].RXROUTE  = (AD5940_MISO_PORT << _GPIO_EUSART_RXROUTE_PORT_SHIFT)
                                   | (AD5940_MISO_PIN  << _GPIO_EUSART_RXROUTE_PIN_SHIFT);
    GPIO->EUSARTROUTE[1].SCLKROUTE = (AD5940_SCLK_PORT << _GPIO_EUSART_SCLKROUTE_PORT_SHIFT)
                                    | (AD5940_SCLK_PIN  << _GPIO_EUSART_SCLKROUTE_PIN_SHIFT);
    GPIO->EUSARTROUTE[1].ROUTEEN  = GPIO_EUSART_ROUTEEN_TXPEN
                                  | GPIO_EUSART_ROUTEEN_RXPEN
                                  | GPIO_EUSART_ROUTEEN_SCLKPEN;

    /* ── Step 4: External interrupt on INT pin ── */
    GPIO_PinModeSet(AD5940_INT_PORT, AD5940_INT_PIN, gpioModeInputPull, 1); /* input, pull-up */

    /*
     * Falling edge trigger — AD5940 INT pin is active low,
     * matching the original: XINT CFG0 = falling edge.
     */
    GPIO_ExtIntConfig(AD5940_INT_PORT,
                      AD5940_INT_PIN,
                      AD5940_INT_PIN,   /* intNo = pin number on EFR32 */
                      false,            /* rising edge: no  */
                      true,             /* falling edge: yes */
                      true);            /* enable immediately */

    /*
     * EFR32MG24 routes GPIO interrupts to either GPIO_ODD_IRQn or
     * GPIO_EVEN_IRQn depending on whether the pin number is odd or even.
     */
    if (AD5940_INT_PIN % 2 == 0) {
        NVIC_ClearPendingIRQ(GPIO_EVEN_IRQn);
        NVIC_EnableIRQ(GPIO_EVEN_IRQn);
    } else {
        NVIC_ClearPendingIRQ(GPIO_ODD_IRQn);
        NVIC_EnableIRQ(GPIO_ODD_IRQn);
    }

    /* ── Step 5: leave CS and RST high (idle state) ── */
    AD5940_CsSet();
    AD5940_RstSet();

    return 0;
}


/* ── GPIO interrupt handler ─────────────────────────────────────────────── */
/*
 * The original has a single Ext_Int0_Handler(). On EFR32MG24 there are
 * two GPIO IRQ vectors. You only need the one matching your pin number.
 * If AD5940_INT_PIN is odd, rename this to GPIO_ODD_IRQHandler.
 */

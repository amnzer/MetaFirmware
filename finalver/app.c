/***************************************************************************//**
 * @file
 * @brief Core application logic.
 *******************************************************************************
 * # License
 * <b>Copyright 2020 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/
 #include "em_common.h"
 #include "app_assert.h"
 #include "sl_bluetooth.h"
#include "sl_bt_api.h"
 #include "app.h"
 #include "sl_sleeptimer.h"
 #include "sl_power_manager.h"
 #include "gatt_db.h"
 #include "ad5940.h"

 /** Advertised when no valid ADC sample this cycle (avoids stale sensor_1). */
 #define SENSOR1_INVALID_SENTINEL 0

 /* AD594x family CHIPID is 0x55xx (used for sensor4 chipid_ok bit). */
 #define CHIPID_FAMILY_MASK  (0xFF00u)
 #define CHIPID_FAMILY_AD594 (0x5500u)

 /*
  * After ADCPowerCtrl + SINC2 enable, the AFE/ADC path needs time to settle before
  * the first conversion (bench ~50 ms). AD5940_Delay10us() units are 10 µs.
  */
 #define ADC_SETUP_SETTLE_MIN_10US  (5000u) /* 50 ms */
 #define ADC_SETUP_SETTLE_10US      (8000u) /* 80 ms — >= minimum, extra margin for SINC2 */
#define RE0_BIAS_DACIN12_CODE      (0x069Eu)

 // for perma advert. debug setitng
 //#define DEBUG_ALWAYS_ADV 1
 //
 static uint8_t advertising_set_handle = 0xff;
 #define ADVERT_MODE            1      // 1 for advert, 0 for gatt
#define SIG_CYCLE_START (1u << 0)
 #define POTENTIOMETRIC 1
 #define AMPEROMETRIC 0
 /** Set to 1 for bring-up: keep legacy advertising running (no duration stop, no SIG_ADV_STOP path). */
 #ifndef DEBUG_ALWAYS_ADV
 #define DEBUG_ALWAYS_ADV 0
 #endif

 // Configs for rest of file
 #if ADVERT_MODE

 /*
  * Period between NEW samples + new adv payload. This drives firmware cadence.
  * (If you see ~20 s between phone updates, check this value and that you flashed
  * a build that uses it — 20000 ms was the previous default.)
  * 3 s is realistic on the MCU; the phone must use a foreground / low-latency scan.
  */
 #define WAKE_PERIOD_MS         1000
 #define ADV_DURATION_MS        300  /* ms of advertising after each sample — tune vs phone catch rate */
#define ADV_INTERVAL_MS        20     // Interval within a burst (ms)
 #else
 #define ADV_INTERVAL_MS        50      // Interval within a burst (ms)
 #define WAKE_PERIOD_MS         10000    // Period between bursts (ms)
 #define ADV_DURATION_MS        5000    // Duration to stay advertising (ms)
 #endif

 // SPI debug vars
 static volatile uint32_t init_stage;
 static volatile uint32_t spi_step;
 static volatile uint16_t chip_id_manual;

 static volatile uint32_t spiflags;
 static volatile uint32_t good;
 static volatile uint32_t sample;
 static volatile uint32_t dbg_have_sample;
 static volatile uint32_t dbg_spi_attempts;
static volatile uint32_t dbg_periodic_isr_count;
static volatile uint32_t dbg_cycle_start_isr_delta;
static volatile uint32_t dbg_cycle_ms;
static volatile uint32_t dbg_spi_ms;
static volatile uint32_t dbg_intercycle_ms;
static volatile uint16_t dbg_cycle_counter;
static volatile uint32_t dbg_advstop_elapsed_ms;
static volatile uint64_t dbg_advstop_arm_tick;
static volatile uint32_t dbg_wake_elapsed_ms;
static volatile uint64_t dbg_wake_arm_tick;
static volatile uint16_t dbg_wake_arm_count;
static volatile uint16_t dbg_wake_fire_count;
static volatile uint32_t dbg_process_count;
 // 1. Advertising Variables
 float sensor_1 = SENSOR1_INVALID_SENTINEL;
 uint8_t chapter = 0; // "Transmission Index" (0-16). for uniqueness tracking
 uint8_t page = 0; // "subindex" (0-16). distinguishes what "sensor 1/2/3/4" refers to
 uint8_t chpg = 0;
 sl_status_t sc;
 extern volatile uint32_t g_ad5940_spi_timeouts;
 extern volatile uint32_t g_ad5940_spi_last_status;

 uint8_t adv_data[] = { //
   0x14, 0xFF, 0xFF, 0x02, 0x00, // CHIP ID
   0x00, 0x00, 0x00, 0x00, // sensor 1
   0x00, 0x00, 0x00, 0x00, // sensor 2
   0x00, 0x00, 0x00, 0x00, // sensor 3
   0x00, 0x00, 0x00, 0x00,  // sensor 4
   0x09, 0x09, 'M','e','t','a','n','o','i','a' // device name
 };
 enum sensor_offsets {
     OFFSET_1 = 5,
     OFFSET_2 = 9,
     OFFSET_3 = 13,
     OFFSET_4 = 17
 };
 // 1.5 SPI functions
 // // details of conversion may vary...
 float spiread_to_float(bool modality, uint32_t adc_val){
   // the actual details of this function vary a whoooole lot whenever pga gain, vref selection,
   // , switch matrix, and bias voltage change

   if (modality == POTENTIOMETRIC){

         // 1. Extract the 16-bit ADC code from the 32-bit FIFO word [4, 5]
         uint16_t raw_code = (uint16_t)(adc_val & 0xFFFF);

         // 2. Define constants based on ADC setup... which may change lots...
         const float pga_gain = 1.5f;      // Your current ADCPga setting
         const float v_ref = 1.835f;       // Factory-calibrated correction factor
         const float midscale = 32768.0f;  // 0x8000 offset for pseudo-differential ADC

         // 3. Calculate the differential voltage (V_diff) [3]
         // Formula: V_diff = (Correction_Factor / PGA) * (Code - 0x8000) / 2^15
        /* Report AIN1 - RE0 (invert hardware mux sign: VRE0 - AIN1). */
        float diff_voltage = -((v_ref / pga_gain) * ((float)raw_code - midscale) / midscale);

         return diff_voltage; // in V
   }
   return 0.0;
 }


 // 2. Advertising Functions
 // create the (txindex, subindex) byte
 uint8_t create_chapter_page_composite(){
   // the mask determines max unique # of pages and chapters
   // // the number of chapters is lowk irrelevant but # page determines
   // // ... how many unique sensors metasense can support
   uint8_t fourbitmask = 0x0F;
   return ((chapter&fourbitmask)<<4)|(page&fourbitmask);
 }

 void update_adv_index(uint8_t *adv_data, uint8_t chpg_byte){
   // !impt: if adv_data changes structure this will need to change
   adv_data[4] = chpg_byte;
 }

 // copy a sensor reading to its corresponding portion of adv_data
 void update_adv_data(uint8_t *adv_data, uint8_t offset, float floatval){
   uint8_t start_idx = offset; //uint8_t offset, (5,9,13,17 are only valid values as of 11/23)
   uint8_t databytes[4];
   // first ensure that the function is safe
   memcpy(databytes, &floatval, sizeof(float));

   for (int i=0; i<4; i++){
       adv_data[start_idx+i] = databytes[i];
   }
}

static void update_adv_data_u32(uint8_t *adv_data, uint8_t offset, uint32_t val)
{
  if (val == 0u) {
    val = 1u; /* keep slot visible for clients that drop all-zero payloads */
  }
  memcpy(&adv_data[offset], &val, sizeof(val));
}
 // 3. GATT items
 uint8_t flags = SL_BT_SM_CONFIGURATION_BONDING_REQUIRED;     // Encryption requires bonding

 static uint8_t connection = 0xFF;

// 4. Rest of code
static sl_sleeptimer_timer_handle_t periodic_timer;
static sl_sleeptimer_timer_handle_t adv_stop_timer;
 static bool ble_ready = false;
 static bool spi_ready = false;
 static bool cycle_active = false;
 static bool advertising_active = false;
static volatile bool pending_cycle = false;
static volatile uint32_t dbg_wake_missed;
#if !DEBUG_ALWAYS_ADV
static volatile bool pending_adv_stop = false;
#endif
static volatile uint32_t dbg_cycle_start_while_active;
static void periodic_timer_callback(sl_sleeptimer_timer_handle_t *handle, void *data);
static void adv_stop_timer_callback(sl_sleeptimer_timer_handle_t *handle, void *data);
static void app_run_cycle_start(void);
static void app_try_start_cycle(void);

/*
 * RE0 bias scaffolding for fully differential AIN1(v-) vs RE0(v+) sensing path.
 * This performs the register sequence requested for startup:
 * 1) AFECON.DACBUFEN
 * 2) BUFSENCON.V1P1HPADCEN (datasheet naming sometimes refers to HS buffer)
 * 3) LPDACCON0: RSTEN=1 (MMR writes enabled), PWDEN=0 (LPDAC powered on)
 * 4) LPDACDAT0.DACIN12 = 0x69E
 * 5) LPDACSW0.SW4 = 1
 * 6) LPTIASW0: SW10=1, SW4=1, SW2=0
 * 7) LPTIACON0: potentiostat enabled (PA power-down cleared)
 */
static void ad5940_configure_re0_bias(void)
{
  uint32_t r;

  /* 1) Enable DC DAC buffer path in AFE core control. */
  r = AD5940_ReadReg(REG_AFE_AFECON);
  r |= BITM_AFE_AFECON_DACBUFEN;
  AD5940_WriteReg(REG_AFE_AFECON, r);

  /* 2) Enable 1.1V high-speed/high-power ADC common-mode buffer. */
  r = AD5940_ReadReg(REG_AFE_BUFSENCON);
  r |= BITM_AFE_BUFSENCON_V1P1HPADCEN;
  AD5940_WriteReg(REG_AFE_BUFSENCON, r);

  /* 3) Enable LPDAC data writes and power up LPDAC0. */
  r = AD5940_ReadReg(REG_AFE_LPDACCON0);
  r |= BITM_AFE_LPDACCON0_RSTEN;
  r &= ~BITM_AFE_LPDACCON0_PWDEN;
  AD5940_WriteReg(REG_AFE_LPDACCON0, r);

  /* 4) Program DACIN12 target while preserving DACIN6 field. */
  r = AD5940_ReadReg(REG_AFE_LPDACDAT0);
  r &= ~BITM_AFE_LPDACDAT0_DACIN12;
  r |= (RE0_BIAS_DACIN12_CODE & BITM_AFE_LPDACDAT0_DACIN12);
  AD5940_WriteReg(REG_AFE_LPDACDAT0, r);

  /* 5) Close LPDACSW0 SW4. */
  r = AD5940_ReadReg(REG_AFE_LPDACSW0);
  r |= (1u << 4);
  AD5940_WriteReg(REG_AFE_LPDACSW0, r);

  /* 6) Close LPTIA SW10/SW4 and force SW2 open. */
  r = AD5940_ReadReg(REG_AFE_LPTIASW0);
  r |= LPTIASW(10);
  r &= ~(LPTIASW(2)|LPTIASW(4));
  AD5940_WriteReg(REG_AFE_LPTIASW0, r);

  /* 7) Turn on potentiostat path (PA not powered down). */
  r = AD5940_ReadReg(REG_AFE_LPTIACON0);
  r &= ~BITM_AFE_LPTIACON0_PAPDEN;
  AD5940_WriteReg(REG_AFE_LPTIACON0, r);
}

static void periodic_timer_callback(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle;
  (void)data;
  if (dbg_wake_arm_tick != 0u) {
    uint64_t now_tick = sl_sleeptimer_get_tick_count64();
    dbg_wake_elapsed_ms = sl_sleeptimer_tick_to_ms((uint32_t)(now_tick - dbg_wake_arm_tick));
  } else {
    dbg_wake_elapsed_ms = 0u;
  }
  dbg_wake_fire_count++;
  dbg_periodic_isr_count++;
  if (pending_cycle || cycle_active) {
    dbg_wake_missed++;
  }
  pending_cycle = true;
  sl_bt_external_signal(SIG_CYCLE_START);
}

static void adv_stop_timer_callback(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle;
  (void)data;
#if !DEBUG_ALWAYS_ADV
  pending_adv_stop = true;
#endif
}

static void app_try_start_cycle(void)
{
  if (!ble_ready || cycle_active || !pending_cycle) {
    return;
  }
  pending_cycle = false;
  app_run_cycle_start();
}

 int16_t set_min;
 int16_t set_max;

 /**************************************************************************//**
  * @brief Long ass SPI startup sequence
  *****************************************************************************/
 void spi_init_sequence(){
   init_stage = 1u;
       AD5940_MCUResourceInit(NULL);
       init_stage = 2u;

       /* Manual reset pulse to isolate HWReset/delay issues. */
       AD5940_RstClr();
       for (volatile uint32_t i = 0; i < 200000u; i++) { }
       AD5940_RstSet();
       for (volatile uint32_t i = 0; i < 500000u; i++) { }
       init_stage = 3u;

       /*
        * CHIPID is only reliable after the post-reset register table (same as
        * AD5940_Initialize in ad5940.c). Reading 0x0404 immediately after reset
        * often yields garbage (e.g. 0x0101), not 0x5502.
        */
       AD5940_Initialize();
       ad5940_configure_re0_bias();
       /* Let register file settle; first CHIPID read can be garbage if too soon. */
       AD5940_Delay10us(200); /* 2 ms */
       /* Retry: marginal SPI can return 0x8080 once; sensor2 shows last attempt (live canary). */
       for (uint8_t cid_try = 0; cid_try < 16u; cid_try++) {
         chip_id_manual = (uint16_t)AD5940_ReadReg(REG_AFECON_CHIPID);
         if (((uint32_t)chip_id_manual & CHIPID_FAMILY_MASK) == CHIPID_FAMILY_AD594) {
           break;
         }
         AD5940_Delay10us(100); /* 1 ms between tries */
       }
       spi_step = 22u;
       /*
        * ADC config/enable here inshallah
        */
       // structs

       FIFOCfg_Type fifocfg; // covers fifo setup and cmd setup
         fifocfg.FIFOEn = 0b1; // on
         fifocfg.FIFOMode = 0b11; // stream
         fifocfg.FIFOSize = 0b001; // 2kB sram
         fifocfg.FIFOSrc = 0b011; // sinc2 out
         fifocfg.FIFOThresh = 0b0; // idk what this is

       ADCFilterCfg_Type adcfilter_cfg;
         adcfilter_cfg.ADCSinc3Osr = ADCSINC3OSR_5; // oversampling rate for first filter
         adcfilter_cfg.ADCSinc2Osr = ADCSINC2OSR_178; // oversampling rate for 2nd
         adcfilter_cfg.ADCAvgNum = ADCAVGNUM_2; // doesnt matter much methinks
         adcfilter_cfg.ADCRate = ADCRATE_800KHZ; // low power rate
         adcfilter_cfg.BpNotch = bFALSE; // notch filter rejects AC noise. increases setup time to 37ms from 3ms
         // ^ ... big accuracy vs speed concern here
         adcfilter_cfg.BpSinc3 = bFALSE; // do not pypass this...
         adcfilter_cfg.Sinc2NotchEnable = 0b1; // unnecessary since no AC power

       ADCBaseCfg_Type adc_cfg;
         adc_cfg.ADCMuxP = ADCMUXP_VRE0; // re0 for this test
         adc_cfg.ADCMuxN = ADCMUXN_AIN1; // ain1 for this test
         adc_cfg.ADCPga = ADCPGA_1P5;

       // function calls
       AD5940_FIFOCfg(&fifocfg);
       AD5940_ADCRepeatCfgS(0b1); // take a single measurement. below enables

       uint32_t r = AD5940_ReadReg(REG_AFE_REPEATADCCNV);
       r |= BITM_AFE_REPEATADCCNV_EN;   /* set EN */
       AD5940_WriteReg(REG_AFE_REPEATADCCNV, r);

       AD5940_WriteReg(REG_AFE_ADCBUFCON, 0x005F3D0F); // recommended value
       r = AD5940_ReadReg(REG_AFE_BUFSENCON);  // enable necessary reference 1.82v high speed
       r |= BITM_AFE_BUFSENCON_V1P8HPADCEN;
       AD5940_WriteReg(REG_AFE_BUFSENCON, r);
         // ^buffering, apparently good for preventing voltage drop for pot measurements

       AD5940_AFEPwrBW(AFEPWR_LP, AFEBW_50KHZ); // seems reasonable for low frequency signal
       AD5940_ADCFilterCfgS(&adcfilter_cfg);
       AD5940_ADCBaseCfgS(&adc_cfg);

       AD5940_ADCPowerCtrlS(bTRUE); // turn on adc
       r = AD5940_ReadReg(REG_AFE_AFECON); // turn on sinc2 filter
       r |= BITM_AFE_AFECON_SINC2EN;
       AD5940_WriteReg(REG_AFE_AFECON, r);


       // wait a long time
       AD5940_Delay10us(8000); // 80ms
       // clear stale flags and arm data-ready interrupt before conversion
       AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY);
       AD5940_INTCCfg(AFEINTC_0, AFEINTSRC_SINC2RDY, bTRUE);
       // enable conversion
       AD5940_ADCConvtCtrlS(bTRUE);
 }

 // Read latest FIFO sample when SINC2 data-ready is asserted, or if FIFO has data (INTC can be missed).
 static bool spi_read(uint32_t *sample_out)
 {
   spiflags = AD5940_INTCGetFlag(AFEINTC_0);
   good = AD5940_INTCTestFlag(AFEINTC_0, AFEINTSRC_SINC2RDY) == bTRUE; // sinc2 is filtered reading
   if (!good && (AD5940_FIFOGetCnt() == 0u)) {
     return false;
   }
   *sample_out = AD5940_ReadReg(REG_AFE_DATAFIFORD);
   AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY);
   return true;
 }

 static void spi_on(void)
 {
   spi_init_sequence();
   spi_ready = true;
 }

 static void spi_off(void)
 {
   if (!spi_ready) {
     return;
   }

   uint32_t afecon = AD5940_ReadReg(REG_AFE_AFECON);
     afecon &= ~(BITM_AFE_AFECON_SINC2EN | BITM_AFE_AFECON_DACBUFEN);
     AD5940_WriteReg(REG_AFE_AFECON, afecon);

     /* 2. Power down precision buffers in BUFSENCON */
     AD5940_WriteReg(REG_AFE_BUFSENCON, 0x0);

     /* 3. Power down LPDAC and Potentiostat (PA) */
     uint32_t lpdaccon = AD5940_ReadReg(REG_AFE_LPDACCON0);
     lpdaccon |= BITM_AFE_LPDACCON0_PWDEN; // PWDEN is active high for power down
     AD5940_WriteReg(REG_AFE_LPDACCON0, lpdaccon);

     uint32_t lptiacon = AD5940_ReadReg(REG_AFE_LPTIACON0);
     lptiacon |= BITM_AFE_LPTIACON0_PAPDEN; // Power down PA
     AD5940_WriteReg(REG_AFE_LPTIACON0, lptiacon);

     /* 4. Global Hibernate */
     AD5940_SleepKeyCtrlS(SLPKEY_UNLOCK);
     AD5940_EnterSleepS();

     spi_ready = false;
   }

#if !DEBUG_ALWAYS_ADV
static void app_run_adv_stop(void)
{
  sl_status_t sc;
  uint64_t now_tick = sl_sleeptimer_get_tick_count64();
  if (dbg_advstop_arm_tick != 0u) {
    dbg_advstop_elapsed_ms = sl_sleeptimer_tick_to_ms((uint32_t)(now_tick - dbg_advstop_arm_tick));
  } else {
    dbg_advstop_elapsed_ms = 0u;
  }
  if (advertising_active) {
    sc = sl_bt_advertiser_stop(advertising_set_handle);
    app_assert_status(sc);
    advertising_active = false;
  }
  if (connection != 0xFF) {
    sc = sl_bt_connection_close(connection);
    app_assert_status(sc);
    connection = 0xFF;
  }
  spi_off();
  cycle_active = false;
  /* If wakes stacked while we were active, run the next cycle ASAP. */
  if (pending_cycle) {
    /* Let app_process_action handle the actual cycle start; just ensure we don't wait a full period. */
    sl_bt_external_signal(SIG_CYCLE_START);
  }
}
#endif

static void app_run_cycle_start(void)
{
  sl_status_t sc;
  static uint32_t last_start_isr_count;
  static uint64_t last_cycle_start_tick;
  uint64_t cycle_t0;
  uint64_t spi_t0;
  uint64_t spi_t1;
  uint64_t cycle_t1;

  cycle_active = true;
  cycle_t0 = sl_sleeptimer_get_tick_count64();
  if (last_cycle_start_tick == 0u) {
    dbg_intercycle_ms = 0u;
  } else {
    dbg_intercycle_ms = sl_sleeptimer_tick_to_ms((uint32_t)(cycle_t0 - last_cycle_start_tick));
  }
  last_cycle_start_tick = cycle_t0;
  dbg_cycle_start_isr_delta = dbg_periodic_isr_count - last_start_isr_count;
  last_start_isr_count = dbg_periodic_isr_count;

  chapter += 1;
  dbg_cycle_counter++;
  chpg = create_chapter_page_composite();
  update_adv_index(adv_data, chpg);

  spi_t0 = sl_sleeptimer_get_tick_count64();
  spi_on();

  if (spi_ready) {
    bool have_sample = false;
    dbg_spi_attempts = 0u;
    for (uint8_t i = 0; i < 20; i++) {
      dbg_spi_attempts++;
      if (spi_read((uint32_t *)&sample)) {
        have_sample = true;
        break;
      }
      if (i == 5u) {
        AD5940_ADCConvtCtrlS(bFALSE);
        AD5940_INTCClrFlag(AFEINTSRC_SINC2RDY);
        AD5940_Delay10us(500);
        AD5940_ADCConvtCtrlS(bTRUE);
      }
      AD5940_Delay10us(1000);
    }
    dbg_have_sample = have_sample ? 1u : 0u;
    if (have_sample) {
      sensor_1 = spiread_to_float(POTENTIOMETRIC, sample);
    } else {
      sensor_1 = SENSOR1_INVALID_SENTINEL;
    }
  }
  spi_t1 = sl_sleeptimer_get_tick_count64();
  dbg_spi_ms = sl_sleeptimer_tick_to_ms((uint32_t)(spi_t1 - spi_t0));
  update_adv_data(adv_data, OFFSET_1, sensor_1);
  {
    uint32_t chipid_ok =
        (((uint32_t)chip_id_manual & CHIPID_FAMILY_MASK) == CHIPID_FAMILY_AD594) ? 1u : 0u;
    /* s2 high16 = monotonic cycle counter; low16 = chipid */
    uint32_t s2 = ((uint32_t)chip_id_manual & 0xFFFFu)
                | (((uint32_t)dbg_cycle_counter) << 16);
    /* Final truth telemetry:
     * s3: [31:24] wake_fire_count(lsb), [23:16] wake_arm_count(lsb), [15:0] process_count(lsb16)
     * s4: [31] cycle_active, [30] pending_cycle, [29] pending_adv_stop, [28] wake_missed_nonzero,
     *     [27:16] reserved, [15:0] advstop_elapsed_ms
     */
    uint32_t s3 = (((uint32_t)dbg_wake_fire_count & 0xFFu) << 24)
                | (((uint32_t)dbg_wake_arm_count & 0xFFu) << 16)
                | ((uint32_t)(dbg_process_count & 0xFFFFu));
    uint32_t s4 = ((cycle_active ? 1u : 0u) << 31)
                | ((pending_cycle ? 1u : 0u) << 30)
#if !DEBUG_ALWAYS_ADV
                | ((pending_adv_stop ? 1u : 0u) << 29)
#else
                | (0u << 29)
#endif
                | ((dbg_wake_missed > 0u ? 1u : 0u) << 28)
                | ((dbg_advstop_elapsed_ms > 0xFFFFu ? 0xFFFFu : dbg_advstop_elapsed_ms) & 0xFFFFu);
    update_adv_data_u32(adv_data, OFFSET_2, s2);
    update_adv_data_u32(adv_data, OFFSET_3, s3);
    update_adv_data_u32(adv_data, OFFSET_4, s4);
  }

#if ADVERT_MODE
  if (advertising_active) {
    sc = sl_bt_advertiser_stop(advertising_set_handle);
    app_assert_status(sc);
    advertising_active = false;
  }
  sc = sl_bt_legacy_advertiser_set_data(advertising_set_handle,
                                        sl_bt_advertiser_advertising_data_packet,
                                        sizeof(adv_data),
                                        adv_data);
  app_assert_status(sc);
  sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                     sl_bt_legacy_advertiser_non_connectable);
  app_assert_status(sc);
#else
  sc = sl_bt_gatt_server_write_attribute_value(
          gattdb_readings,
          0,
          sizeof(adv_data),
          adv_data);
  app_assert_status(sc);
  if (advertising_active) {
    sc = sl_bt_advertiser_stop(advertising_set_handle);
    app_assert_status(sc);
    advertising_active = false;
  }
  sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                     sl_bt_legacy_advertiser_connectable);
  app_assert_status(sc);
#endif
  advertising_active = true;
  cycle_t1 = sl_sleeptimer_get_tick_count64();
  dbg_cycle_ms = sl_sleeptimer_tick_to_ms((uint32_t)(cycle_t1 - cycle_t0));

#if !DEBUG_ALWAYS_ADV
  dbg_advstop_arm_tick = sl_sleeptimer_get_tick_count64();
  sc = sl_sleeptimer_restart_timer_ms(&adv_stop_timer,
                                      ADV_DURATION_MS,
                                      adv_stop_timer_callback,
                                      NULL,
                                      0,
                                      0);
  app_assert_status(sc);
  /* Diagnostic: do not gate next cycle on ADV_STOP. */
  spi_off();
  cycle_active = false;
#else
  spi_off();
  cycle_active = false;
#endif
}

 /**************************************************************************//**
  * Application Init.
  *****************************************************************************/
 SL_WEAK void app_init(void)
 {
   /////////////////////////////////////////////////////////////////////////////
   // Put your additional application init code here!                         //
   // This is called once during start-up.                                    //
   /////////////////////////////////////////////////////////////////////////////
  dbg_wake_arm_count++;
  dbg_wake_arm_tick = sl_sleeptimer_get_tick_count64();
  app_assert_status(sl_sleeptimer_start_periodic_timer_ms(&periodic_timer,
                                                           WAKE_PERIOD_MS,
                                                           periodic_timer_callback,
                                                           NULL,
                                                           0,
                                                           0));
 }

 /**************************************************************************//**
  * Application Process Action.
  *****************************************************************************/
 SL_WEAK void app_process_action(void)
 {
  dbg_process_count++;
#if !DEBUG_ALWAYS_ADV
  if (pending_adv_stop) {
    pending_adv_stop = false;
    app_run_adv_stop();
  }
#endif
  app_try_start_cycle();
 }

 /**************************************************************************//**
  * Bluetooth stack event handler.
  * This overrides the dummy weak implementation.
  *
  * @param[in] evt Event coming from the Bluetooth stack.
  *****************************************************************************/
 void sl_bt_on_event(sl_bt_msg_t *evt)
 {
   sl_status_t sc;

   switch (SL_BT_MSG_ID(evt->header)) {

     case sl_bt_evt_system_boot_id:
       // reduce power
       sl_bt_system_halt(1);
       sl_bt_system_set_tx_power(-200,
                               -150,
                               &set_min,
                               &set_max);
       sl_bt_system_halt(0);
       // proceed
       ble_ready = true;

       // Create an advertising set now that stack is ready
       sc = sl_bt_advertiser_create_set(&advertising_set_handle);
       app_assert_status(sc);

       app_assert_status(sc);
       #if ADVERT_MODE
           sc = sl_bt_legacy_advertiser_set_data(advertising_set_handle,
                                              sl_bt_advertiser_advertising_data_packet,
                                              sizeof(adv_data),
                                              adv_data);
       #else
           sc = sl_bt_sm_store_bonding_configuration(2,0); // max 2 devices, fail if more are tried
           sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                        sl_bt_advertiser_general_discoverable);
       #endif
       // Configure advertising timing
       sc = sl_bt_advertiser_set_timing(
         advertising_set_handle,
         (uint16_t)(ADV_INTERVAL_MS * 1.6), // min interval (0.625ms units)
         (uint16_t)(ADV_INTERVAL_MS * 1.6), // max interval
         0, 0);

    app_assert_status(sc);
     break;
    // add support for opening a connection as well

    case sl_bt_evt_system_soft_timer_id:
      break;

    case sl_bt_evt_system_external_signal_id:
      if (evt->data.evt_system_external_signal.extsignals & SIG_CYCLE_START) {
        app_try_start_cycle();
      }
      break;

     case sl_bt_evt_connection_opened_id:
       // enable bonding
       sc = sl_bt_sm_configure(flags, sl_bt_sm_io_capability_displayonly);
       //sc = sl_bt_sm_delete_bondings(); // new
       sc = sl_bt_sm_set_bondable_mode(1);
       app_assert_status(sc);

       // establish bonding
       connection = evt->data.evt_connection_opened.connection;
         if (evt->data.evt_connection_opened.bonding == 0xFF) { // 0xff means no bond exists
           // No existing bond: start pairing/bonding
           sl_bt_sm_increase_security(connection);
         }

       break;

     default:
       break;
   }
 }


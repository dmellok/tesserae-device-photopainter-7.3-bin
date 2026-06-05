/* AXP2101 PMIC wrapper. See pmic.h for the contract. */

#include "pmic.h"
#include "app_config.h"

#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"     /* esp_rom_delay_us */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pmic";

/* AXP2101 register addresses (from Waveshare's bundled XPowersLib, file
 *   01_Example/.../components/pmicpower/src/REG/AXP2101Constants.h ).
 * Only the registers we actually touch are inlined here. */
#define AXP2101_REG_STATUS1               0x00   /* battery present in bit 3 */
#define AXP2101_REG_ADC_DATA_RESULT0      0x34   /* VBAT high (5 bits)       */
#define AXP2101_REG_ADC_DATA_RESULT1      0x35   /* VBAT low  (8 bits)       */
#define AXP2101_REG_INPUT_CURRENT_LIMIT   0x16   /* VBUS current cap         */
#define AXP2101_REG_CHG_CURRENT           0x62   /* constant-current setting */
#define AXP2101_REG_CHG_TERMINATION       0x63   /* termination current      */
#define AXP2101_REG_LDO_ONOFF_CTRL0       0x90   /* ALDO1..4 in bits 0..3    */
#define AXP2101_REG_LDO_VOL0_CTRL         0x92   /* ALDO1 voltage            */
#define AXP2101_REG_LDO_VOL1_CTRL         0x93   /* ALDO2 voltage            */
#define AXP2101_REG_LDO_VOL2_CTRL         0x94   /* ALDO3 voltage            */
#define AXP2101_REG_LDO_VOL3_CTRL         0x95   /* ALDO4 voltage            */
#define AXP2101_REG_BAT_PERCENT_DATA      0xA4   /* fuel-gauge percent       */

/* ALDOn = 0.5V + N * 0.1V, encoded in the low 5 bits of LDO_VOLn_CTRL. */
#define ALDO_VOL_STEP_MV   100
#define ALDO_VOL_MIN_MV    500
#define ALDO_VOL_REG_3V3   ((3300 - ALDO_VOL_MIN_MV) / ALDO_VOL_STEP_MV)   /* 28 */

/* VBUS current limits (datasheet Table 7-16): low 4 bits of reg 0x16.
 *   1500 mA == 0x05, 2000 mA == 0x06. The reference picks 2 A. */
#define VBUS_CURRENT_LIMIT_2A   0x06

/* Constant charge current 500 mA -- low 5 bits of reg 0x62, step 25 mA
 * from 0..200, then 100 mA from 200..1000. 500 mA == 0x09. */
#define CHG_CURRENT_500MA   0x09

/* Termination current 25 mA -- low 4 bits of reg 0x63, 25 mA per step. */
#define CHG_TERMINATION_25MA   0x00

static i2c_master_bus_handle_t  s_i2c_bus  = NULL;
static i2c_master_dev_handle_t  s_axp2101  = NULL;
static bool                     s_inited   = false;

/* --------------------------- I2C plumbing --------------------------- */

static esp_err_t axp_read(uint8_t reg, uint8_t *out) {
    return i2c_master_transmit_receive(s_axp2101, &reg, 1, out, 1, 50);
}

static esp_err_t axp_write(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_axp2101, buf, sizeof(buf), 50);
}

/* Read-modify-write a 5-bit voltage selector while preserving the top
 * 3 control bits in the same register. */
static esp_err_t axp_set_voltage_reg(uint8_t reg, uint8_t code) {
    uint8_t v = 0;
    esp_err_t err = axp_read(reg, &v);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t nv = (uint8_t)((v & 0xE0) | (code & 0x1F));
    if (nv == v) {
        return ESP_OK;
    }
    return axp_write(reg, nv);
}

/* --------------------------- public API ----------------------------- */

esp_err_t pmic_init(void) {
    if (s_inited) {
        return ESP_OK;
    }

    /* Wake the AXP2101 by pulsing its IRQ pin (GPIO21) LOW for >16 ms
     * before touching the bus. After certain PMIC sleep states the
     * chip powers its I2C interface DOWN; the IRQ pin is the documented
     * wake source (REG26H[4]). Without this, every transaction below
     * returns ESP_ERR_INVALID_STATE because the slave never ACKs.
     * Doing the wake before the bus rescue means the chip is alive by
     * the time our 9 SCL pulses run, so it sees a normal STOP rather
     * than a stream of phantom bits during its wake-up window. */
    {
        gpio_config_t irq_conf = {
            .pin_bit_mask = (1ULL << PMIC_PIN_IRQ),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&irq_conf);
        gpio_set_level(PMIC_PIN_IRQ, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(PMIC_PIN_IRQ, 1);
        vTaskDelay(pdMS_TO_TICKS(300));   /* generous post-wake settle */
        ESP_LOGI(TAG, "AXP2101 IRQ wake pulse done (pre-rescue)");
    }

    /* Manual I2C bus rescue. If a slave is mid-byte from a previous
     * power-cycle (SDA held LOW waiting for more SCL pulses), the
     * master can't initiate a START and every transaction will return
     * ESP_ERR_INVALID_STATE.
     *
     * Standard recovery, ported from aitjcize/esp32-photoframe
     * (components/board_hal/src/driver_waveshare_photopainter_73.c):
     *
     *   1. Configure SCL and SDA as open-drain outputs with pull-ups.
     *      Open-drain means we force LOW (drive 0) or release HIGH
     *      (drive 1 = high-Z, pull-up wins). Slaves can only hold SDA;
     *      we're guaranteed master of SCL.
     *   2. Toggle SCL 9 times. Each pulse clocks one bit out of the
     *      stuck slave; after at most 9 clocks it finishes its byte
     *      and releases SDA.
     *   3. Issue a STOP condition (SDA low->high while SCL high) so
     *      the slave returns to idle.
     *   4. gpio_reset_pin() detaches the pins so the I2C driver can
     *      re-claim them via IOMUX. */
    {
        gpio_set_direction(PMIC_I2C_SCL, GPIO_MODE_OUTPUT_OD);
        gpio_set_direction(PMIC_I2C_SDA, GPIO_MODE_OUTPUT_OD);
        gpio_set_pull_mode(PMIC_I2C_SCL, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(PMIC_I2C_SDA, GPIO_PULLUP_ONLY);
        for (int i = 0; i < 9; i++) {
            gpio_set_level(PMIC_I2C_SCL, 0); esp_rom_delay_us(5);
            gpio_set_level(PMIC_I2C_SCL, 1); esp_rom_delay_us(5);
        }
        /* STOP: SDA low, SCL high, SDA high (LH transition). */
        gpio_set_level(PMIC_I2C_SDA, 0); esp_rom_delay_us(5);
        gpio_set_level(PMIC_I2C_SCL, 1); esp_rom_delay_us(5);
        gpio_set_level(PMIC_I2C_SDA, 1); esp_rom_delay_us(5);
        gpio_reset_pin(PMIC_I2C_SCL);
        gpio_reset_pin(PMIC_I2C_SDA);
        ESP_LOGI(TAG, "I2C bus rescue: 9 SCL pulses + STOP issued");
    }


    /* I2C0 master bus, internal pull-ups enabled (the AXP2101 and SHTC3
     * modules both have on-board pulls but enabling the internal ones
     * too is harmless and rescues us on hand-wired boards). */
    if (s_i2c_bus == NULL) {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port          = PMIC_I2C_PORT,
            .scl_io_num        = PMIC_I2C_SCL,
            .sda_io_num        = PMIC_I2C_SDA,
            .clk_source        = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags             = { .enable_internal_pullup = true },
        };
        esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (s_axp2101 == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = PMIC_AXP2101_ADDR,
            .scl_speed_hz    = PMIC_I2C_HZ,
        };
        esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_axp2101);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c add device failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    /* Probe: a successful read of STATUS1 tells us the chip is alive. */
    uint8_t status = 0;
    esp_err_t err = axp_read(AXP2101_REG_STATUS1, &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 not responding at 0x%02X: %s",
                 PMIC_AXP2101_ADDR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AXP2101 alive, STATUS1=0x%02X", status);

    /* Match Waveshare's reference init: cap VBUS at 2 A, charge at 500 mA
     * constant-current with 25 mA termination. We *don't* touch DCDC1
     * (the SoC 3V3 rail) -- the bootrom and ESP-IDF brownout detector
     * are already happy with whatever the factory set. */
    axp_set_voltage_reg(AXP2101_REG_INPUT_CURRENT_LIMIT, VBUS_CURRENT_LIMIT_2A);
    axp_write(AXP2101_REG_CHG_CURRENT,    CHG_CURRENT_500MA);
    axp_write(AXP2101_REG_CHG_TERMINATION, CHG_TERMINATION_25MA);

    /* ALDO1..4 to 3.3 V (unknown which rail does what, but Waveshare's
     * factory firmware turns all four on together at 3.3 V on every
     * boot, so doing the same is the safe default). */
    axp_set_voltage_reg(AXP2101_REG_LDO_VOL0_CTRL, ALDO_VOL_REG_3V3);
    axp_set_voltage_reg(AXP2101_REG_LDO_VOL1_CTRL, ALDO_VOL_REG_3V3);
    axp_set_voltage_reg(AXP2101_REG_LDO_VOL2_CTRL, ALDO_VOL_REG_3V3);
    axp_set_voltage_reg(AXP2101_REG_LDO_VOL3_CTRL, ALDO_VOL_REG_3V3);

    /* Energise the analog rails so the panel and any peripherals
     * downstream have power before we drive their data lines. */
    err = pmic_rails_set(true);
    if (err != ESP_OK) {
        return err;
    }

    s_inited = true;
    return ESP_OK;
}

esp_err_t pmic_rails_set(bool enabled) {
    /* ALDO1..4 -> bits 0..3 of LDO_ONOFF_CTRL0. The reference flips them
     * one by one; we batch the write so the panel and SD don't see
     * staggered rail transitions on every wake. */
    uint8_t v = 0;
    esp_err_t err = axp_read(AXP2101_REG_LDO_ONOFF_CTRL0, &v);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t mask = 0x0F;   /* ALDO1..4 = bits 0..3 */
    uint8_t nv = enabled ? (v | mask) : (v & ~mask);
    if (nv == v) {
        return ESP_OK;
    }
    err = axp_write(AXP2101_REG_LDO_ONOFF_CTRL0, nv);
    if (err == ESP_OK && enabled) {
        /* Panel ICs need a few ms after rail rise before talking SPI. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return err;
}

uint16_t pmic_battery_mv(void) {
    if (!s_inited) {
        return 0;
    }
    uint8_t hi = 0, lo = 0;
    if (axp_read(AXP2101_REG_ADC_DATA_RESULT0, &hi) != ESP_OK) {
        return 0;
    }
    if (axp_read(AXP2101_REG_ADC_DATA_RESULT1, &lo) != ESP_OK) {
        return 0;
    }
    /* AXP2101 datasheet 7.4.1: VBAT = (H[4:0] << 8 | L) mV. */
    return (uint16_t)(((hi & 0x1F) << 8) | lo);
}

int pmic_battery_pct(void) {
    if (!s_inited) {
        return -1;
    }
    uint8_t v = 0;
    if (axp_read(AXP2101_REG_BAT_PERCENT_DATA, &v) != ESP_OK) {
        return -1;
    }
    if (v > 100) {
        return -1;   /* sentinel: gauge not yet calibrated */
    }
    return (int) v;
}

bool pmic_battery_present(void) {
    if (!s_inited) {
        return false;
    }
    uint8_t v = 0;
    if (axp_read(AXP2101_REG_STATUS1, &v) != ESP_OK) {
        return false;
    }
    /* STATUS1 bit 3 (BATT_PRESENT): 1 = battery on, 0 = absent. */
    return (v & (1u << 3)) != 0;
}

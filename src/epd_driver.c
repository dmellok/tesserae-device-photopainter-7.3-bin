#include "epd_driver.h"
#include "pmic.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";

/* --- panel command opcodes (from Waveshare PhotoPainter ref) --- */
#define EPD_CMD_PSR          0x00   /* panel setting                  */
#define EPD_CMD_PWR          0x01   /* power setting                  */
#define EPD_CMD_POF          0x02   /* power off                      */
#define EPD_CMD_PFS          0x03   /* power frame settings           */
#define EPD_CMD_PON          0x04   /* power on                       */
#define EPD_CMD_BTST1        0x05   /* booster soft start 1           */
#define EPD_CMD_BTST2        0x06   /* booster soft start 2           */
#define EPD_CMD_BTST3        0x08   /* booster soft start 3           */
#define EPD_CMD_DTM          0x10   /* data transfer (frame data)     */
#define EPD_CMD_DRF          0x12   /* display refresh                */
#define EPD_CMD_30           0x30   /* PLL control                    */
#define EPD_CMD_CDI          0x50   /* VCOM/data interval setting     */
#define EPD_CMD_TCON         0x60   /* TCON setting                   */
#define EPD_CMD_TRES         0x61   /* resolution (800 x 480)         */
#define EPD_CMD_84           0x84   /* (vendor-undocumented)          */
#define EPD_CMD_INIT_UNLOCK  0xAA   /* required preamble              */
#define EPD_CMD_PWS          0xE3   /* power saving                   */

/* Canned init parameter blobs (do NOT edit). Cross-reference:
 *   ESP32-S3-PhotoPainter / 01_Example / components / port_bsp /
 *   display_bsp.cpp -> ePaperPort::EPD_Init                       */
static const uint8_t INIT_UNLOCK_V[] = {0x49, 0x55, 0x20, 0x08, 0x09, 0x18};
static const uint8_t PWR_V[]         = {0x3F};
static const uint8_t PSR_V[]         = {0x5F, 0x69};
static const uint8_t PFS_V[]         = {0x00, 0x54, 0x00, 0x44};
static const uint8_t BTST1_V[]       = {0x40, 0x1F, 0x1F, 0x2C};
static const uint8_t BTST2_V[]       = {0x6F, 0x1F, 0x17, 0x49};
static const uint8_t BTST3_V[]       = {0x6F, 0x1F, 0x1F, 0x22};
static const uint8_t CMD_30_V[]      = {0x03};
static const uint8_t CDI_V[]         = {0x3F};
static const uint8_t TCON_V[]        = {0x02, 0x00};
static const uint8_t TRES_V[]        = {0x03, 0x20, 0x01, 0xE0};   /* 800 x 480 */
static const uint8_t CMD_84_V[]      = {0x01};
static const uint8_t PWS_V[]         = {0x2F};
static const uint8_t DRF_V[]         = {0x00};
static const uint8_t POF_V[]         = {0x00};

static spi_device_handle_t s_spi;
static bool s_port_inited = false;
static bool s_panel_inited = false;

/* ---------- low-level pin/SPI wrappers ---------- */

static esp_err_t spi_tx_raw(const uint8_t *data, size_t len) {
    /* Chunk so we never blow past max_transfer_sz; 4 KiB keeps each
     * transaction short. The full-frame 192000-byte payload becomes
     * ~47 transactions -- still fast at 10 MHz. */
    const size_t CHUNK = 4096;
    spi_transaction_t t;
    for (size_t off = 0; off < len; off += CHUNK) {
        size_t n = (len - off > CHUNK) ? CHUNK : (len - off);
        memset(&t, 0, sizeof(t));
        t.length    = n * 8;
        t.tx_buffer = data + off;
        esp_err_t err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi tx fail at off=%u: %s",
                     (unsigned) off, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

static void send_cmd(uint8_t cmd) {
    gpio_set_level(EPD_PIN_DC, 0);
    gpio_set_level(EPD_PIN_CS, 0);
    spi_tx_raw(&cmd, 1);
    gpio_set_level(EPD_PIN_CS, 1);
}

static void send_data_buf(const uint8_t *buf, size_t len) {
    gpio_set_level(EPD_PIN_DC, 1);
    gpio_set_level(EPD_PIN_CS, 0);
    spi_tx_raw(buf, len);
    gpio_set_level(EPD_PIN_CS, 1);
}

static void cmd_with_data(uint8_t cmd, const uint8_t *buf, size_t len) {
    send_cmd(cmd);
    send_data_buf(buf, len);
}

/* Block until BUSY goes HIGH (idle). Panel pulls it LOW while busy.
 * A full Spectra 6 refresh takes ~25 s on the 7.3" panel; warn only
 * after 60 s. */
static void wait_idle(void) {
    int ticks = 0;
    bool warned = false;
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (!warned && ++ticks >= 6000) {
            ESP_LOGW(TAG, "BUSY still low after 60 s -- panel may be stuck");
            warned = true;
        }
    }
}

/* Reset pulse: high(50) -> low(20) -> high(50). Different shape from the
 * 13.3" board's 5-pulse sequence -- the Spectra 6 controller variants
 * have different reset-latch requirements. */
static void hw_reset(void) {
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
}

/* ---------- public API ---------- */

esp_err_t epd_port_init(void) {
    if (s_port_inited) {
        return ESP_OK;
    }

    gpio_config_t out = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << EPD_PIN_RST) |
                        (1ULL << EPD_PIN_DC)  |
                        (1ULL << EPD_PIN_CS),
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    gpio_config_t in = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << EPD_PIN_BUSY),
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    gpio_set_level(EPD_PIN_RST, 1);
    gpio_set_level(EPD_PIN_CS,  1);

    spi_bus_config_t bus = {
        .miso_io_num     = -1,
        .mosi_io_num     = EPD_PIN_MOSI,
        .sclk_io_num     = EPD_PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = EPD_BUF_BYTES,
    };
    spi_device_interface_config_t dev = {
        .clock_speed_hz = EPD_SPI_HZ,
        .mode           = 0,
        .spics_io_num   = -1,         /* we drive CS by hand */
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(EPD_SPI_HOST, &dev, &s_spi));

    s_port_inited = true;
    return ESP_OK;
}

void epd_init(void) {
    /* Panel rail comes up via the PMIC. pmic_rails_set(true) waits 10 ms
     * after the rail rises before returning, so we're safe to talk SPI
     * immediately after. */
    pmic_rails_set(true);

    hw_reset();
    wait_idle();
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Init sequence in the order Waveshare's reference uses. The 0xAA
     * "unlock" command is unique to this Spectra 6 controller variant
     * and must come first. */
    cmd_with_data(EPD_CMD_INIT_UNLOCK, INIT_UNLOCK_V, sizeof(INIT_UNLOCK_V));
    cmd_with_data(EPD_CMD_PWR,         PWR_V,         sizeof(PWR_V));
    cmd_with_data(EPD_CMD_PSR,         PSR_V,         sizeof(PSR_V));
    cmd_with_data(EPD_CMD_PFS,         PFS_V,         sizeof(PFS_V));
    cmd_with_data(EPD_CMD_BTST1,       BTST1_V,       sizeof(BTST1_V));
    cmd_with_data(EPD_CMD_BTST2,       BTST2_V,       sizeof(BTST2_V));
    cmd_with_data(EPD_CMD_BTST3,       BTST3_V,       sizeof(BTST3_V));
    cmd_with_data(EPD_CMD_30,          CMD_30_V,      sizeof(CMD_30_V));
    cmd_with_data(EPD_CMD_CDI,         CDI_V,         sizeof(CDI_V));
    cmd_with_data(EPD_CMD_TCON,        TCON_V,        sizeof(TCON_V));
    cmd_with_data(EPD_CMD_TRES,        TRES_V,        sizeof(TRES_V));
    cmd_with_data(EPD_CMD_84,          CMD_84_V,      sizeof(CMD_84_V));
    cmd_with_data(EPD_CMD_PWS,         PWS_V,         sizeof(PWS_V));

    /* Power-on -- this charges the boost rails; the panel stays in this
     * state until POF. */
    send_cmd(EPD_CMD_PON);
    wait_idle();

    s_panel_inited = true;
    ESP_LOGI(TAG, "init complete");
}

/* PON -> re-send BTST2 -> DRF -> wait -> POF. The reference firmware
 * always re-issues BTST2 right before DRF; skipping it produces washed-
 * out reds and yellows. */
static void trigger_refresh(void) {
    cmd_with_data(EPD_CMD_BTST2, BTST2_V, sizeof(BTST2_V));

    cmd_with_data(EPD_CMD_DRF, DRF_V, sizeof(DRF_V));
    wait_idle();

    cmd_with_data(EPD_CMD_POF, POF_V, sizeof(POF_V));
    wait_idle();
    ESP_LOGI(TAG, "refresh done");
}

void epd_clear(uint8_t color) {
    /* Allocate the full 192000-byte panel buffer in PSRAM and memset
     * it. The 7.3" panel is small enough that one contiguous transfer
     * is fine -- no need for the split-buffer trick the 13.3" needs. */
    uint8_t *buf = heap_caps_malloc(EPD_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "OOM allocating %u-byte clear buffer",
                 (unsigned) EPD_BUF_BYTES);
        return;
    }
    memset(buf, (color << 4) | color, EPD_BUF_BYTES);

    cmd_with_data(EPD_CMD_DTM, buf, EPD_BUF_BYTES);
    free(buf);

    trigger_refresh();
}

void epd_display(const uint8_t *image) {
    /* Single contiguous DTM transfer. */
    cmd_with_data(EPD_CMD_DTM, image, EPD_BUF_BYTES);
    trigger_refresh();
}

void epd_show_color_bars(void) {
    /* Six horizontal bands, full panel width. Each band is HEIGHT/6 = 80
     * rows tall. We stream one packed row (EPD_WIDTH/2 = 400 bytes)
     * repeated N times per band rather than allocating a 192 KB buffer
     * for a constant pattern. */
    static const uint8_t palette[6] = {
        EPD_COL_BLACK, EPD_COL_WHITE,  EPD_COL_YELLOW,
        EPD_COL_RED,   EPD_COL_BLUE,   EPD_COL_GREEN,
    };
    const size_t ROW_BYTES = EPD_WIDTH / 2;   /* 400 */
    uint8_t row[ROW_BYTES];

    send_cmd(EPD_CMD_DTM);
    gpio_set_level(EPD_PIN_DC, 1);
    gpio_set_level(EPD_PIN_CS, 0);
    for (int b = 0; b < 6; b++) {
        uint8_t packed = (palette[b] << 4) | palette[b];
        memset(row, packed, ROW_BYTES);

        size_t band_h = EPD_HEIGHT / 6;
        if (b == 5) {
            band_h += EPD_HEIGHT % 6;   /* last band absorbs the remainder */
        }
        for (size_t r = 0; r < band_h; r++) {
            spi_tx_raw(row, ROW_BYTES);
        }
    }
    gpio_set_level(EPD_PIN_CS, 1);

    trigger_refresh();
}

void epd_show_palette_sweep(void) {
    /* 8 horizontal bands of HEIGHT/8 = 60 rows each, one per possible
     * nibble value 0x0..0x7. */
    const size_t ROW_BYTES = EPD_WIDTH / 2;   /* 400 */
    const size_t BAND_H    = EPD_HEIGHT / 8;  /* 60  */
    uint8_t row[ROW_BYTES];

    send_cmd(EPD_CMD_DTM);
    gpio_set_level(EPD_PIN_DC, 1);
    gpio_set_level(EPD_PIN_CS, 0);
    for (uint8_t n = 0; n < 8; n++) {
        uint8_t packed = (n << 4) | n;
        memset(row, packed, ROW_BYTES);
        for (size_t r = 0; r < BAND_H; r++) {
            spi_tx_raw(row, ROW_BYTES);
        }
    }
    gpio_set_level(EPD_PIN_CS, 1);

    trigger_refresh();
}

void epd_sleep(void) {
    /* Spectra 6 controllers don't have a separate "DEEP_SLEEP" opcode
     * -- the Waveshare reference simply leaves the chip in POF state
     * (which trigger_refresh() already left it in) and cuts the rail.
     * Dropping power is what brings the average current down; the chip
     * is just dead silicon below ~1.6 V on its VDD. */
    pmic_rails_set(false);
    gpio_set_level(EPD_PIN_RST, 0);
    s_panel_inited = false;
}

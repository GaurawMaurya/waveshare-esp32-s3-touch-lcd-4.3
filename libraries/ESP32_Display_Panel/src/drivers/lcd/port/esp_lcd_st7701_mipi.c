/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../esp_panel_lcd_conf_internal.h"
#if ESP_PANEL_DRIVERS_LCD_ENABLE_ST7701

#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include <stdlib.h>
#include <sys/cdefs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_st7701_interface.h"

#include "utils/esp_panel_utils_log.h"
#include "esp_utils_helpers.h"
#include "esp_panel_lcd_vendor_types.h"
#include "esp_lcd_st7789.h"

static const char *TAG = "st7701_mipi";

static esp_err_t panel_st7701_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7701_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7701_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7701_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st7701_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st7701_disp_on_off(esp_lcd_panel_t *panel, bool off);
static esp_err_t panel_st7701_sleep(esp_lcd_panel_t *panel, bool sleep);

typedef struct {
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save surrent value of LCD_CMD_COLMOD register
    const esp_panel_lcd_vendor_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int reset_level: 1;
    } flags;
    // To save the original functions of MIPI DPI panel
    esp_err_t (*del)(esp_lcd_panel_t *panel);
    esp_err_t (*init)(esp_lcd_panel_t *panel);
} st7701_panel_t;

esp_err_t esp_lcd_new_panel_st7701_mipi(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    esp_panel_lcd_vendor_config_t *vendor_config = (esp_panel_lcd_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor_config && vendor_config->mipi_config.dpi_config && vendor_config->mipi_config.dsi_bus, ESP_ERR_INVALID_ARG, TAG,
                        "invalid vendor config");

    esp_err_t ret = ESP_OK;
    st7701_panel_t *st7701 = (st7701_panel_t *)calloc(1, sizeof(st7701_panel_t));
    ESP_RETURN_ON_FALSE(st7701, ESP_ERR_NO_MEM, TAG, "no mem for st7701 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->color_space) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        st7701->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        st7701->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported rgb element order");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        st7701->colmod_val = 0x55;
        break;
    case 18: // RGB666
        st7701->colmod_val = 0x66;
        break;
    case 24: // RGB888
        st7701->colmod_val = 0x77;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    st7701->io = io;
    st7701->init_cmds = vendor_config->init_cmds;
    st7701->init_cmds_size = vendor_config->init_cmds_size;
    st7701->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st7701->flags.reset_level = panel_dev_config->flags.reset_active_high;

    // Create MIPI DPI panel
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_dpi(vendor_config->mipi_config.dsi_bus, vendor_config->mipi_config.dpi_config, &panel_handle), err, TAG,
                      "create MIPI DPI panel failed");
    ESP_LOGD(TAG, "new MIPI DPI panel @%p", panel_handle);

    // Save the original functions of MIPI DPI panel
    st7701->del = panel_handle->del;
    st7701->init = panel_handle->init;
    // Overwrite the functions of MIPI DPI panel
    panel_handle->del = panel_st7701_del;
    panel_handle->init = panel_st7701_init;
    panel_handle->reset = panel_st7701_reset;
    panel_handle->mirror = panel_st7701_mirror;
    panel_handle->invert_color = panel_st7701_invert_color;
    panel_handle->disp_on_off = panel_st7701_disp_on_off;
    panel_handle->disp_sleep = panel_st7701_sleep;
    panel_handle->user_data = st7701;
    *ret_panel = panel_handle;
    ESP_LOGD(TAG, "new st7701 panel @%p", st7701);

    return ESP_OK;
err:
    if (st7701) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
    }
    return ret;
}

static esp_err_t panel_st7701_del(esp_lcd_panel_t *panel)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;

    if (st7701->reset_gpio_num >= 0) {
        gpio_reset_pin(st7701->reset_gpio_num);
    }

    // Delete MIPI DPI panel
    st7701->del(panel);
    ESP_LOGD(TAG, "del st7701 panel @%p", st7701);
    free(st7701);

    return ESP_OK;
}

static esp_err_t panel_st7701_reset(esp_lcd_panel_t *panel)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7701->io;

    // perform hardware reset
    if (st7701->reset_gpio_num >= 0) {
        gpio_set_level(st7701->reset_gpio_num, st7701->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(st7701->reset_gpio_num, !st7701->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else if (io) { // perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(20)); // spec, wait at least 5ms before sending new command
    }

    return ESP_OK;
}

static const esp_panel_lcd_vendor_init_cmd_t vendor_specific_init_default[] = {
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t []){0x08}, 1, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t []){0x2c, 0x00}, 2, 0},
    {0xC1, (uint8_t []){0x10, 0x0C}, 2, 0},
    {0xC2, (uint8_t []){0x21, 0x0A}, 2, 0},
    {0xCC, (uint8_t []){0x10}, 1, 0},
    {0xB0, (uint8_t []){0x00, 0x0B, 0x12, 0x0D, 0x10, 0x06, 0x02, 0x08, 0x07, 0x1F, 0x04, 0x11, 0x0F, 0x29, 0x31, 0x1E}, 16, 0},
    {0xB1, (uint8_t []){0x00, 0x0B, 0x13, 0x0D, 0x11, 0x06, 0x03, 0x08, 0x07, 0x20, 0x04, 0x12, 0x11, 0x29, 0x31, 0x1E}, 16, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t []){0x5D}, 1, 0},
    {0xB1, (uint8_t []){0x72}, 1, 0},
    {0xB2, (uint8_t []){0x84}, 1, 0},
    {0xB3, (uint8_t []){0x80}, 1, 0},
    {0xB5, (uint8_t []){0x4D}, 1, 0},
    {0xB7, (uint8_t []){0x85}, 1, 0},
    {0xB8, (uint8_t []){0x20}, 1, 0},
    {0xC1, (uint8_t []){0x78}, 1, 0},
    {0xC2, (uint8_t []){0x78}, 1, 0},
    {0xD0, (uint8_t []){0x88}, 1, 0},
    {0xE0, (uint8_t []){0x80, 0x00, 0x02}, 3, 0},
    {0xE1, (uint8_t []){0x05, 0x00, 0x07, 0x00, 0x06, 0x00, 0x08, 0x00, 0x00, 0x33, 0x33}, 11, 0},
    {0xE2, (uint8_t []){0x00, 0x00, 0x30, 0x30, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}, 12, 0},
    {0xE3, (uint8_t []){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE4, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t []){0x0C, 0x78, 0x00, 0xE0, 0x0E, 0x7A, 0x00, 0xE0, 0x08, 0x74, 0x00, 0xE0, 0x0A, 0x76, 0x00, 0xE0}, 16, 0},
    {0xE6, (uint8_t []){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE7, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t []){0x0D, 0x79, 0x00, 0xE0, 0x0F, 0x7B, 0x00, 0xE0, 0x09, 0x75, 0x00, 0xE0, 0x0B, 0x77, 0x00, 0xE0}, 16, 0},
    {0xE9, (uint8_t []){0x36, 0x00}, 2, 0},
    {0xEB, (uint8_t []){0x00, 0x01, 0xE4, 0xE4, 0x44, 0x88, 0x40}, 7, 0},
    {0xED, (uint8_t []){0xA1, 0xC2, 0xFB, 0x0F, 0x67, 0x45, 0xFF, 0xFF, 0xFF, 0xFF, 0x54, 0x76, 0xF0, 0xBF, 0x2C, 0x1A}, 16, 0},
    {0xEF, (uint8_t []){0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F}, 6, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xE8, (uint8_t []){0x00, 0x0E}, 2, 0},
    {0xE8, (uint8_t []){0x00, 0x0C}, 2, 20},
    {0xE8, (uint8_t []){0x00, 0x00}, 2, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x29, (uint8_t []){0x00}, 0, 0},

    // {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x12}, 5, 0}, /* This part of the parameters can be used for screen self-test */
    // {0xD1, (uint8_t []){0x81}, 1, 0},
    // {0xD2, (uint8_t []){0x08}, 1, 0},
};

static esp_err_t panel_st7701_init(esp_lcd_panel_t *panel)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7701->io;
    const esp_panel_lcd_vendor_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    bool is_command2_disable = true;
    bool is_cmd_overwritten = false;

    uint8_t ID[3];
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_rx_param(io, 0x04, ID, 3), TAG, "read ID failed");
    ESP_LOGI(TAG, "LCD ID: %02X %02X %02X", ID[0], ID[1], ID[2]);

    // back to CMD_Page 0
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7701_CMD_CND2BKxSEL, (uint8_t []) {
        ST7701_CMD_BKxSEL_BYTE0, ST7701_CMD_BKxSEL_BYTE1, ST7701_CMD_BKxSEL_BYTE2, ST7701_CMD_BKxSEL_BYTE3, 0x00
    }, 5), TAG, "Write cmd failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7701->madctl_val,
    }, 1), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) {
        st7701->colmod_val,
    }, 1), TAG, "send command failed");
    ESP_LOGI(TAG, " st7701->madctl_val: 0x%x, st7701->colmod_val: 0x%x",  st7701->madctl_val, st7701->colmod_val);

    // vendor specific initialization, it can be different between manufacturers
    // should consult the LCD supplier for initialization sequence code
    if (st7701->init_cmds) {
        init_cmds = st7701->init_cmds;
        init_cmds_size = st7701->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(esp_panel_lcd_vendor_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++) {
        // Check if the command has been used or conflicts with the internal only when command2 is disable
        if (is_command2_disable && (init_cmds[i].data_bytes > 0)) {
            switch (init_cmds[i].cmd) {
            case LCD_CMD_MADCTL:
                is_cmd_overwritten = true;
                st7701->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            case LCD_CMD_COLMOD:
                is_cmd_overwritten = true;
                st7701->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            default:
                is_cmd_overwritten = false;
                break;
            }

            if (is_cmd_overwritten) {
                is_cmd_overwritten = false;
                ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence",
                         init_cmds[i].cmd);
            }
        }

        // Send command
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes),
                            TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));

        // Check if the current cmd is the command2 disable cmd
        if ((init_cmds[i].cmd == ST7701_CMD_CND2BKxSEL) && (init_cmds[i].data_bytes > 4)) {
            is_command2_disable = !(((uint8_t *)init_cmds[i].data)[4] & ST7701_CMD_CN2_BIT);
        }
    }
    ESP_LOGD(TAG, "send init commands success");

    ESP_RETURN_ON_ERROR(st7701->init(panel), TAG, "init MIPI DPI panel failed");

    return ESP_OK;
}

static esp_err_t panel_st7701_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7701->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_st7701_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7701->io;
    uint8_t sdir_val = 0;

    ESP_RETURN_ON_FALSE(io, ESP_FAIL, TAG, "Panel IO is deleted, cannot send command");
    // Control mirror through LCD command
    if (mirror_x) {
        sdir_val = ST7701_CMD_SS_BIT;
    } else {
        sdir_val = 0;
    }
    if (mirror_y) {
        st7701->madctl_val |= LCD_CMD_ML_BIT;
    } else {
        st7701->madctl_val &= ~LCD_CMD_ML_BIT;
    }

    // Enable the Command2 BK0
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7701_CMD_CND2BKxSEL, (uint8_t []) {
        ST7701_CMD_BKxSEL_BYTE0, ST7701_CMD_BKxSEL_BYTE1, ST7701_CMD_BKxSEL_BYTE2, ST7701_CMD_BKxSEL_BYTE3,
                                 ST7701_CMD_BKxSEL_BK0 | ST7701_CMD_CN2_BIT,
    }, 5), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7701_CMD_SDIR, (uint8_t[]) {
        sdir_val,
    }, 1), TAG, "send command failed");;

    // Disable Command2
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7701_CMD_CND2BKxSEL, (uint8_t []) {
        ST7701_CMD_BKxSEL_BYTE0, ST7701_CMD_BKxSEL_BYTE1, ST7701_CMD_BKxSEL_BYTE2, ST7701_CMD_BKxSEL_BYTE3, 0,
    }, 5), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7701->madctl_val,
    }, 1), TAG, "send command failed");;

    return ESP_OK;
}

static esp_err_t panel_st7701_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7701->io;
    int command = 0;

    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_st7701_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7701->io;
    int command = 0;
    if (sleep) {
        command = LCD_CMD_SLPIN;
    } else {
        command = LCD_CMD_SLPOUT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG,
                        "io tx param failed");
    vTaskDelay(pdMS_TO_TICKS(100));

    return ESP_OK;
}
#endif

#endif // ESP_PANEL_DRIVERS_LCD_ENABLE_ST7701

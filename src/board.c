#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "BOARD";
static bool s_done;

/* Interrupts that must not live on the Wi-Fi core are installed from a task pinned to core 1: the GPIO ISR service, and
 * the I2C driver (its ISR runs on the installing core). With the I2C ISR on core 0, TLS uploads delayed it enough for
 * transactions to time out; the legacy driver's FSM reset then storms the interrupt -> int_wdt reset (seen twice, 2026-10-23). */
static void isr_install_task(void *arg)
{
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));
    xSemaphoreGive((SemaphoreHandle_t)arg);
    vTaskDelete(NULL);
}

esp_err_t board_init(void)
{
    if (s_done) return ESP_OK;

    i2c_config_t i2c = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA, .scl_io_num = PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_DISABLE, .scl_pullup_en = GPIO_PULLUP_DISABLE,   /* R31/R32 on board */
        .master.clk_speed = I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &i2c));
    /* i2c_driver_install happens in isr_install_task (core 1), below */

    spi_bus_config_t spi3 = {
        .sclk_io_num = PIN_SPI3_SCK, .mosi_io_num = PIN_SPI3_MOSI, .miso_io_num = PIN_SPI3_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &spi3, SPI_DMA_CH_AUTO));

    spi_bus_config_t spi2 = {
        .sclk_io_num = PIN_SPI2_CLK, .mosi_io_num = PIN_SPI2_MOSI, .miso_io_num = PIN_SPI2_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 2112 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &spi2, SPI_DMA_CH_AUTO));

    /* GPIO ISR service installed from core 1 so every GPIO interrupt (IMU DRDY, BQ /INT) is serviced there */
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(isr_install_task, "isr_inst", 2048, done, 10, NULL, 1);
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);

    s_done = true;
    ESP_LOGI(TAG, "hw rev %d: I2C0 %d kHz, SPI3 (IMU), SPI2 (NAND) up, GPIO + I2C ISRs on core 1", PERCEPTIO_HW_REV, I2C_FREQ_HZ / 1000);
    return ESP_OK;
}

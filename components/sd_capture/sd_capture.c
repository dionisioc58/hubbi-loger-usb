#include "sd_capture.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_capture";
#define SD_MOUNT_POINT "/sdcard"
#define SD_FILE_PATH SD_MOUNT_POINT "/uart0_capture.log"

typedef struct {
    uint16_t length;
    uint8_t data[CONFIG_SD_CAPTURE_BLOCK_SIZE];
} capture_block_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static FILE *s_file;
static bool s_bus_initialized;
static bool s_fs_mounted;
static sdmmc_card_t *s_card;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static sd_capture_stats_t s_stats;

static void update_pending(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.blocks_pending = s_queue ? (uint32_t)uxQueueMessagesWaiting(s_queue) : 0;
    portEXIT_CRITICAL(&s_stats_lock);
}

static bool mount_and_open(void)
{
    if (s_file) return true;
    if (!s_fs_mounted) {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.max_freq_khz = CONFIG_SD_CAPTURE_SPI_FREQ_KHZ;
        spi_bus_config_t bus_config = {
            .mosi_io_num = CONFIG_SD_CAPTURE_MOSI_GPIO,
            .miso_io_num = CONFIG_SD_CAPTURE_MISO_GPIO,
            .sclk_io_num = CONFIG_SD_CAPTURE_SCK_GPIO,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4000,
        };
        if (!s_bus_initialized) {
            esp_err_t err = spi_bus_initialize(host.slot, &bus_config, SDSPI_DEFAULT_DMA);
            if (err != ESP_OK) {
                portENTER_CRITICAL(&s_stats_lock); s_stats.mount_errors++; portEXIT_CRITICAL(&s_stats_lock);
                ESP_LOGW(TAG, "SPI do SD indisponivel: %s", esp_err_to_name(err));
                return false;
            }
            s_bus_initialized = true;
        }
        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config.gpio_cs = CONFIG_SD_CAPTURE_CS_GPIO;
        slot_config.host_id = host.slot;
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false, .max_files = 1, .allocation_unit_size = 16 * 1024,
        };
        esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config,
                                                &mount_config, &s_card);
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_stats_lock); s_stats.mount_errors++; portEXIT_CRITICAL(&s_stats_lock);
            ESP_LOGW(TAG, "Falha ao montar SD: %s", esp_err_to_name(err));
            return false;
        }
        s_fs_mounted = true;
    }
    s_file = fopen(SD_FILE_PATH, "ab");
    if (!s_file) {
        portENTER_CRITICAL(&s_stats_lock); s_stats.mount_errors++; portEXIT_CRITICAL(&s_stats_lock);
        ESP_LOGW(TAG, "Falha ao abrir %s: errno=%d (%s)", SD_FILE_PATH, errno, strerror(errno));
        return false;
    }
    portENTER_CRITICAL(&s_stats_lock); s_stats.card_mounted = true; portEXIT_CRITICAL(&s_stats_lock);
    ESP_LOGI(TAG, "Captura bruta em %s (SCK=%d MOSI=%d MISO=%d CS=%d)", SD_FILE_PATH,
             CONFIG_SD_CAPTURE_SCK_GPIO, CONFIG_SD_CAPTURE_MOSI_GPIO,
             CONFIG_SD_CAPTURE_MISO_GPIO, CONFIG_SD_CAPTURE_CS_GPIO);
    return true;
}

static void flush_file(void)
{
    if (!s_file) return;
    if (fflush(s_file) != 0 || fsync(fileno(s_file)) != 0) {
        portENTER_CRITICAL(&s_stats_lock); s_stats.write_errors++; portEXIT_CRITICAL(&s_stats_lock);
        ESP_LOGW(TAG, "Falha no flush do SD");
        return;
    }
    portENTER_CRITICAL(&s_stats_lock); s_stats.flush_count++; portEXIT_CRITICAL(&s_stats_lock);
}

static void sd_capture_task(void *arg)
{
    (void)arg;
    capture_block_t block;
    size_t bytes_since_flush = 0;
    TickType_t last_flush = xTaskGetTickCount();
    TickType_t next_mount_attempt = 0;
    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (!s_file && now >= next_mount_attempt) {
            if (!mount_and_open()) next_mount_attempt = now + pdMS_TO_TICKS(5000);
        }
        if (xQueueReceive(s_queue, &block, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (s_file && fwrite(block.data, 1, block.length, s_file) == block.length) {
                portENTER_CRITICAL(&s_stats_lock); s_stats.bytes_written += block.length; portEXIT_CRITICAL(&s_stats_lock);
                bytes_since_flush += block.length;
            } else {
                portENTER_CRITICAL(&s_stats_lock); s_stats.write_errors++; s_stats.bytes_dropped += block.length; portEXIT_CRITICAL(&s_stats_lock);
                if (s_file) { fclose(s_file); s_file = NULL; }
            }
        }
        update_pending();
        now = xTaskGetTickCount();
        if (bytes_since_flush >= CONFIG_SD_CAPTURE_FLUSH_BYTES ||
            now - last_flush >= pdMS_TO_TICKS(CONFIG_SD_CAPTURE_FLUSH_PERIOD_S * 1000)) {
            flush_file(); bytes_since_flush = 0; last_flush = now;
        }
    }
}

esp_err_t sd_capture_init(void)
{
    if (s_queue) return ESP_OK;
    s_queue = xQueueCreate(CONFIG_SD_CAPTURE_QUEUE_LENGTH, sizeof(capture_block_t));
    return s_queue ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t sd_capture_start(void)
{
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    if (s_task) return ESP_OK;
    return xTaskCreate(sd_capture_task, "sd_capture", CONFIG_SD_CAPTURE_TASK_STACK_SIZE,
                       NULL, CONFIG_SD_CAPTURE_TASK_PRIORITY, &s_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t sd_capture_enqueue(const uint8_t *data, size_t length)
{
    if (!data || length == 0) return ESP_OK;
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    while (length) {
        capture_block_t block;
        const size_t chunk = length > sizeof(block.data) ? sizeof(block.data) : length;
        block.length = (uint16_t)chunk;
        memcpy(block.data, data, chunk);
        portENTER_CRITICAL(&s_stats_lock); s_stats.bytes_received += chunk; portEXIT_CRITICAL(&s_stats_lock);
        if (xQueueSend(s_queue, &block, 0) != pdTRUE) {
            portENTER_CRITICAL(&s_stats_lock); s_stats.bytes_dropped += chunk; portEXIT_CRITICAL(&s_stats_lock);
            return ESP_ERR_TIMEOUT;
        }
        data += chunk; length -= chunk;
    }
    update_pending();
    return ESP_OK;
}

void sd_capture_get_stats(sd_capture_stats_t *stats)
{
    if (!stats) return;
    update_pending();
    portENTER_CRITICAL(&s_stats_lock); *stats = s_stats; portEXIT_CRITICAL(&s_stats_lock);
}

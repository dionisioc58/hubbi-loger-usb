#include "sd_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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
#define SD_FILE_PREFIX SD_MOUNT_POINT "/uart0_capture_"
#define SD_MAX_FILE_SIZE UINT64_C(0xFFFFFFFF)

typedef struct {
    uint16_t length;
    uint8_t data[CONFIG_SD_CAPTURE_BLOCK_SIZE];
} capture_block_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static FILE *s_file;
static uint64_t s_file_size;
static uint32_t s_next_file_index;
static bool s_bus_initialized;
static bool s_fs_mounted;
static bool s_storage_full;
static sdmmc_card_t *s_card;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static sd_capture_stats_t s_stats;

static void update_pending(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.blocks_pending = s_queue ? (uint32_t)uxQueueMessagesWaiting(s_queue) : 0;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void set_storage_full(void)
{
    if (s_storage_full) return;
    s_storage_full = true;
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.storage_full = true;
    portEXIT_CRITICAL(&s_stats_lock);
    ESP_LOGE(TAG, "SD cheio; captura interrompida sem sobrescrever arquivos");
}

static bool open_capture_file(void)
{
    char path[64];
    for (uint32_t index = s_next_file_index; index < 1000000U; index++) {
        if (index == 0) snprintf(path, sizeof(path), "%s", SD_FILE_PATH);
        else snprintf(path, sizeof(path), "%s%04" PRIu32 ".log", SD_FILE_PREFIX, index);

        struct stat info;
        if (stat(path, &info) == 0) {
            if ((uint64_t)info.st_size >= SD_MAX_FILE_SIZE) continue;
            s_file = fopen(path, "ab");
            if (!s_file) return false;
            s_file_size = (uint64_t)info.st_size;
            s_next_file_index = index;
            ESP_LOGI(TAG, "Continuando %s em %" PRIu64 " bytes", path, s_file_size);
            return true;
        }
        if (errno != ENOENT) return false;

        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0) {
            s_file = fdopen(fd, "ab");
            if (!s_file) { close(fd); return false; }
            s_file_size = 0;
            s_next_file_index = index;
            ESP_LOGI(TAG, "Criado novo arquivo de captura: %s", path);
            return true;
        }
        if (errno == EEXIST) continue;
        if (errno == ENOSPC) set_storage_full();
        return false;
    }
    set_storage_full();
    return false;
}

static bool mount_and_open(void)
{
    if (s_file || s_storage_full) return s_file != NULL;
    if (!s_fs_mounted) {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.max_freq_khz = CONFIG_SD_CAPTURE_SPI_FREQ_KHZ;
        spi_bus_config_t bus_config = {
            .mosi_io_num = CONFIG_SD_CAPTURE_MOSI_GPIO,
            .miso_io_num = CONFIG_SD_CAPTURE_MISO_GPIO,
            .sclk_io_num = CONFIG_SD_CAPTURE_SCK_GPIO,
            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4000,
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
    if (!open_capture_file()) return false;
    portENTER_CRITICAL(&s_stats_lock); s_stats.card_mounted = true; portEXIT_CRITICAL(&s_stats_lock);
    ESP_LOGI(TAG, "SD pronto: limite por arquivo=%" PRIu64 " bytes", SD_MAX_FILE_SIZE);
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
        if (!s_file && !s_storage_full && now >= next_mount_attempt) {
            if (!mount_and_open()) next_mount_attempt = now + pdMS_TO_TICKS(5000);
        }
        if (xQueueReceive(s_queue, &block, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (s_storage_full) {
                /* O produtor ja contou esses bytes como descartados. */
            } else {
                if (s_file && s_file_size + block.length > SD_MAX_FILE_SIZE) {
                    flush_file(); fclose(s_file); s_file = NULL; s_next_file_index++;
                    open_capture_file();
                }
                if (s_file && fwrite(block.data, 1, block.length, s_file) == block.length) {
                    s_file_size += block.length;
                    portENTER_CRITICAL(&s_stats_lock); s_stats.bytes_written += block.length; portEXIT_CRITICAL(&s_stats_lock);
                    bytes_since_flush += block.length;
                } else {
                    portENTER_CRITICAL(&s_stats_lock); s_stats.write_errors++; s_stats.bytes_dropped += block.length; portEXIT_CRITICAL(&s_stats_lock);
                    if (errno == ENOSPC) set_storage_full();
                    if (s_file) { fclose(s_file); s_file = NULL; }
                }
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
    if (s_storage_full) {
        portENTER_CRITICAL(&s_stats_lock); s_stats.bytes_received += length; s_stats.bytes_dropped += length; portEXIT_CRITICAL(&s_stats_lock);
        return ESP_ERR_NO_MEM;
    }
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

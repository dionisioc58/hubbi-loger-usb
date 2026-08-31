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
#include "led_strip.h"
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
static bool s_capture_enabled;
static bool s_drain_requested;
static bool s_error;
static bool s_safe_to_remove = true;
static sdmmc_card_t *s_card;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static sd_capture_stats_t s_stats;
static led_strip_handle_t s_status_led;
static TickType_t s_last_led_tick;
static TickType_t s_last_flush_tick;
static bool s_led_on;
static uint8_t s_led_red;
static uint8_t s_led_green;
static uint8_t s_led_blue;
typedef enum {
    STATUS_LED_OFF,
    STATUS_LED_RECORDING,
    STATUS_LED_DRAINING,
    STATUS_LED_ERROR,
} status_led_state_t;
static status_led_state_t s_led_state = STATUS_LED_OFF;

#define STATUS_LED_FULL CONFIG_SD_CAPTURE_STATUS_LED_BRIGHTNESS
#define STATUS_LED_BLINK_MS 250

static void status_led_set(bool on, uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_status_led) return;
    if (s_led_on == on && (!on || (s_led_red == red && s_led_green == green && s_led_blue == blue))) return;
    s_led_on = on;
    s_led_red = red;
    s_led_green = green;
    s_led_blue = blue;
    if (on) {
        led_strip_set_pixel(s_status_led, 0, red, green, blue);
        led_strip_refresh(s_status_led);
    } else {
        led_strip_clear(s_status_led);
    }
}

static void status_led_apply(status_led_state_t state, bool on)
{
    switch (state) {
    case STATUS_LED_ERROR:     status_led_set(on, STATUS_LED_FULL, 0, 0); break;
    case STATUS_LED_RECORDING: status_led_set(on, 0, STATUS_LED_FULL, 0); break;
    case STATUS_LED_DRAINING:  status_led_set(on, STATUS_LED_FULL, STATUS_LED_FULL, 0); break;
    case STATUS_LED_OFF:       status_led_set(false, 0, 0, 0); break;
    }
}

static void status_led_update(TickType_t now)
{
    if (!s_status_led) return;
    status_led_state_t state = STATUS_LED_OFF;
    if (s_safe_to_remove) {
        /* Ocioso, ou SD ja drenado: apagado significa seguro para remover. */
        state = STATUS_LED_OFF;
    } else if (s_storage_full || s_error) {
        state = STATUS_LED_ERROR;
    } else if (s_drain_requested) {
        state = STATUS_LED_DRAINING;
    } else if (s_capture_enabled && s_file) {
        state = STATUS_LED_RECORDING;
    }

    if (state != s_led_state) {
        s_led_state = state;
        s_last_led_tick = now;
        status_led_apply(state, true);
        return;
    }

    /* Gravando e drenando piscam; erro fica fixo e ocioso fica apagado. */
    if ((state == STATUS_LED_RECORDING || state == STATUS_LED_DRAINING) &&
        now - s_last_led_tick >= pdMS_TO_TICKS(STATUS_LED_BLINK_MS)) {
        s_last_led_tick = now;
        status_led_apply(state, !s_led_on);
    }
}

static void status_led_selftest(void)
{
    /* Acende as tres cores no boot, antes de qualquer estado depender de USB ou
       de SD. Serve como prova imediata de que o GPIO e o LED estao corretos. */
    static const uint8_t sequence[3][3] = {
        {STATUS_LED_FULL, 0, 0},
        {0, STATUS_LED_FULL, 0},
        {0, 0, STATUS_LED_FULL},
    };
    for (size_t i = 0; i < 3; i++) {
        led_strip_set_pixel(s_status_led, 0, sequence[i][0], sequence[i][1], sequence[i][2]);
        led_strip_refresh(s_status_led);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    led_strip_clear(s_status_led);
    s_led_on = false;
    s_led_red = s_led_green = s_led_blue = 0;
}

static void status_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_SD_CAPTURE_STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .flags = {.invert_out = false},
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags = {.with_dma = false},
    };
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &s_status_led) != ESP_OK) {
        s_status_led = NULL;
        ESP_LOGW(TAG, "LED RGB indisponivel no GPIO %d", CONFIG_SD_CAPTURE_STATUS_LED_GPIO);
        return;
    }
    led_strip_clear(s_status_led);
    ESP_LOGI(TAG, "LED RGB WS2812 no GPIO %d (brilho %d); teste: vermelho, verde, azul",
             CONFIG_SD_CAPTURE_STATUS_LED_GPIO,
             CONFIG_SD_CAPTURE_STATUS_LED_BRIGHTNESS);
    status_led_selftest();
    /* Terminado o teste, o LED fica apagado ate a captura comecar. */
    s_led_state = STATUS_LED_OFF;
    s_last_led_tick = xTaskGetTickCount();
}

static void update_pending(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.blocks_pending = s_queue ? (uint32_t)uxQueueMessagesWaiting(s_queue) : 0;
    if (s_stats.blocks_pending > s_stats.max_blocks_pending) {
        s_stats.max_blocks_pending = s_stats.blocks_pending;
    }
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

static void set_error(void)
{
    s_error = true;
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
            if (!s_file) { set_error(); return false; }
            s_file_size = (uint64_t)info.st_size;
            s_next_file_index = index;
            ESP_LOGI(TAG, "Continuando %s em %" PRIu64 " bytes", path, s_file_size);
            return true;
        }
        if (errno != ENOENT) { set_error(); return false; }

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
        else set_error();
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
                set_error();
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
            set_error();
            return false;
        }
        s_fs_mounted = true;
    }
    if (!open_capture_file()) return false;
    s_error = false;
    s_safe_to_remove = false;
    portENTER_CRITICAL(&s_stats_lock); s_stats.card_mounted = true; portEXIT_CRITICAL(&s_stats_lock);
    ESP_LOGI(TAG, "SD pronto: limite por arquivo=%" PRIu64 " bytes", SD_MAX_FILE_SIZE);
    return true;
}

static bool flush_file(void)
{
    if (!s_file) return true;
    const TickType_t now = xTaskGetTickCount();
    if (fflush(s_file) != 0 || fsync(fileno(s_file)) != 0) {
        portENTER_CRITICAL(&s_stats_lock); s_stats.write_errors++; portEXIT_CRITICAL(&s_stats_lock);
        ESP_LOGW(TAG, "Falha no flush do SD");
        set_error();
        return false;
    }
    const uint32_t interval_ms = pdTICKS_TO_MS(now - s_last_flush_tick);
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.flush_count++;
    s_stats.last_flush_interval_ms = interval_ms;
    if (interval_ms > s_stats.max_flush_interval_ms) s_stats.max_flush_interval_ms = interval_ms;
    portEXIT_CRITICAL(&s_stats_lock);
    s_last_flush_tick = now;
    ESP_LOGI(TAG, "flush SD: intervalo=%" PRIu32 " ms, arquivo=%" PRIu64 " bytes",
             interval_ms, s_file_size);
    return true;
}

static void unmount_capture(void)
{
    if (s_file) {
        flush_file();
        fclose(s_file);
        s_file = NULL;
    }
    if (s_fs_mounted) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
        s_fs_mounted = false;
    }
    if (s_bus_initialized) {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        spi_bus_free(host.slot);
        s_bus_initialized = false;
    }
}

static void sd_capture_task(void *arg)
{
    (void)arg;
    capture_block_t block;
    size_t bytes_since_flush = 0;
    TickType_t last_flush = xTaskGetTickCount();
    TickType_t last_stats = last_flush;
    s_last_flush_tick = last_flush;
    TickType_t next_mount_attempt = 0;
    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (!s_file && !s_storage_full && s_capture_enabled && now >= next_mount_attempt) {
            if (!mount_and_open()) next_mount_attempt = now + pdMS_TO_TICKS(5000);
        }
        /* Sem arquivo aberto nao ha destino: consumir a fila aqui perderia o
           bloco. Durante a montagem, que leva ~100 ms apos a conexao, os dados
           esperam enfileirados em vez de virar bytes perdidos. */
        if (!s_file && s_capture_enabled && !s_storage_full) {
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if (xQueueReceive(s_queue, &block, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (s_storage_full) {
                /* O produtor ja contou esses bytes como descartados. */
            } else if (!s_file && s_capture_enabled) {
                /* Reconectou enquanto a tarefa ja estava bloqueada no receive: o
                   bloco chegou antes de a montagem abrir o arquivo. Devolve ao
                   inicio da fila para preservar a ordem e espera a montagem. */
                if (xQueueSendToFront(s_queue, &block, 0) != pdTRUE) {
                    portENTER_CRITICAL(&s_stats_lock); s_stats.bytes_dropped += block.length; portEXIT_CRITICAL(&s_stats_lock);
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            } else if (!s_file) {
                /* Captura encerrada e arquivo fechado: nao ha para onde gravar.
                   Nao e falha de escrita, entao nao conta em write_errors. */
                portENTER_CRITICAL(&s_stats_lock); s_stats.bytes_dropped += block.length; portEXIT_CRITICAL(&s_stats_lock);
            } else {
                if (s_file_size + block.length > SD_MAX_FILE_SIZE) {
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
                    else set_error();
                    if (s_file) { fclose(s_file); s_file = NULL; }
                }
            }
        }
        update_pending();
        now = xTaskGetTickCount();
        status_led_update(now);
        /* O ramo periodico so vale se houver algo por gravar: sem isso o flush
           acorda o cartao a cada periodo mesmo com o buffer vazio. */
        if (bytes_since_flush > 0 &&
            (bytes_since_flush >= CONFIG_SD_CAPTURE_FLUSH_BYTES ||
             now - last_flush >= pdMS_TO_TICKS(CONFIG_SD_CAPTURE_FLUSH_PERIOD_S * 1000))) {
            flush_file(); bytes_since_flush = 0; last_flush = now;
        }
        /* Resumo periodico so enquanto ha captura: parado, nao ha o que relatar. */
        if (CONFIG_SD_CAPTURE_STATS_PERIOD_S > 0 && s_capture_enabled &&
            now - last_stats >= pdMS_TO_TICKS(CONFIG_SD_CAPTURE_STATS_PERIOD_S * 1000)) {
            last_stats = now;
            sd_capture_log_stats("periodico");
        }
        if (s_drain_requested && uxQueueMessagesWaiting(s_queue) == 0) {
            if (flush_file()) {
                unmount_capture();
                s_drain_requested = false;
                s_safe_to_remove = true;
                status_led_update(xTaskGetTickCount());
                ESP_LOGI(TAG, "captura parada; SD seguro para remover");
                sd_capture_log_stats("desconexao");
            }
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
    status_led_init();
    return xTaskCreate(sd_capture_task, "sd_capture", CONFIG_SD_CAPTURE_TASK_STACK_SIZE,
                       NULL, CONFIG_SD_CAPTURE_TASK_PRIORITY, &s_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void sd_capture_input_connected(void)
{
    s_capture_enabled = true;
    s_safe_to_remove = false;
    if (!s_storage_full) s_error = false;
}

void sd_capture_input_disconnected(void)
{
    s_capture_enabled = false;
    s_drain_requested = true;
}

esp_err_t sd_capture_enqueue(const uint8_t *data, size_t length)
{
    if (!data || length == 0) return ESP_OK;
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    if (!s_capture_enabled || s_drain_requested) return ESP_ERR_INVALID_STATE;
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

void sd_capture_log_stats(const char *motivo)
{
    sd_capture_stats_t st;
    sd_capture_get_stats(&st);
    /* bytes_dropped e o numero que importa: descartes so acontecem aqui dentro.
       Zero com bytes faltando no arquivo significa que a perda foi antes da
       logger, na saida da placa principal. */
    ESP_LOGI(TAG,
             "stats (%s): recebidos=%" PRIu32 " gravados=%" PRIu32 " descartados=%" PRIu32
             " | fila=%" PRIu32 "/%d pico=%" PRIu32
             " | flushes=%" PRIu32 " intervalo_max=%" PRIu32 " ms"
             " | erros: montagem=%" PRIu32 " escrita=%" PRIu32
             " | cartao=%s%s",
             motivo ? motivo : "-",
             st.bytes_received, st.bytes_written, st.bytes_dropped,
             st.blocks_pending, CONFIG_SD_CAPTURE_QUEUE_LENGTH, st.max_blocks_pending,
             st.flush_count, st.max_flush_interval_ms,
             st.mount_errors, st.write_errors,
             st.card_mounted ? "montado" : "ausente",
             st.storage_full ? " CHEIO" : "");
}

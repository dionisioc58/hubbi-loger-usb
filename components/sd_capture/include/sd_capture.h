#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t bytes_received;
    uint32_t bytes_written;
    uint32_t bytes_dropped;
    uint32_t mount_errors;
    uint32_t write_errors;
    uint32_t flush_count;
    uint32_t last_flush_interval_ms;
    uint32_t max_flush_interval_ms;
    uint32_t blocks_pending;
    uint32_t max_blocks_pending; /* marca d'agua: pico de ocupacao da fila */
    bool card_mounted;
    bool storage_full;
} sd_capture_stats_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sd_capture_init(void);
esp_err_t sd_capture_start(void);
esp_err_t sd_capture_enqueue(const uint8_t *data, size_t length);
void sd_capture_input_connected(void);
void sd_capture_input_disconnected(void);
void sd_capture_get_stats(sd_capture_stats_t *stats);
void sd_capture_log_stats(const char *motivo);

#ifdef __cplusplus
}
#endif

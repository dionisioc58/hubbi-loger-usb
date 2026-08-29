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
    uint32_t blocks_pending;
    bool card_mounted;
    bool storage_full;
} sd_capture_stats_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sd_capture_init(void);
esp_err_t sd_capture_start(void);
esp_err_t sd_capture_enqueue(const uint8_t *data, size_t length);
void sd_capture_get_stats(sd_capture_stats_t *stats);

#ifdef __cplusplus
}
#endif

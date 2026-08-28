#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"

// A placa principal expoe o console pela USB nativa do ESP32-S3, que enumera
// como USB Serial/JTAG: VID 303A / PID 1001, CDC-ACM padrao. Nao e um CH340,
// entao o driver VCP do CH34x nao serve aqui (a lista de PIDs dele cobre
// apenas 7522/7523/5523).
#define MAIN_BOARD_VID 0x303A
#define MAIN_BOARD_PID 0x1001

// No USB Serial/JTAG a interface 0 e o canal CDC-ACM; a 2 e o canal JTAG.
#define MAIN_BOARD_CDC_INTERFACE 0

// Porta temporaria do console da logger no computador durante os testes:
// /dev/tty.usbmodem5C372764841 (CH343 da propria logger)
static const char *TAG = "hubbi_logger_usb";
static SemaphoreHandle_t s_disconnected;

static bool on_rx(const uint8_t *data, size_t length, void *)
{
    // Primeira etapa: confirmar que o fluxo do console chega intacto.
    // Depois este ponto sera ligado ao escritor do microSD.
    fwrite(data, 1, length, stdout);
    fflush(stdout);
    return true;
}

static void on_event(const cdc_acm_host_dev_event_data_t *event, void *)
{
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGW(TAG, "placa principal desconectada");
        // O handle so pode ser liberado aqui; depois disso ele fica invalido.
        cdc_acm_host_close(event->data.cdc_hdl);
        xSemaphoreGive(s_disconnected);
    } else if (event->type == CDC_ACM_HOST_ERROR) {
        ESP_LOGE(TAG, "erro USB: %d", event->data.error);
    }
}

static void usb_events_task(void *)
{
    while (true) {
        uint32_t flags = 0;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &flags));
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
        }
    }
}

extern "C" void app_main(void)
{
    s_disconnected = xSemaphoreCreateBinary();
    assert(s_disconnected != nullptr);

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_ERROR_CHECK(cdc_acm_host_install(nullptr));

    assert(xTaskCreate(usb_events_task, "usb_events", 4096, nullptr, 20, nullptr) == pdPASS);

    ESP_LOGI(TAG, "USB host pronto; aguardando %04X:%04X da placa principal",
             MAIN_BOARD_VID, MAIN_BOARD_PID);

    while (true) {
        const cdc_acm_host_device_config_t config = {
            .connection_timeout_ms = 5000,
            .out_buffer_size = 512,
            .in_buffer_size = 512,
            .event_cb = on_event,
            .data_cb = on_rx,
            .user_arg = nullptr,
        };

        cdc_acm_dev_hdl_t device = nullptr;
        const esp_err_t err = cdc_acm_host_open(MAIN_BOARD_VID, MAIN_BOARD_PID,
                                                MAIN_BOARD_CDC_INTERFACE, &config, &device);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "placa principal nao encontrada (%s); tentando novamente",
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // O USB Serial/JTAG nao tem UART fisica atras dele, entao o line coding
        // e decorativo e o dispositivo pode responder com STALL. Nao e fatal.
        cdc_acm_line_coding_t line = {
            .dwDTERate = 115200,
            .bCharFormat = 0,
            .bParityType = 0,
            .bDataBits = 8,
        };
        if (cdc_acm_host_line_coding_set(device, &line) != ESP_OK) {
            ESP_LOGD(TAG, "line coding recusado; seguindo assim mesmo");
        }

        // O monitor e somente receptor. Manter DTR/RTS baixos: no ESP32-S3 essa
        // dupla e justamente o que o esptool usa para resetar a placa e cair no
        // bootloader, e um pulso acidental derrubaria a principal.
        if (cdc_acm_host_set_control_line_state(device, false, false) != ESP_OK) {
            ESP_LOGD(TAG, "set_control_line_state recusado; seguindo assim mesmo");
        }

        ESP_LOGI(TAG, "placa principal conectada; capturando console");

        xSemaphoreTake(s_disconnected, portMAX_DELAY);
    }
}

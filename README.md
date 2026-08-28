# Hubbi-logger-usb

Firmware ESP-IDF para uma ESP32-S3 atuar como USB host e capturar, através de
um cabo USB, o console produzido pela placa ESP32-S3 principal (HUBBI-OBD-IDF).

## Alvo USB

A placa principal tem duas portas USB, com chips diferentes:

| Porta | Dispositivo | VID:PID |
|-------|-------------|---------|
| Nativa | ESP32-S3 USB Serial/JTAG | `303A:1001` |
| Secundária | CH343 (bridge UART) | `1A86:55D3` |

Este firmware mira a **porta nativa** (`303A:1001`), que é CDC-ACM padrão. Por
isso ele usa `cdc_acm_host_open()` diretamente, e **não** o driver VCP do CH34x:
aquele driver só reconhece os PIDs `7522`, `7523` e `5523`, ou seja, nem o USB
Serial/JTAG nem o CH343 desta placa.

O firmware da principal precisa estar com `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`
para espelhar o console na porta nativa — que é a configuração atual dela.

## Estado atual

A primeira etapa escreve no console da logger os bytes recebidos da principal.
O próximo passo é substituir o `stdout` de teste por um escritor com buffer
para o microSD, mantendo a recepção USB independente da latência do cartão.

## Hardware

O ESP32-S3 tem **um único PHY USB interno**, disputado pelo controlador USB
Serial/JTAG e pelo USB-OTG. Como aqui o OTG atua como host, o console
secundário pela USB nativa fica desligado (`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`)
e o console da logger sai pelo UART0 (GPIO43/44) → CH343 → computador.

A porta USB nativa da logger precisa ser capaz de atuar como host: fornecer
VBUS de 5 V e assumir o papel de host no conector. Uma porta de device comum
(pull-downs CC de 5.1 k, VBUS como entrada) **não** estabelece link com outra
porta de device por um cabo C-para-C — nenhuma das duas assume o papel de host.
Nesse caso é preciso ligar D+/D- (GPIO19/GPIO20) e GND manualmente até o alvo.

Não una duas fontes de 5 V: se a principal estiver alimentada separadamente,
use uma conexão USB com VBUS controlado/isolado.

## Compilação

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

Durante os testes atuais, o console serial da logger aparece no computador em
`/dev/tty.usbmodem5C372764841` (CH343 da própria logger, serial `5C37276484`).

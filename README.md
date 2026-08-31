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

A logger escreve no console e no arquivo bruto `/sdcard/uart0_capture.log` os
bytes recebidos da principal. O arquivo preserva o fluxo sem cabeçalho ou
conversão; a fila desacopla a recepção USB da latência do cartão. Cada arquivo
fica limitado a 4 GiB para compatibilidade com FAT32. Ao atingir o limite, a
captura continua em `uart0_capture_0001.log`, depois `..._0002.log` e assim por
diante, sem apagar nem sobrescrever arquivos existentes. Quando não houver
mais espaço, a captura para.

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

O padrão para o ESP32-S3 usa SPI nos GPIOs 4 (SCK), 5 (MOSI), 6 (MISO) e 7
(CS). Esses pinos e os parâmetros da fila podem ser ajustados com `idf.py menuconfig`, no menu
`Hubbi logger USB - captura no SD`.

## Indicador RGB e intervalo de gravação

O LED RGB endereçável (WS2812) usa brilho baixo e indica o estado do cartão:

- verde piscando: captura ativa e gravando dados no SD;
- amarelo piscando: drenando a fila depois da desconexão USB;
- vermelho fixo: erro de montagem/escrita ou cartão cheio;
- apagado: fila drenada, arquivo fechado e cartão seguro para remover.

O GPIO padrão é o 38, usado na revisão v1.1 da ESP32-S3-DevKitC-1. A revisão
original usa GPIO48; confirme a revisão/modelo da sua placa e ajuste
`SD_CAPTURE_STATUS_LED_GPIO` no `menuconfig` se necessário. O brilho padrão é
16/255 para reduzir o consumo.

O gravador faz `fflush()` e `fsync()` quando acumula 4096 bytes ou quando passam
5 segundos desde o último flush, o que ocorrer primeiro. A cada flush a logger
emite no console local uma mensagem como:

```text
I (... ) sd_capture: flush SD: intervalo=... ms, arquivo=... bytes
```

Assim é possível verificar o intervalo real. A fila possui 128 blocos de 512
bytes, totalizando 65.536 bytes de reserva. Se a escrita no cartão ficar atrás
do USB e a fila encher, os bytes excedentes são descartados e contabilizados em
`bytes_dropped`. O modo verbose da principal é aceito pelo transporte USB; ele
apenas aumenta a taxa e, portanto, aumenta a chance de saturar essa fila.

## Remoção segura do cartão

O firmware interrompe a entrada quando recebe o evento de desconexão USB. Em
seguida, mantém o LED amarelo piscando, grava os blocos restantes, executa
`fflush()` e `fsync()`, fecha o arquivo e desmonta o filesystem. Só então apaga
o LED e registra `SD seguro para remover` no console local.

A sequência segura executada pelo firmware é:

1. desconectar o cabo USB da placa principal;
2. impedir novas entradas USB na fila;
3. aguardar `blocks_pending == 0`;
4. executar `fflush()` e `fsync()`;
5. fechar o arquivo e desmontar o filesystem;
6. apagar o LED, indicando “cartão liberado”.

Na implementação atual, o LED apagado substitui o estado azul: ele só ocorre
depois da sequência acima. Portanto, a sequência “desconectar o USB, aguardar o
LED apagar e remover o cartão” é segura, desde que o LED esteja funcionando e
não esteja vermelho. Se o LED não acender, deve-se aguardar a mensagem
`SD seguro para remover` no console ou desligar a logger antes de retirar o
cartão.

## Alteração de hardware recomendada

O botão de ejeção continua sendo útil para um comando explícito, mas não é
necessário para o procedimento acima: desconectar o cabo USB já inicia a
sequência de parada. Um botão seria recomendado apenas se a logger precisar
continuar conectada à principal enquanto o usuário troca o cartão.

Bluetooth está desabilitado no `sdkconfig`. No ESP32-S3, o Kconfig do alvo
mantém o componente Wi-Fi compilável por padrão, mas o firmware não chama
`esp_wifi_init()` nem inicia qualquer interface de rede; portanto o rádio não
é ativado e não há consumo operacional de Wi-Fi. O firmware usa somente USB
host, UART do console, SPI do SD e o LED RGB.

## Estimativa de capacidade e riscos

Uma análise do projeto `HUBBI-OBD-IDF` encontrou como maior linha determinística
o log `send_raw_flash_data_mqtt`, que imprime um JSON contendo um bloco Base64
de 1024 bytes. O cálculo é:

```text
JSON completo                         ≈ 1.554 bytes
mensagem do log                      ≈ 1.598 bytes
prefixo ESP-IDF + newline            ≈    22 bytes
linha completa                        ≈ 1.620 bytes
```

Usando 1.620 bytes por linha, a estimativa fica:

```text
4.294.967.295 / 1.620 = 2.651.214 linhas por arquivo de 4 GiB
32.000.000.000 / 1.620 = 19.753.086 linhas em um cartão nominal de 32 GB
```

O valor de 32 GB é decimal e ignora a sobrecarga do FAT, espaço reservado,
arquivos já existentes e perdas de capacidade do cartão. Portanto, é uma
estimativa nominal, não uma garantia.

### Riscos relacionados ao tamanho das linhas

- O maior log encontrado é `ESP_LOGV`; ele não aparece no nível padrão `INFO`,
  mas pode ser ativado pelo controle dinâmico de logs.
- Os módulos do ESP-IDF podem emitir mensagens com campos dinâmicos e dumps.
  Não existe um limite universal seguro para qualquer mensagem futura; o valor
  de 1.620 bytes é o pior caso identificado no código atual.
- Em `obd_provisioning.c`, o `post_handler` usa `char buf[400]` e registra o
  conteúdo com `%s` sem garantir a terminação `\0`. Isso pode fazer o log ler
  memória além do buffer, produzindo uma linha maior que 400 bytes ou
  comportamento indefinido. Esse ponto deve ser corrigido antes de considerar
  um limite rigoroso.

Na prática, a logger preserva os bytes recebidos e troca de arquivo ao atingir
4 GiB. A estimativa deve ser recalculada se forem adicionados logs que imprimam
buffers maiores, dumps hexadecimais ou payloads completos.

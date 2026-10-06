# STM32F4 + HAL + FreeRTOS UART

Bu klasörü yeni projeye kopyala. `uart_comm.c` ve `uart_comm_port.c` derlenir; uygulama yalnız `uart_comm.h` kullanır. Kütüphane tek UART için tek statik task ve kopyalı TX kuyruğu oluşturur. İş yokken task notification ile süresiz bekler. RX/TX servis döngüsü veya ikinci UART taskı yazılmaz.

Protokol, CRC, joystick ve uygulama kodu bu pakete dahil değildir. Gelen byte'lar uygulamaya callback ile teslim edilir. Aynı çerçeve formatını kullanmak istersen projedeki `protocol.c/.h` ve `protocol_uart.c/.h` adaptörünü ayrıca al.

## Yeni projeye ekleme

1. CubeIDE'de `Lib/Uart` klasörünü kaynak olarak ekle; C include paths listesine `../Lib/Uart` ekle. İki C dosyasının derlendiğini kontrol et.
2. CubeMX'te seçilen UART için RX DMA **circular**, TX DMA **normal**, ayrı stream ve doğru UART/channel bağlantısını kur. Bu paket byte tamponları kullanır: UART 8N1, RX/TX peripheral ve memory alignment **byte**, memory increment **enabled** olmalı. DMA tamponları erişilebilir SRAM'de kalmalı; F407 CCM'ye taşınmamalı.
3. UART ve her iki DMA IRQ'sunu etkinleştir. Priority grouping NVIC GROUP4; IRQ öncelikleri FreeRTOS API çağrısına uygun olmalı. Mevcut projede max syscall priority5 ve UART/DMA IRQ priority5 kullanılıyor; yeni projede kendi `FreeRTOSConfig.h` değerine göre seç.
4. FreeRTOS static allocation ve task notifications açık, `INCLUDE_xTaskGetSchedulerState=1`, `INCLUDE_vTaskSuspend=1` olmalı. Süresiz notification beklemesi için `portMAX_DELAY` desteği gerekir. HAL tick kesmesi scheduler öncesinde de çalışmalı; HAL ve RTOS tick/IRQ sahipliğini projenin CubeMX düzeni belirler. Mevcut örnekte HAL TIM6, RTOS SysTick kullanır.
5. Aşağıdaki callback/IRQ yönlendirmelerini USER CODE alanlarına ekle. HAL init ve RTOS kernel kurulumu sonrası, scheduler başlamadan `uart_comm_init` çağır.

DMA stream IRQ'su handle'dan bulunur; USART2/Stream5/6/Channel4 sabiti yoktur. GPIO, saat, DMA channel seçimi ve NVIC kurulumu projeye aittir. F407 üzerinde kart testi vardır; USART1/DMA2 için ayrı protokolsüz ARM link testi vardır. Diğer UART bağlantısının kartta çalışması bu derleme testiyle kanıtlanmış sayılmaz.

## Ham byte kullanımı

```c
#include "uart_comm.h"

static uint32_t received_bytes;
static uint32_t on_rx(uart_comm_rx_event_t event,
                      const uint8_t *data, uint16_t len, void *user)
{
    (void)user;
    if (event == UART_COMM_RX_DATA) {
        received_bytes += len;
        /* data[0..len-1] burada işle veya kendi tamponuna kopyala. */
        (void)data;
    }
    return 0U; /* Ham byte kullanımı: eksik mesaj/deadline yok. */
}

static void on_tx(const uart_comm_tx_result_t *result, void *user)
{
    (void)user;
    /* result->tag ve result->code ile gönderim sonucunu işle. */
    (void)result;
}

static const uart_comm_handlers_t handlers = {
    .on_rx = on_rx,
    .on_tx_result = on_tx,
    .rx_user = NULL,
    .user = NULL
};

/* HAL/DMA ve RTOS kernel kurulumu sonrası, scheduler öncesi bir kez. */
if (uart_comm_init(&huart2, &handlers) != HAL_OK) Error_Handler();

/* Scheduler çalışırken uygulama taskından; kuyrukta yer beklemez. */
const uint8_t bytes[] = {1, 2, 3};
uart_comm_send_status_t accepted = uart_comm_send_copy(bytes, sizeof(bytes), 42U);
```

`ACCEPTED`, byte'ların kuyruğa kopyalandığı anlamına gelir; karşı cihaz onayı değildir. Kaynak tampon API döndüğünde tekrar kullanılabilir. En fazla `UART_TX_BUF_SIZE` byte gönderilir. Başarılı kuyruğa giriş sırası FIFO'dur; kabul edilmiş her öğeye tek sonuç gelir. Sonuç API dönmeden de gelebilir. Otomatik tekrar gönderim yoktur.

Handler'lar UART taskında, IRQ ve kritik bölüm dışında çalışır; kısa tutulur, beklemez. RX data ve TX result işaretçileri yalnız callback süresince geçerlidir. Handler içinden `send_copy` çağrılabilir. RX ve TX callback kullanıcı hedefleri ayrı tutulur; hedefler modül ömrü boyunca yaşamalıdır. Başka tasklara veri aktarırken uygulama kendi paylaşım korumasını sağlar.

## HAL callback ve IRQ bağlantısı

Bu tanımlar projeye aittir; kütüphane global HAL callback tanımlamaz. Mevcut callback'ler varsa içlerine yönlendirme ekle; ikinci tanım oluşturma.

```c
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *u, uint16_t n)
{ uart_comm_on_rx_event(u, n); }
void HAL_UART_ErrorCallback(UART_HandleTypeDef *u)
{ uart_comm_on_error(u); }
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *u)
{ uart_comm_on_tx_complete(u); }
void HAL_UART_AbortReceiveCpltCallback(UART_HandleTypeDef *u)
{ uart_comm_on_rx_abort_complete(u); }
void HAL_UART_AbortTransmitCpltCallback(UART_HandleTypeDef *u)
{ uart_comm_on_tx_abort_complete(u); }

/* CubeMX UART IRQ handler USER CODE alanında HAL handler'dan sonra: */
HAL_UART_IRQHandler(&huart2);
uart_comm_on_uart_irq_exit(&huart2);
```

DMA IRQ handler'ları mevcut `HAL_DMA_IRQHandler` çağrılarını korur. Kütüphane seçili handle dışındaki olayları yok sayar; başka UART'lara ait callback davranışı projede yönlendirilir. RX TC olayını başka yoldan ikinci kez bildirme.

## Mevcut paket protokolünü kullanma

```c
#include "protocol_uart.h"
#include "app_protocol.h" /* Bu projeye özgü joystick/SEQ örneği. */
static protocol_uart_t protocol;
static const uart_comm_handlers_t handlers = {
    .on_rx = protocol_uart_on_rx,
    .on_tx_result = on_tx,
    .rx_user = &protocol
};

app_protocol_init();
protocol_uart_init(&protocol, app_protocol_on_frame, NULL);
if (uart_comm_init(&huart2, &handlers) != HAL_OK) Error_Handler();
```

Bu alternatif kurulumdur; ham byte örneğiyle birlikte ikinci init yapılmaz. Parser ve uygulama handler'ı aynı UART taskında çalışır. Protokol adaptörü DATA byte'larını çözer, RESET ile eski adayı istatistikleri koruyarak atar, TIMEOUT ile eksik adayı yeniden tarar. CRC/format doğrulanmış mesaj bildirimi bounded RX toparlanmasının erken kapanmasına yardımcı olur; ham byte teslimi bu kanıtı üretmez.

Özel protokol için `on_rx` DATA/TIMEOUT sonrası `UART_COMM_RX_PENDING` ve/veya `UART_COMM_RX_VALIDATED` bitlerini döndürür. Bekleyen aday varken son DMA üretici ilerlemesinden `UART_RX_TIMEOUT_MS` sonra TIMEOUT gelir; timeout sonrası hâlâ aday varsa süre yeniden kurulur. RESET verisizdir ve eksik adayı temizlemelidir. Aday yoksa timeout için periyodik uyanma olmaz.

## Ayarlar ve durum

`uart_comm_config.h`: RX halka256, scratch32, tur bütçesi64; TX öğesi64, kuyruk8 bekleyen +1 aktif; task stack512 **word**, priority25. RX halka boyutu uint32 sayaç sarımında doğru DMA konumunu korumak için 2'nin kuvveti olmalı (2..32768). Scratch halkadan büyük olamaz. Task priority, kendi `configMAX_PRIORITIES` değerinin altında olmalıdır. Deadline'lar pozitif ve uint32 yarım aralığından küçüktür; geçersiz seçenekler derlemede reddedilir. TX boyutunu veya baud'u değiştirirsen wire süresinin `UART_TX_TIMEOUT_MS` içine sığdığını kontrol et.

`uart_comm_get_snapshot` yayımlanan tutarlı durum/sayaç kopyasını verir. RX `RUNNING` ve TX `IDLE` normal çalışma durumlarıdır. `FAULT` donanımın durmuş olduğu anlamına gelmez; duruş ayrıca doğrulanır. `uart_comm_request_recovery` isteği kaydeder, başarı sonucu değildir. Init tek seferliktir; ilk çağrıdan sonra handle/handler değiştirme ve ikinci UART instance bu sürümde desteklenmez.

Test API'si yalnız `UART_COMM_TEST` derlemesindedir; normal uygulama `uart_comm_internal.h` kullanmaz. Kart testinde servis+ISR CPU ölçülür; bu, bütün task runtime ölçümü değildir.

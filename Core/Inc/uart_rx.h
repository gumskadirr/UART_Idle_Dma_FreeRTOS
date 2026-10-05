/*
 * uart_rx.h
 *
 * UART alim altyapisi: circular DMA tamponu, okuma konumu takibi ve
 * ayristiriciya besleme. HAL callback'lerini bu modul sahiplenir.
 *
 * Kullanim:
 *   uart_rx_start(&huart2);     bir kez, alimi baslatir
 *   uart_rx_service();          dongude, bekleyen veri varsa isler
 */

#ifndef INC_UART_RX_H_
#define INC_UART_RX_H_

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "parser.h"

#define UART_RX_BUF_SIZE   256U

/* Bekleyen yarim cerceve icin tamamlama zaman asimi.
   En buyuk cerceve 64 bayt; 115200 8N1'de 64 x 86,8 us ~ 5,6 ms. 50 ms bunun
   ~9 katidir: isletim sistemi kaynakli parcalanma ve gecikmelere tolerans
   birakir, buna karsilik tikanmayi sinirli tutar. */
#define UART_RX_FRAME_TIMEOUT_MS   50U

/* Basarisiz yeniden baslatma denemeleri arasindaki en kisa bekleme. Mesgul
   dongude saniyede binlerce sonucsuz deneme yapmayi engeller. */
#define UART_RX_RESTART_RETRY_MS    5U

/* Bu kadar denemeden sonra kalici hata durumuna gecilir (stats.faulted).
   5 x 5 ms = ~25 ms: gecici bir donanim hatasi icin bol, sonsuz dongu icin
   degil. */
#define UART_RX_RESTART_MAX_TRIES   5U

/* Alim olay sayaclari. Kesme icinde yazilip main baglaminda okundugu icin
   volatile: derleyici bu degerleri register'da onbellekleyemez. */
typedef struct
{
    volatile uint16_t rx_events;      /* toplam RxEvent sayisi */
    volatile uint16_t idle_events;    /* IDLE kaynakli */
    volatile uint16_t ht_events;      /* yarim tampon */
    volatile uint16_t tc_events;      /* tam tampon */
    volatile uint16_t last_size;      /* callback'in bildirdigi Size (MUTLAK KONUM) */
    volatile uint16_t error_events;   /* UART hata callback sayisi */
    volatile uint32_t last_error;     /* ORE/FE/NE/PE bit maskesi */

    /* Asagidakiler yalnizca main baglaminda yazilir: volatile gerekmez */
    uint16_t restarts;                /* kontrollu yeniden baslatma sayisi */
    uint16_t restart_fails;           /* yeniden baslatma basarisiz oldu */
    uint16_t frame_timeouts;          /* zaman asimiyla dusurulen aday sayisi */

    /* UART_RX_RESTART_MAX_TRIES denemede toparlanamadi: ALIM DURDU.
       Sessizce olmek yerine gorunur olmek icin var. Cikis yolu yalnizca
       uart_rx_start() ile yeniden kurmaktir. */
    uint8_t  faulted;
} uart_rx_stats_t;

/* Cozulmus cercevelerden turetilen durum. Yalnizca main baglaminda
   guncellenir (uart_rx_service -> uart_rx_drain -> frame_parser_feed ->
   frame_received), bu yuzden volatile gerekmez.

   NOT: sira takibi (protokol katmani) ile joystick ornegi (uygulama verisi)
   simdilik ayni yapida. M8'de joystick modu ve komutlar eklenirken
   seq_tracker_t / joystick_sample_t olarak ayrilacak. */
typedef struct
{
    uint16_t last_seq;        /* en son alinan SEQUENCE */
    uint16_t next_seq;        /* siradaki beklenen SEQUENCE (TCP: rcv_nxt) */
    uint16_t seq_gaps;        /* eksik sira numarasi tespit edilen olay sayisi */
    uint8_t  seq_synced;      /* ilk cerceve referans alindi mi */
    int16_t  joy_x;
    int16_t  joy_y;
} uart_rx_state_t;

extern uart_rx_stats_t uart_rx_stats;
extern uart_rx_state_t uart_rx_state;

/* Alimi baslatir: ayristiriciyi ve okuma konumunu sifirlar, circular DMA'yi
   IDLE olaylariyla kurar. Loopback testinde gonderimden ONCE cagrilmali. */
HAL_StatusTypeDef uart_rx_start(UART_HandleTypeDef *huart);

/* Bekleyen veri bildirimi varsa isler. Dongude cagrilir. */
void uart_rx_service(void);

/* DMA tamponunu kosulsuz bosaltir (bildirimi beklemeden); loopback testinde
   gonderim dongusu icinden cagrilir. */
void uart_rx_drain(void);

/* Ayristirici sayaclarina salt okunur erisim. Dogrulanmis cerceve sayisi
   burada: uart_rx_get_parser()->frames_ok */
const frame_parser_t *uart_rx_get_parser(void);

/* --- Test kancalari ---
   Donanimda "yeniden baslatma basarisiz oldu" durumunu deterministik uretmenin
   temiz bir yolu yok: gercek bir ORE'nin restart'i dusurup dusurmeyecegi
   garanti edilemez. Tekrar deneme ve kalici hata durumu (T6) ancak bu kanca
   ile sinanabilir. Uretim akisinda hicbiri cagrilmaz; enable her zaman 0. */

/* Her yeniden baslatma denemesini basarisiz saydirir. */
void uart_rx_force_restart_fail(uint8_t enable);

/* Kesmeden gelmis gibi bir hata bildirimi enjekte eder. */
void uart_rx_test_inject_error(void);

#endif /* INC_UART_RX_H_ */

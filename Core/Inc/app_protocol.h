/*
 * app_protocol.h
 *
 * KISA uygulama handler'i: cozulmus cerceveleri yorumlar.
 *
 * NEDEN AYRI DOSYA (R5):
 * Sira takibi ve joystick cozme uart_rx.c icindeydi. Bunlar TASIMA degil
 * UYGULAMA isleridir: tasima katmanini "bu protokolde SEQ alani var ve
 * 0x10 tipi joystick demek" bilgisine baglar. Ayrildiginda uart_rx yalnizca
 * ayristirma ve teslimden sorumlu kalir; M1'de uart_comm icine tasinacak
 * olan da tam olarak o cekirdektir.
 *
 * SOZLESME: handler UartCommTask/ana dongu baglaminda KISA calisir.
 * info->payload yalnizca cagri suresince gecerlidir; saklanacaksa
 * KOPYALANIR. Handler HAL cagirmaz, beklemez, service'e tekrar girmez.
 */

#ifndef INC_APP_PROTOCOL_H_
#define INC_APP_PROTOCOL_H_

#include <stdint.h>
#include "parser.h"

typedef struct
{
    uint16_t last_seq;        /* en son alinan SEQUENCE */
    uint16_t next_seq;        /* siradaki beklenen SEQUENCE (TCP: rcv_nxt) */

    /* DIKKAT: bu bir KAYIP PAKET SAYACI DEGILDIR. Sira surekliliginin
       bozuldugu OLAY sayisini tutar. Bir olayda bir paket de kaybolmus
       olabilir elli paket de; ayrica gondericinin yeniden baslamasi ya da
       sira sarmasi da olay uretir. "seq_gap_events = kayip paket" demek
       olcumu oldugundan iyi ya da kotu gosterir. */
    uint16_t seq_gap_events;

    uint8_t  seq_synced;      /* ilk cerceve referans alindi mi */
    int16_t  joy_x;
    int16_t  joy_y;
    uint32_t frames_handled;  /* handler'a teslim edilen cerceve sayisi */
} app_proto_state_t;

extern app_proto_state_t app_proto_state;

/* Sira takibini sifirlar. Alim baslatilmadan ONCE cagrilir. */
void app_protocol_init(void);

/* frame_handler_t imzasi: uart_rx_set_handler ile kaydedilir. */
void app_protocol_on_frame(const frame_info_t *info, void *user_data);

#endif /* INC_APP_PROTOCOL_H_ */

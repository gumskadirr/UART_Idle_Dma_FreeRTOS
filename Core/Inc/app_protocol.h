/* Uygulama handler'i: SEQ takibi ve joystick X/Y degerleri.
 * UartCommTask baglaminda kisa calisir; HAL cagirmaz ve beklemez.
 * Diger tasklar durumu app_protocol_get_snapshot ile deger kopyasi olarak okur.
 * Payload yalniz handler cagrisi boyunca gecerlidir.
 */
#ifndef INC_APP_PROTOCOL_H_
#define INC_APP_PROTOCOL_H_

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

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

/* Tutarlı snapshot: NULL ise false; mevcut kesme maskesi korunur. */
bool app_protocol_get_snapshot(app_proto_state_t *out);

/* Sira takibini sifirlar. Alim baslatilmadan ONCE cagrilir. */
void app_protocol_init(void);

/* frame_handler_t imzasi: protocol_uart_init ile adaptore kaydedilir. */
void app_protocol_on_frame(const frame_info_t *info, void *user_data);

#endif /* INC_APP_PROTOCOL_H_ */

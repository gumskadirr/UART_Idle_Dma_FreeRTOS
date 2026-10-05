/*
 * parser.h
 *
 * UART bayt akisindan dogrulanmis cerceve cikarma.
 * Bu modul HAL, DMA ve FreeRTOS'tan bagimsizdir: bayt alir, cerceve verir.
 */

#ifndef INC_PARSER_H_
#define INC_PARSER_H_

#include <stdint.h>
#include "frame.h"

/* Cozulmus cercevenin alanlari.
   DIKKAT: payload isaretcisi yalnizca handler cagrisi suresince gecerlidir.
   Veriyi saklayacaksan kopyala; ayristirici tamponu sonra degisir.
   Bu yapi veriyi SAHIPLENMEZ, yalnizca tarif eder. */
typedef struct
{
    uint8_t        type;
    uint16_t       seq;
    uint8_t        payload_len;
    const uint8_t *payload;      /* payload_len 0 ise NULL */
} frame_info_t;

/* Gecerli cerceve bulununca cagrilir.
   user_data: cagirana ait serbest isaretci; ayristirici icine bakmaz. */
typedef void (*frame_handler_t)(const frame_info_t *info, void *user_data);

typedef struct
{
    uint8_t  buf[FRAME_MAX_SIZE];   /* aday cerceve penceresi */
    uint8_t  len;                   /* buf icindeki gecerli bayt sayisi */

    uint16_t frames_ok;             /* dogrulanmis cerceve sayisi */
    uint16_t err_crc;               /* CRC uyusmadi */
    uint16_t err_len;               /* LENGTH sinir disi */
    uint16_t err_version;           /* VERSION uyusmadi */
    uint16_t bytes_dropped;         /* yeniden tarama sirasinda atilan bayt */
    uint16_t timeouts;              /* zaman asimiyla dusurulen aday sayisi */
} frame_parser_t;

/* Yapiyi ilk kullanima hazirlar: len ve butun sayaclar sifirlanir.
   buf icerigi temizlenmez; len == 0 oldugu icin okunmaz. */
void frame_parser_init(frame_parser_t *p);

/* Gelen baytlari isler. Bir cagride sifir, bir veya birden fazla cerceve
   bulunabilir; her biri icin handler cagrilir. */
void frame_parser_feed(frame_parser_t *p,
                       const uint8_t *data, uint16_t len,
                       frame_handler_t handler, void *user_data);

/* Bekleyen adayin zaman asimina dustugunu bildirir: adayin ilk bayti atilir
   ve kalan veri yeniden taranir. Boylece bozuk bir LENGTH alani, arkasindaki
   gecerli cerceveyi sonsuza kadar bekletemez.

   Zaman asimi karari CAGIRANA aittir; bu modul saat bilmez ve bilmemelidir
   (HAL/FreeRTOS bagimsizligi). Bekleyen aday yoksa hicbir sey yapmaz. */
void frame_parser_timeout(frame_parser_t *p,
                          frame_handler_t handler, void *user_data);

/* Bekleyen adayi sayaclari KORUYARAK atar. Hata toparlamada kullanilir:
   tampon durumu sifirlanmali ama istatistikler kaybolmamali. */
void frame_parser_discard(frame_parser_t *p);

#endif /* INC_PARSER_H_ */

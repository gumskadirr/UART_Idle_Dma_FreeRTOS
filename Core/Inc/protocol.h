/* UART cerceve formati, olusturma, CRC ve akis ayristirma.
 * HAL/DMA/FreeRTOS bagimsizdir. SEQ ve CRC little-endian'dir.
 * AA 55 | VERSION | TYPE | LENGTH | SEQ(2) | PAYLOAD | CRC16(2)
 * CRC: VERSION alanindan payload sonuna kadar CCITT-FALSE.
 */
#ifndef INC_PROTOCOL_H_
#define INC_PROTOCOL_H_

#include <stdint.h>

/* --- Sabit alan degerleri --- */
#define FRAME_SYNC0                 0xAAU
#define FRAME_SYNC1                 0x55U
#define PROTOCOL_VERSION            0x01U

/* --- Boyutlar --- */
#define FRAME_HEADER_SIZE              7U   /* SYNC(2) VERSION TYPE LENGTH SEQ(2) */
#define FRAME_CRC_SIZE                 2U
#define FRAME_OVERHEAD                 (FRAME_HEADER_SIZE + FRAME_CRC_SIZE)  /*  9 */
#define FRAME_MAX_PAYLOAD             55U
#define FRAME_MAX_SIZE                 (FRAME_OVERHEAD + FRAME_MAX_PAYLOAD)  /* 64 */

/* --- Alan konumlari (cerceve basindan itibaren) --- */
#define FRAME_OFF_SYNC0                0U
#define FRAME_OFF_SYNC1                1U
#define FRAME_OFF_VERSION              2U
#define FRAME_OFF_TYPE                 3U
#define FRAME_OFF_LENGTH               4U
#define FRAME_OFF_SEQ                  5U
#define FRAME_OFF_PAYLOAD              7U

/* --- Mesaj turleri --- */
#define FRAME_TYPE_JOYSTICK         0x10U   /* X: int16, Y: int16  (4 bayt) */
#define FRAME_TYPE_JOYSTICK_MODE    0x11U   /* 0 kapali / 1 acik   (1 bayt) */
#define FRAME_TYPE_SET_OUTPUT       0x20U   /* cikis no + durum    (2 bayt) */
#define FRAME_TYPE_RESPONSE         0x80U   /* TYPE + sonuc kodu   (2 bayt) */

/**
  * @brief  Verilen alanlardan tam bir cerceve kurar.
  * @param  buf          Cercevenin yazilacagi tampon.
  * @param  buf_size     buf icindeki KULLANILABILIR yer (capacity).
  * @param  type         FRAME_TYPE_* degerlerinden biri.
  * @param  seq          Sira numarasi.
  * @param  payload      Payload baytlari; payload_len 0 ise NULL olabilir.
  * @param  payload_len  Payload boyutu, en fazla FRAME_MAX_PAYLOAD.
  * @retval Yazilan toplam bayt sayisi. Hata durumunda 0 (gecerli bir cerceve
  *         en az FRAME_OVERHEAD bayt oldugundan 0 karismaz).
  *         Hata halinde buf'a tek bayt yazilmaz.
  */
uint8_t frame_build(uint8_t *buf, uint8_t buf_size,
                    uint8_t type, uint16_t seq,
                    const uint8_t *payload, uint8_t payload_len);

/**
  * @brief  Joystick cercevesi kurar (TYPE 0x10, 4 baytlik payload).
  * @param  x, y  Isaretli eksen degerleri; little-endian yazilir.
  * @retval frame_build ile ayni.
  */
uint8_t frame_build_joystick(uint8_t *buf, uint8_t buf_size,
                             int16_t x, int16_t y, uint16_t seq);



/**
  * @brief  CRC-16/IBM-3740 (CRC-16/CCITT-FALSE) hesaplar.
  *         Polinom 0x1021, baslangic 0xFFFF, yansitma yok, cikis XOR yok.
  * @param  data     Hesaba katilacak baytlar. Fonksiyon veriyi degistirmez.
  * @param  len      data icindeki bayt sayisi. 0 olabilir.
  * @retval Hesaplanan CRC. len 0 veya data NULL ise 0xFFFF doner.
  */
uint16_t crc16_ccitt(const uint8_t *data, uint16_t len);



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

#endif /* INC_PROTOCOL_H_ */

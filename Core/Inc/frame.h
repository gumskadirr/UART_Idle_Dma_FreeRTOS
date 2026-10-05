/*
 * frame.h
 *
 * Protokol cercevesi (frame) olusturma. Cerceve, kendi sinir isaretlerini
 * tasiyan bir birimdir: senkron sozcugu + uzunluk alani + CRC. Bu yuzden
 * "packet" degil "frame" denir.
 *
 * Duzen:
 *   SYNC0 SYNC1 | VERSION | TYPE | LENGTH | SEQ(2) | PAYLOAD | CRC16(2)
 *
 *   LENGTH  : yalnizca payload boyutu. Toplam = FRAME_OVERHEAD + LENGTH.
 *   SEQ/CRC : little-endian (dusuk bayt once)
 *   CRC     : VERSION alanindan payload sonuna kadar hesaplanir;
 *             senkron baytlari ve CRC alaninin kendisi haric.
 */

#ifndef INC_FRAME_H_
#define INC_FRAME_H_

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

#endif /* INC_FRAME_H_ */

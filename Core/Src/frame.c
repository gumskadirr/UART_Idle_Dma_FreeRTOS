/*
 * frame.c
 *
 * Cerceve olusturma (gonderme yonu). Okuma yonu parser.c'de.
 *
 * Baytlar acik donusumlerle yaziliyor; C struct bellegi dogrudan UART'a
 * gonderilmiyor. Boylece padding ve bayt sirasi varsayimlarina dayanilmiyor.
 */
#include <stddef.h>
#include "frame.h"
#include "crc16.h"

/* 16 bit degeri little-endian yazar (dusuk bayt once).
   Ad sonundaki _le kasitli: endianness cagri yerinde gorunur olmali.
   Donus: yazilan bayt sayisi, her zaman FRAME_CRC_SIZE kadar (2). */


   static uint8_t put_u16_le(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value & 0xFFU);
    buf[1] = (uint8_t)((value >> 8) & 0xFFU);
    return 2U;
}

uint8_t frame_build(uint8_t *buf, uint8_t buf_size,
                    uint8_t type, uint16_t seq,
                    const uint8_t *payload, uint8_t payload_len)
{
    uint8_t  pos = 0U;   /* yazma imleci: siradaki bos indeks */
    uint8_t  i;          /* payload kopyalama sayaci */
    uint16_t crc;

    /* --- 1) Kontroller: tek bayt yazmadan once hepsi.
           Yarisi yazilmis bozuk bir cerceve birakmak, hic yazmamaktan
           daha kotudur: cagiran donus degerini kontrol etmezse bozuk
           veriyi hatta gonderir. --- */
    if (buf == NULL)
    {
        return 0U;
    }

    if ((payload == NULL) && (payload_len > 0U))
    {
        return 0U;
    }

    if (payload_len > FRAME_MAX_PAYLOAD)
    {
        return 0U;
    }

    /* Toplami 16 bitte hesapla: 8 bit aritmetikte tasma riski olmasin.
       Boylece bu satirin dogrulugu bir ustteki kontrolun varligina
       bagli kalmiyor. */
    if ((uint16_t)buf_size < ((uint16_t)FRAME_OVERHEAD + (uint16_t)payload_len))
    {
        return 0U;
    }

    /* --- 2) Baslik --- */
    buf[pos++] = FRAME_SYNC0;
    buf[pos++] = FRAME_SYNC1;
    buf[pos++] = PROTOCOL_VERSION;
    buf[pos++] = type;
    buf[pos++] = payload_len;          /* LENGTH: sadece payload boyutu */

    pos += put_u16_le(&buf[pos], seq);

    /* --- 3) Payload --- */
    for (i = 0U; i < payload_len; i++)
    {
        buf[pos++] = payload[i];
    }

    /* --- 4) CRC ---
       Bu noktada pos = FRAME_HEADER_SIZE + payload_len.
       CRC, VERSION alanindan payload sonuna kadar hesaplanir. */
    crc = crc16_ccitt(&buf[FRAME_OFF_VERSION],
                      (uint16_t)(pos - FRAME_OFF_VERSION));

    pos += put_u16_le(&buf[pos], crc);

    return pos;   /* toplam yazilan bayt sayisi */
}

uint8_t frame_build_joystick(uint8_t *buf, uint8_t buf_size,
                             int16_t x, int16_t y, uint16_t seq)
{
    uint8_t payload[4];

    /* Isaretli degerleri once uint16_t'ye cevir: bit islemleri isaretsiz
       tiplerde yapilir, isaret yorumu karsi tarafa birakilir. */
    (void)put_u16_le(&payload[0], (uint16_t)x);
    (void)put_u16_le(&payload[2], (uint16_t)y);

    return frame_build(buf, buf_size,
                       FRAME_TYPE_JOYSTICK, seq,
                       payload, (uint8_t)sizeof(payload));
}

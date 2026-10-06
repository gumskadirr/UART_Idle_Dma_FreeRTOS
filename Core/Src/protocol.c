/* Cerceve protokolu: CRC, gonderim olusturma ve alim ayristirma.
 * Donanim ve saat yonetimi cagirana aittir.
 */
#include "protocol.h"
#include <stddef.h>
#include <string.h>

/* --- CRC-16/CCITT-FALSE --- */
#define CRC16_POLY   0x1021U
#define CRC16_INIT   0xFFFFU

uint16_t crc16_ccitt(const uint8_t *data, uint16_t len)
{
    uint16_t crc = CRC16_INIT;
    uint16_t i;
    uint8_t  bit;

    if (data == NULL)
    {
        return crc;
    }

    for (i = 0U; i < len; i++)
    {
        /* Bayti crc'nin ust yarisina XOR'la: reflect in = false oldugu icin
           bitler en anlamlidan isleniyor. */
        crc ^= (uint16_t)((uint16_t)data[i] << 8);

        for (bit = 0U; bit < 8U; bit++)
        {
            if ((crc & 0x8000U) != 0U)
            {
                /* Ust bit 1: kaydir ve polinomu uygula */
                crc = (uint16_t)((uint16_t)(crc << 1) ^ CRC16_POLY);
            }
            else
            {
                /* Ust bit 0: sadece kaydir */
                crc = (uint16_t)(crc << 1);
            }
        }
    }

    return crc;   /* XOR out = 0x0000, ek islem yok */
}

/* --- Cerceve olusturma --- */
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

/* --- Bayt akisindan cerceve ayristirma --- */
/* Cozme denemesinin sonucu */
typedef enum
{
    PARSE_NEED_MORE = 0,   /* karar icin yeterli bayt yok, bekle */
    PARSE_OK,              /* tamponun basinda gecerli cerceve var */
    PARSE_INVALID          /* bu aday cerceve degil, bir bayt ilerle */
} parse_result_t;


void frame_parser_init(frame_parser_t *p)
{
    if (p == NULL)
    {
        return;
    }

    p->len           = 0U;
    p->frames_ok     = 0U;
    p->err_crc       = 0U;
    p->err_len       = 0U;
    p->err_version   = 0U;
    p->bytes_dropped = 0U;
    p->timeouts      = 0U;
}


/* Tamponun basindan n bayt cikarir, kalan baytlari basa kaydirir. */
static void buf_consume(frame_parser_t *p, uint8_t n)
{
    uint8_t remaining;

    if (n >= p->len)
    {
        /* Hepsi cikiyor: tasinacak bayt yok */
        p->len = 0U;
    }
    else
    {
        remaining = (uint8_t)(p->len - n);

        /* memmove, memcpy DEGIL: kaynak ve hedef ust uste biniyor ve
           memcpy'de bu durumun davranisi tanimsizdir. */
        memmove(&p->buf[0], &p->buf[n], remaining);

        p->len = remaining;
    }
}


/* Tamponun basindan bir cerceve cozmeyi dener.
   PARSE_OK donerse *out_frame_len toplam cerceve boyutunu tasir.
   Gecersiz adaylarda ilgili hata sayacini artirir.

   Kontrol sirasi veri bagimliligiyla zorunludur: CRC icin butun cerceve,
   toplam boyut icin LENGTH, LENGTH'e guvenmek icin SYNC ve VERSION gerekir.

   DIKKAT: len == 0 iken PARSE_INVALID DONULMEZ. Donulurse frame_parser_feed
   icindeki dongu hicbir bayt cikaramaz ve sonsuza kadar doner. */
static parse_result_t try_parse_frame(frame_parser_t *p, uint8_t *out_frame_len)
{
    uint16_t total_len;
    uint16_t crc_received;
    uint16_t crc_computed;

    if (p->len < 1U)
    {
        return PARSE_NEED_MORE;
    }
    if (p->buf[FRAME_OFF_SYNC0] != FRAME_SYNC0)
    {
        return PARSE_INVALID;
    }
    if (p->len < 2U)
    {
        return PARSE_NEED_MORE;
    }
    if (p->buf[FRAME_OFF_SYNC1] != FRAME_SYNC1)
    {
        return PARSE_INVALID;
    }
    if (p->len < FRAME_HEADER_SIZE)
    {
        return PARSE_NEED_MORE;             /* baslik henuz tam degil */
    }
    if (p->buf[FRAME_OFF_VERSION] != PROTOCOL_VERSION)
    {
        p->err_version++;
        return PARSE_INVALID;
    }
    if (p->buf[FRAME_OFF_LENGTH] > FRAME_MAX_PAYLOAD)
    {
        p->err_len++;
        return PARSE_INVALID;
    }

    /* LENGTH dogrulandi: toplam boyut artik guvenle hesaplanabilir */
    total_len = (uint16_t)FRAME_OVERHEAD + (uint16_t)p->buf[FRAME_OFF_LENGTH];

    if ((uint16_t)p->len < total_len)
    {
        return PARSE_NEED_MORE;
    }

    /* CRC alani cercevenin son iki bayti, little-endian */
    crc_received = (uint16_t)p->buf[total_len - FRAME_CRC_SIZE] |
                   ((uint16_t)p->buf[total_len - FRAME_CRC_SIZE + 1U] << 8);

    /* CRC, VERSION alanindan payload sonuna kadar hesaplanir: senkron
       baytlari ve CRC alaninin kendisi haric. */
    crc_computed = crc16_ccitt(&p->buf[FRAME_OFF_VERSION],
                               (uint16_t)(total_len - FRAME_OFF_VERSION
                                                    - FRAME_CRC_SIZE));

    if (crc_received != crc_computed)
    {
        p->err_crc++;
        return PARSE_INVALID;
    }

    *out_frame_len = (uint8_t)total_len;
    return PARSE_OK;
}


/* Tamponun basindan cozebildigi kadar cerceve cozer.
   Dongu gerekli, cunku bir PARSE_INVALID zincirleme tetiklenebilir: bir bayt
   cikarinca yeni aday hemen yeni bir karar uretebilir, yeni veri beklemeden. */
static void run_decode_loop(frame_parser_t *p,
                            frame_handler_t handler, void *user_data)
{
    uint8_t        frame_len;
    parse_result_t result;

    for (;;)
    {
        frame_len = 0U;
        result = try_parse_frame(p, &frame_len);

        if (result == PARSE_NEED_MORE)
        {
            break;
        }

        if (result == PARSE_OK)
        {
            frame_info_t info;

            p->frames_ok++;

            info.type        = p->buf[FRAME_OFF_TYPE];
            info.payload_len = p->buf[FRAME_OFF_LENGTH];
            info.seq         = (uint16_t)p->buf[FRAME_OFF_SEQ] |
                               ((uint16_t)p->buf[FRAME_OFF_SEQ + 1U] << 8);
            info.payload     = (info.payload_len > 0U)
                                 ? &p->buf[FRAME_OFF_PAYLOAD]
                                 : NULL;

            if (handler != NULL)
            {
                handler(&info, user_data);
            }

            /* Cikarma handler'dan SONRA: payload tampona isaret ediyor */
            buf_consume(p, frame_len);
        }
        else   /* PARSE_INVALID */
        {
            buf_consume(p, 1U);          /* sadece bir bayt: yeniden tarama */
            p->bytes_dropped++;
        }
    }
}


void frame_parser_feed(frame_parser_t *p,
                       const uint8_t *data, uint16_t len,
                       frame_handler_t handler, void *user_data)
{
    uint16_t i;

    if ((p == NULL) || ((data == NULL) && (len > 0U)))
    {
        return;
    }

    for (i = 0U; i < len; i++)
    {
        /* Guvenlik: tampon doluysa en eski bayti at. Dogru calisan bir
           try_parse_frame ile buraya normalde hic girilmez. */
        if (p->len >= (uint8_t)sizeof(p->buf))
        {
            buf_consume(p, 1U);
            p->bytes_dropped++;
        }

        p->buf[p->len] = data[i];
        p->len++;

        /* Yeni bayt geldi: cozebildigimiz kadar coz */
        run_decode_loop(p, handler, user_data);
    }
}


void frame_parser_timeout(frame_parser_t *p,
                          frame_handler_t handler, void *user_data)
{
    if ((p == NULL) || (p->len == 0U))
    {
        return;                      /* bekleyen aday yok */
    }

    p->timeouts++;

    /* Bir bayt at: bekleyen aday gecersiz sayiliyor. Tamponu bosaltmiyoruz,
       cunku adayin ICINDE gercek bir cerceve baslamis olabilir. Asagidaki
       dongu bunu hemen bulur. */
    buf_consume(p, 1U);
    p->bytes_dropped++;

    run_decode_loop(p, handler, user_data);
}


void frame_parser_discard(frame_parser_t *p)
{
    if (p == NULL)
    {
        return;
    }

    /* Atilan baytlar sayaca yazilir: hata toparlamada ne kaybettigimiz
       gorunur kalmali. */
    p->bytes_dropped = (uint16_t)(p->bytes_dropped + p->len);
    p->len = 0U;
}

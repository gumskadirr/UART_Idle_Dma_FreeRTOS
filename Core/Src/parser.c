/*
 * parser.c
 *
 * Kayan aday penceresi yaklasimi:
 *   gelen her bayti tamponun sonuna ekle, sonra tamponun BASINDAN cerceve
 *   cozmeyi dene. Cozulemezse SADECE BIR BAYT at ve tekrar dene.
 *
 * Cekirdek karar: hicbir bayt "islendi" diye atilmaz, yalnizca kanitlandiginda
 * atilir. Bir bayt atmak, bozuk adayin ICINDE baslayan gercek bir cercevenin
 * kaybolmamasini saglar.
 */
#include <stddef.h>
#include <string.h>
#include "parser.h"
#include "crc16.h"

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

/*
 * uart_comm_tests.c
 *
 * Birlesik UART calismasinin test kosucusu. Sozlesme uart_comm_tests.h'de.
 *
 * Bu dosya P0'da yalnizca ALTYAPIYI ve altyapinin kendi dogrulamasini icerir.
 * R1'den itibaren her adim kendi testlerini buraya ekler.
 */
#include <stddef.h>
#include <string.h>

#include "uart_comm_tests.h"

#ifdef UART_COMM_TEST

#include "tests.h"

/* --- Kayit defteri ---
   Butun sayim mantigi bir baglam uzerinden calisir. Boylece altyapinin
   KENDISI, uretim tablosunu kirletmeden ayri bir baglamla sinanabilir:
   "NOT_RUN gercekten basariyi engelliyor mu" sorusunu ancak icinde NOT_RUN
   bulunan bir tablo cevaplayabilir. */
typedef struct
{
    uart_comm_test_record_t *kayit;
    uint16_t cap;
    uint16_t sayisi;
    uint16_t gecen;
    uint16_t kalan;
    uint16_t kosmayan;
    uint16_t atlanan;
    uint8_t  tasti;
} ctx_t;

uart_comm_test_record_t uart_comm_test_kayit[UART_COMM_TEST_MAX];
uint16_t uart_comm_test_sayisi;
uint16_t uart_comm_test_gecen;
uint16_t uart_comm_test_kalan;
uint16_t uart_comm_test_kosmayan;
uint16_t uart_comm_test_atlanan;
uint8_t  uart_comm_test_tablo_tasti;

static ctx_t s_ctx;

static void ctx_kur(ctx_t *c, uart_comm_test_record_t *tablo, uint16_t cap)
{
    (void)memset(tablo, 0, (size_t)cap * sizeof(tablo[0]));
    c->kayit    = tablo;
    c->cap      = cap;
    c->sayisi   = 0U;
    c->gecen    = 0U;
    c->kalan    = 0U;
    c->kosmayan = 0U;
    c->atlanan  = 0U;
    c->tasti    = 0U;
}


static uint16_t ctx_kaydet(ctx_t *c, const char *id, uint8_t step,
                           uint8_t needs_hw)
{
    uart_comm_test_record_t *r;

    if (c->sayisi >= c->cap)
    {
        c->tasti = 1U;              /* sessizce dusurmek yerine gorunur ol */
        return 0xFFFFU;
    }

    r = &c->kayit[c->sayisi];
    r->id       = (id != NULL) ? id : "?";
    r->result   = (uint8_t)UART_COMM_TEST_NOT_RUN;
    r->source   = (uint8_t)UART_COMM_TEST_SRC_SW;
    r->needs_hw = needs_hw;
    r->step     = step;
    r->expected = 0U;
    r->found    = 0U;

    c->kosmayan++;                  /* kosana kadar NOT_RUN sayilir */
    c->sayisi++;
    return (uint16_t)(c->sayisi - 1U);
}


static void ctx_sonuc(ctx_t *c, uint16_t idx, uint8_t result, uint8_t source,
                      uint32_t expected, uint32_t found)
{
    uart_comm_test_record_t *r;

    if (idx >= c->sayisi)
    {
        return;                     /* kaydedilmemis indeks: yok sayilir */
    }

    r = &c->kayit[idx];

    if (r->result != (uint8_t)UART_COMM_TEST_NOT_RUN)
    {
        /* Ayni teste ikinci sonuc: hangisinin dogru oldugu bilinemez. Sessizce
           ustune yazmak gecmis bir FAIL'i silebilirdi; bu yuzden FAIL. */
        if (r->result == (uint8_t)UART_COMM_TEST_PASS)
        {
            c->gecen--;
            c->kalan++;
        }
        else if (r->result == (uint8_t)UART_COMM_TEST_SKIP)
        {
            c->atlanan--;
            c->kalan++;
        }
        else
        {
            /* zaten FAIL */
        }
        r->result = (uint8_t)UART_COMM_TEST_FAIL;
        return;
    }

    c->kosmayan--;
    r->result   = result;
    r->source   = source;
    r->expected = expected;
    r->found    = found;

    if (result == (uint8_t)UART_COMM_TEST_PASS)
    {
        c->gecen++;
    }
    else if (result == (uint8_t)UART_COMM_TEST_SKIP)
    {
        c->atlanan++;
    }
    else
    {
        c->kalan++;
    }
}


static uint8_t ctx_ok(const ctx_t *c)
{
    return (uint8_t)((c->sayisi > 0U) &&
                     (c->kalan == 0U) &&
                     (c->kosmayan == 0U) &&
                     (c->atlanan == 0U) &&
                     (c->tasti == 0U));
}


/* Baglam sayaclarini disaridan okunan global sayaclara yansitir. */
static void ctx_yayimla(const ctx_t *c)
{
    uart_comm_test_sayisi      = c->sayisi;
    uart_comm_test_gecen       = c->gecen;
    uart_comm_test_kalan       = c->kalan;
    uart_comm_test_kosmayan    = c->kosmayan;
    uart_comm_test_atlanan     = c->atlanan;
    uart_comm_test_tablo_tasti = c->tasti;
}


/* --- Public API: uretim tablosu uzerinde calisir --- */

uint16_t uart_comm_test_kaydet(const char *id, uint8_t step, uint8_t needs_hw)
{
    uint16_t idx = ctx_kaydet(&s_ctx, id, step, needs_hw);
    ctx_yayimla(&s_ctx);
    return idx;
}


void uart_comm_test_sonuc(uint16_t idx, uint8_t result, uint8_t source,
                          uint32_t expected, uint32_t found)
{
    ctx_sonuc(&s_ctx, idx, result, source, expected, found);
    ctx_yayimla(&s_ctx);
}


void uart_comm_test_bool(uint16_t idx, uint8_t dogru_mu)
{
    uart_comm_test_sonuc(idx,
                         (uint8_t)((dogru_mu != 0U) ? UART_COMM_TEST_PASS
                                                    : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_SW, 1U,
                         (uint32_t)(dogru_mu != 0U));
}


uint8_t uart_comm_tests_ok(void)
{
    return ctx_ok(&s_ctx);
}


/* --- P0: altyapinin kendi dogrulamasi ---
   Donanim gerektirmez. Ayri bir baglam kullanir, uretim tablosunu kirletmez. */

static uint8_t p0_not_run_baslangici(void)
{
    uart_comm_test_record_t tablo[2];
    ctx_t c;
    uint16_t a;

    ctx_kur(&c, tablo, 2U);
    a = ctx_kaydet(&c, "PROBE_A", 0U, 0U);

    return (uint8_t)((a == 0U) &&
                     (c.sayisi == 1U) &&
                     (c.kosmayan == 1U) &&
                     (c.gecen == 0U) &&
                     (tablo[0].result == (uint8_t)UART_COMM_TEST_NOT_RUN));
}


static uint8_t p0_not_run_basariyi_engeller(void)
{
    uart_comm_test_record_t tablo[2];
    ctx_t c;
    uint16_t a;
    uint8_t  ok_kosmayanla;

    ctx_kur(&c, tablo, 2U);
    a = ctx_kaydet(&c, "PROBE_A", 0U, 0U);
    (void)ctx_kaydet(&c, "PROBE_B", 0U, 0U);   /* bilerek kosturulmuyor */
    ctx_sonuc(&c, a, (uint8_t)UART_COMM_TEST_PASS,
              (uint8_t)UART_COMM_TEST_SRC_SW, 0U, 0U);

    /* ASIL IDDIA: dusen test YOK ama kosmayan var -> basari DEGIL. */
    ok_kosmayanla = ctx_ok(&c);

    return (uint8_t)((c.kalan == 0U) &&
                     (c.kosmayan == 1U) &&
                     (ok_kosmayanla == 0U));
}


static uint8_t p0_cift_sonuc_fail(void)
{
    uart_comm_test_record_t tablo[2];
    ctx_t c;
    uint16_t a;

    ctx_kur(&c, tablo, 2U);
    a = ctx_kaydet(&c, "PROBE_A", 0U, 0U);
    ctx_sonuc(&c, a, (uint8_t)UART_COMM_TEST_PASS,
              (uint8_t)UART_COMM_TEST_SRC_SW, 0U, 0U);
    ctx_sonuc(&c, a, (uint8_t)UART_COMM_TEST_PASS,
              (uint8_t)UART_COMM_TEST_SRC_SW, 0U, 0U);

    return (uint8_t)((tablo[0].result == (uint8_t)UART_COMM_TEST_FAIL) &&
                     (c.gecen == 0U) &&
                     (c.kalan == 1U));
}


static uint8_t p0_tablo_tasmasi_gorunur(void)
{
    uart_comm_test_record_t tablo[1];
    ctx_t c;
    uint16_t b;

    ctx_kur(&c, tablo, 1U);
    (void)ctx_kaydet(&c, "PROBE_A", 0U, 0U);
    b = ctx_kaydet(&c, "PROBE_B", 0U, 0U);     /* yer yok */

    return (uint8_t)((b == 0xFFFFU) && (c.tasti == 1U) && (ctx_ok(&c) == 0U));
}


void uart_comm_tests_run(void)
{
    uint16_t i_not_run;
    uint16_t i_engel;
    uint16_t i_cift;
    uint16_t i_tasma;
    uint16_t i_eski_birim;
    uint16_t i_eski_lb;

    ctx_kur(&s_ctx, uart_comm_test_kayit, UART_COMM_TEST_MAX);

    /* Once HEPSI kaydedilir: kosucu asagida erken cikarsa bile kosmayanlar
       listede NOT_RUN olarak gorunur. */
    i_not_run    = uart_comm_test_kaydet("P0_REGISTRY_STARTS_NOT_RUN",  0U, 0U);
    i_engel      = uart_comm_test_kaydet("P0_NOT_RUN_BLOCKS_OK",        0U, 0U);
    i_cift       = uart_comm_test_kaydet("P0_DOUBLE_RESULT_IS_FAIL",    0U, 0U);
    i_tasma      = uart_comm_test_kaydet("P0_TABLE_OVERFLOW_VISIBLE",   0U, 0U);
    i_eski_birim = uart_comm_test_kaydet("P0_LEGACY_UNIT_COUNT",        0U, 0U);
    i_eski_lb    = uart_comm_test_kaydet("P0_LEGACY_LOOPBACK_COMPLETE", 0U, 1U);

    uart_comm_test_bool(i_not_run, p0_not_run_baslangici());
    uart_comm_test_bool(i_engel,   p0_not_run_basariyi_engeller());
    uart_comm_test_bool(i_cift,    p0_cift_sonuc_fail());
    uart_comm_test_bool(i_tasma,   p0_tablo_tasmasi_gorunur());

    /* Eski kosucular main.c'de zaten calisti; burada SONUCLARI dogrulanir.
       Beklenen sayida test kostu mu sorusu, "dusen yok" sorusundan ayridir. */
    uart_comm_test_sonuc(i_eski_birim,
                         (uint8_t)(((test_sayim_dogru != 0U) &&
                                    (test_kalan == 0U))
                                   ? UART_COMM_TEST_PASS
                                   : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_SW,
                         (uint32_t)TEST_BEKLENEN_ADET,
                         (uint32_t)test_sayisi);

    /* Loopback jumper yoksa kosucu erken cikar: bu FAIL degil SKIP'tir, ama
       uart_comm_tests_ok() yine de 0 doner. Donanimsiz "gecti" olmaz. */
    uart_comm_test_sonuc(i_eski_lb,
                         (uint8_t)((lb_sayisi == 0U)
                                   ? UART_COMM_TEST_SKIP
                                   : (((lb_kalan == 0U) &&
                                       (lb_calismayan == 0U) &&
                                       (lb_sayisi == LB_BEKLENEN_ADET))
                                      ? UART_COMM_TEST_PASS
                                      : UART_COMM_TEST_FAIL)),
                         (uint8_t)UART_COMM_TEST_SRC_SW,
                         (uint32_t)LB_BEKLENEN_ADET,
                         (uint32_t)lb_sayisi);
}

#endif /* UART_COMM_TEST */

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
#include "uart_rx.h"
#include "uart_tx.h"
#include "frame.h"
#include "parser.h"

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


/* ==================== R1: start sahipligi ve acik RX durumlari ==========
   Hepsi kart + PA2-PA3 jumper gerektirir: sinanan sey yazilim durumunun
   CALISAN bir alim karsisindaki davranisi. */

static void r1_service_dondur(uint32_t ms)
{
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < ms)
    {
        uart_rx_service();
    }
}


/* Alimi GUVENLI bicimde durdurur: uart_rx_start donanim hala aktifken
   HAL_BUSY dondugu icin basarisiz-baslatma testleri bunu gerektirir. */
static void r1_alimi_durdur(UART_HandleTypeDef *huart)
{
    (void)HAL_UART_AbortReceive(huart);
    r1_service_dondur(2U);
}


/* Alimi tekrar RUNNING'e getirir; testler arasi ortak toparlama. */
static uint8_t r1_alimi_kur(UART_HandleTypeDef *huart)
{
    uart_rx_force_start_fail(0U);
    uart_rx_test_sync_error_on_start(0U);
    uart_rx_force_restart_fail(0U);

    if (uart_rx_start(huart) != HAL_OK)
    {
        return 0U;
    }
    return (uint8_t)(uart_rx_get_phase() == UART_RX_PHASE_RUNNING);
}


/* Calisan alimi FAULT'a dusurup donanimi durdurur. RUNNING'de start
   reddedildigi icin basarisiz-baslatma testleri once buradan gecer.
   Kullanilan yol GERCEK gecis yoludur; phase elle yazilmaz. */
static uint8_t r1_fault_ve_durdur(UART_HandleTypeDef *huart)
{
    uart_rx_force_restart_fail(1U);
    uart_rx_test_inject_error();
    r1_service_dondur(60U);              /* 5 deneme x 5 ms */
    uart_rx_force_restart_fail(0U);

    if (uart_rx_get_phase() != UART_RX_PHASE_FAULT)
    {
        return 0U;
    }

    r1_alimi_durdur(huart);
    return 1U;
}


/* RX_START_BUSY_PRESERVES_STATE
   Calisan alima yarim cerceve ver, ikinci start'i dene. HAL_BUSY donmeli ve
   TEK BIR yazilim alani degismemeli; kalan baytlar gelince cerceve BIR KEZ
   cozulmeli. Eski kod once sifirlayip sonra BUSY donuyordu: aday kaybolur,
   okuma konumu basa doner ve eski DMA verisi yeniden tuketilirdi. */
static uint8_t r1_start_busy_durumu_korur(UART_HandleTypeDef *huart)
{
    uint8_t  cerceve[FRAME_MAX_SIZE];
    uint8_t  n;
    uint16_t aday_len;
    uint16_t ok_once;
    uint16_t drop_once;
    uint16_t gaps_once;
    uint16_t seq_once;
    uint16_t restarts_once;
    HAL_StatusTypeDef st;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    n = frame_build_joystick(cerceve, (uint8_t)sizeof(cerceve), 321, -321, 601U);
    if (n == 0U)
    {
        return 0U;
    }

    /* Yarim cerceve: baslik gider, govde beklemede kalir */
    if (HAL_UART_Transmit(huart, cerceve, FRAME_HEADER_SIZE, 100U) != HAL_OK)
    {
        return 0U;
    }
    HAL_Delay(2U);
    uart_rx_service();

    aday_len      = uart_rx_get_parser()->len;
    ok_once       = uart_rx_get_parser()->frames_ok;
    drop_once     = uart_rx_get_parser()->bytes_dropped;
    gaps_once     = uart_rx_state.seq_gaps;
    seq_once      = uart_rx_state.last_seq;
    restarts_once = uart_rx_stats.restarts;

    if (aday_len != FRAME_HEADER_SIZE)
    {
        return 0U;                       /* onkosul kurulamadi */
    }

    /* ASIL CAGRI: calisan alima ikinci start */
    st = uart_rx_start(huart);
    if (st != HAL_BUSY)
    {
        return 0U;
    }

    /* Reddedilen cagri HICBIR SEYI degistirmemeli */
    if ((uart_rx_get_parser()->len           != aday_len)  ||
        (uart_rx_get_parser()->frames_ok     != ok_once)   ||
        (uart_rx_get_parser()->bytes_dropped != drop_once) ||
        (uart_rx_state.seq_gaps  != gaps_once) ||
        (uart_rx_state.last_seq  != seq_once)  ||
        (uart_rx_stats.restarts  != restarts_once) ||
        (uart_rx_get_phase()     != UART_RX_PHASE_RUNNING))
    {
        return 0U;
    }

    /* Kalan baytlar: cerceve bir kez ve dogru cozulmeli */
    if (HAL_UART_Transmit(huart, &cerceve[FRAME_HEADER_SIZE],
                          (uint16_t)(n - FRAME_HEADER_SIZE), 100U) != HAL_OK)
    {
        return 0U;
    }
    HAL_Delay(3U);
    uart_rx_service();

    return (uint8_t)((uart_rx_get_parser()->frames_ok ==
                      (uint16_t)(ok_once + 1U)) &&
                     (uart_rx_state.last_seq == 601U) &&
                     (uart_rx_state.joy_x == 321) &&
                     (uart_rx_state.joy_y == -321) &&
                     (uart_rx_get_parser()->bytes_dropped == drop_once));
}


/* RX_REBIND_ACTIVE
   Alim calisirken BASKA bir handle ile start reddedilmeli ve eski sahiplik
   bozulmamali: ret sonrasi gercek handle uzerinden cerceve hala gelmeli. */
static uint8_t r1_baska_handle_reddedilir(UART_HandleTypeDef *huart)
{
    UART_HandleTypeDef sahte;
    DMA_HandleTypeDef  sahte_dma;
    uint8_t  cerceve[FRAME_MAX_SIZE];
    uint8_t  n;
    uint16_t ok_once;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    (void)memset(&sahte, 0, sizeof(sahte));
    (void)memset(&sahte_dma, 0, sizeof(sahte_dma));
    sahte.Instance = USART1;              /* bu projede kullanilmiyor */
    sahte.hdmarx   = &sahte_dma;

    if (uart_rx_start(&sahte) != HAL_BUSY)
    {
        return 0U;
    }

    /* Eski sahiplik bozulmadi mi: gercek handle hala alim yapiyor olmali */
    ok_once = uart_rx_get_parser()->frames_ok;
    n = frame_build_joystick(cerceve, (uint8_t)sizeof(cerceve), 5, -5, 602U);
    if (n == 0U)
    {
        return 0U;
    }
    if (HAL_UART_Transmit(huart, cerceve, n, 100U) != HAL_OK)
    {
        return 0U;
    }
    HAL_Delay(3U);
    uart_rx_service();

    return (uint8_t)((uart_rx_get_parser()->frames_ok ==
                      (uint16_t)(ok_once + 1U)) &&
                     (uart_rx_state.last_seq == 602U) &&
                     (uart_rx_get_phase() == UART_RX_PHASE_RUNNING));
}


/* RX_START_REFUSED_WHILE_HW_ACTIVE
   FAULT'ta olsak bile donanim hala tamponu yaziyorsa yeni oturum
   KURULMAMALI: yeni start eski DMA'nin ustune yazardi. Ayrica reddedilen
   istek bir "basarisiz baslatma" degildir, start_fails artmamali. */
static uint8_t r1_donanim_aktifken_start_yok(UART_HandleTypeDef *huart)
{
    uint16_t rejects_once;
    uint16_t fails_once;
    HAL_StatusTypeDef st;

    /* Senkron hata ile FAULT: donanim calisiyor ama oturum saglik disi */
    r1_alimi_durdur(huart);
    uart_rx_test_sync_error_on_start(1U);
    st = uart_rx_start(huart);
    uart_rx_test_sync_error_on_start(0U);

    if ((st != HAL_ERROR) || (uart_rx_get_phase() != UART_RX_PHASE_FAULT))
    {
        r1_alimi_durdur(huart);
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    rejects_once = uart_rx_stats.start_rejects;
    fails_once   = uart_rx_stats.start_fails;
    st = uart_rx_start(huart);

    if ((st != HAL_BUSY) ||
        (uart_rx_stats.start_rejects != (uint16_t)(rejects_once + 1U)) ||
        (uart_rx_stats.start_fails   != fails_once))
    {
        r1_alimi_durdur(huart);
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    /* Durus saglandiktan SONRA kurulum kabul edilmeli */
    r1_alimi_durdur(huart);
    return r1_alimi_kur(huart);
}


/* RX_START_FAIL_VISIBLE
   Baslatma dustugunde alim RUNNING GORUNMEMELI ve sayacta iz birakmali. */
static uint8_t r1_basarisiz_start_gorunur(UART_HandleTypeDef *huart)
{
    uint16_t fails_once;
    HAL_StatusTypeDef st;
    uint8_t  sonuc;

    if (r1_fault_ve_durdur(huart) == 0U)
    {
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    fails_once = uart_rx_stats.start_fails;

    uart_rx_force_start_fail(1U);
    st = uart_rx_start(huart);

    sonuc = (uint8_t)((st == HAL_ERROR) &&
                      (uart_rx_get_phase() == UART_RX_PHASE_FAULT) &&
                      (uart_rx_stats.start_fails ==
                       (uint16_t)(fails_once + 1U)));

    /* Kanca HAL'i gercekten baslatmis olabilir: once durdur, sonra kur. */
    r1_alimi_durdur(huart);
    if (r1_alimi_kur(huart) == 0U)
    {
        sonuc = 0U;
    }
    return sonuc;
}


/* RX_SYNC_ERROR_BEFORE_HAL_RETURN
   HAL cagrisi DONMEDEN hata callback'i gelirse HAL_OK'a ragmen alim saglikli
   sayilmamali. "HAL_OK dondu" tek basina kanit degildir (RX-4, TX-5). */
static uint8_t r1_senkron_hata_running_olmaz(UART_HandleTypeDef *huart)
{
    HAL_StatusTypeDef st;
    uint8_t  sonuc;

    r1_alimi_durdur(huart);

    uart_rx_test_sync_error_on_start(1U);
    st = uart_rx_start(huart);
    uart_rx_test_sync_error_on_start(0U);

    sonuc = (uint8_t)((st == HAL_ERROR) &&
                      (uart_rx_get_phase() != UART_RX_PHASE_RUNNING) &&
                      (uart_rx_get_phase() == UART_RX_PHASE_FAULT));

    r1_alimi_durdur(huart);
    if (r1_alimi_kur(huart) == 0U)
    {
        sonuc = 0U;
    }
    return sonuc;
}


void uart_comm_tests_run(UART_HandleTypeDef *huart)
{
    uint16_t i_not_run;
    uint16_t i_engel;
    uint16_t i_cift;
    uint16_t i_tasma;
    uint16_t i_eski_birim;
    uint16_t i_eski_lb;
    uint16_t i_r1_busy;
    uint16_t i_r1_rebind;
    uint16_t i_r1_hwaktif;
    uint16_t i_r1_startfail;
    uint16_t i_r1_senkron;
    uint8_t  hw;

    ctx_kur(&s_ctx, uart_comm_test_kayit, UART_COMM_TEST_MAX);

    /* Donanim onkosulu: alim RUNNING degilse R1 testleri kosamaz. Bunu FAIL
       degil SKIP olarak isaretlemek dogru; ama uart_comm_tests_ok() yine 0
       doner, yani jumper yokken "gecti" denemez. */
    hw = (uint8_t)((huart != NULL) &&
                   (uart_rx_get_phase() == UART_RX_PHASE_RUNNING));

    /* Once HEPSI kaydedilir: kosucu asagida erken cikarsa bile kosmayanlar
       listede NOT_RUN olarak gorunur. */
    i_not_run    = uart_comm_test_kaydet("P0_REGISTRY_STARTS_NOT_RUN",  0U, 0U);
    i_engel      = uart_comm_test_kaydet("P0_NOT_RUN_BLOCKS_OK",        0U, 0U);
    i_cift       = uart_comm_test_kaydet("P0_DOUBLE_RESULT_IS_FAIL",    0U, 0U);
    i_tasma      = uart_comm_test_kaydet("P0_TABLE_OVERFLOW_VISIBLE",   0U, 0U);
    i_eski_birim = uart_comm_test_kaydet("P0_LEGACY_UNIT_COUNT",        0U, 0U);
    i_eski_lb    = uart_comm_test_kaydet("P0_LEGACY_LOOPBACK_COMPLETE", 0U, 1U);

    i_r1_busy      = uart_comm_test_kaydet("RX_START_BUSY_PRESERVES_STATE",   1U, 1U);
    i_r1_rebind    = uart_comm_test_kaydet("RX_REBIND_ACTIVE",                1U, 1U);
    i_r1_hwaktif   = uart_comm_test_kaydet("RX_START_REFUSED_WHILE_HW_ACTIVE",1U, 1U);
    i_r1_startfail = uart_comm_test_kaydet("RX_START_FAIL_VISIBLE",           1U, 1U);
    i_r1_senkron   = uart_comm_test_kaydet("RX_SYNC_ERROR_BEFORE_HAL_RETURN", 1U, 1U);

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

    /* --- R1 --- */
    if (hw == 0U)
    {
        uint16_t i;
        for (i = i_r1_busy; i <= i_r1_senkron; i++)
        {
            uart_comm_test_sonuc(i, (uint8_t)UART_COMM_TEST_SKIP,
                                 (uint8_t)UART_COMM_TEST_SRC_SW, 0U, 0U);
        }
        return;
    }

    uart_comm_test_bool(i_r1_busy,   r1_start_busy_durumu_korur(huart));
    uart_comm_test_bool(i_r1_rebind, r1_baska_handle_reddedilir(huart));

    /* Asagidakiler alimi bilerek FAULT'a dusurup geri kuruyor; kaynak
       etiketi ENJEKTE, cunku hatalar test kancasindan geliyor. */
    uart_comm_test_sonuc(i_r1_hwaktif,
                         (uint8_t)(r1_donanim_aktifken_start_yok(huart)
                                   ? UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_INJECTED, 0U, 0U);

    uart_comm_test_sonuc(i_r1_startfail,
                         (uint8_t)(r1_basarisiz_start_gorunur(huart)
                                   ? UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_INJECTED, 0U, 0U);

    uart_comm_test_sonuc(i_r1_senkron,
                         (uint8_t)(r1_senkron_hata_running_olmaz(huart)
                                   ? UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_INJECTED, 0U, 0U);
}

#endif /* UART_COMM_TEST */

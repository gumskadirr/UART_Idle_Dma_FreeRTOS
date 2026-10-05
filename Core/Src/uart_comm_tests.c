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
#include "app_protocol.h"
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
    gaps_once     = app_proto_state.seq_gap_events;
    seq_once      = app_proto_state.last_seq;
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
        (app_proto_state.seq_gap_events  != gaps_once) ||
        (app_proto_state.last_seq  != seq_once)  ||
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
                     (app_proto_state.last_seq == 601U) &&
                     (app_proto_state.joy_x == 321) &&
                     (app_proto_state.joy_y == -321) &&
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
                     (app_proto_state.last_seq == 602U) &&
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


/* ==================== R2: gercek tur sayaci ve tutarli ornek ===========
   Sinir aritmetigi donanimsiz kosar (saf fonksiyon); sarim ve oturum
   davranisi karttan dogrulanir. Ikisi AYRI testlerdir: model sonucu
   donanim NDTR/TC sirasinin kaniti degildir. */

/* N bayt uretilmis durumun "ISR islemis" gosterimi:
   wrap_base = tam turlar, pending_tc = 0, ndtr = 256 - tur ici konum. */
static uint32_t r2_yerlesik(uint32_t n)
{
    uint32_t tur = n / UART_RX_BUF_SIZE;
    uint32_t poz = n % UART_RX_BUF_SIZE;

    return uart_rx_producer_from(tur * UART_RX_BUF_SIZE, 0U,
                                 UART_RX_BUF_SIZE - poz);
}


/* Ayni N'in "TC bayragi kalkti ama ISR calismadi" gosterimi:
   son tur wrap_base'e HENUZ eklenmemis. */
static uint32_t r2_bekleyen_tc(uint32_t n)
{
    uint32_t tur = n / UART_RX_BUF_SIZE;
    uint32_t poz = n % UART_RX_BUF_SIZE;

    if (tur == 0U)
    {
        return r2_yerlesik(n);        /* bekleyen tur yok */
    }

    return uart_rx_producer_from((tur - 1U) * UART_RX_BUF_SIZE, 1U,
                                 UART_RX_BUF_SIZE - poz);
}


/* RX_PRODUCER_BOUNDARIES
   Tur sinirlarinda mutlak konum tam olarak uretilen bayt sayisi olmali.
   Eski modulo konum 0 ile 256'yi ayni gosteriyordu; asil hata buydu. */
static uint8_t r2_sinir_degerleri(void)
{
    static const uint32_t ornekler[] =
        { 0U, 1U, 127U, 128U, 255U, 256U, 257U, 511U, 512U, 513U, 768U };
    uint16_t i;

    for (i = 0U; i < (sizeof(ornekler) / sizeof(ornekler[0])); i++)
    {
        if (r2_yerlesik(ornekler[i]) != ornekler[i])
        {
            return 0U;
        }
    }

    /* 0 ile 256 ARTIK ayni degil: tasmanin gorulebilmesi buna bagli. */
    return (uint8_t)(r2_yerlesik(0U) != r2_yerlesik(UART_RX_BUF_SIZE));
}


/* RX_PENDING_TC
   TC bayragi set, ISR henuz calismamis, NDTR yeniden yuklenmis durumda da
   ayni konum bulunmali; ISR calistiktan SONRA deger DEGISMEMELI. */
static uint8_t r2_bekleyen_tc_telafisi(void)
{
    static const uint32_t ornekler[] =
        { 256U, 257U, 300U, 511U, 512U, 700U };
    uint16_t i;

    for (i = 0U; i < (sizeof(ornekler) / sizeof(ornekler[0])); i++)
    {
        if (r2_bekleyen_tc(ornekler[i]) != ornekler[i])
        {
            return 0U;
        }
        if (r2_bekleyen_tc(ornekler[i]) != r2_yerlesik(ornekler[i]))
        {
            return 0U;      /* ISR oncesi ve sonrasi ayni konum */
        }
    }
    return 1U;
}


/* RX_COUNTER_WRAP
   wrap_base ve consumed UINT32 sinirini gecerken FARK dogru kalmali.
   Mutlak sayaclar buyudugu icin bu kacinilmaz bir durum, hata degil. */
static uint8_t r2_sayac_sarimi(void)
{
    uint32_t base = 0xFFFFFF00UL;      /* bir sonraki turda sarar */
    uint32_t produced;
    uint32_t consumed;

    /* Tur ici 100 bayt: 0xFFFFFF00 + 100 */
    produced = uart_rx_producer_from(base, 0U, UART_RX_BUF_SIZE - 100U);
    if (produced != (uint32_t)(base + 100U))
    {
        return 0U;
    }

    /* Tuketici sarimin ONCESINDE, uretici SONRASINDA */
    consumed = (uint32_t)(base + 200U);
    produced = uart_rx_producer_from((uint32_t)(base + UART_RX_BUF_SIZE),
                                     0U, UART_RX_BUF_SIZE - 50U);

    /* Beklenen fark: (base+256+50) - (base+200) = 106 */
    if ((uint32_t)(produced - consumed) != 106U)
    {
        return 0U;
    }

    /* Tam sarim: consumed UINT32_MAX'a cok yakin, produced sarmis */
    consumed = 0xFFFFFFF0UL;
    produced = 0x00000010UL;           /* 32 bayt ileri */
    return (uint8_t)((uint32_t)(produced - consumed) == 32U);
}


/* RX_SAMPLE_STUCK
   Surekli gecersiz ornekte: tuketici ILERLEMEZ, erteleme sayaci artar,
   CPU sonsuz donguye girmez ve 20 ms sonunda GORUNUR saglik hatasi olusur. */
static uint8_t r2_ornek_takildi(UART_HandleTypeDef *huart)
{
    uint32_t consumed_once;
    uint16_t defers_once;
    uint16_t fails_once;
    uint32_t t0;
    uint8_t  sonuc;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    consumed_once = uart_rx_get_consumed();
    defers_once   = uart_rx_stats.sample_defers;
    fails_once    = uart_rx_stats.sample_fails;

    uart_rx_test_force_sample_fail(1U);

    /* 40 ms boyunca servis dondur: 20 ms sinirini gecmeli. Sonsuz dongu
       olsaydi bu cagri hic donmezdi; donmesi testin bir parcasi. */
    t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < 40U)
    {
        uart_rx_service();
    }

    uart_rx_test_force_sample_fail(0U);

    sonuc = (uint8_t)((uart_rx_get_consumed() == consumed_once) &&
                      (uart_rx_stats.sample_defers > defers_once) &&
                      (uart_rx_stats.sample_fails ==
                       (uint16_t)(fails_once + 1U)));

    /* Saglik hatasi RX toparlanmasini tetikledi; alimi tekrar kur. */
    r1_alimi_durdur(huart);
    if (r1_alimi_kur(huart) == 0U)
    {
        sonuc = 0U;
    }
    return sonuc;
}


/* RX_ABORT_TC_IGNORED
   RUNNING/STARTING disindaki bir oturumda gelen TC normal uretime
   EKLENMEMELI: iptal edilen oturumun turu yeni oturumu ileri sicratirdi. */
static uint8_t r2_abort_tc_sayilmaz(UART_HandleTypeDef *huart)
{
    uint32_t base_once;
    uint8_t  sonuc;

    /* Alimi FAULT'a dusur: phase artik RUNNING/STARTING degil */
    if (r1_fault_ve_durdur(huart) == 0U)
    {
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    base_once = uart_rx_test_get_wrap_base();

    /* Bu oturuma ait OLMAYAN bir TC bildirimi */
    huart->RxEventType = HAL_UART_RXEVENT_TC;
    HAL_UARTEx_RxEventCallback(huart, UART_RX_BUF_SIZE);

    sonuc = (uint8_t)(uart_rx_test_get_wrap_base() == base_once);

    if (r1_alimi_kur(huart) == 0U)
    {
        sonuc = 0U;
    }
    return sonuc;
}


/* RX_PRODUCER_RESTART
   Birden cok turdan sonra yeniden kurulan oturumun ilk bayti producer = 1
   olmali; eski wrap_base sizip sahte tasma uretmemeli. */
static uint8_t r2_restart_sifirlar(UART_HandleTypeDef *huart)
{
    uint8_t  cerceve[FRAME_MAX_SIZE];
    uint8_t  n;
    uint32_t produced;
    uint16_t overruns_once;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    /* Birkac tur uret: 24 x 13 = 312 bayt, bir turu asar */
    for (n = 0U; n < 24U; n++)
    {
        uint8_t m = frame_build_joystick(cerceve, (uint8_t)sizeof(cerceve),
                                         1, 1, (uint16_t)(700U + n));
        if (m == 0U)
        {
            return 0U;
        }
        if (HAL_UART_Transmit(huart, cerceve, m, 100U) != HAL_OK)
        {
            return 0U;
        }
        HAL_Delay(1U);
        uart_rx_service();
    }

    if (uart_rx_test_get_wrap_base() == 0U)
    {
        return 0U;                  /* onkosul: en az bir tur donmus olmali */
    }

    overruns_once = uart_rx_stats.overruns;

    /* Yeniden kur: sayaclar sifirlanmali */
    r1_alimi_durdur(huart);
    if (r1_alimi_kur(huart) == 0U)
    {
        return 0U;
    }

    if ((uart_rx_test_get_wrap_base() != 0U) ||
        (uart_rx_get_consumed() != 0U))
    {
        return 0U;
    }

    /* Yeni oturumun ILK bayti: producer tam olarak 1 olmali */
    if (HAL_UART_Transmit(huart, cerceve, 1U, 100U) != HAL_OK)
    {
        return 0U;
    }
    HAL_Delay(2U);

    if (uart_rx_get_produced(&produced) == 0U)
    {
        return 0U;
    }

    uart_rx_service();

    return (uint8_t)((produced == 1U) &&
                     (uart_rx_stats.overruns == overruns_once));
}


/* RX_TC_IDLE_SAME_POSITION
   Sarim sinirinda TC ve IDLE ayni konumu bildirir. IDLE'in Size == 256
   bildirimi IKINCI bir tur olarak sayilmamali: 512 bayt gonderip tam iki
   tur bekliyoruz, uc degil. */
static uint8_t r2_tc_idle_ayni_konum(UART_HandleTypeDef *huart)
{
    uint8_t  blok[64];
    uint16_t i;
    uint32_t base_once;
    uint32_t produced_once;
    uint32_t produced;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    for (i = 0U; i < sizeof(blok); i++)
    {
        blok[i] = 0x00U;            /* SYNC degil: ayristirici hepsini eler */
    }

    if (uart_rx_get_produced(&produced_once) == 0U)
    {
        return 0U;
    }
    base_once = uart_rx_test_get_wrap_base();

    /* 8 x 64 = 512 bayt: tam iki tur. Her blok sonrasi tuket ki tasma
       politikasi devreye girip olcumu bozmasin. */
    for (i = 0U; i < 8U; i++)
    {
        if (HAL_UART_Transmit(huart, blok, (uint16_t)sizeof(blok),
                              100U) != HAL_OK)
        {
            return 0U;
        }
        HAL_Delay(2U);
        uart_rx_service();
    }

    HAL_Delay(5U);
    uart_rx_service();

    if (uart_rx_get_produced(&produced) == 0U)
    {
        return 0U;
    }

    /* TAM 512 bayt ilerlemis olmali: IDLE'in sarim sinirindaki Size == 256
       bildirimi ikinci kez sayilsaydi 768 gorurduk. */
    return (uint8_t)(((uint32_t)(produced - produced_once) == 512U) &&
                     ((uint32_t)(uart_rx_test_get_wrap_base() - base_once) ==
                      (2U * UART_RX_BUF_SIZE)));
}


/* ==================== R3: tasma, guvenli scratch, servis butcesi =======
   Hepsi kart gerektirir: sinanan sey tuketici ile CALISAN DMA arasindaki
   yaris ve kaybin gorunurlugu. */

/* Ayristiriciyi tetiklemeyen dolgu: hicbiri SYNC0 degil, hepsi elenir.
   Amac tampon konumunu ilerletmek, cerceve uretmek degil. */
static uint8_t r3_dolgu[64];

static void r3_dolgu_hazirla(void)
{
    uint16_t i;
    for (i = 0U; i < sizeof(r3_dolgu); i++)
    {
        r3_dolgu[i] = 0x11U;         /* FRAME_SYNC0 (0xAA) DEGIL */
    }
}


/* n bayt gonderir ve bu sirada HIC tuketim yapmaz: HAL_UART_Transmit
   bloklayicidir, kesmeler acik kalir (tur sayaci islemeye devam eder). */
static uint8_t r3_tuketmeden_gonder(UART_HandleTypeDef *huart, uint32_t n)
{
    uint32_t kalan = n;

    while (kalan > 0U)
    {
        uint16_t blok = (uint16_t)((kalan > sizeof(r3_dolgu))
                                   ? sizeof(r3_dolgu) : kalan);
        if (HAL_UART_Transmit(huart, r3_dolgu, blok, 200U) != HAL_OK)
        {
            return 0U;
        }
        kalan -= blok;
    }
    HAL_Delay(2U);                   /* son IDLE/TC olayi otursun */
    return 1U;
}


/* RX_FULL_LAP_LOSS_VISIBLE
   TAM 256 ve 300 bayt AYRI AYRI tasma saymali. Eski modulo konumla 256
   bayt "veri yok" gorunuyordu: kaybin tamamen gizlendigi durum buydu. */
static uint8_t r3_tam_tur_kaybi_gorunur(UART_HandleTypeDef *huart)
{
    uint16_t ovr_once;
    uint32_t disc_once;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    r3_dolgu_hazirla();
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);   /* temiz baslangic */

    /* --- TAM 256 --- */
    ovr_once  = uart_rx_stats.overruns;
    disc_once = uart_rx_stats.discarded_bytes;

    if (r3_tuketmeden_gonder(huart, UART_RX_BUF_SIZE) == 0U)
    {
        return 0U;
    }
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);

    if ((uart_rx_stats.overruns != (uint16_t)(ovr_once + 1U)) ||
        (uart_rx_stats.discarded_bytes <= disc_once))
    {
        return 0U;
    }

    /* --- 300 --- */
    ovr_once  = uart_rx_stats.overruns;
    disc_once = uart_rx_stats.discarded_bytes;

    if (r3_tuketmeden_gonder(huart, 300U) == 0U)
    {
        return 0U;
    }
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);

    return (uint8_t)((uart_rx_stats.overruns == (uint16_t)(ovr_once + 1U)) &&
                     (uart_rx_stats.discarded_bytes > disc_once));
}


/* RX_MULTILAP
   768 bayt (tam uc tur) da sessiz "bos sonuc" uretmemeli. */
static uint8_t r3_cok_tur(UART_HandleTypeDef *huart)
{
    uint16_t ovr_once;
    uint32_t disc_once;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    r3_dolgu_hazirla();
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);

    ovr_once  = uart_rx_stats.overruns;
    disc_once = uart_rx_stats.discarded_bytes;

    if (r3_tuketmeden_gonder(huart, 768U) == 0U)
    {
        return 0U;
    }
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);

    return (uint8_t)((uart_rx_stats.overruns == (uint16_t)(ovr_once + 1U)) &&
                     ((uart_rx_stats.discarded_bytes - disc_once) >=
                      (2U * UART_RX_BUF_SIZE)));
}


/* RX_BUDGET_REMAINS
   100 bayt beklerken ilk tur EN FAZLA 64 tuketmeli ve "is kaldi" demeli;
   yeni bir kesme OLMADAN sonraki tur kalani bitirmeli. */
static uint8_t r3_butce_kalani(UART_HandleTypeDef *huart)
{
    uint32_t c0;
    uint32_t c1;
    uint32_t c2;
    uint8_t  devam1;
    uint8_t  devam2;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    r3_dolgu_hazirla();
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);

    /* 100 bayt: butceden (64) buyuk, tampondan (256) kucuk */
    if (r3_tuketmeden_gonder(huart, 100U) == 0U)
    {
        return 0U;
    }

    c0     = uart_rx_get_consumed();
    devam1 = uart_rx_service_budget(UART_RX_SERVICE_BUDGET);
    c1     = uart_rx_get_consumed();

    /* Ilk tur: en fazla butce kadar ve "is kaldi" */
    if (((c1 - c0) > UART_RX_SERVICE_BUDGET) || (devam1 == 0U))
    {
        return 0U;
    }

    /* Ikinci tur: YENI KESME OLMADAN kalani bitirsin */
    devam2 = uart_rx_service_budget(UART_RX_SERVICE_BUDGET);
    c2     = uart_rx_get_consumed();

    return (uint8_t)(((c2 - c0) == 100U) && (devam2 == 0U));
}


/* RX_OVERWRITE_DURING_COPY
   Kopya ile dogrulama arasinda uretici bir tur sicrarsa kopya
   ayristiriciya VERILMEMELI: tuketici ilerlemez, handler cagrilmaz.
   Ayni kontrol hata nesli degisimi icin de yapilir. */
static uint8_t r3_kopya_sirasinda_ezilme(UART_HandleTypeDef *huart)
{
    uint32_t c_once;
    uint16_t ok_once;
    uint16_t red_once;
    uint8_t  cerceve[FRAME_MAX_SIZE];
    uint8_t  n;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);

    /* Gecerli bir cerceve gonder: kopya dogrulanmazsa COZULMEMELI */
    n = frame_build_joystick(cerceve, (uint8_t)sizeof(cerceve), 9, -9, 801U);
    if (n == 0U)
    {
        return 0U;
    }
    if (HAL_UART_Transmit(huart, cerceve, n, 100U) != HAL_OK)
    {
        return 0U;
    }
    HAL_Delay(3U);

    /* --- 1) Uretici kopya sirasinda bir tur sicriyor --- */
    c_once   = uart_rx_get_consumed();
    ok_once  = uart_rx_get_parser()->frames_ok;
    red_once = uart_rx_stats.copy_rejects;

    uart_rx_test_set_copy_hook(UART_RX_COPY_HOOK_OVERWRITE);
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);

    if ((uart_rx_get_consumed() != c_once) ||
        (uart_rx_get_parser()->frames_ok != ok_once) ||
        (uart_rx_stats.copy_rejects != (uint16_t)(red_once + 1U)))
    {
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    /* Sahte sicrama kalici: oturumu temiz bir noktadan yeniden kur. */
    r1_alimi_durdur(huart);
    if (r1_alimi_kur(huart) == 0U)
    {
        return 0U;
    }

    /* --- 2) Kopya sirasinda hata nesli degisiyor --- */
    n = frame_build_joystick(cerceve, (uint8_t)sizeof(cerceve), 8, -8, 802U);
    if ((n == 0U) ||
        (HAL_UART_Transmit(huart, cerceve, n, 100U) != HAL_OK))
    {
        return 0U;
    }
    HAL_Delay(3U);

    c_once   = uart_rx_get_consumed();
    ok_once  = uart_rx_get_parser()->frames_ok;
    red_once = uart_rx_stats.copy_rejects;

    uart_rx_test_set_copy_hook(UART_RX_COPY_HOOK_ERROR_GEN);
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);

    if ((uart_rx_get_consumed() != c_once) ||
        (uart_rx_get_parser()->frames_ok != ok_once) ||
        (uart_rx_stats.copy_rejects != (uint16_t)(red_once + 1U)))
    {
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    /* Kanca kapandi: ayni veri bu kez normal cozulmeli (veri kaybolmadi) */
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);

    return (uint8_t)((uart_rx_get_parser()->frames_ok ==
                      (uint16_t)(ok_once + 1U)) &&
                     (app_proto_state.last_seq == 802U));
}


/* RX_POST_COPY_SAMPLE_FAIL
   P0 gecerli, P1 BASARISIZ: eski/ilklenmemis P1 degeri KULLANILMAMALI.
   Tuketici ayni kalmali, handler cagrilmamali ve tekrar son tarihine
   kadar mesgul dongu olusmamali. */
static uint8_t r3_kopya_sonrasi_ornek_dustu(UART_HandleTypeDef *huart)
{
    uint32_t c_once;
    uint16_t ok_once;
    uint16_t defers_once;
    uint8_t  cerceve[FRAME_MAX_SIZE];
    uint8_t  n;
    uint8_t  devam;
    uint8_t  sonuc;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);

    n = frame_build_joystick(cerceve, (uint8_t)sizeof(cerceve), 4, -4, 803U);
    if ((n == 0U) ||
        (HAL_UART_Transmit(huart, cerceve, n, 100U) != HAL_OK))
    {
        return 0U;
    }
    HAL_Delay(3U);

    c_once      = uart_rx_get_consumed();
    ok_once     = uart_rx_get_parser()->frames_ok;
    defers_once = uart_rx_stats.sample_defers;

    uart_rx_test_set_copy_hook(UART_RX_COPY_HOOK_SAMPLE_FAIL);
    devam = uart_rx_service_budget(UART_RX_SERVICE_BUDGET);

    sonuc = (uint8_t)((uart_rx_get_consumed() == c_once) &&
                      (uart_rx_get_parser()->frames_ok == ok_once) &&
                      (devam == 0U) &&      /* spin cagrisi yapilmaz */
                      (uart_rx_stats.sample_defers > defers_once));

    /* Erteleme suresi dolmadan yapilan cagri hicbir sey tuketmemeli */
    if (uart_rx_service_budget(UART_RX_SERVICE_BUDGET) != 0U)
    {
        sonuc = 0U;
    }
    if (uart_rx_get_consumed() != c_once)
    {
        sonuc = 0U;
    }

    /* Kanca kapali; erteleme dolunca ayni veri normal cozulmeli */
    uart_rx_test_force_sample_fail(0U);
    HAL_Delay(3U);
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);

    if ((uart_rx_get_parser()->frames_ok != (uint16_t)(ok_once + 1U)) ||
        (app_proto_state.last_seq != 803U))
    {
        sonuc = 0U;
    }
    return sonuc;
}


/* RX_RESYNC_AFTER_OVERRUN
   Tasmadan sonra gelen gecerli cerceve YALNIZCA BIR KEZ teslim edilmeli ve
   onceki yarim cerceveye EKLENMEMELI. */
static uint8_t r3_tasma_sonrasi_senkron(UART_HandleTypeDef *huart)
{
    uint8_t  cerceve[FRAME_MAX_SIZE];
    uint8_t  n;
    uint16_t ok_once;
    uint16_t ovr_once;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    r3_dolgu_hazirla();
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);

    /* Yarim cerceve birak: tasma bunu DUSURMELI */
    n = frame_build_joystick(cerceve, (uint8_t)sizeof(cerceve), 3, -3, 804U);
    if ((n == 0U) ||
        (HAL_UART_Transmit(huart, cerceve, FRAME_HEADER_SIZE, 100U) != HAL_OK))
    {
        return 0U;
    }
    HAL_Delay(2U);
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);

    if (uart_rx_get_parser()->len == 0U)
    {
        return 0U;                   /* onkosul: bekleyen aday olmali */
    }

    ovr_once = uart_rx_stats.overruns;

    /* Tasmayi uret */
    if (r3_tuketmeden_gonder(huart, 300U) == 0U)
    {
        return 0U;
    }
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);

    if ((uart_rx_stats.overruns != (uint16_t)(ovr_once + 1U)) ||
        (uart_rx_get_parser()->len != 0U))
    {
        return 0U;                   /* aday birakilmis olmali */
    }

    /* Yeni gecerli cerceve: TAM BIR KEZ cozulmeli */
    ok_once = uart_rx_get_parser()->frames_ok;
    n = frame_build_joystick(cerceve, (uint8_t)sizeof(cerceve), 2, -2, 805U);
    if ((n == 0U) ||
        (HAL_UART_Transmit(huart, cerceve, n, 100U) != HAL_OK))
    {
        return 0U;
    }
    HAL_Delay(3U);
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);   /* ikinci tur */

    return (uint8_t)((uart_rx_get_parser()->frames_ok ==
                      (uint16_t)(ok_once + 1U)) &&
                     (app_proto_state.last_seq == 805U) &&
                     (app_proto_state.joy_x == 2));
}


/* ==================== R4: bloklamayan RX toparlanmasi ================== */

/* RX_ERROR_DMA_FE
   Gercek bir framing error sonrasi alim kendi kendine geri gelmeli ve
   cerceve teslim etmeli. Hata FIZIKSEL: SBK hatta break gonderir. */
static uint8_t r4_fe_sonrasi_toparlanma(UART_HandleTypeDef *huart)
{
    uint8_t  cerceve[FRAME_MAX_SIZE];
    uint8_t  n;
    uint16_t err_once;
    uint16_t ok_once;
    uint32_t t0;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    err_once = uart_rx_stats.error_events;

    SET_BIT(huart->Instance->CR1, USART_CR1_SBK);

    /* Toparlanma BLOKLAMAMALI: her servis turu bir adim ilerletir.
       Toplam sure abort (<=20 ms) + denemeler (<=100 ms) icinde kalmali. */
    t0 = HAL_GetTick();
    while (((HAL_GetTick() - t0) < 200U) &&
           (uart_rx_get_phase() != UART_RX_PHASE_RUNNING))
    {
        uart_rx_service();
    }

    if ((uart_rx_stats.error_events <= err_once) ||
        (uart_rx_get_phase() != UART_RX_PHASE_RUNNING))
    {
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    /* ASIL IDDIA: toparlanmadan sonra alim calisiyor */
    ok_once = uart_rx_get_parser()->frames_ok;
    n = frame_build_joystick(cerceve, (uint8_t)sizeof(cerceve), 11, -11, 901U);
    if ((n == 0U) ||
        (HAL_UART_Transmit(huart, cerceve, n, 100U) != HAL_OK))
    {
        return 0U;
    }
    HAL_Delay(3U);
    uart_rx_service();
    uart_rx_service();

    return (uint8_t)((uart_rx_get_parser()->frames_ok >
                      ok_once) &&
                     (app_proto_state.last_seq == 901U));
}


/* RX_RETRY_EXHAUSTED
   5 start denemesi / 100 ms butcesi yeni denemeleri KAPATMALI ve gorunur
   FAULT uretmeli. Durus saglandigi icin rx_quiescent 1 olmali. */
static uint8_t r4_deneme_butcesi_dolar(UART_HandleTypeDef *huart)
{
    uint16_t rf_once;
    uint32_t t0;
    uint8_t  sonuc;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    rf_once = uart_rx_stats.restart_fails;

    uart_rx_force_restart_fail(1U);
    uart_rx_test_inject_error();

    /* 200 ms: abort + 5 x 5 ms deneme + 100 ms butce icin fazlasiyla yeterli.
       Bloklayan bir akis olsaydi bu dongu hic donmezdi. */
    t0 = HAL_GetTick();
    while (((HAL_GetTick() - t0) < 200U) &&
           (uart_rx_get_phase() != UART_RX_PHASE_FAULT))
    {
        uart_rx_service();
    }
    uart_rx_force_restart_fail(0U);

    sonuc = (uint8_t)((uart_rx_get_phase() == UART_RX_PHASE_FAULT) &&
                      (uart_rx_stats.faulted == 1U) &&
                      (uart_rx_stats.restart_fails >=
                       (uint16_t)(rf_once + UART_RX_RESTART_MAX_TRIES)) &&
                      (uart_rx_is_quiescent() == 1U));

    /* FAULT'ta yeni deneme OLMAMALI: 50 ms daha servis dondur, sayac sabit. */
    {
        uint16_t rf_fault = uart_rx_stats.restart_fails;

        t0 = HAL_GetTick();
        while ((HAL_GetTick() - t0) < 50U)
        {
            uart_rx_service();
        }
        if (uart_rx_stats.restart_fails != rf_fault)
        {
            sonuc = 0U;
        }
    }

    if (r1_alimi_kur(huart) == 0U)
    {
        sonuc = 0U;
    }
    return sonuc;
}


/* RX_ERROR_EACH_RESTART
   Her denemede yeni hata bildirimi gelse bile deneme sayaci BASA DONMEMELI.
   Eski kodda her hata s_restart_tries'i sifirliyordu ve FAULT'a hic
   ulasilamiyordu; bu testin yakaladigi hata tam olarak budur. */
static uint8_t r4_her_denemede_hata(UART_HandleTypeDef *huart)
{
    uint32_t t0;
    uint8_t  sonuc;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    uart_rx_force_restart_fail(1U);
    uart_rx_test_inject_error();

    /* Her turda YENI hata enjekte et: butce yine de dolmali. */
    t0 = HAL_GetTick();
    while (((HAL_GetTick() - t0) < 300U) &&
           (uart_rx_get_phase() != UART_RX_PHASE_FAULT))
    {
        uart_rx_test_inject_error();
        uart_rx_service();
    }
    uart_rx_force_restart_fail(0U);

    sonuc = (uint8_t)(uart_rx_get_phase() == UART_RX_PHASE_FAULT);

    if (r1_alimi_kur(huart) == 0U)
    {
        sonuc = 0U;
    }
    return sonuc;
}


/* RX_NEW_ERROR_AFTER_HEALTHY_IDLE
   Saglikli sessizlikten sonra gelen hata YENI bir donem acmali; onceki
   donemin suresi yuzunden aninda FAULT olmamali. Periyodik uyanma
   gerektirmeden calismali. */
static uint8_t r4_saglikli_sessizlik_sonrasi(UART_HandleTypeDef *huart)
{
    uint32_t t0;
    uint8_t  sonuc;

    if (r1_alimi_kur(huart) == 0U)
    {
        return 0U;
    }

    /* UART_RX_HEALTHY_MS'in cok uzerinde hatasiz calisma: donem kapanmali.
       (Plandaki 2 saniye yerine 400 ms kullaniliyor; kapanma kosulu 100 ms
       oldugu icin ayni seyi kanitlar ve test suresini kisa tutar.) */
    t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < 400U)
    {
        uart_rx_service();
    }

    if (uart_rx_test_recovery_active() != 0U)
    {
        return 0U;                  /* donem kapanmis olmaliydi */
    }

    /* Simdi tek bir hata: ANINDA FAULT OLMAMALI, yeni donem acilmali. */
    uart_rx_test_inject_error();
    uart_rx_service();

    if (uart_rx_get_phase() == UART_RX_PHASE_FAULT)
    {
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    /* Toparlanma tamamlanip RUNNING'e donmeli */
    t0 = HAL_GetTick();
    while (((HAL_GetTick() - t0) < 200U) &&
           (uart_rx_get_phase() != UART_RX_PHASE_RUNNING))
    {
        uart_rx_service();
    }

    sonuc = (uint8_t)(uart_rx_get_phase() == UART_RX_PHASE_RUNNING);

    if (r1_alimi_kur(huart) == 0U)
    {
        sonuc = 0U;
    }
    return sonuc;
}


/* RX_FAULT_RECOVERY_REQUEST
   FAULT'tan cikis ACIK istekle olmali; istek once DURUSU dogrulamali.
   Calisan bir alim varken istek kabul EDILMEMELI. */
static uint8_t r4_kurtarma_istegi(UART_HandleTypeDef *huart)
{
    uint32_t t0;
    uint8_t  sonuc;

    /* RUNNING iken istek reddedilmeli: calisan yone dokunulmaz */
    if (r1_alimi_kur(huart) == 0U)
    {
        return 0U;
    }
    if (uart_rx_request_recovery() != 0U)
    {
        return 0U;
    }

    /* FAULT'a dusur */
    if (r1_fault_ve_durdur(huart) == 0U)
    {
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    /* Istek olmadan FAULT'ta kalmali */
    t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < 30U)
    {
        uart_rx_service();
    }
    if (uart_rx_get_phase() != UART_RX_PHASE_FAULT)
    {
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    /* Acik istek: toparlanma islemeli */
    if (uart_rx_request_recovery() == 0U)
    {
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    t0 = HAL_GetTick();
    while (((HAL_GetTick() - t0) < 200U) &&
           (uart_rx_get_phase() != UART_RX_PHASE_RUNNING))
    {
        uart_rx_service();
    }

    sonuc = (uint8_t)(uart_rx_get_phase() == UART_RX_PHASE_RUNNING);

    if (r1_alimi_kur(huart) == 0U)
    {
        sonuc = 0U;
    }
    return sonuc;
}


/* RX_FAULT_TX_IRQ_NO_RETRY
   FAULT sirasinda SAGLIKLI TX tamamlanmalari RX'te yeni otomatik deneme
   baslatmamali. IRQ cikis kancasi RUNNING/STARTING disinda sessiz kalir;
   aksi halde her TX TC kesmesi RX butcesini sonsuza kadar tazelerdi. */
static uint8_t r4_tx_irq_yeni_deneme_acmaz(UART_HandleTypeDef *huart)
{
    uint16_t rf_once;
    uint16_t health_once;
    uint8_t  cerceve[FRAME_OVERHEAD + 4U];
    uint8_t  n;
    uint8_t  sonuc;
    uint32_t t0;

    if (r1_fault_ve_durdur(huart) == 0U)
    {
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    rf_once     = uart_rx_stats.restart_fails;
    health_once = uart_rx_stats.irq_health_events;

    /* FAULT'ta RX dururken TX gonderimi yap: USART IRQ'su ORTAK oldugu icin
       her TX olayi RX IRQ cikis kancasini da tetikler. */
    n = frame_build_joystick(cerceve, (uint8_t)sizeof(cerceve), 1, 1, 950U);
    if (n == 0U)
    {
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    if (uart_tx_send_copy(cerceve, n) == UART_TX_OK)
    {
        t0 = HAL_GetTick();
        while (((HAL_GetTick() - t0) < 50U) &&
               (uart_tx_get_state() != UART_TX_IDLE))
        {
            uart_tx_service();
            uart_rx_service();
        }
    }

    t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < 30U)
    {
        uart_rx_service();
    }

    sonuc = (uint8_t)((uart_rx_get_phase() == UART_RX_PHASE_FAULT) &&
                      (uart_rx_stats.restart_fails == rf_once) &&
                      (uart_rx_stats.irq_health_events == health_once));

    if (r1_alimi_kur(huart) == 0U)
    {
        sonuc = 0U;
    }
    return sonuc;
}


/* RX_NON_RUNNING_NO_PARSER
   RUNNING disindayken parser/drain/timeout CALISMAMALI: durmus ya da
   belirsiz bir DMA'nin tamponunu ayristiriciya vermek, kurtarilmis gibi
   gorunen bozuk cerceveler uretir. */
static uint8_t r4_running_disi_tuketim_yok(UART_HandleTypeDef *huart)
{
    uint32_t c_once;
    uint16_t ok_once;
    uint16_t to_once;
    uint32_t t0;
    uint8_t  sonuc;

    if (r1_fault_ve_durdur(huart) == 0U)
    {
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    c_once  = uart_rx_get_consumed();
    ok_once = uart_rx_get_parser()->frames_ok;
    to_once = uart_rx_stats.frame_timeouts;

    /* FAULT'ta butce cagrisi hicbir sey tuketmemeli */
    if (uart_rx_service_budget(UART_RX_SERVICE_BUDGET) != 0U)
    {
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < 80U)   /* 50 ms zaman asimi siniri gecsin */
    {
        uart_rx_service();
    }

    sonuc = (uint8_t)((uart_rx_get_consumed() == c_once) &&
                      (uart_rx_get_parser()->frames_ok == ok_once) &&
                      (uart_rx_stats.frame_timeouts == to_once));

    if (r1_alimi_kur(huart) == 0U)
    {
        sonuc = 0U;
    }
    return sonuc;
}


/* ==================== R5: zaman asimi, handler, kapanis ================ */

/* RX_TIMEOUT_QUIET
   Yarim baslik + 50 ms ilerlemesizlik: aday dusurulmeli. */
static uint8_t r5_sessizlikte_zaman_asimi(UART_HandleTypeDef *huart)
{
    static const uint8_t bozuk[FRAME_HEADER_SIZE] =
        { 0xAA, 0x55, 0x01, 0x20, 0x37, 0x01, 0x00 };   /* LENGTH=55 der */
    uint16_t to_once;
    uint32_t t0;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);
    to_once = uart_rx_stats.frame_timeouts;

    if (HAL_UART_Transmit(huart, bozuk, (uint16_t)sizeof(bozuk),
                          100U) != HAL_OK)
    {
        return 0U;
    }
    HAL_Delay(2U);
    uart_rx_service();

    if (uart_rx_get_parser()->len != FRAME_HEADER_SIZE)
    {
        return 0U;                   /* onkosul: aday tikanmis olmali */
    }

    /* Son tarih ETKIN olmali: suresiz uyku bildirilmemeli */
    if (uart_rx_next_wait_ms(HAL_GetTick()) == UINT32_MAX)
    {
        return 0U;
    }

    t0 = HAL_GetTick();
    while (((HAL_GetTick() - t0) < 120U) &&
           (uart_rx_stats.frame_timeouts == to_once))
    {
        uart_rx_service();
    }

    return (uint8_t)((uart_rx_stats.frame_timeouts ==
                      (uint16_t)(to_once + 1U)) &&
                     (uart_rx_get_parser()->len == 0U));
}


/* RX_TIMEOUT_CONTINUATION
   50 ms siniri AKMAKTA OLAN gecerli bir cercevenin ortasina dustugunde
   cerceve DUSMEMELI. Devam yayini TX DMA ile gonderiliyor ki servis
   dongusu serbest kalsin; bloklayan Transmit ile bu hata gorunmez olurdu. */
static uint8_t r5_devam_eden_cerceve_dusmez(UART_HandleTypeDef *huart)
{
    uint8_t  cerceve[FRAME_MAX_SIZE];
    uint8_t  payload[FRAME_MAX_PAYLOAD];
    uint16_t i;
    uint8_t  n;
    uint16_t to_once;
    uint16_t ok_once;
    uint32_t t0;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    for (i = 0U; i < FRAME_MAX_PAYLOAD; i++)
    {
        payload[i] = (uint8_t)i;
    }

    n = frame_build(cerceve, (uint8_t)sizeof(cerceve), FRAME_TYPE_SET_OUTPUT,
                    970U, payload, FRAME_MAX_PAYLOAD);
    if (n != FRAME_MAX_SIZE)
    {
        return 0U;
    }

    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);
    to_once = uart_rx_stats.frame_timeouts;
    ok_once = uart_rx_get_parser()->frames_ok;

    /* Once baslik: aday olusur ve pencere baslar */
    if (HAL_UART_Transmit(huart, cerceve, FRAME_HEADER_SIZE, 100U) != HAL_OK)
    {
        return 0U;
    }
    HAL_Delay(2U);
    uart_rx_service();

    /* Sessizce sinira yaklas */
    t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < (UART_RX_FRAME_TIMEOUT_MS - 3U))
    {
        uart_rx_service();
    }

    /* 57 bayt ~4,95 ms surer: 50 ms siniri yayinin ORTASINA duser */
    if (uart_tx_send_copy(&cerceve[FRAME_HEADER_SIZE],
                          (uint16_t)(n - FRAME_HEADER_SIZE)) != UART_TX_OK)
    {
        return 0U;
    }

    t0 = HAL_GetTick();
    while (((HAL_GetTick() - t0) < 60U) &&
           (uart_rx_get_parser()->frames_ok == ok_once))
    {
        uart_tx_service();
        uart_rx_service();
    }

    return (uint8_t)((uart_rx_get_parser()->frames_ok ==
                      (uint16_t)(ok_once + 1U)) &&
                     (uart_rx_stats.frame_timeouts == to_once) &&
                     (app_proto_state.last_seq == 970U));
}


/* RX_TIMEOUT_NO_CANDIDATE
   Ayristirici BOSKEN zaman asimi uyanmasi PLANLANMAMALI. Aksi halde task
   bos hatta 50 ms'de bir bos yere uyanirdi. */
static uint8_t r5_aday_yokken_son_tarih_yok(UART_HandleTypeDef *huart)
{
    uint32_t t0;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    /* Hatti bosalt ve ayristiriciyi bosalt */
    t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < 80U)
    {
        uart_rx_service();
    }

    if (uart_rx_get_parser()->len != 0U)
    {
        return 0U;                   /* onkosul: aday kalmamali */
    }

    /* Toparlanma donemi de kapali olmali ki suresiz uyku bildirilsin */
    if (uart_rx_test_recovery_active() != 0U)
    {
        return 0U;
    }

    return (uint8_t)(uart_rx_next_wait_ms(HAL_GetTick()) == UINT32_MAX);
}


/* RX_TIMEOUT_REARM
   Zaman asimi sonrasi HALA aday varsa YENI bir 50 ms penceresi kurulmali;
   uretici ilerlemis gibi gosterilmemeli. Aday bittiginde son tarih kalkar.
   Eski davranista dolmus son tarih ayni servis turunu tekrar tekrar
   tetikleyebilirdi. */
static uint8_t r5_zaman_asimi_yeniden_kurulur(UART_HandleTypeDef *huart)
{
    /* Iki ARDISIK bozuk baslik: ilk zaman asimi bir bayt atar, kalanlar
       yeniden taranir ve arkadaki ikinci aday ayakta kalir. */
    static const uint8_t ikili[2U * FRAME_HEADER_SIZE] =
        { 0xAA, 0x55, 0x01, 0x20, 0x37, 0x01, 0x00,
          0xAA, 0x55, 0x01, 0x20, 0x37, 0x02, 0x00 };
    uint16_t to_once;
    uint32_t t0;
    uint32_t bekleme;

    if (uart_rx_get_phase() != UART_RX_PHASE_RUNNING)
    {
        return 0U;
    }

    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);
    to_once = uart_rx_stats.frame_timeouts;

    if (HAL_UART_Transmit(huart, ikili, (uint16_t)sizeof(ikili),
                          100U) != HAL_OK)
    {
        return 0U;
    }
    HAL_Delay(3U);
    uart_rx_service();

    if (uart_rx_get_parser()->len == 0U)
    {
        return 0U;
    }

    /* Ilk zaman asimini bekle */
    t0 = HAL_GetTick();
    while (((HAL_GetTick() - t0) < 120U) &&
           (uart_rx_stats.frame_timeouts == to_once))
    {
        uart_rx_service();
    }

    if (uart_rx_stats.frame_timeouts != (uint16_t)(to_once + 1U))
    {
        return 0U;
    }

    bekleme = uart_rx_next_wait_ms(HAL_GetTick());

    if (uart_rx_get_parser()->len != 0U)
    {
        /* Hala aday var: YENI pencere kurulmus olmali, 0 DEGIL. */
        return (uint8_t)((bekleme != 0U) && (bekleme != UINT32_MAX) &&
                         (bekleme <= UART_RX_FRAME_TIMEOUT_MS));
    }

    /* Aday kalmadi: son tarih tamamen kalkmali */
    return (uint8_t)(bekleme == UINT32_MAX);
}


/* --- RX_PAYLOAD_LIFETIME ---
   Handler'in KOPYALADIGI deger, ayristirici ilerlese de sabit kalmali.
   Isaretci saklamanin guvensiz oldugunu gosteren test budur. */
static uint8_t  r5_kopya[FRAME_MAX_PAYLOAD];
static uint8_t  r5_kopya_len;
static uint16_t r5_kopya_seq;
static uint16_t r5_handler_cagri;

static void r5_test_handler(const frame_info_t *info, void *user_data)
{
    (void)user_data;

    r5_handler_cagri++;

    if (r5_kopya_len != 0U)
    {
        return;                      /* yalnizca ILK cerceveyi sakla */
    }

    if ((info->payload != NULL) && (info->payload_len > 0U))
    {
        (void)memcpy(r5_kopya, info->payload, info->payload_len);
        r5_kopya_len = info->payload_len;
        r5_kopya_seq = info->seq;
    }
}


static uint8_t r5_payload_omru(UART_HandleTypeDef *huart)
{
    uint8_t  cerceve[FRAME_MAX_SIZE];
    uint8_t  payload[8];
    uint8_t  n;
    uint16_t i;
    uint8_t  sonuc;
    uint32_t t0;

    for (i = 0U; i < sizeof(payload); i++)
    {
        payload[i] = (uint8_t)(0xA0U + i);
    }

    r5_kopya_len     = 0U;
    r5_handler_cagri = 0U;
    (void)memset(r5_kopya, 0, sizeof(r5_kopya));

    /* Handler YALNIZCA start oncesi degistirilebilir */
    r1_alimi_durdur(huart);
    uart_rx_set_handler(r5_test_handler, NULL);
    if (r1_alimi_kur(huart) == 0U)
    {
        uart_rx_set_handler(app_protocol_on_frame, NULL);
        return 0U;
    }

    n = frame_build(cerceve, (uint8_t)sizeof(cerceve), FRAME_TYPE_SET_OUTPUT,
                    981U, payload, (uint8_t)sizeof(payload));
    if ((n == 0U) ||
        (HAL_UART_Transmit(huart, cerceve, n, 100U) != HAL_OK))
    {
        r1_alimi_durdur(huart);
        uart_rx_set_handler(app_protocol_on_frame, NULL);
        (void)r1_alimi_kur(huart);
        return 0U;
    }
    HAL_Delay(3U);
    uart_rx_service();

    /* Ayristiriciyi ILERLET: baska cerceveler gelsin, tampon degissin */
    for (i = 0U; i < 6U; i++)
    {
        uint8_t m = frame_build_joystick(cerceve, (uint8_t)sizeof(cerceve),
                                         (int16_t)i, (int16_t)-i,
                                         (uint16_t)(982U + i));
        if ((m == 0U) ||
            (HAL_UART_Transmit(huart, cerceve, m, 100U) != HAL_OK))
        {
            break;
        }
        HAL_Delay(2U);
        uart_rx_service();
    }

    t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < 20U)
    {
        uart_rx_service();
    }

    /* Saklanan KOPYA degismemis olmali */
    sonuc = (uint8_t)((r5_kopya_len == (uint8_t)sizeof(payload)) &&
                      (r5_kopya_seq == 981U) &&
                      (r5_handler_cagri > 1U) &&
                      (memcmp(r5_kopya, payload, sizeof(payload)) == 0));

    /* Uygulama handler'ini geri koy */
    r1_alimi_durdur(huart);
    uart_rx_set_handler(app_protocol_on_frame, NULL);
    if (r1_alimi_kur(huart) == 0U)
    {
        sonuc = 0U;
    }
    return sonuc;
}


/* RX_STALE_DEADLINE_NO_SPIN
   FAULT'ta eski frame son tarihi dolmus olsa bile bekleme 0'a
   SABITLENMEMELI: o state'te uygulanabilir bir is yok. Bu, bolum 7.2'nin
   "gecersiz state'in eski deadline'i taski dondurmesin" kuralidir. */
static uint8_t r5_eski_son_tarih_spin_yapmaz(UART_HandleTypeDef *huart)
{
    uint8_t  sonuc;

    if (r1_fault_ve_durdur(huart) == 0U)
    {
        (void)r1_alimi_kur(huart);
        return 0U;
    }

    /* FAULT'ta, acik kurtarma istegi YOKKEN periyodik uyanma olmamali */
    sonuc = (uint8_t)(uart_rx_next_wait_ms(HAL_GetTick()) == UINT32_MAX);

    /* Acik istek varsa hemen is vardir */
    if (uart_rx_request_recovery() == 0U)
    {
        sonuc = 0U;
    }
    if (uart_rx_next_wait_ms(HAL_GetTick()) != 0U)
    {
        sonuc = 0U;
    }

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
    uint16_t i_r2_sinir;
    uint16_t i_r2_pendtc;
    uint16_t i_r2_sarim;
    uint16_t i_r2_takildi;
    uint16_t i_r2_aborttc;
    uint16_t i_r2_restart;
    uint16_t i_r2_tcidle;
    uint16_t i_r3_tamtur;
    uint16_t i_r3_coktur;
    uint16_t i_r3_butce;
    uint16_t i_r3_ezilme;
    uint16_t i_r3_p1dustu;
    uint16_t i_r3_senkron;
    uint16_t i_r4_fe;
    uint16_t i_r4_butce;
    uint16_t i_r4_herhata;
    uint16_t i_r4_sessizlik;
    uint16_t i_r4_istek;
    uint16_t i_r4_txirq;
    uint16_t i_r4_norun;
    uint16_t i_r5_sessiz;
    uint16_t i_r5_devam;
    uint16_t i_r5_adayyok;
    uint16_t i_r5_rearm;
    uint16_t i_r5_payload;
    uint16_t i_r5_eskison;
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

    /* Ilk ucu saf aritmetik: donanim GEREKTIRMEZ. */
    i_r2_sinir   = uart_comm_test_kaydet("RX_PRODUCER_BOUNDARIES",    2U, 0U);
    i_r2_pendtc  = uart_comm_test_kaydet("RX_PENDING_TC",             2U, 0U);
    i_r2_sarim   = uart_comm_test_kaydet("RX_COUNTER_WRAP",           2U, 0U);
    i_r2_takildi = uart_comm_test_kaydet("RX_SAMPLE_STUCK",           2U, 1U);
    i_r2_aborttc = uart_comm_test_kaydet("RX_ABORT_TC_IGNORED",       2U, 1U);
    i_r2_restart = uart_comm_test_kaydet("RX_PRODUCER_RESTART",       2U, 1U);
    i_r2_tcidle  = uart_comm_test_kaydet("RX_TC_IDLE_SAME_POSITION",  2U, 1U);

    i_r3_tamtur  = uart_comm_test_kaydet("RX_FULL_LAP_LOSS_VISIBLE",  3U, 1U);
    i_r3_coktur  = uart_comm_test_kaydet("RX_MULTILAP",               3U, 1U);
    i_r3_butce   = uart_comm_test_kaydet("RX_BUDGET_REMAINS",         3U, 1U);
    i_r3_ezilme  = uart_comm_test_kaydet("RX_OVERWRITE_DURING_COPY",  3U, 1U);
    i_r3_p1dustu = uart_comm_test_kaydet("RX_POST_COPY_SAMPLE_FAIL",  3U, 1U);
    i_r3_senkron = uart_comm_test_kaydet("RX_RESYNC_AFTER_OVERRUN",   3U, 1U);

    i_r4_fe        = uart_comm_test_kaydet("RX_ERROR_DMA_FE",              4U, 1U);
    i_r4_butce     = uart_comm_test_kaydet("RX_RETRY_EXHAUSTED",           4U, 1U);
    i_r4_herhata   = uart_comm_test_kaydet("RX_ERROR_EACH_RESTART",        4U, 1U);
    i_r4_sessizlik = uart_comm_test_kaydet("RX_NEW_ERROR_AFTER_HEALTHY_IDLE",4U,1U);
    i_r4_istek     = uart_comm_test_kaydet("RX_FAULT_RECOVERY_REQUEST",    4U, 1U);
    i_r4_txirq     = uart_comm_test_kaydet("RX_FAULT_TX_IRQ_NO_RETRY",     4U, 1U);
    i_r4_norun     = uart_comm_test_kaydet("RX_NON_RUNNING_NO_PARSER",     4U, 1U);

    i_r5_sessiz  = uart_comm_test_kaydet("RX_TIMEOUT_QUIET",         5U, 1U);
    i_r5_devam   = uart_comm_test_kaydet("RX_TIMEOUT_CONTINUATION",  5U, 1U);
    i_r5_adayyok = uart_comm_test_kaydet("RX_TIMEOUT_NO_CANDIDATE",  5U, 1U);
    i_r5_rearm   = uart_comm_test_kaydet("RX_TIMEOUT_REARM",         5U, 1U);
    i_r5_payload = uart_comm_test_kaydet("RX_PAYLOAD_LIFETIME",      5U, 1U);
    i_r5_eskison = uart_comm_test_kaydet("RX_STALE_DEADLINE_NO_SPIN",5U, 1U);

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

    /* --- R2: donanimsiz sinir aritmetigi (jumper olmasa da kosar) --- */
    uart_comm_test_bool(i_r2_sinir,  r2_sinir_degerleri());
    uart_comm_test_bool(i_r2_pendtc, r2_bekleyen_tc_telafisi());
    uart_comm_test_bool(i_r2_sarim,  r2_sayac_sarimi());

    /* --- R1 --- */
    if (hw == 0U)
    {
        uint16_t i;
        for (i = i_r1_busy; i <= i_r5_eskison; i++)
        {
            if ((i == i_r2_sinir) || (i == i_r2_pendtc) || (i == i_r2_sarim))
            {
                continue;           /* zaten kostu */
            }
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

    /* --- R2: donanimli --- */
    uart_comm_test_sonuc(i_r2_takildi,
                         (uint8_t)(r2_ornek_takildi(huart)
                                   ? UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_INJECTED, 0U, 0U);

    uart_comm_test_sonuc(i_r2_aborttc,
                         (uint8_t)(r2_abort_tc_sayilmaz(huart)
                                   ? UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_INJECTED, 0U, 0U);

    uart_comm_test_bool(i_r2_restart, r2_restart_sifirlar(huart));
    uart_comm_test_bool(i_r2_tcidle,  r2_tc_idle_ayni_konum(huart));

    /* --- R3 --- */
    uart_comm_test_bool(i_r3_tamtur, r3_tam_tur_kaybi_gorunur(huart));
    uart_comm_test_bool(i_r3_coktur, r3_cok_tur(huart));
    uart_comm_test_bool(i_r3_butce,  r3_butce_kalani(huart));

    uart_comm_test_sonuc(i_r3_ezilme,
                         (uint8_t)(r3_kopya_sirasinda_ezilme(huart)
                                   ? UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_INJECTED, 0U, 0U);

    uart_comm_test_sonuc(i_r3_p1dustu,
                         (uint8_t)(r3_kopya_sonrasi_ornek_dustu(huart)
                                   ? UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_INJECTED, 0U, 0U);

    uart_comm_test_bool(i_r3_senkron, r3_tasma_sonrasi_senkron(huart));

    /* --- R4 --- */
    uart_comm_test_sonuc(i_r4_fe,
                         (uint8_t)(r4_fe_sonrasi_toparlanma(huart)
                                   ? UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_PHYSICAL, 0U, 0U);

    uart_comm_test_sonuc(i_r4_butce,
                         (uint8_t)(r4_deneme_butcesi_dolar(huart)
                                   ? UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_INJECTED, 0U, 0U);

    uart_comm_test_sonuc(i_r4_herhata,
                         (uint8_t)(r4_her_denemede_hata(huart)
                                   ? UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_INJECTED, 0U, 0U);

    uart_comm_test_sonuc(i_r4_sessizlik,
                         (uint8_t)(r4_saglikli_sessizlik_sonrasi(huart)
                                   ? UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_INJECTED, 0U, 0U);

    uart_comm_test_sonuc(i_r4_istek,
                         (uint8_t)(r4_kurtarma_istegi(huart)
                                   ? UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_INJECTED, 0U, 0U);

    uart_comm_test_sonuc(i_r4_txirq,
                         (uint8_t)(r4_tx_irq_yeni_deneme_acmaz(huart)
                                   ? UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_INJECTED, 0U, 0U);

    uart_comm_test_sonuc(i_r4_norun,
                         (uint8_t)(r4_running_disi_tuketim_yok(huart)
                                   ? UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_INJECTED, 0U, 0U);

    /* --- R5 --- */
    uart_comm_test_bool(i_r5_sessiz,  r5_sessizlikte_zaman_asimi(huart));
    uart_comm_test_bool(i_r5_devam,   r5_devam_eden_cerceve_dusmez(huart));
    uart_comm_test_bool(i_r5_adayyok, r5_aday_yokken_son_tarih_yok(huart));
    uart_comm_test_bool(i_r5_rearm,   r5_zaman_asimi_yeniden_kurulur(huart));
    uart_comm_test_bool(i_r5_payload, r5_payload_omru(huart));

    uart_comm_test_sonuc(i_r5_eskison,
                         (uint8_t)(r5_eski_son_tarih_spin_yapmaz(huart)
                                   ? UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL),
                         (uint8_t)UART_COMM_TEST_SRC_INJECTED, 0U, 0U);
}

#endif /* UART_COMM_TEST */

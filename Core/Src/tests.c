/*
 * tests.c
 *
 * Dogrulama kosucularI. main.c'den ayrilmistir: uretim kodu bu dosyaya
 * bagimli degildir, cagrilari kaldirmak yeterlidir.
 */
#include <stddef.h>
#include <string.h>

#include "tests.h"
#include "crc16.h"
#include "frame.h"
#include "parser.h"
#include "uart_rx.h"
#include "uart_tx.h"

/* P0: butun dogrulama kosucusu UART_COMM_TEST ile sinirlidir. Uretim
   derlemesinde bu cevirim birimi bos kalir; test sayaclari, hata enjeksiyon
   kancalari ve loopback kodu BINARY'E HIC GIRMEZ. Boylece "uretimde
   cagrilmiyor" niyeti yerine derleyici garantisi gecerli olur.
   Test derlemesi: tools/build.sh test   (-DUART_COMM_TEST) */
#ifdef UART_COMM_TEST

/* Sonuclar debugger'da okunur. Dis baglantili (static degil) olmalari
   derleyicinin bunlari atmasini engeller. */
uint8_t  test_sonuc[TEST_SONUC_ADET];
uint8_t  test_sayisi;
uint8_t  test_gecen;
uint8_t  test_kalan;
uint16_t test_beklenen;
uint16_t test_bulunan;
uint8_t  test_sayim_dogru;

/* Loopback (donanim) testleri icin ayri sayaclar */
uint8_t  lb_sonuc[LB_SONUC_ADET];
uint8_t  lb_kaynak[LB_SONUC_ADET];
uint8_t  lb_sayisi;
uint8_t  lb_gecen;
uint8_t  lb_kalan;
uint8_t  lb_calismayan;
uint8_t  lb_iptal_adim;
uint8_t  lb_iptal_nedeni;

/* Onkosul dustugunde kosucuyu GORUNUR bicimde sonlandirir.
   lb_iptal_adim = lb_sayisi: iptal aninda sirada olan testin indeksi, yani
   "buraya kadar kostu" bilgisi. Elle indeks yazmaya gerek yok, kayma olmaz. */
#define LB_IPTAL(neden)                     \
  do {                                      \
    lb_iptal_nedeni = (uint8_t)(neden);     \
    lb_iptal_adim   = lb_sayisi;            \
    goto bitir;                             \
  } while (0)

/* T6 uart_rx_start cagirdigi icin ayristirici sayaclari sifirlanir;
   T6 oncesindeki degerler debugger'da gorunsun diye burada saklanir. */
uint16_t lb_frames_ok;
uint16_t lb_last_seq;
uint16_t lb_seq_gaps;
uint16_t lb_bytes_dropped;

/* Ayristirici testlerinde bulunan cerceveleri biriktirir */
typedef struct
{
  uint8_t  adet;
  uint8_t  turler[4];
  uint16_t siralar[4];
  int16_t  son_x;
  int16_t  son_y;
} collector_t;

static void test_kaydet(uint16_t bulunan, uint16_t beklenen);
static void test_kaydet_bool(uint8_t dogru_mu);
static void crc16_testleri_kosur(void);
static void paket_testleri_kosur(void);
static void frame_collector(const frame_info_t *info, void *user_data);
static void frame_parser_testleri_kosur(void);


static void test_kaydet(uint16_t bulunan, uint16_t beklenen)
{
  if (test_sayisi >= (TEST_SONUC_ADET))
  {
    return;                       /* dizi doldu, sessizce birak */
  }

  if (bulunan == beklenen)
  {
    test_sonuc[test_sayisi] = 1U;
    test_gecen++;
  }
  else
  {
    test_sonuc[test_sayisi] = 2U;
    test_kalan++;
    test_beklenen = beklenen;     /* hatayi incelemek icin sakla */
    test_bulunan  = bulunan;
  }

  test_sayisi++;
}


static void test_kaydet_bool(uint8_t dogru_mu)
{
  test_kaydet((dogru_mu != 0U) ? 1U : 0U, 1U);
}


static void crc16_testleri_kosur(void)
{
  static const uint8_t v_ascii[9]    = {'1','2','3','4','5','6','7','8','9'};
  static const uint8_t v_sifir[1]    = {0x00};
  static const uint8_t v_ff[1]       = {0xFF};
  static const uint8_t v_aa55[2]     = {0xAA, 0x55};
  static const uint8_t v_joystick[9] = {0x01,0x10,0x04,0x01,0x00,0xE8,0x03,0x0C,0xFE};

  test_kaydet(crc16_ccitt(v_ascii,    9U), 0x29B1U);   /* "123456789" */
  test_kaydet(crc16_ccitt(v_ascii,    0U), 0xFFFFU);   /* uzunluk 0   */
  test_kaydet(crc16_ccitt(v_sifir,    1U), 0xE1F0U);
  test_kaydet(crc16_ccitt(v_ff,       1U), 0xFF00U);
  test_kaydet(crc16_ccitt(v_aa55,     2U), 0xE5EAU);
  test_kaydet(crc16_ccitt(v_joystick, 9U), 0x5946U);   /* plandaki joystick paketi */
}


static void paket_testleri_kosur(void)
{
  /* Beklenen bayt dizileri: bagimsiz bir uygulamayla uretildi */
  static const uint8_t b_joystick[13] =
    {0xAA,0x55,0x01,0x10,0x04,0x01,0x00,0xE8,0x03,0x0C,0xFE,0x46,0x59};
  static const uint8_t b_mod[10] =
    {0xAA,0x55,0x01,0x11,0x01,0x02,0x00,0x01,0x4E,0xED};
  static const uint8_t b_bos[9] =
    {0xAA,0x55,0x01,0x11,0x00,0x07,0x00,0xD9,0x4F};
  static const uint8_t b_aa55[11] =
    {0xAA,0x55,0x01,0x20,0x02,0x2C,0x01,0xAA,0x55,0x8D,0x8F};

  static const uint8_t p_mod[1]   = {0x01};
  static const uint8_t p_aa55[2]  = {0xAA, 0x55};
  static const uint8_t p_uzun[56] = {0};      /* FRAME_MAX_PAYLOAD + 1 */

  uint8_t cikti[FRAME_MAX_SIZE];
  uint8_t n;

  /* T1: joystick paketi, donus degeri 13 olmali */
  n = frame_build_joystick(cikti, (uint8_t)sizeof(cikti), 1000, -500, 1U);
  test_kaydet(n, 13U);

  /* T2: joystick paketi, baytlar birebir esit olmali */
  test_kaydet_bool((uint8_t)(memcmp(cikti, b_joystick, sizeof(b_joystick)) == 0));

  /* T3: joystick modu acik, TYPE 0x11, payload 1 bayt */
  n = frame_build(cikti, (uint8_t)sizeof(cikti),
                    FRAME_TYPE_JOYSTICK_MODE, 2U, p_mod, 1U);
  test_kaydet_bool((uint8_t)((n == 10U) &&
                   (memcmp(cikti, b_mod, sizeof(b_mod)) == 0)));

  /* T4: payload'siz paket - NULL + uzunluk 0 gecerli bir kullanimdir */
  n = frame_build(cikti, (uint8_t)sizeof(cikti),
                    FRAME_TYPE_JOYSTICK_MODE, 7U, NULL, 0U);
  test_kaydet_bool((uint8_t)((n == 9U) &&
                   (memcmp(cikti, b_bos, sizeof(b_bos)) == 0)));

  /* T5: payload icinde AA 55 - baslangic isareti veri olarak da gecebilir */
  n = frame_build(cikti, (uint8_t)sizeof(cikti),
                    FRAME_TYPE_SET_OUTPUT, 300U, p_aa55, 2U);
  test_kaydet_bool((uint8_t)((n == 11U) &&
                   (memcmp(cikti, b_aa55, sizeof(b_aa55)) == 0)));

  /* T6: payload cok uzun (56 > 55) -> reddedilmeli */
  test_kaydet(frame_build(cikti, (uint8_t)sizeof(cikti),
                            FRAME_TYPE_SET_OUTPUT, 1U, p_uzun, 56U), 0U);

  /* T7: hedef cok kucuk (13 gerekli, 12 verildi) -> reddedilmeli */
  test_kaydet(frame_build_joystick(cikti, 12U, 1000, -500, 1U), 0U);

  /* T8: hedef NULL -> reddedilmeli */
  test_kaydet(frame_build_joystick(NULL, 13U, 1000, -500, 1U), 0U);

  /* T9: tam sinir (13) kabul edilmeli */
  test_kaydet(frame_build_joystick(cikti, 13U, 1000, -500, 1U), 13U);
}


/* Gecerli paket bulununca cagrilir. kullanici -> collector_t */
static void frame_collector(const frame_info_t *info, void *user_data)
{
  collector_t *t = (collector_t *)user_data;

  if (t->adet < 4U)
  {
    t->turler[t->adet]  = info->type;
    t->siralar[t->adet] = info->seq;
  }

  if ((info->type == FRAME_TYPE_JOYSTICK) && (info->payload_len == 4U))
  {
    t->son_x = (int16_t)((uint16_t)info->payload[0] |
                        ((uint16_t)info->payload[1] << 8));
    t->son_y = (int16_t)((uint16_t)info->payload[2] |
                        ((uint16_t)info->payload[3] << 8));
  }

  t->adet++;
}


static void frame_parser_testleri_kosur(void)
{
  /* Akislar ve beklenen sayaclar protokol.py akis ile uretildi */
  static const uint8_t s_tek[13] =
    {0xAA,0x55,0x01,0x10,0x04,0x01,0x00,0xE8,0x03,0x0C,0xFE,0x46,0x59};
  static const uint8_t s_ikili[23] =
    {0xAA,0x55,0x01,0x10,0x04,0x01,0x00,0xE8,0x03,0x0C,0xFE,0x46,0x59,
     0xAA,0x55,0x01,0x11,0x01,0x02,0x00,0x01,0x4E,0xED};
  static const uint8_t s_cop[17] =
    {0x00,0xFF,0xAA,0x13,
     0xAA,0x55,0x01,0x10,0x04,0x01,0x00,0xE8,0x03,0x0C,0xFE,0x46,0x59};
  static const uint8_t s_aa55[11] =
    {0xAA,0x55,0x01,0x20,0x02,0x2C,0x01,0xAA,0x55,0x8D,0x8F};
  static const uint8_t s_bozuk[13] =
    {0xAA,0x55,0x01,0x10,0x04,0x01,0x00,0xE8,0x03,0x0C,0xFE,0x47,0x59};
  static const uint8_t s_kurtarma[24] =
    {0xAA,0x55,0x01,0x20,0x02,0x2C,0x01,0xAA,0x55,0x8D,0x8E,
     0xAA,0x55,0x01,0x10,0x04,0x01,0x00,0xE8,0x03,0x0C,0xFE,0x46,0x59};
  static const uint8_t s_uzunluk[20] =
    {0xAA,0x55,0x01,0x20,0x38,0x01,0x00,
     0xAA,0x55,0x01,0x10,0x04,0x01,0x00,0xE8,0x03,0x0C,0xFE,0x46,0x59};
  static const uint8_t s_surum[20] =
    {0xAA,0x55,0x02,0x10,0x04,0x01,0x00,
     0xAA,0x55,0x01,0x10,0x04,0x01,0x00,0xE8,0x03,0x0C,0xFE,0x46,0x59};
  static const uint8_t s_yarim_mod[16] =
    {0xAA,0x55,0x01,0x10,0x04,0x01,
     0xAA,0x55,0x01,0x11,0x01,0x02,0x00,0x01,0x4E,0xED};
  static const uint8_t s_yarim[8] =
    {0xAA,0x55,0x01,0x10,0x04,0x01,0x00,0xE8};

  /* S13: LENGTH=55 diyen bir baslik (64 bayt bekler) + arkasinda gecerli
     cerceve. Toplam 20 bayt geldigi icin aday asla tamamlanmaz ve arkadaki
     gecerli cerceve de bekler. Zaman asimi bu tikanikligi acar. */
  static const uint8_t s_tikanik[20] =
    {0xAA,0x55,0x01,0x20,0x37,0x01,0x00,
     0xAA,0x55,0x01,0x10,0x04,0x01,0x00,0xE8,0x03,0x0C,0xFE,0x46,0x59};

  frame_parser_t    p;
  collector_t t;
  uint8_t     i;

  /* S1: tek tam paket -> 1 gecerli, X ve Y dogru */
  frame_parser_init(&p);
  memset(&t, 0, sizeof(t));
  frame_parser_feed(&p, s_tek, (uint16_t)sizeof(s_tek), frame_collector, &t);
  test_kaydet_bool((uint8_t)((t.adet == 1U) && (t.son_x == 1000) &&
                             (t.son_y == -500) && (p.frames_ok == 1U)));

  /* S2: ayni paket iki parcaya bolunmus (5 + 8) */
  frame_parser_init(&p);
  memset(&t, 0, sizeof(t));
  frame_parser_feed(&p, &s_tek[0], 5U, frame_collector, &t);
  frame_parser_feed(&p, &s_tek[5], 8U, frame_collector, &t);
  test_kaydet_bool((uint8_t)((t.adet == 1U) && (t.son_x == 1000)));

  /* S3: bayt bayt beslenmis */
  frame_parser_init(&p);
  memset(&t, 0, sizeof(t));
  for (i = 0U; i < (uint8_t)sizeof(s_tek); i++)
  {
    frame_parser_feed(&p, &s_tek[i], 1U, frame_collector, &t);
  }
  test_kaydet_bool((uint8_t)((t.adet == 1U) && (t.son_y == -500)));

  /* S4: iki paket tek beslemede birlesik -> 2 gecerli, sira korunmus */
  frame_parser_init(&p);
  memset(&t, 0, sizeof(t));
  frame_parser_feed(&p, s_ikili, (uint16_t)sizeof(s_ikili), frame_collector, &t);
  test_kaydet_bool((uint8_t)((t.adet == 2U) &&
                             (t.turler[0] == FRAME_TYPE_JOYSTICK) &&
                             (t.turler[1] == FRAME_TYPE_JOYSTICK_MODE)));

  /* S5: basta cop bayt -> paket yine bulunur, 4 bayt atilir */
  frame_parser_init(&p);
  memset(&t, 0, sizeof(t));
  frame_parser_feed(&p, s_cop, (uint16_t)sizeof(s_cop), frame_collector, &t);
  test_kaydet_bool((uint8_t)((t.adet == 1U) && (p.bytes_dropped == 4U)));

  /* S6: payload icinde AA 55 -> normal veri sayilmali */
  frame_parser_init(&p);
  memset(&t, 0, sizeof(t));
  frame_parser_feed(&p, s_aa55, (uint16_t)sizeof(s_aa55), frame_collector, &t);
  test_kaydet_bool((uint8_t)((t.adet == 1U) &&
                             (t.turler[0] == FRAME_TYPE_SET_OUTPUT) &&
                             (t.siralar[0] == 300U)));

  /* S7: CRC bozuk -> paket teslim edilmemeli */
  frame_parser_init(&p);
  memset(&t, 0, sizeof(t));
  frame_parser_feed(&p, s_bozuk, (uint16_t)sizeof(s_bozuk), frame_collector, &t);
  test_kaydet_bool((uint8_t)((t.adet == 0U) && (p.err_crc == 1U) &&
                             (p.bytes_dropped == 13U)));

  /* S8: bozuk paket + arkasindan gecerli paket -> KURTARMA SINAVI */
  frame_parser_init(&p);
  memset(&t, 0, sizeof(t));
  frame_parser_feed(&p, s_kurtarma, (uint16_t)sizeof(s_kurtarma), frame_collector, &t);
  test_kaydet_bool((uint8_t)((t.adet == 1U) && (t.son_x == 1000) &&
                             (p.err_crc == 1U)));

  /* S9: LENGTH 56 (sinir disi) + gecerli paket */
  frame_parser_init(&p);
  memset(&t, 0, sizeof(t));
  frame_parser_feed(&p, s_uzunluk, (uint16_t)sizeof(s_uzunluk), frame_collector, &t);
  test_kaydet_bool((uint8_t)((t.adet == 1U) && (p.err_len == 1U) &&
                             (p.bytes_dropped == 7U)));

  /* S10: VERSION 2 + gecerli paket */
  frame_parser_init(&p);
  memset(&t, 0, sizeof(t));
  frame_parser_feed(&p, s_surum, (uint16_t)sizeof(s_surum), frame_collector, &t);
  test_kaydet_bool((uint8_t)((t.adet == 1U) && (p.err_version == 1U) &&
                             (p.bytes_dropped == 7U)));

  /* S11: yarim kalmis paket + tam mod paketi -> KURTARMA SINAVI */
  frame_parser_init(&p);
  memset(&t, 0, sizeof(t));
  frame_parser_feed(&p, s_yarim_mod, (uint16_t)sizeof(s_yarim_mod), frame_collector, &t);
  test_kaydet_bool((uint8_t)((t.adet == 1U) &&
                             (t.turler[0] == FRAME_TYPE_JOYSTICK_MODE) &&
                             (p.err_crc == 1U)));

  /* S12: yarim paket, devami hic gelmiyor -> teslim yok, cokme yok */
  frame_parser_init(&p);
  memset(&t, 0, sizeof(t));
  frame_parser_feed(&p, s_yarim, (uint16_t)sizeof(s_yarim), frame_collector, &t);
  test_kaydet_bool((uint8_t)((t.adet == 0U) && (p.frames_ok == 0U) &&
                             (p.err_crc == 0U)));

  /* S13: bozuk LENGTH tikanikligi ve zaman asimiyla acilmasi.
     Once beslemede hicbir cerceve teslim edilmemeli (aday 64 bayt bekliyor,
     elde 20 var). Sonra zaman asimi bir bayt atar, yeniden tarama arkadaki
     gecerli cerceveyi bulur. */
  frame_parser_init(&p);
  memset(&t, 0, sizeof(t));
  frame_parser_feed(&p, s_tikanik, (uint16_t)sizeof(s_tikanik),
                    frame_collector, &t);
  test_kaydet_bool((uint8_t)((t.adet == 0U) && (p.len == 20U)));

  frame_parser_timeout(&p, frame_collector, &t);
  test_kaydet_bool((uint8_t)((t.adet == 1U) &&
                             (t.turler[0] == FRAME_TYPE_JOYSTICK) &&
                             (t.siralar[0] == 1U) &&
                             (t.son_x == 1000) && (t.son_y == -500) &&
                             (p.timeouts == 1U) &&
                             (p.bytes_dropped == 7U) &&
                             (p.len == 0U)));
}


void birim_testleri_kosur(void)
{
  test_sayisi = 0U;
  test_gecen  = 0U;
  test_kalan  = 0U;
  test_sayim_dogru = 0U;

  crc16_testleri_kosur();
  paket_testleri_kosur();
  frame_parser_testleri_kosur();

  /* "test_kalan == 0" tek basina yetmez: bir test grubu hic cagrilmazsa da
     sifirdir. Beklenen sayida testin GERCEKTEN kostugu ayrica dogrulanir. */
  test_sayim_dogru = (uint8_t)(test_sayisi == TEST_BEKLENEN_ADET);
}


/* --- Loopback (donanim) testleri ---
   Birim testlerinden AYRI sayaclar kullanilir: birim testleri donanimsiz
   kosar, bunlar PA2-PA3 jumper'i ve calisan bir UART ister. Ikisini ayni
   sayacta toplamak "29 test gecti" ifadesinin anlamini bozardi. */

static void lb_kaydet_bool(uint8_t dogru_mu)
{
  if (lb_sayisi >= LB_SONUC_ADET)
  {
    return;
  }

  if (dogru_mu != 0U)
  {
    lb_sonuc[lb_sayisi] = 1U;
    lb_gecen++;
  }
  else
  {
    lb_sonuc[lb_sayisi] = 2U;
    lb_kalan++;
  }

  lb_sayisi++;
}


/* Belirtilen sure boyunca service dondurur. Zaman asimi ve toparlanma
   kararlari yalnizca service icinde verildigi icin testlerin "bekleme"
   adimlari HAL_Delay DEGIL bu olmalidir. */
static void service_dondur(uint32_t ms)
{
  uint32_t t0 = HAL_GetTick();

  while ((HAL_GetTick() - t0) < ms)
  {
    uart_rx_service();
  }
}


/* TX tamamlanana VE beklenen cerceve varana kadar her iki servisi dondurur.
   HAL_Delay ile beklenmez: tamamlanma, zaman asimi ve toparlanma kararlari
   yalnizca service icinde verildigi icin beklemek demek service dondurmek
   demektir. Hedefe ulasilmazsa ms sonunda cikar ve testin assert'i duser. */
static void tx_rx_bekle(uint16_t frames_ok_hedef, uint32_t ms)
{
  uint32_t t0 = HAL_GetTick();

  while ((HAL_GetTick() - t0) < ms)
  {
    uart_tx_service();
    uart_rx_service();

    if ((uart_tx_get_state() == UART_TX_IDLE) &&
        (uart_rx_get_parser()->frames_ok >= frames_ok_hedef))
    {
      break;
    }
  }
}


void loopback_testi_kosur(UART_HandleTypeDef *huart)
{
  /* T3: LENGTH=55 diyen bozuk baslik. 64 bayt bekler, devami hic gelmez. */
  static const uint8_t t3_baslik[FRAME_HEADER_SIZE] =
    {0xAA, 0x55, 0x01, 0x20, 0x37, 0x01, 0x00};

  uint8_t  tx[FRAME_MAX_SIZE];
  uint8_t  tx2[FRAME_OVERHEAD + 4U];   /* T8: ikinci istek, joystick boyu */
  uint8_t  payload[FRAME_MAX_PAYLOAD];
  uint8_t  n;
  uint8_t  n2;
  uint16_t sira;
  uint16_t i;
  uint16_t bekleyen;
  uint16_t ok_once;
  uint16_t to_once;
  uint16_t rf_once;
  uint16_t err_once;
  uint16_t drop_once;
  uint16_t crc_once;
  uint16_t busy_once;
  uint16_t tx_frames_once;
  uint16_t tx_cplt_once;
  uint32_t tx_bytes_once;
  uint32_t t0;
  uart_tx_status_t st1;
  uart_tx_status_t st2;

  lb_sayisi       = 0U;
  lb_gecen        = 0U;
  lb_kalan        = 0U;
  lb_calismayan   = LB_BEKLENEN_ADET;   /* kosana kadar hepsi NOT_RUN */
  lb_iptal_adim   = 0xFFU;
  lb_iptal_nedeni = LB_IPTAL_YOK;
  (void)memset(lb_sonuc,  0, sizeof(lb_sonuc));    /* 0 = NOT_RUN */
  (void)memset(lb_kaynak, LB_KAYNAK_YAZILIM, sizeof(lb_kaynak));

  if (huart == NULL)
  {
    LB_IPTAL(LB_IPTAL_HUART_NULL);
  }

  /* TX modulu T4'ten itibaren kullaniliyor. main.c de init ediyor ama test
     ondan bagimsiz kosabilmeli. Durum IDLE iken tekrar cagirmak zararsiz. */
  if (uart_tx_init(huart) != HAL_OK)
  {
    LB_IPTAL(LB_IPTAL_TX_INIT);
  }

  /* ---------------- T1: tek cerceve ----------------
     X = +1000, Y = -500, SEQUENCE = 1
     Beklenen 13 bayt: AA 55 01 10 04 01 00 E8 03 0C FE 46 59 */
  /* Mutlak degil DELTA olcum: ileride yeniden baslatma istatistikleri
     korunacagi icin "frames_ok == 1" varsayimi kirilgandir (P0). */
  ok_once = uart_rx_get_parser()->frames_ok;

  n = frame_build_joystick(tx, (uint8_t)sizeof(tx), 1000, -500, 1U);
  if (n == 0U)
  {
    LB_IPTAL(LB_IPTAL_FRAME_BUILD);
  }

  if (HAL_UART_Transmit(huart, tx, n, 100U) != HAL_OK)
  {
    LB_IPTAL(LB_IPTAL_HAL_TRANSMIT);
  }

  /* uart_rx_drain() DEGIL uart_rx_service(): boylece
     callback -> s_rx_pending -> service zinciri de sinanir. IDLE son bayttan
     ~87 us sonra tetiklendigi icin 1 ms beklemek yeterli. */
  HAL_Delay(1U);
  uart_rx_service();

  lb_kaydet_bool((uint8_t)((uart_rx_get_parser()->frames_ok ==
                            (uint16_t)(ok_once + 1U)) &&
                           (uart_rx_state.last_seq == 1U) &&
                           (uart_rx_state.joy_x == 1000) &&
                           (uart_rx_state.joy_y == -500)));

  /* ---------------- T2: sarim ----------------
     SEQUENCE 2..40 ile 39 cerceve daha. Toplam 40 x 13 = 520 bayt; tampon
     256 bayt oldugundan sarim iki kez gerceklesir.

     Her gonderimden sonra tuketmek ZORUNLU. Tuketmezsen yaklasik 20.
     cercevede DMA okunmamis veriyi ezmeye baslar ve konumlar esit gorunerek
     kaybi gizler. */
  for (sira = 2U; sira <= 40U; sira++)
  {
    n = frame_build_joystick(tx, (uint8_t)sizeof(tx), 1000, -500, sira);
    if (n == 0U)
    {
      LB_IPTAL(LB_IPTAL_FRAME_BUILD);
    }

    if (HAL_UART_Transmit(huart, tx, n, 100U) != HAL_OK)
    {
      LB_IPTAL(LB_IPTAL_HAL_TRANSMIT);
    }

    HAL_Delay(1U);
    uart_rx_service();
  }

  lb_kaydet_bool((uint8_t)((uart_rx_get_parser()->frames_ok ==
                            (uint16_t)(ok_once + 40U)) &&
                           (uart_rx_state.last_seq == 40U) &&
                           (uart_rx_state.next_seq == 41U) &&
                           (uart_rx_state.seq_gaps == 0U) &&
                           (uart_rx_get_parser()->len == 0U) &&
                           (uart_rx_get_parser()->bytes_dropped == 0U)));

  /* ---------------- T3: otomatik zaman asimi ----------------
     S13 frame_parser_timeout() fonksiyonunu DOGRUDAN cagirir, yani
     algoritmayi sinar. Burada sinanan sey kararin kendiliginden verilmesi:
     gercek 50 ms sessizlik -> uart_rx_service -> check_frame_timeout. */
  to_once   = uart_rx_stats.frame_timeouts;
  drop_once = uart_rx_get_parser()->bytes_dropped;

  if (HAL_UART_Transmit(huart, t3_baslik,
                        (uint16_t)sizeof(t3_baslik), 100U) != HAL_OK)
  {
    LB_IPTAL(LB_IPTAL_HAL_TRANSMIT);
  }

  HAL_Delay(1U);
  uart_rx_service();
  bekleyen = uart_rx_get_parser()->len;      /* 7 bayt tikanmis olmali */

  service_dondur(60U);                       /* 50 ms'de ateslenmeli */

  lb_kaydet_bool((uint8_t)(bekleyen == FRAME_HEADER_SIZE));

  /* Zaman asimi BIR bayt atar, yeniden tarama kalan 6 bayti eler:
     55 01 20 37 01 00 hicbiri SYNC0 degil. Toplam 7. */
  lb_kaydet_bool((uint8_t)((uart_rx_stats.frame_timeouts ==
                            (uint16_t)(to_once + 1U)) &&
                           (uart_rx_get_parser()->len == 0U) &&
                           (uart_rx_get_parser()->bytes_dropped ==
                            (uint16_t)(drop_once + FRAME_HEADER_SIZE))));

  /* ---------------- T4: sinir testi (bulgu 2 regresyonu) ----------------
     Zaman asimi siniri, AKMAKTA OLAN gecerli bir cercevenin ortasina duser.
     Bir yayin surerken hicbir bildirim olusmaz (IDLE son bayttan ~87 us
     sonra, HT/TC yalnizca 128./256. baytta), bu yuzden yalnizca zaman
     damgasina bakan bir kontrol burada gecerli cerceveden bayt atar.

     Devam yayini BLOKLAYAN Transmit ile gonderilemez: o sirada
     uart_rx_service() cagrilamaz ve hata gorunmez kalir. Gonderim DMA ile,
     yani uart_tx uzerinden yapiliyor ve main dongusu serbest kaliyor.

     HAL_UART_Transmit_DMA DOGRUDAN cagrilmaz: ayni UART uzerinde modulu
     atlayan bir gonderim, modulun TxCplt callback'ini de tetikler ve
     s_tx_done'i baslatmadigi bir aktarim icin kaldirir. Ilk kosuda tam bu
     oldu ve T7 dusdu. Sahiplik kurali testlerde de gecerli. */
  for (i = 0U; i < FRAME_MAX_PAYLOAD; i++)
  {
    payload[i] = (uint8_t)i;
  }

  n = frame_build(tx, (uint8_t)sizeof(tx), FRAME_TYPE_SET_OUTPUT, 41U,
                  payload, FRAME_MAX_PAYLOAD);
  if (n != FRAME_MAX_SIZE)                   /* 64 bayt beklenir */
  {
    LB_IPTAL(LB_IPTAL_FRAME_BUILD);
  }

  ok_once = uart_rx_get_parser()->frames_ok;
  to_once = uart_rx_stats.frame_timeouts;

  if (HAL_UART_Transmit(huart, tx, FRAME_HEADER_SIZE, 100U) != HAL_OK)
  {
    LB_IPTAL(LB_IPTAL_HAL_TRANSMIT);
  }

  HAL_Delay(1U);
  uart_rx_service();                         /* aday olustu, tick simdi */

  service_dondur(UART_RX_FRAME_TIMEOUT_MS - 3U);   /* sessizce sinira yaklas */

  /* 57 bayt ~4,95 ms surer: 50 ms siniri yayinin ortasina duser */
  if (uart_tx_send_copy(&tx[FRAME_HEADER_SIZE],
                        (uint8_t)(n - FRAME_HEADER_SIZE)) != UART_TX_OK)
  {
    LB_IPTAL(LB_IPTAL_TX_SEND_COPY);
  }

  /* Teslim olana kadar (veya 30 ms) service dondur */
  t0 = HAL_GetTick();
  while (((HAL_GetTick() - t0) < 30U) &&
         (uart_rx_get_parser()->frames_ok == ok_once))
  {
    uart_tx_service();
    uart_rx_service();
  }

  lb_kaydet_bool((uint8_t)((uart_rx_get_parser()->frames_ok ==
                            (uint16_t)(ok_once + 1U)) &&
                           (uart_rx_stats.frame_timeouts == to_once) &&
                           (uart_rx_state.last_seq == 41U) &&
                           (uart_rx_state.seq_gaps == 0U)));

  /* ---------------- T5: gercek UART hatasi ----------------
     Yazilimdan ErrorCallback cagirmak hicbir sey kanitlamaz. USART_CR1_SBK
     hatta bir break gonderir (baskin 0 bitleri); loopback'te alici bunu
     framing error olarak gorur. FE'de HAL alimi kesmez, yalnizca
     ErrorCallback cagirir; bu yuzden toparlanma BUSY_RX dalina girip
     calisan alima DOKUNMAMALIDIR. */
  err_once = uart_rx_stats.error_events;
  ok_once  = uart_rx_get_parser()->frames_ok;

  SET_BIT(huart->Instance->CR1, USART_CR1_SBK);
  HAL_Delay(2U);
  uart_rx_service();

  lb_kaynak[lb_sayisi] = LB_KAYNAK_FIZIKSEL;   /* gercek FE, enjeksiyon degil */
  lb_kaydet_bool((uint8_t)((uart_rx_stats.error_events > err_once) &&
                           ((uart_rx_stats.last_error &
                             HAL_UART_ERROR_FE) != 0U)));

  /* ASIL IDDIA: hatadan sonra alim hala calisiyor. Break'in tampona
     dusurdugu bayt SYNC0 olmadigi icin elenir. */
  lb_kaynak[lb_sayisi] = LB_KAYNAK_FIZIKSEL;
  n = frame_build_joystick(tx, (uint8_t)sizeof(tx), -250, 750, 42U);
  if (n == 0U)
  {
    LB_IPTAL(LB_IPTAL_FRAME_BUILD);
  }

  if (HAL_UART_Transmit(huart, tx, n, 100U) != HAL_OK)
  {
    LB_IPTAL(LB_IPTAL_HAL_TRANSMIT);
  }

  HAL_Delay(2U);
  uart_rx_service();

  lb_kaydet_bool((uint8_t)((uart_rx_get_parser()->frames_ok ==
                            (uint16_t)(ok_once + 1U)) &&
                           (uart_rx_state.last_seq == 42U) &&
                           (uart_rx_state.joy_x == -250) &&
                           (uart_rx_state.joy_y == 750)));

  /* ---------------- T6: basarisiz yeniden baslatma ----------------
     Gercek donanimda restart'in dusmesini deterministik uretmek mumkun
     degil; test kancasi her denemeyi basarisiz saydirir. Sinanan sey:
     borcun kapanmamasi, denemeler arasi bekleme, sinirda kalici hataya
     gecis ve tek cikis yolunun uart_rx_start olmasi.

     uart_rx_start ayristiriciyi sifirlayacagi icin onceki sayaclar
     debugger'da gorunsun diye once saklaniyor. */
  lb_frames_ok     = uart_rx_get_parser()->frames_ok;
  lb_last_seq      = uart_rx_state.last_seq;
  lb_seq_gaps      = uart_rx_state.seq_gaps;
  lb_bytes_dropped = uart_rx_get_parser()->bytes_dropped;

  rf_once = uart_rx_stats.restart_fails;

  uart_rx_force_restart_fail(1U);
  uart_rx_test_inject_error();

  service_dondur(60U);            /* 5 deneme x 5 ms = ~25 ms; 60 bol */

  uart_rx_force_restart_fail(0U);

  /* Bu sonuc KONTROLLU HATA ENJEKSIYONU ile uretildi: yazilim mantigini
     dogrular, donanimin gercek toparlanma davranisini DEGIL. */
  lb_kaynak[lb_sayisi] = LB_KAYNAK_ENJEKTE;
  lb_kaydet_bool((uint8_t)((uart_rx_stats.restart_fails ==
                            (uint16_t)(rf_once +
                                       UART_RX_RESTART_MAX_TRIES)) &&
                           (uart_rx_stats.faulted == 1U)));

  /* GERCEK bir kalici hatada RxState READY'dir: abort basarili olmus, yalnizca
     ReceiveToIdle_DMA dusmustur. Test kancasi abort'u hic calistirmadigi icin
     alim burada hala BUSY_RX ve uart_rx_start dogru sekilde HAL_BUSY doner.
     Bu satir, kancanin atladigi onkosulu geri veriyor; uart_rx_start'in
     calisan bir alimi sessizce yikmasini istemedigimiz icin duzeltme testte,
     modulde degil. */
  (void)HAL_UART_AbortReceive(huart);

  /* Kalici hatadan tek cikis: yeniden kurmak */
  if (uart_rx_start(huart) != HAL_OK)
  {
    LB_IPTAL(LB_IPTAL_RX_START);
  }

  lb_kaynak[lb_sayisi] = LB_KAYNAK_ENJEKTE;
  lb_kaydet_bool((uint8_t)(uart_rx_stats.faulted == 0U));

  /* Yeniden kurulan alim icin YENI delta tabani: uart_rx_start ayristiriciyi
     sifirliyor ama ileride (R1/R2) sifirlamayacak. Mutlak "frames_ok == 1"
     yerine bu tabana gore olculur. */
  ok_once = uart_rx_get_parser()->frames_ok;

  /* Alim gercekten geri geldi mi: bir cerceve daha cozulmeli (delta) */
  n = frame_build_joystick(tx, (uint8_t)sizeof(tx), 12, -34, 43U);
  if (n == 0U)
  {
    LB_IPTAL(LB_IPTAL_FRAME_BUILD);
  }

  if (HAL_UART_Transmit(huart, tx, n, 100U) != HAL_OK)
  {
    LB_IPTAL(LB_IPTAL_HAL_TRANSMIT);
  }

  HAL_Delay(2U);
  uart_rx_service();

  lb_kaynak[lb_sayisi] = LB_KAYNAK_ENJEKTE;
  lb_kaydet_bool((uint8_t)((uart_rx_get_parser()->frames_ok ==
                            (uint16_t)(ok_once + 1U)) &&
                           (uart_rx_state.last_seq == 43U) &&
                           (uart_rx_state.joy_x == 12) &&
                           (uart_rx_state.joy_y == -34)));

  /* ---------------- T7: TX zincirinin tamami ----------------
     frame_build -> uart_tx_send_copy -> TX DMA -> tel -> RX DMA -> parser.
     Tek test kendi kodumuzun HER IKI yonunu birden geciyor; buraya kadar
     gonderim hep HAL'e dogrudan yapiliyordu.

     Sayaclar DELTA olarak kontrol ediliyor: T4 de artik uart_tx kullandigi
     icin mutlak deger beklemek testi gereksiz yere kirilgan yapar. */
  ok_once        = uart_rx_get_parser()->frames_ok;
  tx_frames_once = uart_tx_stats.frames_sent;
  tx_bytes_once  = uart_tx_stats.bytes_sent;
  tx_cplt_once   = uart_tx_stats.tx_complete_events;

  n = frame_build_joystick(tx, (uint8_t)sizeof(tx), 1000, -500, 44U);
  if (n == 0U)
  {
    LB_IPTAL(LB_IPTAL_FRAME_BUILD);
  }

  st1 = uart_tx_send_copy(tx, n);
  tx_rx_bekle((uint16_t)(ok_once + 1U), 30U);

  /* TX tarafi: bildirim zinciri calisti mi?
     tx_complete_events HAL_UART_TxCpltCallback'ten gelir, yani UART TC'den;
     DMA TC'den degil. bytes_sent ise s_len uzerinden gelir: send_copy ile
     service arasindaki tek bag o degisken. */
  lb_kaydet_bool((uint8_t)((st1 == UART_TX_OK) &&
                           (uart_tx_stats.frames_sent ==
                            (uint16_t)(tx_frames_once + 1U)) &&
                           (uart_tx_stats.bytes_sent ==
                            (tx_bytes_once + (uint32_t)n)) &&
                           (uart_tx_stats.tx_complete_events ==
                            (uint16_t)(tx_cplt_once + 1U)) &&
                           (uart_tx_get_state() == UART_TX_IDLE)));

  /* RX tarafi: ayni cerceve geri geldi mi */
  lb_kaydet_bool((uint8_t)((uart_rx_get_parser()->frames_ok ==
                            (uint16_t)(ok_once + 1U)) &&
                           (uart_rx_state.last_seq == 44U) &&
                           (uart_rx_state.joy_x == 1000) &&
                           (uart_rx_state.joy_y == -500) &&
                           (uart_rx_state.seq_gaps == 0U)));

  /* ---------------- T8: mesgulken ikinci istek ----------------
     Kabul olcutu: "Bir gonderim surerken ikinci baslatma ilk paketi bozamaz."

     13 bayt 115200 8N1'de ~1,13 ms surer; iki send_copy arasindaki is
     mikrosaniye mertebesinde, yani ikincisi DMA mesgulken dusuyor.
     Burada sinanan sey durum makinesinin tamponu kilitlemesi: ikinci istek
     s_buf'a DOKUNMADAN geri cevrilmeli. */
  ok_once   = uart_rx_get_parser()->frames_ok;
  crc_once  = uart_rx_get_parser()->err_crc;
  busy_once = uart_tx_stats.rejected_busy;

  n  = frame_build_joystick(tx,  (uint8_t)sizeof(tx),    7,   -7, 45U);
  n2 = frame_build_joystick(tx2, (uint8_t)sizeof(tx2),  99,  -99, 46U);
  if ((n == 0U) || (n2 == 0U))
  {
    LB_IPTAL(LB_IPTAL_FRAME_BUILD);
  }

  st1 = uart_tx_send_copy(tx,  n);     /* kabul edilmeli */
  st2 = uart_tx_send_copy(tx2, n2);    /* reddedilmeli */

  tx_rx_bekle((uint16_t)(ok_once + 1U), 30U);

  lb_kaydet_bool((uint8_t)((st1 == UART_TX_OK) &&
                           (st2 == UART_TX_BUSY) &&
                           (uart_tx_stats.rejected_busy ==
                            (uint16_t)(busy_once + 1U))));

  /* Ilk cerceve BOZULMADAN varmali. err_crc'nin artmamasi kritik: ikinci
     istek s_buf'in yarisini ezseydi hatta karma bir cerceve cikar ve CRC
     duserdi. 46 numarali cerceve ise hic gonderilmedi, o yuzden bir sonraki
     test onu kullaniyor ve seq_gaps sifir kaliyor. */
  lb_kaydet_bool((uint8_t)((uart_rx_get_parser()->frames_ok ==
                            (uint16_t)(ok_once + 1U)) &&
                           (uart_rx_get_parser()->err_crc == crc_once) &&
                           (uart_rx_state.last_seq == 45U) &&
                           (uart_rx_state.joy_x == 7) &&
                           (uart_rx_state.joy_y == -7)));

  /* ---------------- T9: kopya semantigi ----------------
     Kabul olcutu: "Cagiranin kaynak tamponu gonderim kabulunden sonra degisse
     bile DMA'nin gonderdigi kopya degismez."

     Modul isaretci saklasaydi DMA 0xFF dizisini okur, hatta cop cikar, CRC
     duser (err_crc artar) ve cerceve hic teslim edilmezdi. Yani bu test
     _copy tasarim kararinin dogrudan kaniti: zero-copy'ye gecildigi gun
     duser. */
  ok_once  = uart_rx_get_parser()->frames_ok;
  crc_once = uart_rx_get_parser()->err_crc;

  n = frame_build_joystick(tx, (uint8_t)sizeof(tx), -1234, 4321, 46U);
  if (n == 0U)
  {
    LB_IPTAL(LB_IPTAL_FRAME_BUILD);
  }

  st1 = uart_tx_send_copy(tx, n);

  /* UART_TX_OK dondu: kaynak tampon artik cagiranin, cope cevirmek serbest */
  memset(tx, 0xFF, sizeof(tx));

  tx_rx_bekle((uint16_t)(ok_once + 1U), 30U);

  lb_kaydet_bool((uint8_t)((st1 == UART_TX_OK) &&
                           (uart_rx_get_parser()->frames_ok ==
                            (uint16_t)(ok_once + 1U)) &&
                           (uart_rx_get_parser()->err_crc == crc_once) &&
                           (uart_rx_state.last_seq == 46U) &&
                           (uart_rx_state.joy_x == -1234) &&
                           (uart_rx_state.joy_y == 4321)));

bitir:
  /* Normal bitiste de, LB_IPTAL ile erken cikista da buraya gelinir.
     Kosmayan testler lb_sonuc[] icinde 0 (NOT_RUN) kalir ve burada sayilir;
     boylece "lb_kalan == 0" artik tek basina basari olarak okunamaz. */
  lb_calismayan = (uint8_t)((lb_sayisi < LB_BEKLENEN_ADET)
                            ? (LB_BEKLENEN_ADET - lb_sayisi)
                            : 0U);
}

#endif /* UART_COMM_TEST */

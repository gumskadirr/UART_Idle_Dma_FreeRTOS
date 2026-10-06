/*
 * tests.h
 *
 * Gelistirme sirasinda kullanilan dogrulama kosucularI. Uretim kodu bunlara
 * bagimli degildir; cagrilari main.c'den kaldirmak yeterlidir.
 *
 * Sonuclar debugger'da Live Expressions ile okunur:
 *   test_gecen, test_kalan, test_sonuc[]
 * Bir test basarisizsa test_sonuc dizisinde 2 olan indekse bakilir
 *   0-5   : CRC testleri
 *   6-14  : cerceve olusturucu testleri
 *   15-28 : ayristirici senaryolari (S1..S13; S13 iki kontrol)
 */

#ifndef INC_TESTS_H_
#define INC_TESTS_H_

#include <stdint.h>
#include "stm32f4xx_hal.h"

/* Bu basligin ICERIGI yalnizca UART_COMM_TEST derlemesinde vardir (P0).
   Uretim derlemesinde dosya dahil edilse bile hicbir sey tanimlamaz. */
#ifdef UART_COMM_TEST
/* Bare-metal test owner'i TX sonuc kutusunu her tur tuketir. */
void test_tx_service(void);

#define TEST_SONUC_ADET   32U
#define LB_SONUC_ADET     24U

/* KOSULMASI BEKLENEN test sayisi. "lb_kalan == 0" tek basina basari DEGILDIR:
   bir onkosul dusup kosucu erken ciktiginda da sifirdir. Gercek olcut
   "beklenen sayida test kostu VE hicbiri dusmedi"; bu sabitler o karsilastirmayi
   mumkun kilar. Sayim: CRC 6 + cerceve olusturucu 9 + ayristirici 14 = 29. */
#define TEST_BEKLENEN_ADET  29U
#define LB_BEKLENEN_ADET    15U

extern uint8_t  test_sonuc[TEST_SONUC_ADET];   /* 0 kosulmadi, 1 PASS, 2 FAIL */
extern uint8_t  test_sayisi;
extern uint8_t  test_gecen;
extern uint8_t  test_kalan;
extern uint16_t test_beklenen;                 /* son basarisiz testin beklentisi */
extern uint16_t test_bulunan;                  /* son basarisiz testin sonucu */

/* Beklenen sayida birim testi gercekten kostu mu (1 evet / 0 hayir). */
extern uint8_t  test_sayim_dogru;

/* 29 birim testi: CRC, cerceve olusturucu, ayristirici. Donanim gerektirmez. */
void birim_testleri_kosur(void);

/* Loopback (donanim) testlerinin sonuclari. Birim testlerinden AYRI tutulur:
   bunlar PA2-PA3 jumper'i ve calisan bir UART ister, birim testleri istemez.
   Ikisini ayni sayacta toplamak "29 test gecti" ifadesinin anlamini bozardi.
     0 : T1  tek cerceve
     1 : T2  sarim (40 cerceve / 520 bayt)
     2 : T3a bozuk baslik tamponda tikaniyor
     3 : T3b 50 ms sessizlikte zaman asimi kendiliginden atesleniyor
     4 : T4  sinir: akmakta olan gecerli cerceve DUSMUYOR
     5 : T5a break -> gercek framing error
     6 : T5b hatadan sonra alim hala calisiyor
     7 : T6a 5 basarisiz denemeden sonra kalici hata durumu
     8 : T6b uart_rx_start kalici hatadan cikariyor
     9 : T6c yeniden kurulan alim cerceve teslim ediyor
    10 : T7a uart_tx ile gonderim: TxCplt bildirimi ve sayaclar
    11 : T7b ayni cerceve RX tarafinda cozuldu (zincirin tamami)
    12 : T8a mesgulken ikinci istek UART_TX_BUSY ile reddediliyor
    13 : T8b ilk cerceve bozulmadan vardi (s_buf kilitli kaldi)
    14 : T9  kabul sonrasi kaynak tampon bozulsa bile kopya degismedi */
extern uint8_t  lb_sonuc[LB_SONUC_ADET];       /* 0 kosulmadi, 1 PASS, 2 FAIL */
extern uint8_t  lb_sayisi;
extern uint8_t  lb_gecen;
extern uint8_t  lb_kalan;

/* --- Erken cikisin gorunur kilinmasi ---
   Kosucu icindeki onkosullar (jumper yok, frame_build dustu, HAL_Transmit
   dustu, ...) once sessiz "return" idi: lb_kalan 0 kalir ve kosmayan testler
   gecmis gibi gorunurdu. Artik iptal kaydediliyor ve kosmayan testler
   lb_calismayan altinda sayiliyor.

   KABUL OLCUTU: lb_kalan == 0 && lb_calismayan == 0 && lb_sayisi == LB_BEKLENEN_ADET */
extern uint8_t  lb_calismayan;   /* hic kosmayan (NOT_RUN) test sayisi */
extern uint8_t  lb_iptal_adim;   /* iptal aninda sirada olan test indeksi; 0xFF = iptal yok */
extern uint8_t  lb_iptal_nedeni; /* lb_iptal_* degerlerinden biri */

#define LB_IPTAL_YOK            0U
#define LB_IPTAL_HUART_NULL     1U   /* huart == NULL */
#define LB_IPTAL_TX_INIT        2U   /* uart_tx_init HAL_OK donmedi */
#define LB_IPTAL_FRAME_BUILD    3U   /* frame_build / frame_build_joystick 0 dondu */
#define LB_IPTAL_HAL_TRANSMIT   4U   /* bloklayan HAL_UART_Transmit dustu */
#define LB_IPTAL_TX_SEND_COPY   5U   /* uart_tx_send_copy UART_TX_OK donmedi */
#define LB_IPTAL_RX_START       6U   /* uart_rx_start HAL_OK donmedi */

/* --- Hatanin kaynagi etiketi ---
   "Toparlanma calisiyor" iddiasi, hatayi donanimin mi yoksa test kancasinin mi
   urettigine gore farkli agirlik tasir. Enjekte edilmis hatayla gecen bir test
   yazilim mantigini dogrular, donanim davranisini DEGIL. Ikisi ayni listede
   gorundugu icin etiket sonucun yanina konuyor. */
#define LB_KAYNAK_YAZILIM       0U   /* donanim hatasi kullanmadi */
#define LB_KAYNAK_FIZIKSEL      1U   /* gercek donanim hatasi (ornegin SBK -> FE) */
#define LB_KAYNAK_ENJEKTE       2U   /* kontrollu hata enjeksiyonu (test kancasi) */

extern uint8_t  lb_kaynak[LB_SONUC_ADET];

/* T6 oncesindeki toplamlar debugger'da gorunsun diye saklanir.
   NOT (R1): uart_rx_start artik ayristirici sayaclarini SIFIRLAMIYOR; soguk
   kurulum yalnizca ilk cagridadir, sonrakiler frame_parser_discard kullanir.
   Bu alanlar yine de tutuluyor: T6 oncesi/sonrasi karsilastirmasi icin. */
extern uint16_t lb_frames_ok;
extern uint16_t lb_last_seq;
extern uint16_t lb_seq_gaps;
extern uint16_t lb_bytes_dropped;

/* Loopback testi: PA2-PA3 arasi jumper kablo gerektirir.
   Tuketim uart_rx_service() ile yapilir; bildirim zinciri de sinanir.
   T4 ayrica TX DMA'yi (DMA1 Stream6) kullanir.
   ONKOSUL: uart_rx_start() cagrilmis olmali. */
void loopback_testi_kosur(UART_HandleTypeDef *huart);

#endif /* UART_COMM_TEST */

#endif /* INC_TESTS_H_ */

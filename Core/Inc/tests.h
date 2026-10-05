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

#define TEST_SONUC_ADET   32U
#define LB_SONUC_ADET     24U

extern uint8_t  test_sonuc[TEST_SONUC_ADET];   /* 0 kosulmadi, 1 PASS, 2 FAIL */
extern uint8_t  test_sayisi;
extern uint8_t  test_gecen;
extern uint8_t  test_kalan;
extern uint16_t test_beklenen;                 /* son basarisiz testin beklentisi */
extern uint16_t test_bulunan;                  /* son basarisiz testin sonucu */

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

/* T6, uart_rx_start() cagirdigi icin ayristirici sayaclarini sifirlar.
   T6 oncesindeki toplamlar burada saklanir. */
extern uint16_t lb_frames_ok;
extern uint16_t lb_last_seq;
extern uint16_t lb_seq_gaps;
extern uint16_t lb_bytes_dropped;

/* Loopback testi: PA2-PA3 arasi jumper kablo gerektirir.
   Tuketim uart_rx_service() ile yapilir; bildirim zinciri de sinanir.
   T4 ayrica TX DMA'yi (DMA1 Stream6) kullanir.
   ONKOSUL: uart_rx_start() cagrilmis olmali. */
void loopback_testi_kosur(UART_HandleTypeDef *huart);

#endif /* INC_TESTS_H_ */

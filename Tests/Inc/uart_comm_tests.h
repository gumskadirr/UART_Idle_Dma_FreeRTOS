/*
 * uart_comm_tests.h
 *
 * Birlesik UART RX/TX calismasinin (P0..M2) kontrollu test giris noktasi.
 *
 * NEDEN AYRI BIR KOSUCU:
 * Mevcut tests.c iki sayac ciftiyle calisir (test_*, lb_*) ve bir testin
 * KIMLIGI yalnizca dizi indeksidir. Yol haritasi R1'den itibaren testleri
 * adlariyla aniyor (RX_START_BUSY_PRESERVES_STATE, RX_PENDING_TC, ...) ve
 * her birinin PASS / FAIL / NOT_RUN olarak ayri ayri gorunmesini istiyor.
 * Indeks tabanli listede bir test eklenince sonrakilerin anlami kayar;
 * burada her kaydin kendi kimligi vardir.
 *
 * TEMEL KURAL — SESSIZ BASARI YOK:
 * Butun testler kosmadan ONCE tabloya NOT_RUN olarak yazilir. Kosucu bir
 * onkosul yuzunden erken cikarsa, kosmayan testler listede NOT_RUN olarak
 * durur. Bu yuzden "dusen yok" tek basina basari olarak okunamaz; kabul
 * olcutu asagidaki uart_comm_tests_ok() ifadesidir.
 *
 * Uretimde CAGRILMAZ: butun dosya UART_COMM_TEST ile sinirlidir.
 *   Test derlemesi : tools/build.sh test   (-DUART_COMM_TEST)
 *   Uretim         : tools/build.sh        (bayrak yok)
 */

#ifndef INC_UART_COMM_TESTS_H_
#define INC_UART_COMM_TESTS_H_

#include <stdint.h>
#include "stm32f4xx_hal.h"

#ifdef UART_COMM_TEST

#define UART_COMM_TEST_MAX   96U

typedef enum
{
    UART_COMM_TEST_NOT_RUN = 0,  /* tabloda var, henuz kosmadi */
    UART_COMM_TEST_PASS    = 1,
    UART_COMM_TEST_FAIL    = 2,
    UART_COMM_TEST_SKIP    = 3   /* onkosulu yok (ornegin jumper): FAIL DEGIL,
                                    ama basari da sayilmaz; ayri sayilir */
} uart_comm_test_result_t;

/* Hatanin kaynagi: enjekte edilmis hatayla gecen bir test yazilim mantigini
   dogrular, donanim davranisini DEGIL. Sonuc listesinde ayirt edilebilmeli. */
typedef enum
{
    UART_COMM_TEST_SRC_SW       = 0,  /* donanim hatasi kullanmadi */
    UART_COMM_TEST_SRC_PHYSICAL = 1,  /* gercek donanim hatasi uretildi */
    UART_COMM_TEST_SRC_INJECTED = 2   /* kontrollu hata enjeksiyonu */
} uart_comm_test_source_t;

typedef struct
{
    const char              *id;        /* "RX_PENDING_TC" gibi; asla NULL */
    uint8_t                  result;    /* uart_comm_test_result_t */
    uint8_t                  source;    /* uart_comm_test_source_t */
    uint8_t                  needs_hw;  /* 1: kart/jumper gerektirir */
    uint8_t                  step;      /* 0=P0, 1=R1 ... plan adimi */
    uint32_t                 expected;  /* dusen testte beklenen */
    uint32_t                 found;     /* dusen testte bulunan */
} uart_comm_test_record_t;

/* Debugger'da Live Expressions ile okunur. */
extern uart_comm_test_record_t uart_comm_test_kayit[UART_COMM_TEST_MAX];
extern uint16_t uart_comm_test_sayisi;    /* tabloya yazilmis kayit sayisi */
extern uint16_t uart_comm_test_gecen;
extern uint16_t uart_comm_test_kalan;     /* FAIL */
extern uint16_t uart_comm_test_kosmayan;  /* NOT_RUN */
extern uint16_t uart_comm_test_atlanan;   /* SKIP */
extern uint8_t  uart_comm_test_tablo_tasti; /* 1: UART_COMM_TEST_MAX yetmedi */

/* Tabloya bir test kaydeder ve indeksini dondurur. Kosmadan once cagrilir;
   sonuc NOT_RUN olarak baslar. */
uint16_t uart_comm_test_kaydet(const char *id, uint8_t step, uint8_t needs_hw);

/* Daha once kaydedilmis bir testin sonucunu yazar. Ayni teste ikinci kez
   sonuc yazmak kaydi FAIL yapar: cift sonuc bir test hatasidir. */
void uart_comm_test_sonuc(uint16_t idx, uint8_t result, uint8_t source,
                          uint32_t expected, uint32_t found);

/* Kisayol: kosul dogruysa PASS, degilse FAIL. */
void uart_comm_test_bool(uint16_t idx, uint8_t dogru_mu);

/* Butun adimlarin testlerini sirayla kosar. Uretimde CAGRILMAZ.
   ONKOSUL: uart_rx_start ve uart_tx_init cagrilmis, alim RUNNING olmali.
   Kosucu bitiminde alimi yine RUNNING birakir. */
void uart_comm_tests_run(UART_HandleTypeDef *huart);

/* TEK kabul olcutu. "kalan == 0" bilerek yeterli sayilmaz: kosmayan ve
   atlanan testler de basariyi engeller. */
uint8_t uart_comm_tests_ok(void);

#endif /* UART_COMM_TEST */

#endif /* INC_UART_COMM_TESTS_H_ */

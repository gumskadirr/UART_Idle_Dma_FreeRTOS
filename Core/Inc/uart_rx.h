/*
 * uart_rx.h
 *
 * UART alim altyapisi: circular DMA tamponu, okuma konumu takibi ve
 * ayristiriciya besleme. HAL callback'lerini bu modul sahiplenir.
 *
 * Kullanim:
 *   uart_rx_start(&huart2);     bir kez, alimi baslatir
 *   uart_rx_service();          dongude, bekleyen veri varsa isler
 */

#ifndef INC_UART_RX_H_
#define INC_UART_RX_H_

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "parser.h"

#define UART_RX_BUF_SIZE   256U

/* Bekleyen yarim cerceve icin tamamlama zaman asimi.
   En buyuk cerceve 64 bayt; 115200 8N1'de 64 x 86,8 us ~ 5,6 ms. 50 ms bunun
   ~9 katidir: isletim sistemi kaynakli parcalanma ve gecikmelere tolerans
   birakir, buna karsilik tikanmayi sinirli tutar. */
#define UART_RX_FRAME_TIMEOUT_MS   50U

/* Basarisiz yeniden baslatma denemeleri arasindaki en kisa bekleme. Mesgul
   dongude saniyede binlerce sonucsuz deneme yapmayi engeller. */
#define UART_RX_RESTART_RETRY_MS    5U

/* Bu kadar denemeden sonra kalici hata durumuna gecilir (stats.faulted).
   5 x 5 ms = ~25 ms: gecici bir donanim hatasi icin bol, sonsuz dongu icin
   degil. */
#define UART_RX_RESTART_MAX_TRIES   5U

/* Uretici ornegi bu sure boyunca hic tutarli alinamazsa RX saglik hatasi
   sayilir ve toparlanmaya gidilir (bolum 6.2 adim 4). Tek tek basarisiz
   ornekler normaldir: DMA'nin yeniden yukleme penceresine denk gelmek
   beklenen bir durumdur. Surekli basarisizlik ise NDTR'nin ilerlemedigi
   ya da stream'in bozuldugu anlamina gelir. */
#define UART_RX_SAMPLE_FAIL_MS     20U

/* --- Alim durumu (R1) ---
   Enum adi uart_rx_PHASE_t: "uart_rx_state_t" bu baslikta ZATEN kullaniliyor
   ve joystick/sira takibi yapisinin adi. Iki ayri kavrama ayni adi vermek
   yerine durum makinesi "phase" olarak adlandirildi.

   Neden ayri bir durum alani gerekiyordu: eskiden "alim calisiyor mu"
   sorusunun tek cevabi stats.faulted idi; faulted 0 iken alimin hic
   baslamamis olmasi ile saglikli calismasi ayirt edilemiyordu. Basarisiz bir
   ilk baslatma da sessizce "sorun yok" gibi gorunuyordu.

   R1 kapsami STOPPED/STARTING/RUNNING/FAULT gecisleridir. ABORTING ve
   RETRY_WAIT tanimli ve kullanimda, fakat bloklamayan (_IT) toparlanma akisi
   R4'te tamamlanacak. */
typedef enum
{
    UART_RX_PHASE_STOPPED = 0,  /* hic baslatilmadi veya durduruldu */
    UART_RX_PHASE_STARTING,     /* HAL cagrisi yapildi, sonuc dogrulanmadi */
    UART_RX_PHASE_RUNNING,      /* alim dogrulandi: veri ve zaman asimi islenir */
    UART_RX_PHASE_ABORTING,     /* durdurma suruyor; DMA hala tamponu yaziyor
                                   OLABILIR, parser beslenmez */
    UART_RX_PHASE_RETRY_WAIT,   /* denemeler arasi bekleme */
    UART_RX_PHASE_FAULT         /* guvenli calisma kurulamadi. "Donanim bozuk"
                                   DEMEK DEGILDIR; cikis uart_rx_start ile */
} uart_rx_phase_t;

/* Alim olay sayaclari. Kesme icinde yazilip main baglaminda okundugu icin
   volatile: derleyici bu degerleri register'da onbellekleyemez. */
typedef struct
{
    volatile uint16_t rx_events;      /* toplam RxEvent sayisi */
    volatile uint16_t idle_events;    /* IDLE kaynakli */
    volatile uint16_t ht_events;      /* yarim tampon */
    volatile uint16_t tc_events;      /* tam tampon */
    volatile uint16_t last_size;      /* callback'in bildirdigi Size (MUTLAK KONUM) */
    volatile uint16_t error_events;   /* UART hata callback sayisi */
    volatile uint32_t last_error;     /* ORE/FE/NE/PE bit maskesi */

    /* Asagidakiler yalnizca main baglaminda yazilir: volatile gerekmez */
    uint16_t restarts;                /* kontrollu yeniden baslatma sayisi */
    uint16_t restart_fails;           /* yeniden baslatma basarisiz oldu */
    uint16_t frame_timeouts;          /* zaman asimiyla dusurulen aday sayisi */

    /* --- R1 ---
       start_fails: uart_rx_start cagrildi ve alim KURULAMADI. Eskiden bu
       durum hicbir yere yazilmiyordu; cagiran donus degerini yok sayarsa
       calismayan bir alim sessizce "sorunsuz" gorunurdu.
       start_rejects: uart_rx_start REDDEDILDI (zaten calisan alim veya baska
       handle). Bu bir hata DEGILDIR, sahiplik korunmustur; basarisizliktan
       ayri sayilir ki "kac kez yanlis yerden start cagrildi" gorulebilsin. */
    uint16_t start_fails;
    uint16_t start_rejects;

    /* --- R2 ---
       sample_defers: tutarsiz uretici ornegi nedeniyle ertelenen tuketim
       turu sayisi. Sifirdan buyuk olmasi hata DEGILDIR (tur sinirina denk
       gelmek normaldir); SUREKLI artmasi NDTR'nin ilerlemedigini gosterir.
       sample_fails: 20 ms boyunca gecerli ornek alinamadi -> saglik hatasi.
       overruns / discarded_bytes: tam tur kaybi (politika R3'te tamamlanir;
       sayac R2'de kuruluyor ki kayip hicbir asamada sessiz kalmasin). */
    uint16_t sample_defers;
    uint16_t sample_fails;
    uint16_t overruns;
    uint32_t discarded_bytes;

    /* UART_RX_RESTART_MAX_TRIES denemede toparlanamadi: ALIM DURDU.
       Sessizce olmek yerine gorunur olmek icin var. Cikis yolu yalnizca
       uart_rx_start() ile yeniden kurmaktir. */
    uint8_t  faulted;
} uart_rx_stats_t;

/* Cozulmus cercevelerden turetilen durum. Yalnizca main baglaminda
   guncellenir (uart_rx_service -> uart_rx_drain -> frame_parser_feed ->
   frame_received), bu yuzden volatile gerekmez.

   NOT: sira takibi (protokol katmani) ile joystick ornegi (uygulama verisi)
   simdilik ayni yapida. M8'de joystick modu ve komutlar eklenirken
   seq_tracker_t / joystick_sample_t olarak ayrilacak. */
typedef struct
{
    uint16_t last_seq;        /* en son alinan SEQUENCE */
    uint16_t next_seq;        /* siradaki beklenen SEQUENCE (TCP: rcv_nxt) */
    uint16_t seq_gaps;        /* eksik sira numarasi tespit edilen olay sayisi */
    uint8_t  seq_synced;      /* ilk cerceve referans alindi mi */
    int16_t  joy_x;
    int16_t  joy_y;
} uart_rx_state_t;

extern uart_rx_stats_t uart_rx_stats;
extern uart_rx_state_t uart_rx_state;

/* Alimi baslatir: okuma konumunu sifirlar, circular DMA'yi IDLE olaylariyla
   kurar. Loopback testinde gonderimden ONCE cagrilmali.

   SAHIPLIK (R1): butun sifirlamalardan ONCE aktif alim ve handle kontrolu
   yapilir. Calisan bir alima yapilan ikinci cagri HAL_BUSY doner ve
   ayristirici adayi, okuma konumu, sira takibi ve sayaclardan HICBIRINI
   degistirmez. Eski davranis once sifirlayip sonra HAL_BUSY aliyordu: yarim
   paket kaybolabiliyor ve DMA'nin eski verisi yeniden tuketilebiliyordu.

   Donus:
     HAL_OK     alim kuruldu ve dogrulandi (phase RUNNING)
     HAL_BUSY   REDDEDILDI; hicbir durum degismedi, eski alim surer
     HAL_ERROR  gecersiz arguman ya da kurulum basarisiz (phase FAULT)

   SOGUK KURULUM / YENIDEN BASLATMA: ilk basarili kurulumda ayristirici
   sayaclari sifirlanir (frame_parser_init). Sonraki kurulumlar yalnizca
   bekleyen adayi birakir (frame_parser_discard); hata gecmisi ve cerceve
   sayaclari KORUNUR. */
HAL_StatusTypeDef uart_rx_start(UART_HandleTypeDef *huart);

/* --- R2: mutlak uretim/tuketim konumlari (teshis ve test icin) ---
   produced: DMA'nin tampona yazdigi toplam bayt (ornekleme basarisizsa
   cagri 0 doner ve *out YAZILMAZ).
   consumed: ayristiriciya verilmis toplam bayt.
   Ikisinin farki bekleyen veri; UART_RX_BUF_SIZE'a ulasmasi tam tur
   kaybidir. Modulo konumla bu ayrim YAPILAMIYORDU. */
uint8_t  uart_rx_get_produced(uint32_t *out);
uint32_t uart_rx_get_consumed(void);

/* Uretici konumunun saf aritmetigi (bolum 6.2). Donanimdan bagimsiz
   oldugu icin sinir degerleri dogrudan sinanabilir. */
uint32_t uart_rx_producer_from(uint32_t wrap_base, uint8_t pending_tc,
                               uint32_t ndtr);

/* Alim durum makinesinin o anki durumu. stats.faulted yalnizca
   "phase == UART_RX_PHASE_FAULT" bilgisini tasir; ayrinti buradadir. */
uart_rx_phase_t uart_rx_get_phase(void);

/* Bekleyen veri bildirimi varsa isler. Dongude cagrilir. */
void uart_rx_service(void);

/* DMA tamponunu kosulsuz bosaltir (bildirimi beklemeden); loopback testinde
   gonderim dongusu icinden cagrilir. */
void uart_rx_drain(void);

/* Ayristirici sayaclarina salt okunur erisim. Dogrulanmis cerceve sayisi
   burada: uart_rx_get_parser()->frames_ok */
const frame_parser_t *uart_rx_get_parser(void);

/* --- Test kancalari ---
   UART_COMM_TEST ile sinirli (P0): uretim derlemesinde bu prototipler ve
   uygulamalari HIC derlenmez, boylece "enable her zaman 0" niyetine degil
   derleyiciye dayanilir. Test derlemesi: tools/build.sh test
   Donanimda "yeniden baslatma basarisiz oldu" durumunu deterministik uretmenin
   temiz bir yolu yok: gercek bir ORE'nin restart'i dusurup dusurmeyecegi
   garanti edilemez. Tekrar deneme ve kalici hata durumu (T6) ancak bu kanca
   ile sinanabilir. */
#ifdef UART_COMM_TEST

/* Her yeniden baslatma denemesini basarisiz saydirir. */
void uart_rx_force_restart_fail(uint8_t enable);

/* Kesmeden gelmis gibi bir hata bildirimi enjekte eder. */
void uart_rx_test_inject_error(void);

/* --- R2 kancalari --- */

/* Surekli gecersiz uretici ornegi taklidi: 3 deneme / 1 ms erteleme / 20 ms
   saglik hatasi yolunu sinamak icin. */
void uart_rx_test_force_sample_fail(uint8_t enable);

/* Tamamlanmis turlarin toplami; abort sirasindaki TC'nin sayilmadigini
   dogrulamak icin okunur. */
uint32_t uart_rx_test_get_wrap_base(void);

/* --- R1 kancalari ---
   Gercek donanimda "ReceiveToIdle_DMA basarisiz oldu" durumunu guvenilir
   bicimde uretmenin yolu yok: ORE'yi zorlamak HAL surumune gore farkli
   dallara girer. Basarisiz baslatmanin GORUNUR olmasi (RX-4) ancak bu
   kancayla sinanabilir. */

/* Bir sonraki HAL alim baslatmasini basarisiz saydirir (tek atimlik). */
void uart_rx_force_start_fail(uint8_t enable);

/* HAL baslatma cagrisi DONMEDEN hata callback'i gelmis gibi davranir.
   TX-5'in RX karsiligi: bu HAL'de bazi dallar callback'i senkron cagirir,
   bu yuzden "HAL_OK dondu" tek basina saglikli alim kaniti degildir. */
void uart_rx_test_sync_error_on_start(uint8_t enable);

#endif /* UART_COMM_TEST */

#endif /* INC_UART_RX_H_ */

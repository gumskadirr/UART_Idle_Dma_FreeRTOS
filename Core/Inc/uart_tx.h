/*
 * uart_tx.h
 *
 * Tek aktif gonderim yoneten TX modulu. Yol haritasi Asama 2.
 *
 * Sorumluluk siniri:
 *   frame.c    cerceveyi OLUSTURUR (baytlari uretir)
 *   uart_tx.c  cerceveyi GONDERIR  (baytlari hatta koyar)
 * Modul protokolu bilmez: ne TYPE'a ne SEQUENCE'a bakar, sadece bayt dizisi
 * tasir. Bu yuzden ileride log, ham komut veya yanit da ayni yoldan gider.
 *
 * KOPYA SEMANTIGI (ismin icindeki _copy budur):
 *   uart_tx_send_copy() dondugunde cagiranin tamponu SERBESTTIR. Modul veriyi
 *   kendi tamponuna kopyalar; DMA o kopyayi okur. Isaretci saklasaydik
 *   cagiranin "tamamlanana kadar tamponu canli ve degismez tut" sozlesmesine
 *   uymasi gerekirdi ve bitisi bilmesinin kolay bir yolu yok. 64 baytta kopya
 *   bedava, sozlesme ise cok daha guvenli.
 *
 * IKI AYRI SORU (modulun butun tasariminin dayandigi ayrim):
 *   "Cerceve hatta cikti mi?"       -> AKTARIMIN SONUCU
 *   "Yeni cerceve kabul edebilir?"  -> MODULUN HAZIR OLMASI
 *   Bunlar ayni sey degil. Basarisiz bir aktarim, donanim temiz
 *   durdurulabildigi surece modulu kullanilamaz hale GETIRMEZ. Durum makinesi
 *   bu yuzden dort durumlu (bkz. uart_tx_state_t).
 *
 * SAHIPLIK (kilit gerekmemesinin sebebi: her degiskenin tek yazari var):
 *   s_buf        -> uart_tx_send_copy yazar, DMA okur
 *   s_state      -> YALNIZCA ana baglam yazar
 *   s_tx_done    -> YALNIZCA kesme yazar; ana baglam okur ve sifirlar
 *   s_tx_error   -> YALNIZCA kesme yazar; ana baglam okur ve sifirlar
 *   s_abort_done -> YALNIZCA kesme yazar; ana baglam okur ve sifirlar
 *   Bu uc bayrak TEK anlik goruntude birlikte okunur (bkz. uart_tx_service):
 *   ayri ayri okunursa "hata ile tamamlanma ayni turda geldi" durumu tutarsiz
 *   gorunebilir.
 *
 * Kullanim:
 *   uart_tx_init(&huart2);             bir kez
 *   uart_tx_send_copy(buf, n);         gonderim istegi, BEKLEMEDEN doner
 *   uart_tx_service();                 dongude KOSULSUZ, her turda
 */

#ifndef INC_UART_TX_H_
#define INC_UART_TX_H_

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "frame.h"

/* Aktif TX tamponu. Tek kaynaktan gelir (bkz. MIMARI.md 5.12): protokolun en
   buyuk cercevesi ne ise tampon odur. CCM bellekte OLMAMALI, DMA oraya
   erisemez (5.13) - dosya kapsaminda static dizi kullanildigi surece sorun
   yok. */
#define UART_TX_BUF_SIZE   FRAME_MAX_SIZE

/* AKTARIM suresi siniri. Sorusu: "cerceve hatta cikti mi?"
   115200 8N1'de bir bayt 10 bit tasir (1 start + 8 veri + 1 stop):
     10 / 115200 = 86,8 us/bayt;  64 bayt x 86,8 us = 5,56 ms
   20 ms bunun ~3,6 kati: yavas ama calisan bir aktarimi kesmez, buna karsilik
   "hic ilerlemiyor" durumunu 20 ms icinde gorunur kilar.

   TOPLAM suredir: "NDTR ilerledi, sureyi uzatayim" YAPILMAZ. NDTR ilerlemesi
   fiziksel teslim degildir ve uzatilabilir bir sinir, bayt bayt ilerleyen bir
   arizada sonsuza kadar uzar. */
#define UART_TX_TIMEOUT_MS         20U

/* TOPARLANMA suresi siniri. Sorusu: "donanim gercekten durdu mu?"
   AKTARIM suresinden AYRI bir sabittir, cunku ikisi farkli seye baglidir:
   UART_TX_TIMEOUT_MS baud'a baglidir ve 460800'e cikildiginda dusmelidir;
   iptalin tamamlanmasi baud'dan bagimsizdir ve ayni kalir. Tek sabit olsaydi
   baud degisiminde ilgisiz bir sure de degismis olurdu.

   Plan bu siniri "servis tabanli" diye niteliyor: sureyi uart_tx_service olcer
   ve her turda guvenli durus dogrulamasini TEKRAR dener. Tek seferlik bir
   kontrol degil, sure dolana kadar yoklama. */
#define UART_TX_ABORT_TIMEOUT_MS   20U

/* Modulun durumu.
   DONE diye bir durum YOK: "DMA bitti ama muhasebe yapilmadi" bir durum degil,
   bir BILDIRIMDIR. Bildirimi ayri bir bayrak tasir (uart_rx'teki s_rx_pending
   ile ayni desen), boylece iki modul ayni mantikla okunur.

   GECIS HARITASI (tam liste; bunlarin disinda gecis yok):
     SENDING  --TxCplt (normal)---------------------------->  IDLE
     SENDING  --hata bildirimi / UART_TX_TIMEOUT_MS asimi-->  ABORTING
     ABORTING --guvenli durus + HAL hazir DOGRULANDI------->  IDLE
     ABORTING --UART_TX_ABORT_TIMEOUT_MS doldu------------->  FAULT
     FAULT    --uart_tx_init (durus dogrulanarak)---------->  IDLE

   ABORTING -> IDLE gecisi kaybedilen cerceveyi AFFETMEZ: hata nedeni
   uart_tx_stats.last_fail'de kalir, frames_sent ARTMAZ ve cerceve otomatik
   TEKRAR GONDERILMEZ. Yalnizca modul yeniden kullanilabilir hale gelir. */
typedef enum
{
    UART_TX_IDLE = 0,     /* tampon serbest VE HAL TX yolu yeni gonderime
                             hazir. Iki ayri garanti; ikisi de dogrulanmistir */
    UART_TX_SENDING,      /* DMA tamponu okuyor, DOKUNULMAZ. Teslim bekleniyor */
    UART_TX_ABORTING,     /* DMA tamponu HALA okuyor olabilir, DOKUNULMAZ.
                             Teslim DEGIL, DURUS bekleniyor. send_copy BUSY */
    UART_TX_FAULT         /* guvenli durus dogrulanamadi: donanimin s_buf'i
                             artik okumadigi KANITLANAMADI. send_copy
                             NOT_READY, cikis yalnizca uart_tx_init. Tek
                             basina fiziksel donanim arizasini KANITLAMAZ */
} uart_tx_state_t;

/* Son basarisiz aktarimin NEDENI. Sayac degil, DURUM BILGISI: sayaclar "kac
   kez oldu" sorusunu cevaplar ama sirayi kaybeder. transfer_errors=3 ve
   transfer_timeouts=1 goren biri SON hatanin hangisi oldugunu bilemez; teshis
   icin gereken genelde tam olarak budur.

   Neden ham HAL ErrorCode yetmiyor: zaman asiminda HAL hicbir hata
   bildirmemistir, ErrorCode temizdir. Yani "sebep yok" ile "sebep zaman
   asimi" ayni degere duserdi. */
typedef enum
{
    UART_TX_FAIL_NONE = 0,   /* en son aktarim basariyla tamamlandi */
    UART_TX_FAIL_TRANSFER,   /* HAL hata bildirimi geldi (DMA / transfer) */
    UART_TX_FAIL_TIMEOUT,    /* UART_TX_TIMEOUT_MS doldu, TxCplt gelmedi */
    UART_TX_FAIL_ABORT       /* iptal edilemedi / durus dogrulanamadi -> FAULT */
} uart_tx_fail_t;

/* uart_tx_send_copy sonucu. Cagiranin ne yapacagi her durumda farkli oldugu
   icin tek bir "hata" degeri yeterli degil. */
typedef enum
{
    UART_TX_OK = 0,       /* kabul edildi: kopyalandi ve DMA baslatildi */
    UART_TX_BUSY,         /* tampon mesgul, cagiran SONRA denesin. Iki sebep:
                             SENDING  -> onceki cerceve hala hatta
                             ABORTING -> basarisiz aktarim iptal ediliyor
                             Ikisinde de modul cagiranin cercevesini
                             KENDILIGINDEN tekrar gondermez; tekrar denemek
                             cagiranin isidir */
    UART_TX_INVALID,      /* data NULL, len 0 veya len > UART_TX_BUF_SIZE */
    UART_TX_NOT_READY     /* uart_tx_init cagrilmamis ya da durum FAULT */
} uart_tx_status_t;

/* Sayaclar.
   BOLUM KURALI: alani kim yaziyor?
     kesme yaziyor      -> volatile, ust bolum (derleyici register'da tutamaz)
     yalniz ana baglam  -> volatile DEGIL, alt bolum

   Hata gecmisi ABORTING -> IDLE gecisinde SILINMEZ. Silinseydi otomatik
   toparlanma hatayi gorunmez kilardi ve bu, hatada kilitlenen eski tasarimdan
   daha kotu olurdu: sahada "hic hata yok" gorunurken cerceveler kaybolurdu. */
typedef struct
{
    /* --- Kesme yazar --- */
    volatile uint16_t tx_complete_events;    /* HAL_UART_TxCpltCallback sayisi */
    volatile uint16_t tx_error_events;       /* uart_tx_on_error cagri sayisi */
    volatile uint16_t abort_complete_events; /* uart_tx_on_abort_complete sayisi */

    /* --- Yalnizca ana baglam yazar --- */
    uint16_t frames_sent;                  /* tamamlanmis gonderim sayisi */
    uint32_t bytes_sent;                   /* tamamlanmis toplam bayt */
    uint16_t rejected_busy;                /* BUSY ile geri cevrilen istek */
    uint16_t rejected_invalid;             /* INVALID ile geri cevrilen istek */
    uint16_t start_fails;                  /* HAL_UART_Transmit_DMA dustu */

    /* Dusen aktarimin nedenleri AYRI sayilir: "hat/donanim nasil" sorusu ile
       "toparlanma kodum calisiyor mu" sorusu ayri teshislerdir.
       transfer_errors 100 iken recovery_fails 0 ise tasarim isini yapiyordur;
       recovery_fails artiyorsa toparlanma yolunun KENDISI cokuyordur. Tek
       sayacta toplanirsa bu ikisi ayirt edilemez. */
    uint16_t transfer_errors;              /* hata bildirimiyle dusen aktarim */
    uint16_t transfer_timeouts;            /* UART_TX_TIMEOUT_MS doldu */
    uint16_t abort_start_fails;            /* HAL_UART_AbortTransmit_IT dustu */
    uint16_t recovery_fails;               /* durus dogrulanamadi -> FAULT */

    /* ABORTING veya FAULT durumunda gelen TxCplt. Durumu DEGISTIRMEZ, ama
       gorunur olmasi gerekir: "iptal ettik ama cerceve aslinda gitmisti"
       bilgisi baska hicbir yerden okunamaz. */
    uint16_t late_completions;

    uart_tx_fail_t last_fail;              /* son basarisiz aktarimin nedeni */
    uint32_t       last_hal_error;         /* o anin ham HAL ErrorCode'u */
} uart_tx_stats_t;

extern uart_tx_stats_t uart_tx_stats;

/* Modulu baglar. Adi uart_rx_start'in aksine "init": RX'te cagri gercekten
   alimi BASLATIR (DMA doner), TX'te ise gonderilecek bir sey olmadigi icin
   yalnizca handle baglanir ve durum sifirlanir.

   FAULT'tan cikisin tek yolu budur, ama SIRADAN hatalardan cikis icin
   GEREKMEZ: aktarim hatasi ve zaman asimi, iptal basarili oldugu surece
   uart_tx_service tarafindan kendiliginden IDLE'a cozulur. init yalnizca
   "guvenli durus dogrulanamadi" durumunu temizler.

   Bu yuzden init FAULT'u YAZILIM ALANLARINI SIFIRLAYARAK asamaz: FAULT
   donanim hakkinda bir iddiadir (stream hala s_buf'i okuyor olabilir) ve o
   iddia ancak donanim kaydlari yeniden okunarak curutulebilir. Durus
   dogrulanamazsa init FAULT'ta kalmali ve HAL_ERROR donmeli.

   Sayaclari SIFIRLAMA: gecmis hata bilgisi silinmemeli (uart_rx
   toparlanmasindaki frame_parser_discard / _init ayrimiyla ayni gerekce). */
HAL_StatusTypeDef uart_tx_init(UART_HandleTypeDef *huart);

/* Cerceveyi modulun tamponuna KOPYALAR ve DMA gonderimini baslatir.
   BEKLEMEZ: tampon mesgulse UART_TX_BUSY doner, dongude tekrar denemek
   cagiranin isidir. Donus UART_TX_OK ise cagiranin kaynak tamponu serbesttir.

   "Kabul edildi" hattan gittigi anlamina GELMEZ, yalnizca kopyalandigi ve
   DMA'nin kuruldugu anlamina gelir.

   Yalnizca IDLE durumunda kabul eder. Kabul ederken bekleyen ESKI bildirimleri
   temizlemek zorundadir: iptal edilmis bir aktarimdan arta kalan TxCplt
   bayragi, temizlenmezse yeni cercevenin tamamlanmasi sayilir ve henuz hatta
   olan cerceve icin tampon serbest birakilir. */
uart_tx_status_t uart_tx_send_copy(const uint8_t *data, uint8_t len);

/* Durum makinesini yurutur. Dongude KOSULSUZ cagrilir.

   Muhasebe neden kesmede degil burada: kesme baglaminda yapilacak is en kisa
   olmali ve Asama 4'te siradaki cerceveyi kuyruktan almak ortak taskin isi
   olacak. Kesme yalnizca "oldu" der.

   Bildirim olmasa bile IS YAPAR; "gonderim yok, atlayayim" optimizasyonu
   zaman asimini olcecek kimseyi birakmaz. Sorumluluklari:
     - Bekleyen tamamlanma / hata / iptal bildirimlerini TEK anlik goruntude
       alir ve isler. Hata ile tamamlanma ayni turda gelirse GUVENLI olan
       hata yolunu secer.
     - SENDING'de UART_TX_TIMEOUT_MS suresini olcer; dolarsa iptali baslatir.
     - ABORTING'de UART_TX_ABORT_TIMEOUT_MS butcesi icinde guvenli durusu her
       turda yeniden dogrulamayi dener; basarirsa IDLE'a doner, butce dolarsa
       FAULT'a gecer.
     - Tamamlanmayi YALNIZCA SENDING durumunda basarili gonderim sayar;
       ABORTING/FAULT'ta geleni late_completions'a yazar, durumu degistirmez. */
void uart_tx_service(void);

/* --- Kesme bildirimleri ---
   Bu ikisi modulun DISINDAN cagrilir, cunku HAL_UART_ErrorCallback butun
   UART'lar icin ORTAK ve programda tek tanimi olabilir; su an uart_rx.c
   sahipleniyor. Iki dosyada birden tanimlamak linker'da coklu tanim hatasi
   verir. Cozum: callback tek yerde kalir, icinden hem RX hem TX tarafina
   BILDIRIR. Bu yuzden TX'in bildirim kapisi public.

   Ikisi de KESME baglamindan cagrilir: yalnizca bayrak kaldirir ve sayac
   artirir. HAL cagrisi, durum gecisi, muhasebe YOK. Baska bir UART'in
   bildirimi ise hicbir sey yapmazlar. */

/* TX aktarimi sirasinda hata bildirimi. 'error' AYRI parametre, cunku cagri
   aninda huart->ErrorCode HAL tarafindan zaten temizlenmis olabilir; cagiran
   onu bir kez okuyup degeri tasir. Normal RX hatasi (FE/NE/ORE/PE) buraya
   iletilMEZ: alim gurultusu gonderimi dusurmez. */
void uart_tx_on_error(UART_HandleTypeDef *huart, uint32_t error);

/* Iptalin tamamlandigi bildirimi. BILDIRIMDIR, KANIT DEGIL: bu HAL surumunde
   HAL_UART_AbortTransmit_IT, HAL_OK donerken DMA iptalinde hata olusursa
   tamamlanma callback'ini DOGRUDAN (fonksiyon donmeden) cagirabilir. Yani ne
   donus degeri ne callback'in gelmesi donanimin durdugunu gosterir; durus
   DMA ve USART kaydlarindan AYRICA dogrulanir. */
void uart_tx_on_abort_complete(UART_HandleTypeDef *huart);

/* Gonderim yapilabilir mi. Cagiranin send_copy'yi denemeden once bakmasi
   zorunlu degil; send_copy zaten BUSY/NOT_READY doner. */
uart_tx_state_t uart_tx_get_state(void);

#endif /* INC_UART_TX_H_ */

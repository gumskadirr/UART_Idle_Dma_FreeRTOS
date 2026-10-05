/*
 * uart_rx.c
 *
 * Dairesel DMA tamponunu, ayristiricinin bekledigi ARDISIK bayt
 * araliklarina cevirir. parser.c dairesel tampon nedir bilmez.
 *
 * Sahiplik:
 *   s_dma_buf   -> DMA yazar, bu modul okur
 *   s_read_pos  -> yalnizca uart_rx_drain yazar, DMA hic bakmaz
 * Iki taraf farkli degiskenlere sahip oldugu icin kilit gerekmez.
 *
 * Not: klasik ring buffer literaturu head/tail der, ama bu iki terimin
 * anlami kaynaga gore ters cevrilir (Linux kfifo bu yuzden in/out kullanir).
 * read_pos/write_pos belirsizlik birakmadigi icin tercih edildi. Ayrica
 * write_pos burada saklanan bir durum degil, NDTR'den turetilen anlik deger.
 */
#include <stddef.h>
#include "uart_rx.h"
#include "frame.h"

/* --- Modul ici durum --- */
static UART_HandleTypeDef *s_huart;                  /* uart_rx_start baglar */
static uint8_t             s_dma_buf[UART_RX_BUF_SIZE];
static uint16_t            s_read_pos;               /* okunmamis ilk bayt */
static frame_parser_t      s_parser;                 /* yarim cerceve durumu */
static volatile uint8_t    s_rx_pending;             /* kesme set eder */
static volatile uint8_t    s_rx_error;               /* kesme set eder */
static uint32_t            s_last_rx_tick;           /* son bayt geldigi an */

/* Toparlanma borcu. s_rx_error yalnizca BILDIRIMDIR; borc burada durur ve
   ancak alim gercekten geri geldiginde (ya da kalici hataya dusuldugunde)
   kapanir. Yalnizca main baglaminda yazilir: volatile gerekmez. */
static uint8_t             s_recover_pending;
static uint8_t             s_restart_tries;          /* ust uste basarisizlik */
static uint32_t            s_restart_tick;           /* son deneme ani */
static uint8_t             s_force_restart_fail;     /* yalnizca test kancasi */

uart_rx_stats_t uart_rx_stats;
uart_rx_state_t uart_rx_state;

static void frame_received(const frame_info_t *info, void *user_data);
static void uart_rx_recover(void);
static void restart_failed(void);
static void check_frame_timeout(void);
static uint16_t dma_write_pos(void);


HAL_StatusTypeDef uart_rx_start(UART_HandleTypeDef *huart)
{
    if ((huart == NULL) || (huart->hdmarx == NULL))
    {
        return HAL_ERROR;
    }

    s_huart           = huart;
    s_read_pos        = 0U;
    s_rx_pending      = 0U;
    s_rx_error        = 0U;
    s_last_rx_tick    = HAL_GetTick();
    s_recover_pending = 0U;
    s_restart_tries   = 0U;

    /* Kalici hata durumundan tek cikis yolu burasi */
    uart_rx_stats.faulted = 0U;

    frame_parser_init(&s_parser);

    return HAL_UARTEx_ReceiveToIdle_DMA(huart, s_dma_buf,
                                        (uint16_t)sizeof(s_dma_buf));
}


void uart_rx_service(void)
{
    /* 1) Hata toparlamasi once: alim durmussa tuketmenin anlami yok.
       Kesmenin bildirimi burada BORCA cevrilir. Bayragi temizleyip tek
       deneme yapip gecmek olumcul olurdu: deneme basarisiz olursa DMAR ve
       IDLEIE kapali kalir, bir daha hicbir callback olusmaz ve bayragi set
       edecek kimse kalmaz. Borc s_recover_pending'de durur. */
    if (s_rx_error != 0U)
    {
        s_rx_error        = 0U;
        s_recover_pending = 1U;
        s_restart_tries   = 0U;          /* yeni hata: sayac bastan */
    }

    if (s_recover_pending != 0U)
    {
        uart_rx_recover();
    }

    /* 2) Bekleyen veriyi tuket */
    if (s_rx_pending != 0U)
    {
        s_rx_pending = 0U;
        uart_rx_drain();
    }

    /* 3) Yarim cerceve cok uzun suredir bekliyorsa dusur */
    check_frame_timeout();
}


/* UART hatasindan kontrollu toparlanma. Kesme icinde DEGIL burada yapilir:
   yeniden baslatma sahibi tuketici baglamdir (plan bolum 11). */
static void uart_rx_recover(void)
{
    if (s_huart == NULL)
    {
        s_recover_pending = 0U;
        return;
    }

    /* Basarisiz denemeler arasinda bekle. while(1) icinde bu kontrol olmasa
       saniyede binlerce sonucsuz abort/restart cifti calisirdi. */
    if ((s_restart_tries > 0U) &&
        ((HAL_GetTick() - s_restart_tick) < UART_RX_RESTART_RETRY_MS))
    {
        return;
    }

    /* TEST KANCASI: donanima hic dokunmadan basarisiz deneme uretir.
       BUSY_RX kontrolunden ONCE olmali, cunku testte alim calismaya devam
       ediyor; amac tekrar/kalici hata mantigini sinamak. */
    if (s_force_restart_fail != 0U)
    {
        restart_failed();
        return;
    }

    if (s_huart->RxState == HAL_UART_STATE_BUSY_RX)
    {
        /* HAL alimi surduruyor (ornegin tek bir gurultu hatasi: FE/NE/PE'de
           HAL alimi kesmez, yalnizca ErrorCallback cagirir); mudahale etmek
           calisan bir alimi bozar. Yapacak is yok, borc kapanir. */
        s_recover_pending = 0U;
        s_restart_tries   = 0U;
        return;
    }

    /* YALNIZCA RX iptal edilir. HAL_UART_Abort kullanilsaydi surmekte olan
       bir TX de iptal olurdu; plan bolum 11 bunu acikca yasakliyor.
       Donus kontrol edilir: DMA abort'unu beklerken HAL_TIMEOUT donebilir,
       o durumda periferik belirsiz haldedir ve devam etmek yanlis olur. */
    if (HAL_UART_AbortReceive(s_huart) != HAL_OK)
    {
        restart_failed();
        return;
    }

    /* Bekleyen hata bayraklari TEMIZLENMELI, yoksa tekrar denemek hicbir sey
       degistirmez: AbortReceive bu bayraklara dokunmaz ve ORE duruyorsa
       ReceiveToIdle_DMA, EIE'yi acar acmaz hata kesmesi dogurur; HAL alimi
       iptal eder ve HAL_ERROR doner (HAL kaynagindaki "errors already
       pending when reception is started" notu). Sebebi temizlemeden yapilan
       tekrar, ayni sonucu tekrar uretmektir.
       F4'te PE/FE/NE/ORE/IDLE tek yolla dusurulur: SR oku, DR oku. Bes
       makronun hepsi ayni seyi yapar, bu yuzden bir cagri yeter. */
    __HAL_UART_CLEAR_OREFLAG(s_huart);
    s_huart->ErrorCode = HAL_UART_ERROR_NONE;

    /* Indeksler ve yarim cerceve durumu tutarli sekilde sifirlanir.
       frame_parser_init DEGIL frame_parser_discard: sayaclar korunmali,
       yoksa hata gecmisi her toparlanmada silinir. */
    frame_parser_discard(&s_parser);
    s_read_pos     = 0U;
    s_last_rx_tick = HAL_GetTick();

    if (HAL_UARTEx_ReceiveToIdle_DMA(s_huart, s_dma_buf,
                                     (uint16_t)sizeof(s_dma_buf)) == HAL_OK)
    {
        uart_rx_stats.restarts++;
        s_recover_pending = 0U;          /* borc ancak burada kapanir */
        s_restart_tries   = 0U;
    }
    else
    {
        restart_failed();
    }
}


/* Basarisiz deneme muhasebesi. Sinir asilirsa alim GERCEKTEN durmustur;
   bunu gizlemek yerine gorunur kilip borcu kapatiyoruz, cunku daha fazla
   denemek anlamsiz. */
static void restart_failed(void)
{
    uart_rx_stats.restart_fails++;
    s_restart_tick = HAL_GetTick();
    s_restart_tries++;

    if (s_restart_tries >= UART_RX_RESTART_MAX_TRIES)
    {
        uart_rx_stats.faulted = 1U;
        s_recover_pending     = 0U;
    }
}


/* Bekleyen aday, UART_RX_FRAME_TIMEOUT_MS boyunca TAMPON ILERLEMEDEN
   duruyorsa dusurulur. Bozuk bir LENGTH alani arkasindaki gecerli cerceveyi
   sonsuza kadar bekletmesin. */
static void check_frame_timeout(void)
{
    if ((s_huart == NULL) || (s_parser.len == 0U))
    {
        return;                      /* bekleyen aday yok */
    }

    /* uint32_t cikarma sarimda da dogru sonuc verir */
    if ((HAL_GetTick() - s_last_rx_tick) < UART_RX_FRAME_TIMEOUT_MS)
    {
        return;
    }

    /* Sure doldu, ama karar VERMEDEN once tamponun ilerleyip ilerlemedigine
       bakilir. Kosul "bildirim gelmedi" DEGIL "tampon ilerlemedi"; ikisi ayni
       sey degil, cunku bir yayin (burst) surerken hicbir bildirim olusmaz:
       IDLE son bayttan ~87 us sonra gelir, HT/TC yalnizca 128./256. baytta
       tetiklenir. 57 baytlik bir devam yayini 115200'de 4,95 ms surer ve bu
       sure boyunca DMA yaziyor ama s_rx_pending sifirdir. Yalnizca zaman
       damgasina baksak akmakta olan gecerli bir cerceveden bayt atardik.
       Ilerlemis: cerceve hala geliyor -> zaman asimi yok, tuket.
       uart_rx_drain zaman damgasini da yeniler. */
    if (dma_write_pos() != s_read_pos)
    {
        uart_rx_drain();
        return;
    }

    frame_parser_timeout(&s_parser, frame_received, NULL);
    uart_rx_stats.frame_timeouts++;
    s_last_rx_tick = HAL_GetTick();
}


/* DMA'nin yazacagi SONRAKI konum. Saklanan bir durum degil, NDTR'den
   turetilen anlik deger; bu yuzden her cagrida yeniden okunur.
   Cagiran s_huart'in NULL olmadigini garanti etmelidir. */
static uint16_t dma_write_pos(void)
{
    const uint16_t buf_size = (uint16_t)sizeof(s_dma_buf);

    /* NDTR kalan transfer sayisini tutar ve her baytta azalir:
          yazilan bayt sayisi = buf_size - NDTR
       Modulo tek bir uc durum icin gerekli: DMA son bayti yazip NDTR'yi
       henuz yeniden yuklemediginde 0 okunur, buf_size - 0 = buf_size cikar
       ve bu gecersiz bir indekstir. Modulo onu 0'a cevirir. */
    return (uint16_t)((buf_size -
            (uint16_t)__HAL_DMA_GET_COUNTER(s_huart->hdmarx)) % buf_size);
}


void uart_rx_drain(void)
{
    const uint16_t buf_size = (uint16_t)sizeof(s_dma_buf);
    uint16_t write_pos;
    uint16_t chunk_len;

    if (s_huart == NULL)
    {
        return;
    }

    for (;;)
    {
        write_pos = dma_write_pos();

        if (write_pos == s_read_pos)
        {
            /* Bekleyen veri yok.
               DIKKAT: tam bir tur uzerine yazilmis olsa da konumlar boyle
               gorunur. Modulo aritmetigi tasmayi tespit edemez; koruma
               zamaninda tuketmektir (256 bayt / 11520 bayt/s ~ 22 ms). */
            break;
        }

        /* Yeni bayt geldi: zaman asimi sayaci bastan baslar */
        s_last_rx_tick = HAL_GetTick();

        if (write_pos > s_read_pos)
        {
            /* Sarim yok: tek ardisik aralik */
            chunk_len = (uint16_t)(write_pos - s_read_pos);
            frame_parser_feed(&s_parser, &s_dma_buf[s_read_pos], chunk_len,
                              frame_received, NULL);
            s_read_pos = write_pos;
        }
        else
        {
            /* Sarim var: once tampon SONUNA kadar besle. Kalani dongunun
               sonraki turu artik "sarim yok" durumu olarak halleder. */
            chunk_len = (uint16_t)(buf_size - s_read_pos);
            frame_parser_feed(&s_parser, &s_dma_buf[s_read_pos], chunk_len,
                              frame_received, NULL);
            s_read_pos = 0U;
        }
    }
}


const frame_parser_t *uart_rx_get_parser(void)
{
    return &s_parser;
}


/* --- Test kancalari (bkz. uart_rx.h) --- */

void uart_rx_force_restart_fail(uint8_t enable)
{
    s_force_restart_fail = enable;
}


void uart_rx_test_inject_error(void)
{
    s_rx_error = 1U;
}


/* Dogrulanmis bir cerceve cozuldugunde frame_parser_feed tarafindan cagrilir.
   main baglaminda calisir: uart_rx_service -> uart_rx_drain ->
   frame_parser_feed -> buraya.
   info->payload YALNIZCA bu cagri suresince gecerli; saklanacaksa
   kopyalanmali.

   Cerceve sayisi burada tutulmuyor: s_parser.frames_ok zaten ayni bilgiyi
   veriyor, iki yerde tutmak tutarsizlik riski demek. */
static void frame_received(const frame_info_t *info, void *user_data)
{
    (void)user_data;

    if (uart_rx_state.seq_synced == 0U)
    {
        /* Ilk cerceve: gonderenin hangi degerden basladigini bilemeyiz.
           Karsilastirma yapmadan referans aliyoruz (RTP alicisi da boyle
           yapar: ilk pakette sira numarasina senkronize olur). */
        uart_rx_state.seq_synced = 1U;
    }
    else if (info->seq != uart_rx_state.next_seq)
    {
        uart_rx_state.seq_gaps++;
    }
    else
    {
        /* Beklenen sira geldi */
    }

    uart_rx_state.last_seq = info->seq;

    /* Beklentiyi GELEN degerden turetiyoruz. "next_seq++" yazsaydik tek bir
       kayiptan sonra kalici olarak bir geri kalir ve sonraki her cerceveyi
       kayip sayardik. (uint16_t) cast'i 65535 -> 0 sarimini halleder. */
    uart_rx_state.next_seq = (uint16_t)(info->seq + 1U);

    if ((info->type == FRAME_TYPE_JOYSTICK) && (info->payload_len == 4U))
    {
        uart_rx_state.joy_x = (int16_t)((uint16_t)info->payload[0] |
                                       ((uint16_t)info->payload[1] << 8));
        uart_rx_state.joy_y = (int16_t)((uint16_t)info->payload[2] |
                                       ((uint16_t)info->payload[3] << 8));
    }
}


/* --- HAL callback'leri ---
   Bu iki fonksiyon HAL'de __weak tanimli ve butun UART'lar icin ortaktir;
   bu yuzden hangi UART oldugu kontrol edilir. */

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if ((s_huart != NULL) && (huart->Instance == s_huart->Instance))
    {
        uart_rx_stats.rx_events++;
        uart_rx_stats.last_size = Size;

        /* Size mutlak konum bildirir, "kac yeni bayt" degil. Tuketimde
           KULLANILMAZ; konumu uart_rx_drain kendisi NDTR'den hesaplar. */
        s_rx_pending = 1U;

        switch (HAL_UARTEx_GetRxEventType(huart))
        {
            case HAL_UART_RXEVENT_IDLE:
                uart_rx_stats.idle_events++;
                break;

            case HAL_UART_RXEVENT_HT:
                uart_rx_stats.ht_events++;
                break;

            case HAL_UART_RXEVENT_TC:
                uart_rx_stats.tc_events++;
                break;

            default:
                break;
        }
    }
}


/* --- ADIM 1.2 TASLAGI: ortak hata callback'i (KARAR: RX'te kalir) ---

   KARAR: HAL_UART_ErrorCallback bu dosyada kalacak. Alternatif main.c'ye
   tasimakti; onun tek avantaji uart_rx.c'nin TX'i hic tanimamasiydi. Bedeli:
   bu dosya artik uart_tx.h'i include edecek, yani RX modulu TX modulunu
   TANIYACAK. Bagimlilik tek yonlu (uart_tx, uart_rx'i tanimiyor) ve dongu
   olusmuyor; ama bu bedeli bilerek odedigimizi not ediyoruz.

   COZULECEK UC PROBLEM:

   P1) ErrorCode TEK KEZ okunmali.
       Su anki kod huart->ErrorCode'u dogrudan stats'a yaziyor. Iki tuketici
       olunca bu yetmez: her biri kendi okumasini yaparsa ikisi FARKLI deger
       gorebilir. Somut senaryo: TX tarafi bildirimi alir almaz bir gun
       HAL_UART_AbortTransmit_IT cagirmaya baslarsa, o cagri icinde HAL
       ErrorCode'u HAL_UART_ERROR_NONE yapar; ardindan RX tarafi okudugunda
       hata 0 gorunur ve toparlanma HIC tetiklenmez.
       Cozum: fonksiyonun EN BASINDA yerel bir degiskene al ve herkese o
       degeri DEGER OLARAK ver.

   P2) Hangi hata biti kimin?
       RX kaynakli bitler  : PE, NE, FE, ORE -> alim yolundan gelir
       Yon belirtmeyen bit : HAL_UART_ERROR_DMA
       DMA biti yonu SOYLEMEZ: HAL'in UART_DMAError'u hem hdmatx hem hdmarx
       icin ayni bayragi kaldirir.
       "huart->gState == HAL_UART_STATE_BUSY_TX'e baksak olmaz mi?" OLMAZ:
       UART_DMAError, ErrorCallback'i cagirmadan ONCE UART_EndTxTransfer ile
       gState'i READY'ye cekiyor. Yani callback'e girdigimizde HAL'in TX
       durumu zaten silinmis olabilir.
       Sonuc: "aktif TX var miydi" sorusunu yalnizca TX MODULU cevaplayabilir,
       cunku s_state'i HAL degil o yazar. Bu yuzden yon KARARI BURADA
       VERILMEZ; DMA biti varsa TX'e BILDIRILIR, kararini o verir.

   P3) Normal RX gurultusu TX'i dusurmemeli.
       FE/NE/ORE/PE tek basina geldiginde TX'e hicbir sey bildirilmez. Plan
       bunu acikca yasakliyor: alim gurultusu gonderimi iptal ettirmez.

   YENI YAPI (sozde kod):

       void HAL_UART_ErrorCallback(huart)
       {
           if (baska UART)  return;

           error = huart->ErrorCode;         <-- P1: TEK okuma

           if (error & HAL_UART_ERROR_DMA)   <-- P2/P3: yalnizca DMA biti
               uart_tx_on_error(huart, error);

           uart_rx_stats.error_events++;     <-- mevcut davranis, degismez
           uart_rx_stats.last_error = error;
           s_rx_error = 1U;
       }

   SIRA NEDEN ONEMSIZ: iki tarafin bildirim fonksiyonu da SADECE bayrak
   kaldirir, HAL cagirmaz. Bu yuzden TX'i once ya da sonra cagirmak farketmez.
   Biri HAL cagirmaya baslarsa sira ANINDA onemli hale gelir ve P1'deki
   senaryo gerceklesir: sozlesmenin "yalnizca bayrak" maddesinin gercek
   bedeli budur.
*/
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if ((s_huart != NULL) && (huart->Instance == s_huart->Instance))
    {
        uart_rx_stats.error_events++;
        uart_rx_stats.last_error = huart->ErrorCode;

        /* Toparlanma burada YAPILMAZ: kesme baglaminda HAL'i yeniden
           baslatmak yerine tuketici baglamina bildirilir. */
        s_rx_error = 1U;
    }
}

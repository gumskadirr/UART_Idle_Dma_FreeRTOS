/*
 * uart_rx.c
 *
 * Dairesel DMA tamponunu, ayristiricinin bekledigi ARDISIK bayt
 * araliklarina cevirir. parser.c dairesel tampon nedir bilmez.
 *
 * Sahiplik:
 *   s_dma_buf   -> DMA yazar, bu modul okur
 *   s_wrap_base -> yalnizca RX TC callback'i yazar (kesme)
 *   s_consumed  -> yalnizca tuketici baglami yazar, DMA hic bakmaz
 * Uretici ve tuketici farkli degiskenlere sahip; ortak okuma kisa kritik
 * bolumde tutarli ORNEKLENIR (bolum 6.2).
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
#ifdef UART_COMM_TEST
static uint8_t             s_force_restart_fail;     /* yalnizca test kancasi */
static uint8_t             s_force_start_fail;       /* yalnizca test kancasi */
static uint8_t             s_force_sample_fail;      /* yalnizca test kancasi */
static uint8_t             s_copy_hook;              /* yalnizca test kancasi */
static uint8_t             s_sync_error_on_start;    /* yalnizca test kancasi */
#endif

/* R1: acik alim durumu. Yalnizca main/tuketici baglaminda yazilir. */
static uart_rx_phase_t     s_phase = UART_RX_PHASE_STOPPED;

/* --- R2: MUTLAK uretim/tuketim sayaclari ---
   Eski kod yalnizca modulo konum (s_read_pos) sakliyordu; bu yuzden "0 yeni
   bayt" ile "256 yeni bayt" ayirt EDILEMIYORDU: tam bir tur uzerine
   yazildiginda konumlar esit gorunur ve kayip sessizce gizlenirdi.

   Artik iki taraf da mutlak bayt sayisi tutuyor:
     s_wrap_base  tamamlanmis turlarin toplami. YALNIZCA gercek DMA TC
                  olayinda, RX callback'inde UART_RX_BUF_SIZE artar.
     s_consumed   ayristiriciya verilmis toplam bayt (yalnizca tuketici yazar)
   Anlik uretim konumu wrap_base + tur ici konum (NDTR'den) olarak
   ORNEKLENIR; saklanmaz.

   Sayaclar unsigned modulo calisir: fark (produced - consumed) 2^32 sarimi
   boyunca da dogrudur. Mesru farkin 2^31'den kucuk oldugu kabul edilir. */
static volatile uint32_t   s_wrap_base;
static uint32_t            s_consumed;

/* Oturum kimligi: her durdurma/yeniden kurma yeni bir uretim oturumudur.
   Abort sirasinda olusan TC'nin normal uretime eklenmesini engeller. */
static volatile uint32_t   s_rx_session;

/* Hata nesli: her RX hata bildiriminde artar. Kopya oncesi/sonrasi
   karsilastirilir; degismisse kopyanin ait oldugu oturum artik guvenilir
   degildir (bolum 6.3). Sayac olmasi sart: s_rx_error tek basina
   yetmezdi, cunku arada gelip islenmis bir hata bayragi tekrar 0 olabilir. */
static volatile uint32_t   s_rx_error_gen;

/* Tutarsiz ornekleme penceresi (bkz. bolum 6.2 adim 4). */
static uint8_t             s_sample_failing;    /* acik basarisizlik penceresi */
static uint32_t            s_sample_fail_tick;  /* ilk basarisiz ornek ani */
static uint32_t            s_sample_retry_tick; /* bir sonraki deneme ani */

/* Ayristirici sayaclari SOGUK kurulumda bir kez sifirlanir; sonraki
   kurulumlar hata gecmisini korur (bkz. uart_rx.h sozlesmesi). */
static uint8_t             s_parser_ready;

uart_rx_stats_t uart_rx_stats;
uart_rx_state_t uart_rx_state;

static void frame_received(const frame_info_t *info, void *user_data);
static void uart_rx_recover(void);
static void restart_failed(void);
static void check_frame_timeout(void);
static void rx_set_phase(uart_rx_phase_t phase);
static uint8_t rx_hw_receiving(const UART_HandleTypeDef *huart);
static uint8_t rx_session_healthy(void);


/* --- Kisa kritik bolum ---
   Eski PRIMASK saklanip geri yuklenir; kosulsuz __enable_irq() YAPILMAZ
   (bolum 5.1). Aksi halde zaten kesmeler kapaliyken cagrilan bir yol,
   cikista onlari izinsiz acardi. Icinde HAL cagrisi, parser veya kullanici
   handler'i calismaz; yalnizca birkac register/degisken okunur. */
static uint32_t rx_crit_enter(void)
{
    uint32_t pri = __get_PRIMASK();
    __disable_irq();
    return pri;
}

static void rx_crit_exit(uint32_t pri)
{
    __set_PRIMASK(pri);
}


/* DMA stream'inin HAM transfer-complete bayragi. ISR bu bayragi temizleyip
   ardindan callback'i cagirir; yani "bayrak set" demek "tur bitti ama
   wrap_base henuz artmadi" demektir. */
static uint32_t rx_tcif_raw(void)
{
    const DMA_HandleTypeDef *hdma = s_huart->hdmarx;

    return (uint32_t)(__HAL_DMA_GET_FLAG(hdma,
                      __HAL_DMA_GET_TC_FLAG_INDEX(hdma)) != 0U);
}


/* stats.faulted ile phase tek kaynaktan turetilir: ikisini ayri ayri
   yazmak, birinin digerinden sapmasi demektir. */
static void rx_set_phase(uart_rx_phase_t phase)
{
    s_phase = phase;
    uart_rx_stats.faulted = (uint8_t)(phase == UART_RX_PHASE_FAULT);
}


uart_rx_phase_t uart_rx_get_phase(void)
{
    return s_phase;
}


/* Ilerleme sayaclarinin TEK sifirlama noktasi. Yalnizca DMA durdurulup eski
   IRQ kaynaklari temizlendikten SONRA cagrilir; aksi halde hala calisan bir
   stream'in turlari kaybolur. Butun start/recover yollari buradan gecer:
   tek bir yol atlanirsa eski oturumun wrap_base'i yeni oturuma sizar ve
   sahte taşma gorunur. */
static void rx_reset_progress_after_stop(void)
{
    uint32_t pri = rx_crit_enter();

    s_wrap_base = 0U;
    s_consumed  = 0U;
    s_rx_session++;                 /* yeni uretim oturumu */

    rx_crit_exit(pri);

    s_sample_failing    = 0U;
    s_sample_fail_tick  = 0U;
    s_sample_retry_tick = 0U;
    s_last_rx_tick      = HAL_GetTick();
}


/* Uretici konumunun SAF aritmetigi. Donanimdan ayrilmasinin nedeni: sinir
   degerleri (127/128/255/256/257/512, bekleyen TC, UINT32 sarimi) gercek
   DMA zamanlamasini yakalamaya calismadan sinanabilsin. Donanim sirasi bu
   fonksiyonla KANITLANMAZ; onun icin ayri kart testi vardir.

   pending_tc: tur bitti fakat ISR henuz wrap_base'i artirmadi.
   ndtr      : 1..UART_RX_BUF_SIZE arasi gecerli tur ici kalan. */
uint32_t uart_rx_producer_from(uint32_t wrap_base, uint8_t pending_tc,
                               uint32_t ndtr)
{
    const uint32_t buf_size = UART_RX_BUF_SIZE;

    return wrap_base + ((pending_tc != 0U) ? buf_size : 0U) +
           (buf_size - ndtr);
}


/* Tutarli uretici ornegi (bolum 6.2).
   Donus 0 ise *produced KULLANILMAZ: cagiran tuketiciyi ilerletmez.

   Neden tek okuma yetmez: NDTR ile wrap_base farkli anlarda degisir. Tur
   sinirinda once TCIF kalkar, sonra NDTR yeniden yuklenir, en son ISR
   wrap_base'i artirir. Arada alinan bir ornek bir turu ya iki kez sayar ya
   da hic saymaz. Bu yuzden TCIF ornegin ONCESINDE ve SONRASINDA okunur. */
static uint8_t uart_rx_sample_producer(uint32_t *produced)
{
    const uint32_t buf_size = UART_RX_BUF_SIZE;
    uint8_t  deneme;

    if ((s_huart == NULL) || (s_huart->hdmarx == NULL) || (produced == NULL))
    {
        return 0U;
    }

#ifdef UART_COMM_TEST
    if (s_force_sample_fail != 0U)
    {
        return 0U;                  /* surekli gecersiz NDTR taklidi */
    }
#endif

    /* En fazla 3 kisa deneme: DMA'nin yeniden yukleme aninda sonsuz spin
       yapmak, RX disindaki butun isleri (TX dahil) durdururdu. */
    for (deneme = 0U; deneme < 3U; deneme++)
    {
        uint32_t pri;
        uint32_t base;
        uint32_t tc_once;
        uint32_t tc_sonra;
        uint32_t ndtr;

        pri = rx_crit_enter();
        base     = s_wrap_base;
        tc_once  = rx_tcif_raw();
        ndtr     = (uint32_t)__HAL_DMA_GET_COUNTER(s_huart->hdmarx);
        tc_sonra = rx_tcif_raw();
        rx_crit_exit(pri);
        /* CPU burada ISR calistiramaz, ama DMA calismaya DEVAM eder:
           kritik bolum NDTR'yi dondurmaz, yalnizca wrap_base'i sabitler. */

        if (tc_once != tc_sonra)
        {
            continue;               /* tur sinirina denk geldik, tekrar dene */
        }

        if ((ndtr == 0U) || (ndtr > buf_size))
        {
            /* NDTR 0: yeniden yukleme penceresi. Gecerli bir tur ici konum
               degil; 256 - 0 = 256 yazmak bir sonraki turu erken saydirirdi. */
            continue;
        }

        /* Bekleyen TC: bayrak set ama ISR daha calismadi, yani bu tur
           wrap_base'e HENUZ eklenmedi. Ornek fonksiyonu donanim bayragini
           TEMIZLEMEZ ve wrap_base'e DOKUNMAZ; telafiyi yalnizca kendi
           hesabinda yapar, sayimi ISR'ye birakir.

           Tek yanlilik: TCIF'in kalkmasi ile NDTR'nin yeniden yuklenmesi
           arasindaki cok kisa pencerede bir tur EKSIK sayilabilir. Yon
           onemli: eksik saymak gecicidir ve sonraki ornek duzeltir; fazla
           saymak uretici konumunu GERI sicratirdi. */
        *produced = uart_rx_producer_from(base, (uint8_t)(tc_once != 0U),
                                          ndtr);
        return 1U;
    }

    return 0U;
}


/* Ornekleme basarisizliginin muhasebesi (bolum 6.2 adim 4).
   Donus 1 ise RX saglik hatasi olusmustur ve toparlanmaya gidilmelidir. */
static uint8_t rx_note_sample_fail(void)
{
    uint32_t now = HAL_GetTick();

    if (s_sample_failing == 0U)
    {
        s_sample_failing   = 1U;
        s_sample_fail_tick = now;
    }

    /* Tekrar 1 ms sonraya birakilir: bozuk NDTR yuzunden sinirsiz mesgul
       dongu kurulmaz, RX disindaki isler calismaya devam eder. */
    s_sample_retry_tick = now + 1U;
    uart_rx_stats.sample_defers++;

    return (uint8_t)((now - s_sample_fail_tick) >= UART_RX_SAMPLE_FAIL_MS);
}


static void rx_note_sample_ok(void)
{
    s_sample_failing = 0U;          /* gecerli ornek pencereyi kapatir */
}


/* Ornekleme ertelendiyse henuz zamani gelmemis olabilir. */
static uint8_t rx_sample_deferred(void)
{
    if (s_sample_failing == 0U)
    {
        return 0U;
    }
    return (uint8_t)((int32_t)(HAL_GetTick() - s_sample_retry_tick) < 0);
}


/* Donanim GERCEKTEN alim yapiyor mu? Yazilim durumuna degil uc ayri
   kanita bakilir; herhangi biri "evet" diyorsa tampon dokunulmazdir:
     RxState  : HAL alim surecini acik tutuyor
     CR3.DMAR : USART, DMA'dan alim istegi uretmeye devam ediyor
     SxCR.EN  : DMA stream'i hala etkin, tampona yazabilir
   "RxState READY" tek basina yeterli degildir: HAL durum alanini stream
   durmadan once de temizleyebilir. */
static uint8_t rx_hw_receiving(const UART_HandleTypeDef *huart)
{
    const DMA_HandleTypeDef *hdma;

    if (huart == NULL)
    {
        return 0U;
    }

    if (huart->RxState == HAL_UART_STATE_BUSY_RX)
    {
        return 1U;
    }

    if (READ_BIT(huart->Instance->CR3, USART_CR3_DMAR) != 0U)
    {
        return 1U;
    }

    hdma = huart->hdmarx;
    if (hdma != NULL)
    {
        if (READ_BIT(((DMA_Stream_TypeDef *)hdma->Instance)->CR,
                     DMA_SxCR_EN) != 0U)
        {
            return 1U;
        }
    }

    return 0U;
}


/* HAL_OK donmus olmasi alimin kuruldugunu KANITLAMAZ (RX-4): bu HAL'de bazi
   dallar hata callback'ini cagri donmeden calistirir ve alim daha basliyor
   gorunurken durmus olabilir. Bu yuzden donus degeri degil, donanimin
   kendisi ve bekleyen hata bildirimi birlikte degerlendirilir. */
static uint8_t rx_session_healthy(void)
{
    if (s_huart == NULL)
    {
        return 0U;
    }

    if (s_rx_error != 0U)
    {
        return 0U;      /* baslatma sirasinda hata bildirimi geldi */
    }

    return (uint8_t)(rx_hw_receiving(s_huart) != 0U);
}


HAL_StatusTypeDef uart_rx_start(UART_HandleTypeDef *huart)
{
    HAL_StatusTypeDef hal;

    if ((huart == NULL) || (huart->hdmarx == NULL))
    {
        return HAL_ERROR;
    }

    /* --- RX-1: BUTUN SIFIRLAMALARDAN ONCE sahiplik kontrolu ---
       Eski kod handle'i, okuma konumunu, olaylari ve ayristiriciyi kosulsuz
       sifirliyor, HAL_BUSY'yi ondan SONRA aliyordu. Boylece reddedilen bir
       cagri bile calisan alimi bozuyordu. Artik tek bir alan bile
       degismeden once reddediliyor. */
    if ((s_phase == UART_RX_PHASE_STARTING) ||
        (s_phase == UART_RX_PHASE_RUNNING)  ||
        (s_phase == UART_RX_PHASE_ABORTING))
    {
        uart_rx_stats.start_rejects++;
        return HAL_BUSY;
    }

    /* Yazilim durumu "durdu" dese bile donanim hala tamponu yaziyor
       olabilir: o halde yeni oturum kurmak eski DMA'nin uzerine yazmaktir.
       Bu kontrol handle degisimini de kapsar; eski sahiplik korunur. */
    if (rx_hw_receiving(s_huart) != 0U)
    {
        uart_rx_stats.start_rejects++;
        return HAL_BUSY;
    }

    /* Baska bir handle ile yeniden baglama: eski handle'in donanimi durmus
       olmali. Yukaridaki kontrol s_huart icin bunu dogruladi; yeni handle'in
       kendi donanimi da serbest olmali. */
    if ((s_huart != NULL) && (huart != s_huart) &&
        (rx_hw_receiving(huart) != 0U))
    {
        uart_rx_stats.start_rejects++;
        return HAL_BUSY;
    }

    /* --- Buradan sonrasi kabul edilmis kurulum --- */
    s_huart        = huart;
    s_rx_pending   = 0U;
    s_rx_error     = 0U;
    s_recover_pending = 0U;
    s_restart_tries   = 0U;

    /* Eski oturumun bekleyen TC bayragi yeni oturuma SIZMAMALI: donanim
       durdu, ama TCIF set kalmis olabilir ve ilk ornek onu "bekleyen tur"
       sanip 256 bayt ileri atlardi. */
    __HAL_DMA_CLEAR_FLAG(huart->hdmarx,
                         __HAL_DMA_GET_TC_FLAG_INDEX(huart->hdmarx));
    rx_reset_progress_after_stop();

    /* Soguk kurulum yalnizca ilk kez: yeniden baslatma sayaclari silmez. */
    if (s_parser_ready == 0U)
    {
        frame_parser_init(&s_parser);
        s_parser_ready = 1U;
    }
    else
    {
        frame_parser_discard(&s_parser);
    }

    /* STARTING, HAL cagrisindan ONCE kurulur: cagri donmeden gelen bir hata
       callback'i bu oturuma ait oldugunu buradan anlar. */
    rx_set_phase(UART_RX_PHASE_STARTING);

#ifdef UART_COMM_TEST
    if (s_sync_error_on_start != 0U)
    {
        /* HAL cagrisi donmeden gelen senkron hata bildirimi */
        s_rx_error = 1U;
        s_rx_error_gen++;
        uart_rx_stats.error_events++;
    }
#endif

    hal = HAL_UARTEx_ReceiveToIdle_DMA(huart, s_dma_buf,
                                       (uint16_t)sizeof(s_dma_buf));

#ifdef UART_COMM_TEST
    if (s_force_start_fail != 0U)
    {
        s_force_start_fail = 0U;        /* tek atimlik */
        hal = HAL_ERROR;
    }
#endif

    /* Donus degeri TEK BASINA yeterli degil: donanim ve bekleyen hata
       birlikte degerlendirilir (RX-4). */
    if ((hal == HAL_OK) && (rx_session_healthy() != 0U))
    {
        rx_set_phase(UART_RX_PHASE_RUNNING);
        return HAL_OK;
    }

    /* Basarisizlik GORUNUR olmali: calismayan bir alim RUNNING gorunemez.
       R1'de guvenli kapanis FAULT; otomatik, bloklamayan toparlanma R4'te
       tamamlanacak. */
    uart_rx_stats.start_fails++;
    rx_set_phase(UART_RX_PHASE_FAULT);
    return HAL_ERROR;
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

    /* R1/RX-4: veri ve zaman asimi YALNIZCA RUNNING'de islenir.
       Eskiden toparlanma beklerken de drain/timeout calisabiliyordu: durmus
       ya da belirsiz bir DMA'nin tamponunu ayristiriciya vermek, kurtarilmis
       gibi gorunen bozuk cerceveler uretir. FAULT'ta sessiz kalmak dogru
       davranistir; sayaclar ve phase durumu zaten gorunur kiliyor. */
    if (s_phase != UART_RX_PHASE_RUNNING)
    {
        return;
    }

    /* 2) Bekleyen veriyi tuket. Tur basina en fazla UART_RX_SERVICE_BUDGET
       bayt: sinirsiz drain dongusu TX'in servis almasini engellerdi.
       Kalan is varsa bir sonraki tura birakilir, bildirim beklenmez. */
    if (s_rx_pending != 0U)
    {
        s_rx_pending = 0U;
        if (uart_rx_service_budget(UART_RX_SERVICE_BUDGET) != 0U)
        {
            s_rx_pending = 1U;      /* is kaldi: sonraki tur devam etsin */
        }
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

    /* FAULT'tan otomatik cikis YOK: deneme butcesi tukenmistir ve her
       serviste yeniden denemek sonucsuz bir mesgul dongu olurdu. Tek cikis
       uart_rx_start. (R4 bunu acik kurtarma istegine baglayacak.) */
    if (s_phase == UART_RX_PHASE_FAULT)
    {
        s_recover_pending = 0U;
        return;
    }

    /* Basarisiz denemeler arasinda bekle. while(1) icinde bu kontrol olmasa
       saniyede binlerce sonucsuz abort/restart cifti calisirdi. */
    if ((s_restart_tries > 0U) &&
        ((HAL_GetTick() - s_restart_tick) < UART_RX_RESTART_RETRY_MS))
    {
        rx_set_phase(UART_RX_PHASE_RETRY_WAIT);
        return;
    }

#ifdef UART_COMM_TEST
    /* TEST KANCASI: donanima hic dokunmadan basarisiz deneme uretir.
       BUSY_RX kontrolunden ONCE olmali, cunku testte alim calismaya devam
       ediyor; amac tekrar/kalici hata mantigini sinamak. */
    if (s_force_restart_fail != 0U)
    {
        restart_failed();
        return;
    }
#endif /* UART_COMM_TEST */

    if (s_huart->RxState == HAL_UART_STATE_BUSY_RX)
    {
        /* HAL alimi surduruyor (ornegin tek bir gurultu hatasi: FE/NE/PE'de
           HAL alimi kesmez, yalnizca ErrorCallback cagirir); mudahale etmek
           calisan bir alimi bozar. Yapacak is yok, borc kapanir ve alim
           saglikli kabul edilir: ayni servis turunda tuketim surebilir. */
        s_recover_pending = 0U;
        s_restart_tries   = 0U;
        rx_set_phase(UART_RX_PHASE_RUNNING);
        return;
    }

    /* Durdurma suruyor: bu noktadan sonra tampon dokunulmaz sayilir.
       R1'de HAL_UART_AbortReceive hala BLOKLAYICI; _IT akisi R4'te. */
    rx_set_phase(UART_RX_PHASE_ABORTING);

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

    /* R2: butun start/recover yollari AYNI ilerleme sifirlamasindan gecer.
       Bir yol atlanirsa eski oturumun wrap_base'i yeni oturuma sizar ve
       ilk ornek sahte tasma uretir. */
    __HAL_DMA_CLEAR_FLAG(s_huart->hdmarx,
                         __HAL_DMA_GET_TC_FLAG_INDEX(s_huart->hdmarx));
    rx_reset_progress_after_stop();

    rx_set_phase(UART_RX_PHASE_STARTING);

    if ((HAL_UARTEx_ReceiveToIdle_DMA(s_huart, s_dma_buf,
                                      (uint16_t)sizeof(s_dma_buf)) == HAL_OK) &&
        (rx_session_healthy() != 0U))
    {
        uart_rx_stats.restarts++;
        s_recover_pending = 0U;          /* borc ancak burada kapanir */
        s_restart_tries   = 0U;
        rx_set_phase(UART_RX_PHASE_RUNNING);
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

    /* Bu denemeye SEBEP olan bildirimi burada tuketiyoruz.
       R1 ile baslatma sagligi artik s_rx_error'a da bakiyor; bayrak set
       kalirsa uart_rx_service onu YENI bir hata sanip s_restart_tries'i
       sifirlar ve deneme butcesi hic dolmaz: FAULT'a asla ulasilmayan
       sonsuz bir yeniden deneme dongusu olusur. Hata gorunurlugu
       kaybolmaz; error_events ve last_error korunur.
       NOT: RX-5'in "ayni toparlanma doneminde gelen tekrar bildirimleri
       butceyi sifirlamasin" kurali R4'te hata donemi kimligiyle tam olarak
       uygulanacak; bu, o kuralin R1'de gereken en kucuk parcasidir. */
    s_rx_error = 0U;

    if (s_restart_tries >= UART_RX_RESTART_MAX_TRIES)
    {
        rx_set_phase(UART_RX_PHASE_FAULT);
        s_recover_pending = 0U;
    }
    else
    {
        rx_set_phase(UART_RX_PHASE_RETRY_WAIT);
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
    {
        uint32_t produced;

        /* Gecerli ornek alinamiyorsa zaman asimi KARARI VERILMEZ: ilerleme
           olup olmadigini bilmeden aday dusurmek, akmakta olan gecerli bir
           cerceveden bayt atmak demektir. */
        if (uart_rx_sample_producer(&produced) == 0U)
        {
            return;
        }

        if (produced != s_consumed)
        {
            uart_rx_drain();
            return;
        }
    }

    frame_parser_timeout(&s_parser, frame_received, NULL);
    uart_rx_stats.frame_timeouts++;
    s_last_rx_tick = HAL_GetTick();
}


uint8_t uart_rx_get_produced(uint32_t *out)
{
    return uart_rx_sample_producer(out);
}


uint32_t uart_rx_get_consumed(void)
{
    return s_consumed;
}


/* Ornek alinamadiginda ortak muhasebe. Donus 1: cagiran hemen cikmali. */
static void rx_handle_sample_fail(void)
{
    if (rx_note_sample_fail() != 0U)
    {
        uart_rx_stats.sample_fails++;
        s_sample_failing = 0U;
        s_rx_error       = 1U;          /* ortak RX toparlanma yoluna bagla */
    }
}


/* Tam tur kaybi politikasi (RX-2 / bolum 6.3).
   Konservatif kabul: fark tampon boyuna ULASTIYSA guvenlik payi bitmistir.
   Tam 256'da henuz fiziksel ezilme olmamis olabilir, ama okunmamis en eski
   bayt ile DMA'nin yazma ucu ayni noktadadir ve bir sonraki bayt onu ezer.
   Eski kodda bu durum "veri yok" gorunuyordu ve kayip SESSIZ kaliyordu. */
static void rx_handle_overrun(uint32_t produced, uint32_t available)
{
    uart_rx_stats.overruns++;
    uart_rx_stats.discarded_bytes += available;

    /* Yarim aday artik guvenilmez: arkasina gelen baytlar kayip olabilir,
       birlestirmek bozuk bir cerceveyi gecerli gosterebilirdi. */
    frame_parser_discard(&s_parser);

    /* Tuketici GUNCEL ureticiye alinir; senkron yeni gecerli cerceveyle
       yeniden kurulur. */
    s_consumed     = produced;
    s_last_rx_tick = HAL_GetTick();
}


uint8_t uart_rx_service_budget(uint16_t budget)
{
    const uint32_t buf_size = UART_RX_BUF_SIZE;
    uint8_t  scratch[UART_RX_SCRATCH_SIZE];
    uint32_t kalan = budget;

    if ((s_huart == NULL) || (s_phase != UART_RX_PHASE_RUNNING))
    {
        return 0U;
    }

    /* Ornekleme ertelenmisse tekrar zamani beklenir. Burada 1 donmek
       cagirani bos yere tekrar cagirtir ve tam da onlemek istedigimiz
       mesgul donguyu kurardi. */
    if (rx_sample_deferred() != 0U)
    {
        return 0U;
    }

    while (kalan > 0U)
    {
        uint32_t c;
        uint32_t p0;
        uint32_t p1;
        uint32_t available;
        uint32_t n;
        uint32_t read_idx;
        uint32_t gen0;
        uint32_t sess0;
        uint32_t i;

        if (uart_rx_sample_producer(&p0) == 0U)
        {
            rx_handle_sample_fail();
            return 0U;              /* tuketici ILERLETILMEDI */
        }
        rx_note_sample_ok();

        c         = s_consumed;
        available = p0 - c;         /* unsigned: sarimda da dogru */

        if (available == 0U)
        {
            return 0U;              /* bekleyen veri yok */
        }

        if (available >= buf_size)
        {
            rx_handle_overrun(p0, available);
            return 1U;              /* yeniden senkron sonraki turda */
        }

        /* Parca boyu dort sinirin en kucugu: calisma tamponu, bekleyen veri,
           tamponun fiziksel sonu (sarimi tek parcada gecme) ve kalan butce. */
        n = UART_RX_SCRATCH_SIZE;
        if (n > available)              { n = available; }
        read_idx = c % buf_size;
        if (n > (buf_size - read_idx))  { n = buf_size - read_idx; }
        if (n > kalan)                  { n = kalan; }

        /* Kopya ONCESI tanik degerler: oturum ve hata nesli degisirse bu
           kopya artik gecersiz bir oturuma aittir. */
        sess0 = s_rx_session;
        gen0  = s_rx_error_gen;

        /* DMA bellegi volatile bayt yuklemeleriyle okunur: derleyici bu
           okumalari birlestiremez, yeniden siralayamaz veya atamaz.
           __DMB kopyanin cevre okumalarina gore sirasini korur. F407'de
           D-cache yoktur; cache bakim kodu EKLENMEZ. */
        __DMB();
        for (i = 0U; i < n; i++)
        {
            scratch[i] = ((volatile const uint8_t *)s_dma_buf)[read_idx + i];
        }
        __DMB();

#ifdef UART_COMM_TEST
        /* TEST KANCASI: kopya ile dogrulama ARASINDAKI pencereyi taklit
           eder. Gercek yaristirmayi beklemek deterministik degildir. */
        switch (s_copy_hook)
        {
            case UART_RX_COPY_HOOK_SAMPLE_FAIL:
                s_force_sample_fail = 1U;     /* P1 alinamayacak */
                break;
            case UART_RX_COPY_HOOK_OVERWRITE:
                s_wrap_base += UART_RX_BUF_SIZE;  /* uretici bir tur sicradi */
                break;
            case UART_RX_COPY_HOOK_ERROR_GEN:
                s_rx_error_gen++;             /* kopya sirasinda RX hatasi */
                break;
            default:
                break;
        }
        s_copy_hook = UART_RX_COPY_HOOK_NONE;  /* tek atimlik */
#endif

        /* Kopya SONRASI dogrulama (bolum 6.3). */
        if (uart_rx_sample_producer(&p1) == 0U)
        {
            /* P1 alinamadi: p1'in ESKI/ilklenmemis degeri KULLANILMAZ.
               Tuketici ilerletilmez, scratch ayristiriciya verilmez. */
            rx_handle_sample_fail();
            return 0U;
        }
        rx_note_sample_ok();

        if (((p1 - c) >= buf_size) ||
            (sess0 != s_rx_session) ||
            (gen0  != s_rx_error_gen) ||
            (s_rx_error != 0U))
        {
            /* Kaynak aralik kopya sirasinda ezildi ya da oturum gecersizlesti.
               Kopya DUSURULUR; tuketici ilerletilmez, handler CAGRILMAZ.
               Bir sonraki tur guncel durumla yeniden karar verir. */
            uart_rx_stats.copy_rejects++;
            return 1U;
        }

        /* Guvenli: once tuketiciyi ilerlet, SONRA ayristiriciyi besle.
           Sira onemli: handler icinden gelen bir cagri tutarli bir tuketim
           konumu gormeli. */
        s_consumed     = c + n;
        s_last_rx_tick = HAL_GetTick();
        kalan         -= n;

        frame_parser_feed(&s_parser, scratch, (uint16_t)n,
                          frame_received, NULL);
    }

    /* Butce bitti: hemen islenebilir is kaldi mi? */
    {
        uint32_t p;

        if (uart_rx_sample_producer(&p) == 0U)
        {
            return 0U;
        }
        return (uint8_t)((p - s_consumed) != 0U);
    }
}


/* Geriye donuk destek arayuzu: butce ile ayni isi yapar.
   Yeni testler bunu DOGRUDAN cagirmaz (R3). */
void uart_rx_drain(void)
{
    (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);
}


const frame_parser_t *uart_rx_get_parser(void)
{
    return &s_parser;
}


/* --- Test kancalari (bkz. uart_rx.h) --- */
#ifdef UART_COMM_TEST

void uart_rx_force_restart_fail(uint8_t enable)
{
    s_force_restart_fail = enable;
}


void uart_rx_test_inject_error(void)
{
    s_rx_error = 1U;
    s_rx_error_gen++;
}


void uart_rx_force_start_fail(uint8_t enable)
{
    s_force_start_fail = enable;
}


void uart_rx_test_sync_error_on_start(uint8_t enable)
{
    s_sync_error_on_start = enable;
}


void uart_rx_test_force_sample_fail(uint8_t enable)
{
    s_force_sample_fail = enable;
}


void uart_rx_test_set_copy_hook(uint8_t hook)
{
    s_copy_hook = hook;
}


uint32_t uart_rx_test_get_session(void)
{
    return s_rx_session;
}


uint32_t uart_rx_test_get_wrap_base(void)
{
    return s_wrap_base;
}

#endif /* UART_COMM_TEST */


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
    /* Olay turu callback'in EN BASINDA yerel degiskene alinir: huart
       uzerindeki RxEventType paylasilan bir alandir ve sonraki HAL isleri
       onu degistirebilir. */
    HAL_UART_RxEventTypeTypeDef type = HAL_UARTEx_GetRxEventType(huart);

    if ((s_huart != NULL) && (huart->Instance == s_huart->Instance))
    {
        uart_rx_stats.rx_events++;
        uart_rx_stats.last_size = Size;

        /* Size mutlak konum bildirir, "kac yeni bayt" degil. Tuketimde
           KULLANILMAZ; konumu uretici ornegi NDTR'den hesaplar.
           IDLE yolu sarim sinirinda Size == 256 bildirebilir ama turu
           IDLE'dir: Size toplamak bir turu iki kez saydirirdi. */
        s_rx_pending = 1U;

        /* --- R2: GERCEK tur sayaci ---
           wrap_base yalnizca HAL_UART_RXEVENT_TC turunde artar. HT ve IDLE
           yalnizca bildirim/istatistik uretir.
           Oturum kontrolu: STARTING/RUNNING disindaki (ornegin abort
           sirasinda olusan) bir TC normal uretime EKLENMEZ; aksi halde
           iptal edilen bir oturumun turu yeni oturumu ileri sicratirdi. */
        if ((type == HAL_UART_RXEVENT_TC) &&
            ((s_phase == UART_RX_PHASE_RUNNING) ||
             (s_phase == UART_RX_PHASE_STARTING)))
        {
            s_wrap_base += UART_RX_BUF_SIZE;
        }

        switch (type)
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
        s_rx_error_gen++;

        /* Toparlanma burada YAPILMAZ: kesme baglaminda HAL'i yeniden
           baslatmak yerine tuketici baglamina bildirilir. */
        s_rx_error = 1U;
    }
}

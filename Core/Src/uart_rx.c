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

/* R5: teslim hedefi. Yalnizca start oncesi degistirilir. */
static frame_handler_t     s_handler;
static void               *s_handler_user;

/* R5: zaman asimi ARTIK son TUKETIM anina degil, son gozlenen URETICI
   ILERLEMESINE bagli. Fark onemli: bir yayin (burst) surerken hicbir
   bildirim olusmaz ve tuketim de olmaz, ama DMA yaziyordur. Yalnizca
   tuketim zamanina bakan bir kontrol, akmakta olan gecerli bir cerceveden
   bayt atardi.
   s_frame_base_tick zaman asimi penceresinin BASLANGICI: uretici ilerlemesi
   ya da onceki zaman asimi mudahalesi ile tazelenir. Ikisi ayri olaydir;
   mudahale "veri geldi" gibi gosterilmez. */
static uint32_t            s_last_producer;
static uint32_t            s_frame_base_tick;

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
/* --- R4: toparlanma donemi ve abort durumu --- */
static uint8_t             s_recovery_active;     /* acik toparlanma donemi */
static uint32_t            s_recovery_start_tick; /* donemin baslangici */
static uint8_t             s_final_stop;          /* son durdurma; yeni start yok */
static uint32_t            s_abort_tick;          /* ABORTING'e girildigi an */
static uint8_t             s_abort_issued;        /* HAL abort bir kez cagrildi */
static uint8_t             s_rx_quiescent;        /* durus DOGRULANDI */
static uint32_t            s_last_healthy_tick;   /* son saglikli RUNNING ani */
static volatile uint8_t    s_abort_done;          /* abort tamamlanma callback'i */
static volatile uint8_t    s_health_bad;          /* IRQ cikis saglik kancasi */
static uint8_t             s_recover_request;     /* acik kurtarma istegi */

static uint8_t             s_sample_failing;    /* acik basarisizlik penceresi */
static uint32_t            s_sample_fail_tick;  /* ilk basarisiz ornek ani */
static uint32_t            s_sample_retry_tick; /* bir sonraki deneme ani */

/* Ayristirici sayaclari SOGUK kurulumda bir kez sifirlanir; sonraki
   kurulumlar hata gecmisini korur (bkz. uart_rx.h sozlesmesi). */
static uint8_t             s_parser_ready;

uart_rx_stats_t uart_rx_stats;

static void frame_received(const frame_info_t *info, void *user_data);
static void rx_recover_step(void);
static void check_frame_timeout(void);
static uint8_t rx_safe_stopped(void);
static void rx_open_recovery_period(void);
static void rx_close_recovery_period(void);
static void rx_begin_abort(uint8_t final_stop);
static void rx_set_phase(uart_rx_phase_t phase);
static uint8_t rx_hw_receiving(const UART_HandleTypeDef *huart);
static uint8_t rx_session_healthy(void);


/* --- Kisa kritik bolum ---
   Eski PRIMASK saklanip geri yuklenir; kosulsuz __enable_irq() YAPILMAZ
   (bolum 5.1). Aksi halde zaten kesmeler kapaliyken cagrilan bir yol,
   cikista onlari izinsiz acardi. Icinde HAL cagrisi, parser veya kullanici
   handler'i calismaz; yalnizca birkac register/degisken okunur. */
/* deadline'a kalan sure; dolmussa 0. uint32 cikarma sarimda da dogru. */
static uint32_t rx_until(uint32_t now, uint32_t deadline)
{
    uint32_t fark = deadline - now;

    /* Gecmis bir son tarih buyuk pozitif gorunur; isaretli karsilastirma
       ile ayirt edilir. */
    return ((int32_t)fark <= 0) ? 0U : fark;
}


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


void uart_rx_set_handler(frame_handler_t handler, void *user)
{
    /* YALNIZCA start oncesi: calisan alim sirasinda hedefi degistirmek,
       yarim cercevenin bir handler'a, devaminin baskasina gitmesi demek. */
    if ((s_phase != UART_RX_PHASE_STOPPED) &&
        (s_phase != UART_RX_PHASE_FAULT))
    {
        return;
    }

    s_handler      = handler;
    s_handler_user = user;
}


/* Bir sonraki ETKIN son tarihe kadar beklenebilecek sure.
     0           : hemen yapilacak is var
     UINT32_MAX  : etkin son tarih yok, suresiz uyunabilir

   Son tarih YALNIZCA o anda uygulanabilir olan isler icin verilir
   (bolum 7.2). Dolmus fakat artik gecerli olmayan bir son tarihin 0
   dondurmesi, taski surekli dondurur: bu fonksiyonun asil isi bunu
   onlemektir. */
uint32_t uart_rx_next_wait_ms(uint32_t now)
{
    uint32_t best = UINT32_MAX;

    if (s_huart == NULL)
    {
        return UINT32_MAX;
    }

    /* Islenmemis bildirim veya kurtarma istegi: beklemeden is var. */
    if ((s_rx_error != 0U) || (s_health_bad != 0U))
    {
        return 0U;
    }

    switch (s_phase)
    {
        case UART_RX_PHASE_RUNNING:
            if (s_rx_pending != 0U)
            {
                return 0U;           /* tuketilecek veri var */
            }

            if (s_sample_failing != 0U)
            {
                /* Once ornekleme tekrarina kadar uyunur. Eski frame
                   deadline burada 0 DONDURMEZ; aksi halde bozuk NDTR
                   penceresinde task surekli donerdi. */
                best = rx_until(now, s_sample_retry_tick);
            }
            else if (s_parser.len != 0U)
            {
                best = rx_until(now, s_frame_base_tick +
                                     UART_RX_FRAME_TIMEOUT_MS);
            }
            else
            {
                /* Aday yok: frame zaman asimi son tarihi KALKAR. */
            }

            /* Toparlanma donemi aciksa kapanisini degerlendirmek icin
               uyanilir; bu da etkin bir son tarihtir. */
            if (s_recovery_active != 0U)
            {
                uint32_t t = rx_until(now, s_last_healthy_tick +
                                           UART_RX_HEALTHY_MS);
                if (t < best) { best = t; }
            }
            break;

        case UART_RX_PHASE_ABORTING:
            best = rx_until(now, s_abort_tick + UART_RX_ABORT_TIMEOUT_MS);
            break;

        case UART_RX_PHASE_RETRY_WAIT:
            best = rx_until(now, s_restart_tick + UART_RX_RESTART_RETRY_MS);
            {
                uint32_t t = rx_until(now, s_recovery_start_tick +
                                           UART_RX_RECOVERY_BUDGET_MS);
                if (t < best) { best = t; }
            }
            break;

        case UART_RX_PHASE_FAULT:
            if (s_recover_request != 0U)
            {
                return 0U;
            }
            /* FAULT'ta periyodik uyanma YOK: otomatik yeniden deneme
               olmadigi icin beklenecek bir son tarih de yok. */
            break;

        case UART_RX_PHASE_STARTING:
        case UART_RX_PHASE_STOPPED:
        default:
            break;
    }

    return best;
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
    s_last_producer     = 0U;
    s_last_rx_tick      = HAL_GetTick();
    s_frame_base_tick   = s_last_rx_tick;
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
    s_health_bad      = 0U;
    s_abort_done      = 0U;
    s_abort_issued    = 0U;
    s_recover_request = 0U;
    rx_close_recovery_period();

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
        s_rx_quiescent      = 0U;
        s_last_healthy_tick = HAL_GetTick();
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
    uint32_t now = HAL_GetTick();

    /* 1) Kesmeden gelen bildirimler BORCA cevrilir. Bayragi temizleyip tek
       deneme yapip gecmek olumcul olurdu: deneme basarisiz olursa DMAR ve
       IDLEIE kapali kalir, bir daha hicbir callback olusmaz ve bayragi set
       edecek kimse kalmaz.

       R4: deneme sayaci BURADA SIFIRLANMAZ. Eski kod her yeni hata
       bildiriminde s_restart_tries'i sifirliyordu; surekli hata ureten bir
       hatta butce hic dolmuyor ve FAULT'a ULASILAMIYORDU. Butce artik
       toparlanma donemine ait ve donem yalnizca saglikli calismayla kapanir. */
    if ((s_rx_error != 0U) || (s_health_bad != 0U))
    {
        s_rx_error   = 0U;
        s_health_bad = 0U;

        if ((s_phase == UART_RX_PHASE_RUNNING) ||
            (s_phase == UART_RX_PHASE_STARTING))
        {
            rx_open_recovery_period();
            s_recover_pending = 1U;
            rx_begin_abort(0U);
        }
        /* ABORTING/RETRY_WAIT: zaten acik donemdeyiz, tekrar bildirim
           butceyi uzatmaz. FAULT: otomatik yeniden deneme yok. */
    }

    if ((s_recover_pending != 0U) || (s_recover_request != 0U))
    {
        rx_recover_step();
    }

    /* Saglikli donem kontrolu icin AYRI periyodik uyanma kurulmaz: basarili
       start zamani kaydedilir ve burada degerlendirilir. 100 ms hatasiz
       RUNNING'den sonra donem kapanir; boylece saniyeler sonra gelen bir
       hata YENI donem acar ve aninda FAULT olmaz. */
    if ((s_phase == UART_RX_PHASE_RUNNING) && (s_recovery_active != 0U) &&
        ((now - s_last_healthy_tick) >= UART_RX_HEALTHY_MS))
    {
        rx_close_recovery_period();
        s_recover_pending = 0U;
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
/* ===================== R4: bloklamayan RX toparlanmasi ==================

   Eski akis HAL_UART_AbortReceive() ile BLOKLUYORDU. Tek taskli tasarimda
   bu, RX toparlanirken TX'in de beklemesi demek: bir yonun arizasi digerini
   durduruyordu. Artik _IT akisi kullaniliyor ve her servis turu durumu bir
   adim ilerletip geri donuyor.

   Ikinci sorun deneme butcesinin kapanmamasiydi: her yeni hata bildirimi
   s_restart_tries'i sifirliyordu, yani surekli hata ureten bir hatta FAULT'a
   HIC ulasilamiyordu. Simdi bir TOPARLANMA DONEMI var:
     - ilk hatada acilir,
     - icindeki tekrar bildirimleri butceyi SIFIRLAMAZ,
     - en fazla 5 start denemesi VE 100 ms yeni deneme butcesi,
     - butce dolunca durus belirsizse final_stop ile en fazla 20 ms daha,
     - RUNNING'de 100 ms hatasiz calisma ile kapanir.
   Boylece 2 saniye saglikli sessizlikten sonra gelen hata YENI bir donemdir
   ve onceki donemin suresi yuzunden aninda FAULT olmaz.

   FAULT bir "donanim bozuk" iddiasi DEGILDIR: guvenli calismanin
   kurulamadigini soyler. Durusun kanitlanip kanitlanmadigi AYRI tutulur
   (rx_quiescent): ikisini birlestirmek, DMA hala tamponu yazarken tamponu
   serbest sanmaya yol acardi. */

/* HAL'in kendi baslattigi bir abort suruyor mu? Oyleyse IKINCI bir abort
   baslatilmaz: HAL_DMA_Abort_IT callback isaretcisini degistirir ve ilk
   abort'un tamamlanma bildirimi kaybolur. */
static uint8_t rx_abort_in_progress(void)
{
    const DMA_HandleTypeDef *hdma;

    if (s_huart == NULL)
    {
        return 0U;
    }

    if (s_huart->RxState == HAL_UART_STATE_BUSY_RX)
    {
        /* Alim suruyor; bu bir abort degil. */
    }

    hdma = s_huart->hdmarx;
    if ((hdma != NULL) && (hdma->State == HAL_DMA_STATE_ABORT))
    {
        return 1U;
    }

    return 0U;
}


/* Bolum 6.4: RX icin GUVENLI DURUS kanitlari.
   Yalnizca "callback geldi" veya "HAL_OK dondu" yeterli DEGILDIR; donanimin
   kendisi okunur. */
static uint8_t rx_safe_stopped(void)
{
    const DMA_HandleTypeDef *hdma;

    if (s_huart == NULL)
    {
        return 0U;
    }

    if (READ_BIT(s_huart->Instance->CR3, USART_CR3_DMAR) != 0U)
    {
        return 0U;                      /* USART hala DMA istegi uretiyor */
    }

    hdma = s_huart->hdmarx;
    if (hdma != NULL)
    {
        if (READ_BIT(((DMA_Stream_TypeDef *)hdma->Instance)->CR,
                     DMA_SxCR_EN) != 0U)
        {
            return 0U;                  /* stream hala tampona yazabilir */
        }
        if (hdma->State == HAL_DMA_STATE_ABORT)
        {
            return 0U;                  /* abort callback isini bitirmedi */
        }
    }

    if (s_huart->RxState == HAL_UART_STATE_BUSY_RX)
    {
        return 0U;
    }

    return 1U;
}


/* Toparlanma donemi: yalnizca KAPALIYKEN acilir. Acik bir donem icindeki
   tekrar bildirimleri butceyi uzatmaz. */
static void rx_open_recovery_period(void)
{
    if (s_recovery_active == 0U)
    {
        s_recovery_active     = 1U;
        s_recovery_start_tick = HAL_GetTick();
        s_restart_tries       = 0U;
        s_final_stop          = 0U;
    }
}


static void rx_close_recovery_period(void)
{
    s_recovery_active = 0U;
    s_final_stop      = 0U;
    s_restart_tries   = 0U;
}


/* Deneme butcesi doldu mu: 5 start VEYA 100 ms. */
static uint8_t rx_budget_exhausted(uint32_t now)
{
    if (s_restart_tries >= UART_RX_RESTART_MAX_TRIES)
    {
        return 1U;
    }
    return (uint8_t)((now - s_recovery_start_tick) >=
                     UART_RX_RECOVERY_BUDGET_MS);
}


/* ABORTING'e gec. State ve zaman damgasi HAL cagrisindan ONCE kurulur:
   bu HAL'de abort callback'i cagri DONMEDEN calisabilir ve o an durumu
   tutarli bulmalidir (TX-5'in RX karsiligi). */
static void rx_begin_abort(uint8_t final_stop)
{
    s_abort_done   = 0U;
    s_rx_quiescent = 0U;
    s_final_stop   = final_stop;
    s_abort_tick   = HAL_GetTick();
    rx_set_phase(UART_RX_PHASE_ABORTING);

    if (rx_abort_in_progress() != 0U)
    {
        /* HAL zaten durduruyor: tamamlanmasini BEKLE. Ikinci abort
           callback isaretcisini ezer ve ilk bildirimi kaybederdi. */
        s_abort_issued = 1U;
        return;
    }

    if (HAL_UART_AbortReceive_IT(s_huart) != HAL_OK)
    {
        uart_rx_stats.abort_start_fails++;
        /* Baslatilamadi: zaman asimina kadar donanim dogrudan izlenir.
           Her serviste yeniden cagirmak HAL durumunu daha da bozardi. */
    }
    s_abort_issued = 1U;
}


/* Durus saglandiktan sonra yeni bir alim oturumu dener. */
static void rx_try_restart(void)
{
    s_restart_tick = HAL_GetTick();
    s_restart_tries++;

    /* Bekleyen hata bayraklari TEMIZLENMELI: ORE duruyorsa
       ReceiveToIdle_DMA, EIE'yi acar acmaz yeniden hata dogurur ve ayni
       sonucu tekrar uretirdi. F4'te SR oku + DR oku tek yolla dusurur. */
    __HAL_UART_CLEAR_OREFLAG(s_huart);
    s_huart->ErrorCode = HAL_UART_ERROR_NONE;

    frame_parser_discard(&s_parser);

    __HAL_DMA_CLEAR_FLAG(s_huart->hdmarx,
                         __HAL_DMA_GET_TC_FLAG_INDEX(s_huart->hdmarx));
    rx_reset_progress_after_stop();

    s_rx_error     = 0U;
    s_abort_issued = 0U;
    rx_set_phase(UART_RX_PHASE_STARTING);

#ifdef UART_COMM_TEST
    /* TEST KANCASI: donanima HIC dokunmadan basarisiz deneme uretir.
       HAL cagrisi atlanir, boylece stream durmus kalir ve bir sonraki
       deneme temiz bir noktadan baslar; sinanan sey tekrar/butce
       mantiginin kendisi. */
    if (s_force_restart_fail != 0U)
    {
        uart_rx_stats.restart_fails++;
        s_rx_error = 0U;
        rx_set_phase(UART_RX_PHASE_RETRY_WAIT);
        return;
    }
#endif

    if ((HAL_UARTEx_ReceiveToIdle_DMA(s_huart, s_dma_buf,
                                      (uint16_t)sizeof(s_dma_buf)) == HAL_OK) &&
        (rx_session_healthy() != 0U))
    {
        uart_rx_stats.restarts++;
        s_rx_quiescent     = 0U;
        s_last_healthy_tick = HAL_GetTick();
        rx_set_phase(UART_RX_PHASE_RUNNING);
        return;
    }

    /* Deneme dustu. Bildirimi TUKET: set kalirsa bir sonraki servis onu
       YENI hata sanip donemi yeniden acardi ve butce hic dolmazdi. */
    uart_rx_stats.restart_fails++;
    s_rx_error = 0U;
    rx_set_phase(UART_RX_PHASE_RETRY_WAIT);
}


/* FAULT'a gec. Durusun kanitlanip kanitlanmadigi AYRI bilgidir. */
static void rx_enter_fault(void)
{
    s_rx_quiescent = rx_safe_stopped();

    if (s_rx_quiescent == 0U)
    {
        /* Durus saglanamadi: ilgili RX istek/kesme kaynaklari mumkun
           oldugunca kapatilir ve tampon KILITLI kalir (sessiz yeniden
           kullanim yok). Ortak USART IRQ'su tumuyle kapatilmaz, yoksa
           saglikli TX de kesilirdi. */
        CLEAR_BIT(s_huart->Instance->CR3, USART_CR3_DMAR);
        __HAL_UART_DISABLE_IT(s_huart, UART_IT_IDLE);
        uart_rx_stats.recovery_fails++;
    }

    rx_close_recovery_period();
    s_recover_pending = 0U;
    s_abort_issued    = 0U;
    rx_set_phase(UART_RX_PHASE_FAULT);
}


/* Toparlanma durum makinesinin BIR adimi. Hicbir dalda beklemez. */
static void rx_recover_step(void)
{
    uint32_t now = HAL_GetTick();

    if (s_huart == NULL)
    {
        s_recover_pending = 0U;
        return;
    }

    switch (s_phase)
    {
        case UART_RX_PHASE_ABORTING:
            /* Callback gelmis olsun olmasin DONANIM degerlendirilir:
               abort callback'i kaybolabilir ya da HAL_OK donup is
               bitmemis olabilir (bolum 6.4). */
            if (rx_safe_stopped() != 0U)
            {
                s_rx_quiescent = 1U;

                if (s_final_stop != 0U)
                {
                    rx_enter_fault();       /* yeni start YOK */
                }
                else
                {
                    s_abort_issued = 0U;
                    rx_set_phase(UART_RX_PHASE_RETRY_WAIT);
                }
            }
            else if ((now - s_abort_tick) >= UART_RX_ABORT_TIMEOUT_MS)
            {
                rx_enter_fault();
            }
            else
            {
                /* Durus bekleniyor; bu tur icin yapacak is yok. */
            }
            break;

        case UART_RX_PHASE_RETRY_WAIT:
            if (rx_budget_exhausted(now) != 0U)
            {
                if (rx_safe_stopped() != 0U)
                {
                    rx_enter_fault();
                }
                else
                {
                    /* Butce doldu ama DMA hala aktif olabilir: son bir
                       durdurma denemesi icin 20 ms daha; yeni start YOK. */
                    rx_begin_abort(1U);
                }
            }
            else if ((now - s_restart_tick) >= UART_RX_RESTART_RETRY_MS)
            {
                if (rx_safe_stopped() != 0U)
                {
                    rx_try_restart();
                }
                else
                {
                    rx_begin_abort(0U);     /* once durus */
                }
            }
            else
            {
                /* Denemeler arasi bekleme suresi dolmadi. */
            }
            break;

        case UART_RX_PHASE_FAULT:
            if (s_recover_request != 0U)
            {
                s_recover_request = 0U;

                /* Acik kurtarma istegi: once DURUS dogrulanir. Donanim
                   hala aktifken yeni tampon/start kurmak, calisan DMA'nin
                   ustune yazmak demektir. */
                if (rx_safe_stopped() != 0U)
                {
                    s_rx_quiescent = 1U;
                    rx_open_recovery_period();
                    s_restart_tick = now - UART_RX_RESTART_RETRY_MS;
                    s_abort_issued = 0U;
                    rx_set_phase(UART_RX_PHASE_RETRY_WAIT);
                }
                else
                {
                    rx_open_recovery_period();
                    rx_begin_abort(0U);
                }
            }
            break;

        default:
            /* STOPPED/STARTING/RUNNING: toparlanma adimi yok. */
            break;
    }
}


uint8_t uart_rx_request_recovery(void)
{
    if (s_phase != UART_RX_PHASE_FAULT)
    {
        return 0U;      /* calisan yone dokunulmaz */
    }

    s_recover_request = 1U;
    s_recover_pending = 1U;   /* servis turunun toparlanma adimini acar */
    return 1U;          /* istegin KAYDI; basarili toparlanma DEGIL */
}


uint8_t uart_rx_is_quiescent(void)
{
    return s_rx_quiescent;
}


/* Bekleyen aday, UART_RX_FRAME_TIMEOUT_MS boyunca URETICI ILERLEMEDEN
   duruyorsa dusurulur. Bozuk bir LENGTH alani arkasindaki gecerli cerceveyi
   sonsuza kadar bekletmesin.

   Sira onemli (R5): ONCE butceli tuketim, SONRA zaman asimi karari.
   Backlog varsa ya da uretici ilerlemisse zaman asimi VERILMEZ. */
static void check_frame_timeout(void)
{
    uint32_t produced;
    uint32_t now;

    if ((s_huart == NULL) || (s_parser.len == 0U))
    {
        return;                      /* bekleyen aday yok: son tarih de yok */
    }

    /* Gecerli ornek alinamiyorsa KARAR VERILMEZ: ilerleme olup olmadigini
       bilmeden aday dusurmek, akmakta olan cerceveden bayt atmaktir. */
    if (uart_rx_sample_producer(&produced) == 0U)
    {
        return;
    }

    if (produced != s_consumed)
    {
        /* Backlog var: once tuket. Zaman asimi bu tur verilmez. */
        (void)uart_rx_service_budget(UART_RX_SERVICE_BUDGET);
        return;
    }

    now = HAL_GetTick();

    if (produced != s_last_producer)
    {
        /* Uretici ilerlemis: pencere bastan baslar. */
        s_last_producer   = produced;
        s_frame_base_tick = now;
        return;
    }

    if ((now - s_frame_base_tick) < UART_RX_FRAME_TIMEOUT_MS)
    {
        return;
    }

    frame_parser_timeout(&s_parser, frame_received, NULL);
    uart_rx_stats.frame_timeouts++;

    /* Mudahale ani, uretici ilerleme aninDAN AYRI tutulur: aday hala
       varsa YENI bir 50 ms penceresi kurulur, uretici ilerlemis gibi
       GOSTERILMEZ. Aday kalmadiysa son tarih tamamen kalkar (parser.len
       sifir oldugu icin bu fonksiyon zaten erken doner). */
    s_frame_base_tick = now;
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


uint8_t uart_rx_test_get_restart_tries(void)
{
    return s_restart_tries;
}


uint8_t uart_rx_test_recovery_active(void)
{
    return s_recovery_active;
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
   Tuketici baglaminda calisir: uart_rx_service -> uart_rx_service_budget ->
   frame_parser_feed -> buraya.

   R5: BU KATMAN ARTIK PROTOKOLU YORUMLAMIYOR. Sira takibi ve joystick
   cozme uygulama handler'ina tasindi (app_protocol.c); tasima katmani
   yalnizca ayristirma ve teslimden sorumlu. Boylece M1'de uart_comm icine
   tasinacak cekirdek, protokol bilgisinden bagimsiz kaliyor.

   info->payload YALNIZCA bu cagri suresince gecerli; saklanacaksa
   kopyalanmali. Cerceve sayisi burada tutulmuyor: s_parser.frames_ok zaten
   ayni bilgiyi veriyor. */
static void frame_received(const frame_info_t *info, void *user_data)
{
    (void)user_data;

    if (s_handler != NULL)
    {
        s_handler(info, s_handler_user);
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
/* --- R4: kesme tarafinin RX kapilari ---
   Ucu de YALNIZCA kayit yapar. Durum gecisi, HAL cagrisi ve karar sahibi
   tuketici baglamidir (bolum 5.1): kesme icinde HAL'i yeniden baslatmak,
   abort suren bir donanimi ikinci kez durdurmak demektir. */

void uart_rx_on_error(UART_HandleTypeDef *huart, uint32_t error)
{
    if ((s_huart == NULL) || (huart->Instance != s_huart->Instance))
    {
        return;
    }

    uart_rx_stats.error_events++;
    uart_rx_stats.last_error = error;      /* DEGER olarak alinir */
    s_rx_error_gen++;
    s_rx_error = 1U;
}


void uart_rx_on_abort_complete(UART_HandleTypeDef *huart)
{
    if ((s_huart == NULL) || (huart->Instance != s_huart->Instance))
    {
        return;
    }

    uart_rx_stats.abort_complete_events++;
    s_abort_done = 1U;
    /* Callback'in gelmesi DURUS KANITI DEGILDIR: tuketici yine donanimi
       okur (bolum 6.4). Bu yalnizca bir uyandirma/bilgi kaydidir. */
}


void uart_rx_on_uart_irq_exit(void)
{
    if (s_huart == NULL)
    {
        return;
    }

    /* YALNIZCA calisan bir oturumun saglıksiz hale gelmesi bildirilir.
       FAULT/ABORTING/RETRY_WAIT sirasinda her ilgisiz TX TC kesmesinde
       yeni bir RX hata donemi acilmasi, butceyi sonsuza kadar tazeler ve
       FAULT'a ulasmayi engellerdi. */
    if (s_phase != UART_RX_PHASE_RUNNING)
    {
        /* STARTING bilerek DISARIDA: oturum kurulurken DMAR/EN henuz set
           olmayabilir ve o pencerede gelen bir TX TC kesmesi SAHTE saglik
           hatasi uretirdi. STARTING'in sagligini zaten owner kendisi
           rx_session_healthy() ile dogruluyor. */
        return;
    }

    if (rx_hw_receiving(s_huart) != 0U)
    {
        return;                  /* alim saglikli: bildirilecek sey yok */
    }

    /* Ayni oturumun yinelenen saglik kaydi BIRLESTIRILIR: bayrak zaten
       set ise ikinci kez sayilmaz. */
    if (s_health_bad == 0U)
    {
        s_health_bad = 1U;
        uart_rx_stats.irq_health_events++;
    }
}


/* HAL'in __weak callback'leri. Yon KARARI burada verilmez; ErrorCode TEK
   KEZ okunup deger olarak dagitilir (bolum 6.1 P1). */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    uint32_t error = huart->ErrorCode;     /* TEK okuma */

    uart_rx_on_error(huart, error);
}


void HAL_UART_AbortReceiveCpltCallback(UART_HandleTypeDef *huart)
{
    uart_rx_on_abort_complete(huart);
}

> Arşiv: bu belgedeki eski dosya adları ve test kayıtları tarihsel bilgidir. Güncel kullanım [UART_COMM_KULLANIM.md](../../UART_COMM_KULLANIM.md) içindedir.

# UART alım zinciri — mimari ve kritik noktalar

> 6 Ekim 2026 güncellemesi: Bu belgenin eski alım/sıra anlatımı tarihsel kayıttır. Güncel birleşik RX/TX ve FreeRTOS kullanımı [UART_COMM_KULLANIM.md](../../UART_COMM_KULLANIM.md), mimari ve son kabul [UART_BIRLESIK_MIMARI_VE_UYGULAMA_YOL_HARITASI.md](../../UART_BIRLESIK_MIMARI_VE_UYGULAMA_YOL_HARITASI.md) ve [UART_RTOS_UYGULAMA_PLANI.md](../../UART_RTOS_UYGULAMA_PLANI.md) içindedir.

Proje: `UART_IDLE_DMA` · STM32F407VG · STM32CubeIDE 1.19 · HAL
Durum tarihi: 30 Eylül 2026 · Kapsanan modüller: M2–M6

Bu belge, koda dokunmadan önce bilinmesi gerekenleri anlatır. Geliştirme planı
ve yapılacaklar listesi ayrı belgededir: [UART_GELISTIRME_PLANI.md](UART_GELISTIRME_PLANI.md).

---

## 1. Ne yapıyor

PC'den UART üzerinden gelen bayt akışından, doğrulanmış protokol paketlerini
çıkarır. Şu an joystick verisi (X/Y) ve sıra numarası takibi uygulanmış
durumda; komut uygulama ve yanıt gönderme henüz yok.

Alım zinciri baştan sona çalışır ve donanım üzerinde doğrulanmıştır.

---

## 2. Katmanlar

```
TEL                   gerilim seviyeleri, 8N1 çerçeveleme
                              ↓
USART2 donanımı       start/stop bitlerini soyar, DR'ye bir bayt koyar
                              ↓
DMA1 Stream5          DR'den RxData[] içine yazar (circular), CPU karışmaz
                              ↓
RxData[256]           ham bayt akışı, dairesel tampon
                              ↓
uart_rx_drain()       read_pos/write_pos ile ardışık aralıkları çıkarır
                              ↓
parser_besle()        sınır bulur, VERSION/LENGTH/CRC doğrular
                              ↓
paket_geldi()         sıra takibi, X/Y çözümü
```

Her katman yalnızca altındakinin çıktısını görür. `parser.c` UART, DMA veya
dairesel tampon nedir bilmez; girdisi düz bir bayt dizisidir. Bu, M4'te yazılan
29 birim testinin donanım olmadan koşabilmesinin ve M5–M6'da `parser.c`'ye tek
satır dokunulmamasının sebebidir.

---

## 3. Protokol

```
AA 55 | VERSION | TYPE | LENGTH | SEQUENCE | PAYLOAD | CRC16
  2        1       1      1         2          N        2     bayt
```

- `LENGTH` **yalnızca payload boyutudur**. Toplam paket = `9 + LENGTH`.
- Çok baytlı alanlar **little-endian** (düşük bayt önce).
- `VERSION` şu an `1`.
- Maksimum payload 55, maksimum toplam 64 bayt.

### CRC sözleşmesi

CRC-16/IBM-3740 (CRC-16/CCITT-FALSE):

| Parametre | Değer |
|---|---|
| Polinom | `0x1021` |
| Başlangıç | `0xFFFF` |
| Yansıtma (in/out) | yok |
| Çıkış XOR | `0x0000` |

CRC, **`VERSION` alanından payload sonuna kadar** hesaplanır. Başlangıç baytları
ve CRC alanının kendisi hesaba katılmaz. Hatta düşük bayt önce gider.

"CRC-16" tek bir algoritma değildir; bu parametreler iki tarafta birebir aynı
olmadan hiçbir paket doğrulanmaz.

### Mesaj türleri

| TYPE | Anlam | Payload |
|---|---|---|
| `0x10` | Joystick verisi | X: int16, Y: int16 (4 bayt) |
| `0x11` | Joystick modu | 0 kapalı / 1 açık (1 bayt) |
| `0x20` | Çıkış ayarla | çıkış no + durum (2 bayt) |
| `0x80` | Komut yanıtı | TYPE + sonuç kodu (2 bayt) — henüz uygulanmadı |

### Referans vektörler

```
CRC("123456789")                        = 0x29B1
X=+1000, Y=-500, SEQ=1  →  AA 55 01 10 04 01 00 E8 03 0C FE 46 59   (13 bayt)
TYPE 0x11, payload {01}, SEQ=2          →  10 bayt, CRC 0xED4E
TYPE 0x11, payload yok, SEQ=7           →   9 bayt, CRC 0x4FD9
TYPE 0x20, payload {AA,55}, SEQ=300     →  11 bayt, CRC 0x8F8D
```

---

## 4. Modüller

| Dosya | Sorumluluk | Bağımlılık |
|---|---|---|
| `Core/Src/crc16.c` | CRC-16/CCITT-FALSE hesabı | yok (saf C) |
| `Core/Src/frame.c` | Çerçeve **oluşturma** (gönderme yönü) | `crc16` |
| `Core/Src/parser.c` | Çerçeve **ayrıştırma** (alma yönü) | `crc16`, `frame.h` (sabitler) |
| `Core/Src/uart_rx.c` | DMA tamponu, okuma konumu, HAL callback'leri, uygulama durumu | HAL, `parser`, `frame` |
| `Core/Src/tests.c` | 29 birim testi + loopback testi | tümü |
| `Core/Src/main.c` | Yalnızca CubeMX kurulumu + `uart_rx_start` / `uart_rx_service` | `uart_rx`, `tests` |
| `tools/protokol.py` | Bağımsız referans uygulaması | Python 3 |

`frame.c` ve `parser.c` birbirinin tersidir: biri veriden bayt üretir, diğeri
baytlardan veri çıkarır.

`uart_rx.c` alım altyapısının tek sahibidir. Tampon, okuma konumu ve ayrıştırıcı
örneği dosya kapsamında `static`'tir (`s_` önekiyle); dışarıya yalnızca üç
fonksiyon ve iki salt-okuma yapı açılır:

```c
HAL_StatusTypeDef uart_rx_start(UART_HandleTypeDef *huart);
void              uart_rx_service(void);    /* bildirim varsa tüket */
void              uart_rx_drain(void);   /* koşulsuz tüket */
extern uart_rx_stats_t uart_rx_ist;     /* kesme olayları */
extern uart_rx_state_t      uart_rx_state;   /* joystick + sıra durumu */
```

`tests.c` üretim kodunun bağımlılığı **değildir**: `main.c`'deki iki çağrıyı
(`birim_testleri_kosur`, `loopback_testi_kosur`) kaldırmak yeterlidir.

### Arayüzler

```c
uint16_t crc16_ccitt(const uint8_t *veri, uint16_t uzunluk);

uint8_t  frame_build(uint8_t *hedef, uint8_t hedef_boyut,
                       uint8_t tur, uint16_t sira,
                       const uint8_t *payload, uint8_t payload_uzunluk);
uint8_t  frame_build_joystick(uint8_t *hedef, uint8_t hedef_boyut,
                                int16_t x, int16_t y, uint16_t sira);

void parser_sifirla(parser_t *p);
void parser_besle(parser_t *p, const uint8_t *veri, uint16_t uzunluk,
                  paket_geri_cagri_t geri_cagri, void *kullanici);
```

`frame_build` hata durumunda `0` döndürür (geçerli paket en az 9 bayt
olduğundan karışma ihtimali yoktur). Dönüş değeri kontrol edilmeden
gönderilmemelidir.

---

## 5. Kritik noktalar

Bu bölüm, yanlış anlaşıldığında sessiz hataya yol açan noktaları listeler.

### 5.1 IDLE, USART kesmesine bağlıdır — DMA yetmez

DMA veriyi kaybetmeden taşır ama **haber vermez**. DMA'nın ürettiği olaylar
yalnızca HT (yarım tampon) ve TC (tam tampon), yani sabit bayt sayılarıdır.

100 Hz'de 13 baytlık joystick paketi = 1300 bayt/s. 256 baytlık tamponda:

| Olay | Tetiklenme aralığı |
|---|---|
| HT (128. bayt) | ~98 ms |
| TC (256. bayt) | ~197 ms |
| IDLE | son bayttan ~87 µs sonra |

IDLE olmadan bir paketin geldiğini öğrenmek 98 ms sürebilir. IDLE bayrağı
USART2'nin durum register'ındadır ve CPU'ya ulaşmasının tek yolu USART2
kesmesidir:

```
IDLE bayrağı → USART2_IRQHandler → HAL_UART_IRQHandler → HAL_UARTEx_RxEventCallback
```

`USART2_IRQHandler` yoksa `HAL_UARTEx_ReceiveToIdle_DMA()` çalışır, DMA veri
toplar, ama IDLE callback'i **hiç tetiklenmez**.

IDLE ayrıca tampon boyutu ile gecikme arasındaki bağı koparır: tamponu taşma
payına göre seçersin, gecikmeyi IDLE halleder.

### 5.2 IDLE paket sınırı belirlemez

IDLE'ın tek işi "tamponda yeni veri var, bak" demektir. Paket sınırını **CRC
doğrulaması** belirler.

PC iki paketi birleştirip tek seferde gönderirse tek IDLE gelir ve ayrıştırıcı
ikisini de çözer. Bir paket ikiye bölünürse iki IDLE gelir ve ayrıştırıcı yarım
paketi saklayarak tamamlar. Her ikisi de test edilmiştir (S4, S2).

### 5.3 Callback'in `Size` parametresi mutlak konumdur

```c
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
```

`Size`, **"kaç yeni bayt geldi" değildir**; tamponun başından itibaren dolu
bayt sayısıdır.

| Olay | HAL'in verdiği `Size` |
|---|---|
| HT | her zaman `tampon_boyu / 2` |
| TC | her zaman `tampon_boyu` |
| IDLE | `tampon_boyu - NDTR` (o anki konum) |

Sonuçları: ardışık iki olay aynı `Size`'ı bildirebilir, sarımda değer küçülür,
HT gerçekte kaç yeni bayt geldiğine bakmaz. `Size`'ı yeni bayt sayısı sanıp
`parser_besle(&p, RxData, Size, ...)` yazmak aynı baytların tekrar tekrar
ayrıştırılmasına yol açar.

Kodda `Size` yalnızca `son_size` değişkenine gözlem amacıyla kopyalanır;
tüketimde kullanılmaz.

### 5.4 Yazma konumu NDTR'den türetilir

```c
write_pos = (boyut - __HAL_DMA_GET_COUNTER(&hdma_usart2_rx)) % boyut;
```

NDTR kalan transfer sayısını tutar ve her baytta **azalır**. Yazılan bayt
sayısı `boyut - NDTR`'dir.

Modulo tek bir uç durum için gereklidir: DMA 256. baytı yazıp NDTR'yi henüz
yeniden yüklemediğinde `0` okunur, `256 - 0 = 256` çıkar ve bu geçersiz bir
indekstir. Modulo onu `0`'a çevirir. Bu satır olmadan dizi dışına erişilir.

`write_pos` bir durum değişkeni değil, **anlık fotoğraftır** — saklanmaz, her
turda yeniden okunur. DMA kod çalışırken yazmaya devam ettiği için saklanan
değer anında bayatlar.

### 5.5 Sahiplik ayrımı yarışı önler

```
write_pos  →  DMA yazar (NDTR üzerinden), tüketici yalnızca okur
read_pos   →  yalnızca tüketici yazar, DMA hiç bakmaz
```

İki taraf farklı değişkenlere sahiptir. Bu yüzden kilit, kritik bölüm veya
atomik işlem gerekmez. Tasarımın en önemli özelliği budur ve korunmalıdır:
`read_pos`'a başka bir bağlamdan yazılırsa bu garanti kaybolur.

### 5.6 Konumların eşitliği iki şey anlamına gelir

`read_pos == write_pos` "yeni veri yok" demektir. Ama **tam bir tur üzerine
yazılmışsa da** aynı görünür. 256 baytın tamamı ezilmişse konumlar yine eşit
çıkar ve kayıp fark edilmez.

Modüler aritmetik tam tur taşmasını tespit edemez. Mevcut koruma, **zamanında
tüketmektir** — başka bir mekanizma yoktur. Taşma tespiti gerekirse toplam
üretim/tüketim sayacı veya tur sayacı tasarlanmalıdır.

Somut sınır: 115200 baud'da tampon 256 bayt / 11520 bayt/s ≈ **22 ms** içinde
tüketilmelidir (kesintisiz tam hızlı trafikte). HT/TC ile her yarım tamponun
ezilmeden tüketilmesi hedeflenirse bütçe ~11 ms olur.

### 5.7 Sarımda iki aralık, tek kod yolu

Sarım olduğunda yeni baytlar bellekte ardışık değildir. `uart_rx_drain` her turda
**tek** ardışık aralık tüketir:

- Sarım yoksa: `read_pos … write_pos-1`, sonra `read_pos = write_pos`
- Sarım varsa: `read_pos … boyut-1`, sonra `read_pos = 0`

İkinci durumda kalan kısmı döngünün sonraki turu "sarım yok" hâli olarak
halleder. Böylece sarım için ayrı bir kod yolu yazılmaz.

Sarım sınırında bölünen paket, `rx_parser`'ın yarım paketi saklaması sayesinde
kaybolmaz — M4'teki S2 senaryosunun aynısıdır.

### 5.8 `rx_parser` çağrılar arasında yaşamalıdır

`uart_rx_drain` içinde yerel tanımlanırsa her çağrıda sıfırlanır ve yarım paketler
kaybolur. Sarımda önce 6 bayt, sonra 14 bayt beslenir; ayrıştırıcının ilk 6
baytı hatırlaması zorunludur.

### 5.9 Hatalı adayda tam bir bayt atılır

`AA 55` kanıt değil, **aday** işaretidir — payload içinde de geçebilir
(TYPE `0x20` örnek paketi tam olarak bunu içerir). Kanıt ancak uzunluk ve CRC
doğrulanınca oluşur.

Aday çürüdüğünde tamponun tamamı değil **yalnızca bir bayt** atılır. Sebebi:
`tampon[0]`'ın paket başlangıcı olmadığı kanıtlanmıştır, `tampon[1]` hakkında
hiçbir şey bilinmez. Bozuk adayın içinde gerçek bir paketin başlangıcı olabilir.

Bedeli: bozuk bir paket 13 baytlık bir eleme turuna yol açar ve bu sırada
`sayac_surum_hata` gibi "gürültü" sayaçları artabilir. Bu bir hata değil, yeniden
tarama davranışının doğal sonucudur. Asıl kanıt `sayac_gecerli`'dir.

### 5.10 `volatile` neyi çözer, neyi çözmez

`volatile`, derleyiciye "bu değeri register'da önbelleklemeden her seferinde
bellekten oku" der. **Atomiklik sağlamaz.**

| Değişken | `volatile`? | Sebep |
|---|---|---|
| `s_rx_pending`, `uart_rx_stats.*` | evet | Kesme yazar, main okur — iki farklı çalışma bağlamı |
| `uart_rx_state.*` (`last_seq`, `next_seq`, `seq_gaps`, `joy_x`, `joy_y`), `s_read_pos` | hayır | Yalnızca main bağlamında yazılıp okunur |

`paket_geldi` bir kesme içinde çalışmaz: `while(1) → uart_rx_service → uart_rx_drain →
parser_besle → paket_geldi` zinciri main bağlamındadır.

`volatile`'ın çözmediği şeyler: `sayac++` üç işlemdir (oku, artır, yaz) ve
bölünebilir; iki ayrı değişken birlikte tutarlı okunamaz. İki bağlam da aynı
değişkene yazmaya başlarsa kritik bölüm veya RTOS kuyruğu gerekir.

### 5.11 `payload` işaretçisinin ömrü

```c
typedef struct {
    uint8_t        tur;
    uint16_t       sira;
    uint8_t        uzunluk;
    const uint8_t *payload;
} paket_bilgi_t;
```

`payload`, ayrıştırıcının iç tamponuna işaret eder ve **yalnızca geri çağrı
süresince geçerlidir**. Geri çağrı döndükten sonra o baytlar silinir/üzerine
yazılır. Saklanacaksa kopyalanmalıdır.

### 5.12 Tampon boyutu tek kaynaktan gelmeli

```c
HAL_UARTEx_ReceiveToIdle_DMA(&huart2, RxData, sizeof(RxData));
```

Üçüncü parametre DMA'ya "şu kadar yerin var" der. Dizi boyutundan farklı bir
sayı yazılırsa DMA komşu değişkenlerin üzerine yazar; donanım bunu yapar ve
hiçbir uyarı vermez. `sizeof` kullanmak iki sayının ayrışmasını engeller.

### 5.13 DMA tamponu CCM belleğinde olmamalı

STM32F407'de DMA1, CCM RAM bölgesine (`0x10000000`) erişemez. Normal global
diziler SRAM'e gider, özel bir şey yapmak gerekmez — ancak linker script'i
değiştirilirse bu kısıt hatırlanmalıdır.

### 5.14 Loopback testinde RX, TX'ten önce başlatılmalı

`PA2`–`PA3` jumper'ı ile yapılan loopback testinde `HAL_UART_Transmit`
çağrıldığı anda baytlar RX pininde görünür. DMA hazır değilse baytlar kaybolur
veya ORE hatası oluşur. Kodda `HAL_UARTEx_ReceiveToIdle_DMA` çağrısı gönderimden
öncedir.

### 5.15 Sıra beklentisi gelen değerden türetilir

```c
beklenen_sira = (uint16_t)(paket->sira + 1U);   /* doğru */
beklenen_sira++;                                 /* yanlış */
```

`beklenen_sira++` kullanılırsa tek bir kayıptan sonra kalıcı olarak bir geri
kalınır ve sonraki her paket kayıp sayılır. Gelen değerden türetmek kaybı bir
kez raporlayıp senkronizasyonu geri getirir.

`(uint16_t)` cast'i `65535 → 0` sarımını kendiliğinden halleder; ayrı bir
kontrol gerekmez.

İlk pakette karşılaştırılacak bir beklenti yoktur (gönderenin hangi değerden
başladığı bilinemez). `sira_baslatildi` bayrağı ilk paketi referans olarak alır.

**Varsayım:** gönderen tüm mesaj türleri için **tek ortak sayaç** kullanır. PC
tarafı tür başına ayrı sayaç kullanırsa bu mantık yanlış kayıp raporlar; planın
1. adımında netleştirilecek açık bir karardır.

### 5.16 Hata toparlanması kesme içinde yapılmaz

`HAL_UART_ErrorCallback` yalnızca sayaç artırır ve `s_rx_error` bayrağını
kaldırır. Toparlanma `uart_rx_service()` içinde, tüketici bağlamında yapılır.
Planın 11. bölümü bunu ister: yeniden başlatma sahibi task/döngüdür.

Üç kural:

1. **`HAL_UART_AbortReceive`, `HAL_UART_Abort` değil.** İkincisi sürmekte olan
   bir TX'i de iptal ederdi; plan bunu açıkça yasaklıyor.
2. **`RxState == HAL_UART_STATE_BUSY_RX` ise dokunulmaz.** Tek bir gürültü
   hatasında HAL alımı sürdürür; müdahale çalışan bir alımı bozar.
3. **`frame_parser_discard`, `frame_parser_init` değil.** `init` sayaçları da
   sıfırlar ve her toparlanmada hata geçmişini siler. `discard` yalnızca
   bekleyen adayı atar, attığı baytları `bytes_dropped`'a ekler.
4. **Yeniden başlatmadan önce hata bayrakları temizlenir.** Bu kural olmadan
   tekrar denemek hiçbir şey değiştirmez; aşağıda ayrıca ele alınıyor.

Toparlanmada `s_read_pos = 0` yapılır, çünkü DMA tamponun başından yeniden
başlar.

#### Bildirim ile borç ayrımı

`s_rx_error` yalnızca **bildirimdir**; toparlanma **borcu** `s_recover_pending`
içinde durur. Bayrağı temizleyip tek deneme yapıp geçmek ölümcüldü:

- `HAL_UART_AbortReceive` DMAR ve IDLEIE'yi kapatır,
- yeniden başlatma düşerse ikisi kapalı kalır,
- kapalıyken **hiçbir callback oluşamaz**, yani bayrağı yeniden set edecek
  kimse yoktur → RX kalıcı ölür, geride tek iz `restart_fails = 1`.

Borç yalnızca üç durumda kapanır: alım gerçekten geri geldi, HAL zaten
`BUSY_RX` (müdahale gereksiz), veya kalıcı hataya düşüldü.

#### Sebebi temizlemeyen tekrar işe yaramaz

En kritik satır budur:

```c
__HAL_UART_CLEAR_OREFLAG(s_huart);
s_huart->ErrorCode = HAL_UART_ERROR_NONE;
```

`AbortReceive` hata bayraklarına dokunmaz. ORE duruyorsa
`HAL_UARTEx_ReceiveToIdle_DMA`, EIE'yi açar açmaz hata kesmesi doğurur; HAL
alımı iptal eder ve `HAL_ERROR` döner. HAL kaynağındaki not bunu açıkça yazar:
*"In case of errors already pending when reception is started, interrupts may
have already been raised and lead to reception abortion (overrun error for
instance)."* Yani yeniden başlatma **tam da gerektiği anda** düşer. Sebebi
temizlemeden 5 kez denemek 5 kez aynı sonucu verir.

F4'te PE/FE/NE/ORE/IDLE tek yolla düşürülür: SR oku, DR oku. Beş makro
(`__HAL_UART_CLEAR_PEFLAG` … `_IDLEFLAG`) birebir aynı şeyi yapar, bu yüzden
tek çağrı yeterlidir.

#### Sınırlı tekrar ve kalıcı hata durumu

`UART_RX_RESTART_RETRY_MS = 5` denemeler arasına bekleme koyar; olmasa
`while(1)` saniyede binlerce sonuçsuz abort/restart çifti çalıştırırdı.
`UART_RX_RESTART_MAX_TRIES = 5` denemeden sonra `uart_rx_stats.faulted = 1`
olur ve borç kapanır: daha fazla denemek anlamsız. Amaç **sessizce ölmek
yerine görünür ölmek**. Tek çıkış yolu `uart_rx_start()`.

`HAL_UART_AbortReceive` dönüşü de kontrol edilir: DMA abort'unu beklerken
`HAL_TIMEOUT` dönebilir, o durumda periferik belirsiz haldedir ve devam etmek
yanlış olur.

Sayaçlar: `restarts`, `restart_fails`, `faulted`.

### 5.17 Zaman aşımı kararı ayrıştırıcıya ait değil

`parser.c` saat bilmez ve bilmemelidir — HAL/FreeRTOS bağımsızlığı 29 birim
testinin donanımsız koşabilmesinin sebebi. Bu yüzden sorumluluk ayrılmıştır:

```c
/* parser.h — kararı ÇAĞIRAN verir */
void frame_parser_timeout(frame_parser_t *p, frame_handler_t handler, void *user_data);

/* uart_rx.c — saati olan taraf */
if ((HAL_GetTick() - s_last_rx_tick) >= UART_RX_FRAME_TIMEOUT_MS) { ... }
```

Zaman aşımı tamponu **boşaltmaz, bir bayt atar** — 5.9'daki aynı gerekçe:
bozuk adayın içinde gerçek bir çerçeve başlamış olabilir.

`UART_RX_FRAME_TIMEOUT_MS = 50`: en büyük çerçeve 64 bayt, 115200 8N1'de
~5,6 ms. 50 ms bunun ~9 katı; işletim sistemi kaynaklı parçalanmaya tolerans
bırakır, tıkanmayı sınırlı tutar.

#### Tetikleme koşulu "bildirim gelmedi" değil "tampon ilerlemedi"

Bu ikisi aynı şey değildir ve karıştırmak geçerli veriyi bozar. **Bir yayın
(burst) sürerken hiçbir bildirim oluşmaz:**

- IDLE, son bayttan ~87 µs sonra gelir,
- HT/TC yalnızca 128. ve 256. baytta tetiklenir.

57 baytlık bir devam yayını 115200'de 4,95 ms sürer; o süre boyunca DMA
tampona yazar ama `s_rx_pending` sıfırdır. Yalnızca zaman damgasına bakan bir
kontrol, **akmakta olan geçerli bir çerçeveden bayt atar**. Bu yüzden karar
verilmeden önce DMA'nın ilerleyip ilerlemediğine bakılır:

```c
if ((HAL_GetTick() - s_last_rx_tick) < UART_RX_FRAME_TIMEOUT_MS) return;

if (dma_write_pos() != s_read_pos)   /* çerçeve hâlâ akıyor */
{
    uart_rx_drain();                 /* zaman aşımı yok: tüket */
    return;
}
```

Sıra önemlidir: süre kontrolü önce gelir, böylece normal işleyişte fazladan
NDTR okuması yapılmaz.

Bu düzeltmenin gerekliliği T4 testiyle ölçülerek gösterildi (bkz. 6. bölüm):
düzeltme kapalıyken 64 baytlık geçerli bir çerçeve tamamen kayboluyor.

---

## 6. Doğrulama durumu

### Birim testleri (kartta, `testleri_kosur()`)

| Grup | Adet | İçerik |
|---|---|---|
| CRC | 6 | Standart vektör `0x29B1`, boş girdi, tek bayt, protokol paketi |
| Paketleyici | 9 | Referans baytlar, sınır kontrolleri, 12/13 bayt sınır testi |
| Ayrıştırıcı | 14 | Bölünmüş, birleşik, çöplü, bozuk CRC, yarım, `AA 55` payload, kurtarma, tıkanıklık + zaman aşımı (S13) |
| **Toplam** | **29** | `test_gecen == 29`, `test_kalan == 0` |

Sonuçlar `test_sonuc[]`, `test_gecen`, `test_kalan` üzerinden debugger'da
okunur (`Live Expressions`).

### Donanım doğrulamaları

PA2–PA3 loopback, STM32F4DISCOVERY, ST-LINK üzerinden GDB ile okundu
(30 Eylül 2026).

Loopback testleri birim testlerinden **ayrı** sayaçlar kullanır
(`lb_gecen`, `lb_kalan`, `lb_sonuc[]`): birim testleri donanımsız koşar,
bunlar jumper ve çalışan bir UART ister. İkisini aynı sayaçta toplamak
"29 test geçti" ifadesinin anlamını bozardı.

| Test | Ne kanıtlar | Ölçüm |
|---|---|---|
| Birim testleri | Protokol mantığı, donanımsız | `test_gecen=29`, `test_kalan=0` |
| T1 | Uçtan uca alım | `frames_ok=1`, `joy_x=1000`, `joy_y=-500` |
| T2 | Sarım: 40 çerçeve / 520 bayt, tamponda iki tur | `frames_ok=40`, `last_seq=40`, `next_seq=41`, `seq_gaps=0`, `bytes_dropped=0`, `len=0` |
| T3a | Bozuk `LENGTH` gerçekten tıkanıklık yaratıyor | `len=7` (64 bayt bekliyor) |
| T3b | 50 ms sessizlikte zaman aşımı **kendiliğinden** ateşleniyor | `frame_timeouts=1`, `bytes_dropped` +7, `len=0` |
| T4 | Sınırda akmakta olan geçerli çerçeve **düşmüyor** | `frames_ok` +1, `frame_timeouts` değişmedi, `last_seq=41`, `seq_gaps=0` |
| T5a | `USART_CR1_SBK` ile **gerçek** framing error | `error_events=1`, `last_error=0x04` (FE) |
| T5b | Hatadan sonra alım hâlâ çalışıyor | `restarts=1`, sonraki çerçeve çözüldü: `joy_x=-250`, `joy_y=750` |
| T6a | 5 başarısız denemeden sonra kalıcı hata durumu | `restart_fails=5`, `faulted=1` |
| T6b/c | `uart_rx_start()` kalıcı hatadan çıkarıyor ve alım geri geliyor | `faulted=0`, `frames_ok=1`, `joy_x=12`, `joy_y=-34` |
| **Toplam** | | `lb_gecen=10`, `lb_kalan=0` |

Olay sayaçları: `rx_events=49`, `idle_events=45`, `ht_events=2`,
`tc_events=2`. T6 öncesi toplamlar: `lb_frames_ok=42`, `lb_last_seq=42`,
`lb_seq_gaps=0`, `lb_bytes_dropped=7`.

`last_size=8` ölçümü (T2 sonunda), 5.3'teki tespitin doğrudan kanıtıdır:
520 bayt alınmış olmasına rağmen `Size` **mutlak konumu** bildiriyor, gelen
bayt sayısını değil.

#### T4 bir regresyon testidir: düşmesi de ölçüldü

Bir testin geçmesi tek başına bir şey kanıtlamaz — hatayı yakalayıp
yakalamadığı bilinmelidir. 5.17'deki `dma_write_pos()` kontrolü `#if 0` ile
kapatılıp aynı ikili kartta koşuldu:

| Ölçüm | Düzeltme açık | Düzeltme kapalı |
|---|---|---|
| `lb_sonuc[4]` (T4) | 1 = PASS | **2 = FAIL** |
| `frame_timeouts` | 1 | **2** — yayının ortasında ateşlendi |
| `lb_frames_ok` | 42 | **41** — 64 baytlık çerçeve kayboldu |
| `lb_bytes_dropped` | 7 | **71** = 7 + 64, çerçevenin tamamı çöp olarak elendi |
| `lb_seq_gaps` | 0 | **1** — SEQ 41 hiç gelmedi |

T4'ün devam yayınını `HAL_UART_Transmit_DMA` ile göndermesi zorunludur:
bloklayan `HAL_UART_Transmit` sırasında `uart_rx_service()` çağrılamaz ve hata
görünmez kalır. Yani naif kurulmuş bir test bu hatayı yakalamaz.

### Doğrulanmamış olanlar

- **Kalıcı hata durumu yalnızca test kancasıyla sınandı.** T6, gerçek bir
  donanım arızası değil `uart_rx_force_restart_fail()` kullanır; gerçek
  donanımda yeniden başlatmanın düşmesini deterministik üretmenin temiz bir
  yolu yok. Sınanan şey durum makinesi (tekrar aralığı, sınır, `faulted`,
  tek çıkış yolu), HAL'in davranışı değil.
- **ORE ile toparlanma sınanmadı.** T5 framing error üretir; taşma (ORE)
  yolu, `__HAL_UART_CLEAR_OREFLAG` gerekçesinin asıl kaynağı olduğu hâlde
  ölçülmedi. Bunun için okumayı kasıtlı geciktiren ayrı bir deney gerekir.
- Sürekli tam hızlı trafik altında en kötü gecikme **ölçülmedi**
- Kayıpsızlık iddia **edilemez**: taşma tespiti yok (bkz. 5.6)
- PC'den gerçek veri ile test edilmedi; yalnızca loopback
- FreeRTOS altında hiç çalıştırılmadı

---

## 7. Araç: `tools/protokol.py`

Protokolün Python'daki bağımsız referans uygulaması. C kodundan ayrı yazılmıştır;
iki uygulamanın aynı sonucu vermesi doğruluk kanıtı sayılır.

```bash
python tools/protokol.py test               # kendi testlerini koşar (9/9)
python tools/protokol.py vektor             # C için hazır test dizileri
python tools/protokol.py akis               # ayrıştırıcı test akışları + beklenen sayaçlar
python tools/protokol.py coz "AA 55 01 ..." # bayt dizisini alan alan çözer, CRC doğrular
```

`coz` komutu hata ayıklamada kullanılır: debugger'dan alınan baytlar
(`*RxData@13`, Number Format → Hex) yapıştırıldığında hangi alanın bozuk
olduğunu söyler.

---

## 8. Bilinen eksikler

| # | Konu | Nerede ele alınacak |
|---|---|---|
| 1 | USART2 ve DMA kesme önceliği `0` | M7 — FreeRTOS `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` kuralı |
| 2 | `NVIC_PRIORITYGROUP_0` | M7 — Cortex-M4 + FreeRTOS için `PRIORITYGROUP_4` |
| 3 | Bare-metal bayrak yerine task bildirimi | M7 |
| 4 | Taşma/kayıp tespiti | M9 — sayaç veya tur takibi (bkz. 5.6) |
| 5 | TX yolu, komut yanıtı (`0x80`), tekrar ayıklama | M9 |
| 6 | ORE ile toparlanmanın sınanması | Okumayı kasıtlı geciktiren ayrı deney (FE yolu T5 ile doğrulandı) |
| 7 | Joystick modu (`0x11`) davranışı, komut uygulama | M8 |
| 8 | Ortak/ayrı sıra sayacı kararı | PC arayüzü ile birlikte (plan Adım 1) |

---

## 9. Sürüm işaretleri

| Etiket | İçerik |
|---|---|
| `crc` | M2 — CRC modülü ve test tezgâhı |
| `m5-dma` | M3+M4+M5 — paketleyici, ayrıştırıcı, DMA/IDLE altyapısı |

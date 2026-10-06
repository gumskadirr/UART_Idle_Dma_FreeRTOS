> Arşiv: bu belgedeki eski dosya adları ve test kayıtları tarihsel bilgidir. Güncel kullanım [UART_COMM_KULLANIM.md](../../UART_COMM_KULLANIM.md) içindedir.

# Açık bulgular — RX hata toparlanması ve zaman aşımı

Tarih: 30 Eylül 2026
Durum: **KAPANDI.** Üç maddenin hepsi uygulandı ve kartta doğrulandı;
T6 için A seçeneği (test kancası) uygulandı. Ölçümler en altta, "Sonuç"
bölümünde. Belge, kararların neden böyle verildiğinin kaydı olarak duruyor.

Bu üç madde, `090a04f` ile eklenen hata toparlanması ve kısmi çerçeve zaman
aşımı kodunun incelemesinden çıktı.

---

## Bulgu 1 — Yeniden başlatma, tam da gerektiği anda başarısız olur

Bu nadir bir uç durum değil. HAL'in kendi yorumu
(`Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c:1807-1811`):

```
/* In case of errors already pending when reception is started,
   Interrupts may have already been raised and lead to reception abortion.
   (Overrun error for instance). */
status = HAL_ERROR;
```

### Zincir

1. `HAL_UART_AbortReceive` **ORE bayrağını temizlemez**; yalnızca DMAR ve
   IDLEIE'yi kapatır (`stm32f4xx_hal_uart.c:2013` ve `:2019`).
2. `HAL_UARTEx_ReceiveToIdle_DMA` EIE'yi yeniden açar → bekleyen ORE anında
   hata kesmesi doğurur → HAL alımı iptal eder → `HAL_ERROR` döner.
3. O noktada DMAR ve IDLEIE kapalıdır. **Artık hiçbir callback oluşamaz.**
   `s_rx_error` bir daha set edilmez, `s_rx_pending` bir daha set edilmez.

Sonuç: ORE sonrası ilk deneme muhtemelen başarısız olur ve RX kalıcı ölür.
Geride tek iz `restart_fails = 1`.

### Asıl ders

**Tekrar denemek tek başına işe yaramaz.** Sebebi temizlemeden 5 kez denemek
5 kez aynı sonucu verir. Doğru sıra:

```
AbortReceive → hata bayraklarını temizle (ORE/FE/NE/PE) → ErrorCode = NONE
             → yeniden başlat
```

### Yapılacaklar

| # | Değişiklik | Yer |
|---|---|---|
| 1a | `s_rx_error`'ı hemen temizleyip umut etmek yerine main bağlamında bir `s_recover_pending` latch'i. Yalnızca başarıda veya kalıcı hatada sıfırlanır | `Core/Src/uart_rx.c:61-65` |
| 1b | Yeniden başlatmadan önce hata bayraklarını temizle (`__HAL_UART_CLEAR_OREFLAG` vb.) — tekrar denemeyi anlamlı kılan tek şey | `uart_rx_recover()` |
| 1c | `UART_RX_RESTART_RETRY_MS` aralıklı, `UART_RX_RESTART_MAX_TRIES` sınırlı tekrar; meşgul döngüde art arda denemeyi engeller | `Core/Src/uart_rx.c:106-114` |
| 1d | Sınır aşılırsa açık kalıcı hata durumu: `uart_rx_faulted()` + sayaç. Sessizce ölmek yerine görünür ölmek | `uart_rx.h` / `uart_rx.c` |
| 1e | `HAL_UART_AbortReceive` dönüşü kontrol edilsin; DMA abort'u beklerken `HAL_TIMEOUT` dönebiliyor (`stm32f4xx_hal_uart.c:2033`) | `Core/Src/uart_rx.c:97` |

---

## Bulgu 2 — Zaman aşımı, DMA'ya yeni gelen baytı kaçırabilir

Pencere 87 µs değil, **~5 ms**. Kritik gerçek: *bir yayın (burst) sürerken
hiçbir bildirim yoktur.*

- IDLE: son bayttan ~87 µs sonra
- HT/TC: yalnızca 128. ve 256. bayt sınırında

57 baytlık bir devam yayını 115200'de 4,95 ms sürer. O süre boyunca DMA
tamponu yazıyor, `s_rx_pending` sıfır, ve `s_last_rx_tick` yalnızca
`Core/Src/uart_rx.c:171`'de — drain içinde veri bulunduğunda — yenileniyor.
Yayın t=48 ms'de başlarsa `while(1)` döngüsü t=50'de `check_frame_timeout`'a
girer, `len != 0` ve zaman damgası eski görünür, **geçerli çerçeveden bir bayt
atılır** → CRC düşer.

### Kavram hatası

Zaman aşımı koşulu "bildirim gelmedi" diye yazılmış; doğrusu "**tampon
ilerlemedi**". Bunlar aynı şey değil.

### Yapılacaklar

| # | Değişiklik | Yer |
|---|---|---|
| 2a | `write_pos` hesabını `dma_write_pos()` olarak ayır (şu an drain'e gömülü) | `Core/Src/uart_rx.c:158` |
| 2b | `check_frame_timeout`, karardan **önce** `dma_write_pos() != s_read_pos` kontrol etsin; farklıysa zaman aşımı değil `uart_rx_drain()` çağırsın (tick'i o yeniler) ve dönsün | `Core/Src/uart_rx.c:121-137` |

### M7 için sonuç

FreeRTOS'ta task yalnızca bildirimle uyanırsa, veri gelmediğinde **hiç
uyanmaz** ve zaman aşımı hiç kontrol edilmez. Bekleme süreli olmalı:
`ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10))`.

---

## Bulgu 3 — Test kapsamı: S13 zinciri sınamıyor

`Core/Src/tests.c:311` `frame_parser_timeout()`'u doğrudan çağırıyor.
Algoritma doğrulanıyor, ama *kararın verilmesi* — gerçek 50 ms bekleme →
`uart_rx_service()` → otomatik zaman aşımı — sınanmıyor.

### T3 — otomatik zaman aşımı

`LENGTH=55` diyen 7 baytlık başlığı gönder, sessizlikte 60 ms boyunca
`uart_rx_service()` döndür.
Beklenen: `frame_timeouts == 1`, `bytes_dropped == 7`, `len == 0`.

### T4 — sınır testi (bulgu 2'nin regresyon testi)

Engel: `HAL_UART_Transmit` blokluyor, yani yayın sürerken
`uart_rx_service()` çağrılamıyor — bu yüzden **naif bir test mevcut hatayı
yakalamaz**. Çözüm devamı `HAL_UART_Transmit_IT` ile göndermek; TX kesme
güdümlü olduğu için main döngüsü serbest kalır.

```
64 baytlık çerçeve kur (payload 55)
ilk 7 baytı bloklu gönder → service → len=7, tick=t0
47 ms boyunca service döndür (veri yok, tick yenilenmez)
HAL_UART_Transmit_IT(&tx[7], 57)       ← yayın t=47..52, bildirim YOK
teslim olana kadar veya 20 ms'e kadar service döndür
```

Beklenen: çerçeve çözülür, `frame_timeouts` **artmaz**.
Mevcut kodda t=50'de zaman aşımı yayının ortasında ateşlenir ve test düşer.
Yan faydası: TX ile RX'in gerçekten bağımsız olduğunu, yani `AbortReceive`
kuralının neden var olduğunu da gösterir.

### T5 — gerçek UART hatası

Yazılımdan `ErrorCallback` çağırmak yerine donanımda gerçek bir framing error
üret: `SET_BIT(huart->Instance->CR1, USART_CR1_SBK)` hatta break gönderir,
loopback'te alıcı bunu FE olarak görür.
Beklenen: `error_events >= 1`, `last_error & HAL_UART_ERROR_FE`, ve **asıl
iddia** — sonrasında normal bir çerçeve gönderildiğinde `frames_ok` artıyor,
yani alım hâlâ çalışıyor.

### T6 — başarısız yeniden başlatma ve kalıcı hata durumu

Başarısızlığı donanımdan deterministik üretmenin temiz bir yolu yoktu. İki
seçenek vardı:

- **A (seçilen):** Modüle test amaçlı kanca (`uart_rx_force_restart_fail`,
  `uart_rx_test_inject_error`) ekleyip 1c/1d durum makinesini sınamak.
- **B:** T6'yı yazmamak; 1c/1d'yi "kod incelemesiyle doğrulandı, donanımda
  sınanmadı" olarak belgelemek.

---

## Sonuç

Uygulama sırası: 2a/2b → 1a-1e → T3/T4/T5/T6. Hepsi kartta koştu
(STM32F4DISCOVERY, PA2-PA3 loopback).

```
test_gecen = 29   test_kalan = 0          (birim testleri, donanimsiz)
lb_gecen   = 10   lb_kalan   = 0          (loopback testleri)

rx_events=49  idle_events=45  ht_events=2  tc_events=2
error_events=1  last_error=0x04 (FE)
restarts=1  restart_fails=5  faulted=0  frame_timeouts=1
T6 oncesi: frames_ok=42  last_seq=42  seq_gaps=0  bytes_dropped=7
```

### Beklenmeyen ama değerli sonuç

`restarts = 1`. T5'te break gönderildiğinde HAL alımı `BUSY_RX` durumunda
**sürdürmedi**; gerçek toparlanma yolu (abort → bayrakları temizle →
yeniden başlat) donanımda gerçekten çalıştı ve sonraki çerçeve sorunsuz
çözüldü. Yani bulgu 1'in kodu artık varsayım değil, ölçülmüş.

### T4'ün hatayı yakaladığı da ölçüldü

Geçen bir test tek başına bir şey kanıtlamaz. 2b düzeltmesi `#if 0` ile
kapatılıp aynı ikili tekrar koşuldu:

| Ölçüm | Düzeltme açık | Düzeltme kapalı |
|---|---|---|
| `lb_sonuc[4]` (T4) | 1 = PASS | **2 = FAIL** |
| `frame_timeouts` | 1 | **2** |
| `lb_frames_ok` | 42 | **41** |
| `lb_bytes_dropped` | 7 | **71** = 7 + 64 |
| `lb_seq_gaps` | 0 | **1** |

`71 = 7 + 64`: kaybedilen çerçevenin tamamı bayt bayt çöp olarak elendi.

### Yol boyunca çıkan iki ders

1. **`uart_rx_start()` çalışan bir alımın üzerine başlamaz.** T6 ilk koşuda
   erken çıktı: kanca `AbortReceive`'i atladığı için RxState `BUSY_RX` kaldı
   ve `ReceiveToIdle_DMA` doğru şekilde `HAL_BUSY` döndü. Gerçek bir kalıcı
   hatada RxState `READY` olur (abort başarılı, restart başarısız), yani
   düzeltme testte yapıldı, modülde değil — `uart_rx_start`'ın çalışan bir
   alımı sessizce yıkması istenmez.
2. **Zaman aşımında kontrol sırası:** süre dolmadan NDTR okumanın anlamı yok.
   Önce süre, sonra ilerleme kontrolü.

### Geriye kalan

- ORE ile toparlanma hâlâ sınanmadı (T5 yalnızca FE üretir). `MIMARI.md`
  8. bölüm madde 6.
- Kalıcı hata durumu yalnızca test kancasıyla sınandı; HAL'in gerçek bir
  arızada ne yapacağı ölçülmedi.

## İlgili belgeler

- `MIMARI.md` bölüm 5.16 (toparlanma neden kesme içinde yapılmaz), 5.17
  (zaman aşımı kararı neden ayrıştırıcıda değil), bölüm 6 (doğrulanmamışlar)
- `UART_RX_TX_RTOS_YOL_HARITASI.md` — sonraki aşama (TX + ortak task).
  2. bölüm tablosundaki iki satır eskimiş: kısmi paket zaman aşımı artık var,
  loopback artık `uart_rx_service()` kullanıyor.

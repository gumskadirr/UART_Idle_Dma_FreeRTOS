# Birleşik UART RX/TX mimarisi ve uygulama yol haritası

Güncel paket düzeni: `Lib/Uart`; protokol bağlantısı `protocol_uart.c/.h` adaptöründedir. [Taşıma ve güncel API](Lib/Uart/README.md), [katmanlı tasarım](docs/superpowers/specs/2026-10-06-uart-katmanli-mimari-design.md) ve [uygulama planı](docs/superpowers/plans/2026-10-06-uart-katmanli-mimari.md). Aşağıdaki R/T/F aşamaları ilk geliştirme gereksinimleri ve tarihsel kabul sırasını da içerir.
Tarih: 5 Ekim 2026  
Revizyon: **4 — taşınabilir UART paketi ve protokol adaptörü işlendi (6 Ekim 2026).**
Durum: **RX/TX tek uart_comm modülünde; tek statik FreeRTOS owner, FIFO ve public API uygulanmış durumda. Güncel son kabul kaydı: UART_RTOS_UYGULAMA_PLANI.md.**
Hedef: STM32F407VG, STM32 HAL, STM32CubeIDE.

> Uygulayıcı için: Adımlar sırayla yürütülür. Her adımın test ve geçiş koşulu tamamlanmadan sonraki adıma geçilmez. Uygulamaya başlanacağı zaman `superpowers:executing-plans` veya kullanıcı tarafından seçilirse `superpowers:subagent-driven-development` yöntemi kullanılabilir. Bu belgenin hazırlanması kodlama adımlarının tamamlandığı anlamına gelmez.

**Amaç:** RX ve TX'i güvenilir hale getirip sonunda `uart_comm.c/.h` içinde birleştirmek; UART haberleşmesi için yalnızca **bir FreeRTOS taskı** kullanmak ve boşta CPU tüketimini en aza indirmek.

**Mimari:** Circular RX DMA ve Normal TX DMA aynı anda çalışır. Tek `UartCommTask`, iki ayrı iç durum makinesini, RX parser'ını ve kopya taşıyan TX kuyruğunu yönetir. Kesme yolları kısa olay/bilgi kaydı yapar; task boşta bildirim bekleyerek uyur.

**Teknoloji:** Mevcut HAL, CMSIS çekirdek tanımları, mevcut CRC/çerçeve/parser kodu; RTOS aşamasında FreeRTOS. Yeni üçüncü taraf kütüphane gerekmiyor. FreeRTOS kullanıcı tarafından CubeMX ile eklenmiştir; mevcut kurulum kullanıldı, yeni kütüphane eklenmedi.

**Tasarım şartnamesi:** Bu belgenin 1–7. bölümleri. Uygulama sırası: 8–13. bölümler. Kullanıcı isteği gereği tasarım ve ayrıntılı yol haritası aynı yeni dosyada tutuluyor.

## 1. Kapsam, kararlar ve başlangıç durumu

### 1.1. Kesin hedefler

- UART haberleşmesi için tek task. Ayrı RX/TX taskı veya UART'a özel yazılım timer'ı yok.
- Nihai dosyalar `Lib/Uart/uart_comm.c` ve `Lib/Uart/uart_comm.h`. Eski `uart_rx.c/.h` ve `uart_tx.c/.h` son birleştirme adımında kaldırılacak.
- `crc16.c/.h`, `frame.c/.h`, `parser.c/.h` bağımsız kalacak. Bunların UART dosyasına taşınması gerekmiyor.
- USART2, PA2/PA3, 115200 baud, 8N1; RX DMA1 Stream5 Channel4 Circular; TX DMA1 Stream6 Channel4 Normal korunacak.
- RX DMA tamponu başlangıçta 256, en büyük çerçeve 64 bayt. DMA tamponları DMA erişimli SRAM'de; CCM'de olmayacak.
- UART başlatma, durdurma, yeniden başlatma ve TX başlatma işlemlerinin tek sahibi haberleşme taskı olacak.
- TX beklerken RX işlenmeye devam edecek. Görev içinde `HAL_Delay`, tamamlanma bekleyen döngü ve bloklayan UART gönderimi olmayacak.
- Taşma, kuyruk doluluğu, başlangıç hatası ve belirsiz gönderim sonucu görünür olacak.
- `.ioc` değişiklikleri yalnızca F0 adımındaki CubeMX ayarları açıklanarak yapılacak. Üretilmiş kaynaklara elle yapılacak gerekli eklemeler USER CODE bölgelerinde kalacak.

FreeRTOS'un kendi Idle taskı ve hedef projenin mevcut sistem taskları bu “tek UART taskı” sınırına dahil değildir. UART tasarımı ek timer taskı gerektirmez; hedef projenin başka nedenle kullandığı timer servisi kaldırılmaz.

### 1.2. Açıkça belirtilen varsayımlar

Kullanıcı tek task ve düşük CPU hedefini belirtti. Hedef FreeRTOS projesinin yeri ve CMSIS-RTOS v1/v2 kullanımı henüz belirlenmedi. Bu planın kod karşılığı **native FreeRTOS API** ile tanımlandı. F0'a başlamadan hedef proje/API kesinleştirilir; CMSIS seçilirse bildirim, kuyruk ve task oluşturma çağrıları birlikte uyarlanır. Aynı olay alanında CMSIS thread flags ve native task notification karıştırılmaz.

İlk sürüm tek UART içindir. Çoklu UART, RS485 yön kontrolü, donanım akış kontrolü, ACK/tekrar gönderim ve komuta özgü cihaz davranışları bu çalışmanın dışında tutulur. Protokol alanları değiştirilmez.

### 1.3. Plan yazılırken görülen başlangıç durumu (tarihsel)

| Bileşen | 5 Ekim çalışma ağacında görülen durum |
|---|---|
| TX DMA | Donanım ayarı ve IRQ bağlantısı mevcut |
| TX modülü | Kopya tampon, meşgul reddi ve başarılı tamamlanma mevcut |
| TX hata tasarımı | Header'da ABORTING/FAULT, süreler ve bildirim prototipleri var; `.c` uygulaması tamamlanmamış |
| RX | Circular DMA, NDTR tüketimi, 50 ms kısmi çerçeve zaman aşımı ve yeniden başlatma denemeleri mevcut |
| RX taşma | Tam turu ayırt eden üretim sayacı yok |
| Testler | 29 birim kontrolü ve T1–T9 loopback senaryoları; tamamı DMA TX kullanmıyor |
| FreeRTOS | Bu alt projede task/kuyruk/FreeRTOSConfig yok |
| NVIC | GROUP_0 ve UART/DMA öncelikleri 0; `FromISR` kullanımı için değişmeli |

`UART_RX_TX_RTOS_YOL_HARITASI.md` referanstır; başlangıç tablosundaki “TX DMA yok” ve “RX zaman aşımı yok” bilgileri artık eski. `ACIK_BULGULAR.md` önceki üç bulgunun kapanışını anlatır; bütün RX/TX altyapısının tamamlandığı anlamına gelmez. `docs/superpowers/plans/2026-10-02-uart-guvenilirlik.md` içindeki güvenilirlik işleri bu planda RX önce olacak şekilde yeniden sıralanmıştır. Yeni çalışma için sıralama ve nihai dosya düzeninde bu belge esas alınır.


### 1.4. Güncel uygulama durumu — 6 Ekim 2026

- Nihai kaynak `Lib/Uart/uart_comm.c`, public header `Lib/Uart/uart_comm.h`; eski dört RX/TX dosyası kaldırıldı. Özel kapılar yalnız test derlemesinde dışarı açılır.
- USART2/PA2–PA3/115200 8N1/DMA atamaları ve kullanıcının CubeMX ayarları korundu. FreeRTOS 10.3.1 ARM_CM4F; CMSIS-v2 kernel kurulumu yanında UART modülü native API kullanır. HAL 1.8.5, TIM6 HAL tick, SysTick RTOS 1000 Hz, GROUP4 ve üç UART/DMA IRQ priority 5.
- Tek statik owner priority 25/512 stack elemanı, 8 kopyalı FIFO +1 aktif öğe, tag/epoch, tek sonuç teslimi ve korumalı snapshot. Dört fonksiyonlu kullanım örneği: [UART_COMM_KULLANIM.md](UART_COMM_KULLANIM.md).
- F1'in geçici modül yönlendirme kapıları fiziksel birleşme önce yapıldığı için kurulmadı; aynı kayıt/notification sözleşmesi doğrudan birleşik callback'lerde doğrulandı. F1/F2 owner entegrasyonu birlikte yapıldı; her sözleşme ayrı model/kart kabul kontrolü ile doğrulandı. M1 kutuları saf taşıma ve ardından regresyonun birleşik kanıtını ifade eder.
- Tarihsel kod örnekleri/adım adları API dokümantasyonu yerine geçmez. Kesin firmware, test ve ölçüm sonuçları [uygulama kaydında](UART_RTOS_UYGULAMA_PLANI.md). Native loopback kabulü71/71, birim29/29, loopback15/15; son harici CH340 kontrolü13/13 geçti. CH340100Hz çift yön trafikte ölçülen UART servis+ISR payı yaklaşık%1,47; 10sn boş hatta UART servis turu0. Bekleme/notification yönetimi tam task runtime ölçümüne dahil olmadığından bütün UART taskı için CPU hedefi ve ayrıntılı plan/test açıkları ayrıca kayıtlıdır; F3/M2'nin her maddesi koşulsuz kapanmış sayılmaz.

## 2. Neden aşamalı birleştirme?

| Yaklaşım | Değerlendirme |
|---|---|
| Hemen tek dosyaya taşıyıp aynı anda bütün hataları düzeltmek | Dosya taşıma, durum değişikliği ve test değişikliği aynı anda olur; gerilemenin kaynağını bulmak zorlaşır |
| RX/TX'i kalıcı ayrı bırakıp yalnız taskı ortak yapmak | Teknik olarak mümkün, fakat kullanıcının dosyaları birleştirme hedefini karşılamaz |
| **Önce RX, sonra TX, sonra tek task; son olarak fiziksel birleştirme** | Seçilen yaklaşım. Her davranış önce mevcut modülünde doğrulanır, en son doğrulanmış kod taşınır |

Geçici olarak `uart_comm.c` ayrı RX/TX modüllerini çağırabilir. Bu ara düzen nihai mimari değildir. M1 sonrasında RX/TX fonksiyonları aynı `.c` içindeki `static rx_*` / `static tx_*` yardımcılarına dönüşür; eski header'lar ve build girdileri kalmaz.

```text
Üretici tasklar / kısa uygulama handler'ı
            |
    uart_comm_send_copy(...)
            |
   8 öğelik, kopya taşıyan TX FIFO
            |
      UartCommTask  <--- olaylar / zaman aşımı
       |       |
   RX state   TX state
       |       |
   scratch    aktif 64 baytlık tampon
       |       |
    parser    Normal DMA TX
       |
   kısa uygulama handler'ı

Circular DMA RX ---> 256 baytlık halka
IDLE / HT / TC / hata ---> kısa kayıt + task uyandırma
TX tamamlandı / abort ---> kısa kayıt + task uyandırma
```

İki durum makinesi bağımsızdır: RX toparlanırken sağlıklı TX sürer; TX toparlanırken sağlıklı RX tüketilir. Ortak HAL DMA hatasının her iki yönü etkilediği durum ayrıca ele alınır.

## 3. RX: mevcut sorunlar, nedenleri ve çözüm kararları

### RX-1 — İkinci start çağrısı HAL_BUSY dönmeden önce durumu bozuyor

**Yer:** `uart_rx_start()`. Handle, okuma konumu, olaylar ve parser HAL çağrısından önce koşulsuz sıfırlanıyor. Alım zaten çalışıyorsa HAL_BUSY dönüyor ama yazılım durumu çoktan değişmiş oluyor. Yarım paket kaybolabilir ve DMA'nın eski verileri yeniden tüketilebilir.

**Çözüm:** Aktif modül/handle/donanım kontrolü bütün sıfırlamalardan önce yapılacak. Çalışan alıma start ve eski DMA aktifken handle değişimi reddedilecek; hiçbir parser, sayaç veya konum değişmeyecek. Soğuk kurulum ile hata sonrası yeniden başlatma ayrılacak. Yeniden başlatma istatistikleri silmeyecek.

### RX-2 — Tam tampon turu “veri yok” sanılıyor

**Yer:** `dma_write_pos()` ve `uart_rx_drain()`. Yalnız modulo konumu saklandığı için 0 yeni bayt ile 256 yeni bayt ayırt edilemiyor. `s_rx_pending=1` geçmiş tur sayısını taşımıyor.

**Çözüm:** Üretici ve tüketici konumu `uint32_t` mutlak bayt sayaçlarıyla izlenecek. Gerçek DMA TC olayının RX callback'i tur bilgisini artıracak; NDTR tur içi konumu verecek. Bekleyen TC bayrağı ile henüz çalışmamış ISR de hesaba katılacak. IDLE/HT/TC callback `Size` değerlerini toplamak yasak: `Size` yeni bayt sayısı değildir; çakışan bildirimler aynı konumu bildirebilir. Olay türü `HAL_UARTEx_GetRxEventType()` ile callback başında alınacak.

`available = (uint32_t)(produced - consumed)`; `available >= 256` konservatif taşma kabul edilir. Parser adayı bırakılır, kayıp kaydedilir ve tüketici güncel üreticiye alınır. Yeni geçerli çerçeveyle senkron aranır. Tam 256 baytta henüz fiziksel ezilme olmayabilse de güvenlik payı kalmadığından bu sürüm onu da düşürür.

115200/8N1 hızında 256 bayt yaklaşık 22,22 ms, yarım tampon 11,11 ms sürer. Kesme bayrakları sayaç değildir: kesmeler birden fazla tam tur engellenirse gerçek tur sayısı yazılımdan kesin çıkarılamaz. Bu nedenle sınırsız gecikmede kayıpsızlık iddiası yoktur; gecikme bütçesi ve ölçüm zorunludur.

### RX-3 — Parser canlı DMA belleğini okuyor

**Yer:** `uart_rx_drain()` içindeki `frame_parser_feed(..., &s_dma_buf[...], ...)`. DMA, parser/handler çalışırken aynı belleğe yazabilir. RTOS altında preemption bu pencereyi büyütür.

**Çözüm:** En fazla 32 baytlık parçalar yerel çalışma tamponuna kopyalanacak. Kopyadan önce/sonra üretici konumu ve RX hata nesli doğrulanacak. Kaynak aralığın ezildiği veya RX oturumunun geçersizleştiği anlaşılırsa kopya parser'a verilmeyecek. Parser yalnızca sabit çalışma tamponunu okuyacak. IRQ'ları kapatmak DMA'yı durdurmaz; uzun kritik bölüm bu sorunu çözmez.

### RX-4 — Başlangıç başarısızlığı ve toparlanma durumu yeterince açık değil

**Yer:** `uart_rx_start()` ve `uart_rx_service()`. `faulted` daha RX başlamadan sıfırlanıyor. İlk başlatma başarısızlığı açık durum olarak tutulmuyor. Toparlanma beklerken servis yine drain/timeout çalıştırabiliyor.

**Çözüm:** RX için açık durum makinesi; yalnız RUNNING sırasında veri/timeout işleme. Başlatma sonucu ve HAL/donanım durumu birlikte kontrol edilecek. FAULT, durmuş olduğunun kanıtı değil; güvenli çalışma kurulamamasıdır. DMA'nın durduğu ayrıca doğrulanacak.

### RX-5 — Toparlanma yolu bloklayabilir ve deneme bütçesi yeniden açılabilir

**Yer:** `uart_rx_recover()` içindeki `HAL_UART_AbortReceive()` ve her `s_rx_error` alındığında `s_restart_tries=0`. Bloklayan DMA abort ortak taskın TX/RX hizmetini geciktirebilir. Her restart sırasında yeni hata bildirimi üretilirse deneme sayacı sürekli başa dönebilir.

**Çözüm:** `HAL_UART_AbortReceive_IT()` ve süre sınırlı ABORTING/RETRY_WAIT akışı. Aynı toparlanma dönemi içindeki tekrar bildirimleri deneme sayısını veya dönem süresini sıfırlamayacak. En fazla 5 start denemesi, denemeler arasında 5 ms ve 100 ms yeni deneme bütçesi kullanılacak; gerekiyorsa son duruş için en fazla 20 ms ek süre ayrılacak. Başarılı start dönüşü tek başına geçmiş hataları silmez.

### RX-6 — HAL durumu ve açıklamalar arasında yanlış varsayım var

**Yer:** RX toparlanma yorumları ve T5 açıklamaları. Bu projedeki `HAL_UART_IRQHandler()` DMA RX etkinse FE/NE/PE dahil alım hatalarını durdurucu yoldan işleyebilir. “FE alımı durdurmaz” genellemesi bu yapı için doğru değildir.

**Çözüm:** RX yeniden başlatma kararı hata adı ve `RxState` tek başına değerlendirilerek verilmeyecek. DMAR, stream EN, DMA State ve HAL alım modu kontrol edilecek. HAL'in başlattığı abort sürüyorsa ikinci bir abort başlatılmayacak. ErrorCallback gecikse bile UART IRQ çıkışındaki sağlık bildirimi taskı uyandıracak.

### RX-7 — Sınırsız drain ve uygulama verisinin paylaşımı RTOS'a hazır değil

Sürekli girişte drain döngüsü TX hizmetini erteleyebilir. Joystick alanları/parser işaretçisi başka tasklarca eşzamanlı okunursa tutarsız snapshot alınabilir. Bunlar mevcut tek ana döngüde mutlaka hata oluştuğu iddiası değil, RTOS geçişinde kapatılacak tasarım boşluklarıdır.

**Çözüm:** Servis turu başına 64 RX baytı bütçesi, her tur TX/hata kontrolü ve kısa handler. Uygulama parser/DMA işaretçisi saklamayacak. Durumlar korumalı snapshot ile yayımlanacak. İşlem süresi uzun cihaz işleri mevcut uygulama tasklarına değer kopyasıyla aktarılacak.

## 4. TX: mevcut sorunlar ve çözüm kararları

### TX-1 — Hata veya kayıp tamamlanma SENDING durumunu kalıcı bırakıyor

`uart_tx_service()` yalnız `s_tx_done` işliyor; hata, toplam aktarım süresi ve abort yok. Ortak error callback RX'e bildirim yapıyor, TX'e yapmıyor. HAL DMA hatası sırasında gState READY olsa da modül SENDING kalıyor.

**Çözüm:** DMA hata kaydı, 20 ms aktarım son tarihi, ABORTING ve 20 ms toparlanma sınırı. Hata veya zaman aşımı sonrası TX'e özel iptal; güvenli duruşta IDLE, doğrulanamazsa FAULT. RX tek başına hatalıysa TX gereksiz iptal edilmeyecek.

### TX-2 — HAL_BUSY ve HAL_ERROR aynı sonuçta birleşiyor

`uart_tx_send_copy()` her HAL başarısızlığını BUSY sayıyor ve hemen IDLE'a dönüyor. Çağıran geçici meşguliyetle gerçek başlatma hatasını ayıramıyor; kuyruk entegrasyonunda çerçeve sahipliği belirsizleşiyor.

**Çözüm:** Modül meşguliyeti, HAL başlangıç BUSY sonucu ve HAL başlangıç ERROR sonucu ayrı olacak. HAL başarısızlığında donanım hâlâ tamponu okuyabilir mi kontrol edilecek; otomatik IDLE varsayılmayacak. Kuyruktan çıkarılmış öğe sonucu belirlenene kadar aktif öğe olarak saklanacak. Bu sürüm otomatik yeniden gönderim yapmayacak.

### TX-3 — Geç/tutarsız tamamlanma yeni aktarımı veya sayacı bozabilir

Servis, state kontrolü olmadan her tamamlanmayı başarı sayıyor. Yeni gönderim eski bildirimleri temizlemiyor. Mevcut normal kullanımda tek aktarım olması riski azaltır; abort eklendiğinde bu boşluk kesin olarak kapatılmalıdır.

**Çözüm:** Yalnız SENDING + geçerli donanım tamamlanması başarıdır. Hata ve tamamlanma aynı snapshot'taysa hata öncelikli. ABORTING/FAULT/IDLE durumunda gelen tamamlanma başarı sayılmaz. Yeni aktarım öncesi eski DMA/TX kesme kaynakları ve yazılım olayları güvenli biçimde temizlenir.

Callback'te mevcut bir “generation” değerini okumak tek başına çözüm değildir: HAL callback eski aktarımın kimliğini taşımaz. Yeni aktarım başlamadan önce eski kesme kaynağının artık callback üretemeyeceği doğrulanmalıdır.

### TX-4 — init donanımın serbest olduğunu kanıtlamıyor

Şimdiki init yalnız SENDING kontrolü yapıyor. Header'daki ABORTING/FAULT sözleşmesi uygulandığında bu yetersiz kalır.

**Çözüm:** Init aktif aktarım veya abort üzerinde durumu sıfırlamaz. FAULT'tan çıkış bir kurtarma isteğiyle tek sahibi tarafından yürütülür; DMA EN/DMAT/ilgili kesmeler/HAL durumları doğrulanır. Tampon ancak bundan sonra tekrar kullanılabilir.

### TX-5 — Callback'in daima ISR'de çalıştığı varsayımı yanlış

Bu HAL'de `HAL_UART_AbortTransmit_IT()` / `HAL_UART_AbortReceive_IT()` bazı dallarda callback'i fonksiyon dönmeden çağırır. DMA abort başarısızlığında bile UART abort fonksiyonu HAL_OK dönebilir.

**Çözüm:** State/timestamp çağrıdan önce kurulur. Olay kaydı her iki bağlamda güvenlidir. RTOS uyandırma fonksiyonu `__get_IPSR()` ile gerçek bağlamı ayırır; thread bağlamından `FromISR` çağırmaz. Abort callback'i veya HAL_OK dönüşü tek başına duruş kanıtı sayılmaz.

## 5. Ortak sözleşmeler ve hedef API

### 5.1. Bellek ve sahiplik

| Kaynak | Yazar / okuyucu | Kural |
|---|---|---|
| RX DMA 256 bayt | DMA / task | Task yalnız doğrulanmış küçük kopyalar alır |
| RX üretim tur sayacı | Gerçek TC olaylı RX callback / task | Kısa kritik bölümde tutarlı örnekleme |
| Parser, RX tüketici sayacı | Task | Kesmeden parser çalışmaz |
| ISR olay kutusu | IRQ veya senkron HAL callback / task | Kayıt ve take-and-clear kısa kritik bölümde |
| TX FIFO | Birden fazla üretici / tek task | Öğeler değer olarak kopyalanır |
| Aktif TX tamponu | Task / DMA | Aktarım ve abort boyunca sabit |
| RX/TX state | Task | Kesme yalnız olay ve ham sağlık kaydı üretir |
| Yayımlanmış snapshot | Task / diğer tasklar | Yayınlama ve okuma aynı korumayı kullanır |

`volatile` tek başına yarış koşulunu çözmez. Bayrak okumak ve ardından sıfırlamak iki ayrı işlemdir. Bare-metal aşamada eski PRIMASK saklanıp geri yüklenir; koşulsuz `__enable_irq()` yapılmaz. Kritik bölüm içinde HAL çağrısı, parser, kuyrukta bekleme veya kullanıcı handler'ı çalışmaz.

Kritik bölümler için ilk ölçüm hedefi 10 µs altında kalmaktır. Uygulama frame/result handler'ı için çağrı başına 100 µs başlangıç bütçesi konur; daha uzun işler mevcut uygulama görevlerine devredilir. Bunlar profil ölçümünde doğrulanır. Uygulamanın açtığı kritik bölümün içinde sıfır beklemeli olsa bile FreeRTOS queue/notify API çağrısı yapılmaz; kabul yarışı bölüm 6.5'teki kuyruk dönemiyle çözülür. Bu tercih [FreeRTOS kritik bölüm sözleşmesiyle](https://www.freertos.org/Documentation/02-Kernel/04-API-references/04-RTOS-kernel-control/01-taskENTER_CRITICAL_taskEXIT_CRITICAL) uyumludur.

### 5.2. Nihai public arayüz

Aşağıdaki adlar/signature'lar F1–F3 sırasında kademeli olarak kullanıma girer. R/T adımlarında mevcut API korunur, gerekli eklemeler kendi adımlarında belirtilir. Bir adım kendi kullandığı tipi/fonksiyonu aynı adımda tanımlar; henüz uygulanmamış sonraki aşamaya link bağımlılığı bırakılmaz.

```c
typedef enum {
    UART_COMM_ACCEPTED = 0,
    UART_COMM_QUEUE_FULL,
    UART_COMM_INVALID,
    UART_COMM_NOT_READY
} uart_comm_send_status_t;

typedef enum {
    UART_COMM_TX_COMPLETE = 0,
    UART_COMM_TX_START_BUSY,
    UART_COMM_TX_START_ERROR,
    UART_COMM_TX_DMA_ERROR,
    UART_COMM_TX_TIMEOUT,
    UART_COMM_TX_CANCELLED_FAULT
} uart_comm_tx_code_t;

typedef struct {
    uint32_t tag;             /* Uygulamanın yerel kimliği; hatta eklenmez. */
    uart_comm_tx_code_t code;
    uint32_t hal_error;
    bool recovery_fault;
} uart_comm_tx_result_t;

typedef struct {
    uart_comm_rx_handler_t on_rx;
    void (*on_tx_result)(const uart_comm_tx_result_t *, void *);
    void *rx_user;
    void *user;
} uart_comm_handlers_t;

HAL_StatusTypeDef uart_comm_init(UART_HandleTypeDef *huart,
                                const uart_comm_handlers_t *handlers);
uart_comm_send_status_t uart_comm_send_copy(const uint8_t *data,
                                          uint16_t len, uint32_t tag);
bool uart_comm_request_recovery(uint32_t directions);
bool uart_comm_get_snapshot(uart_comm_snapshot_t *out);
```

- Header `<stdbool.h>`, `<stdint.h>`, mevcut HAL, config ve genel byte callback tiplerini içerir; dışarıya RTOS handle'ı vermez. Parser adaptöre aittir.
- `init`, scheduler başlamadan tek kez çağrılır; konfigürasyonu kopyalar, statik FIFO ve tek taskı kurar. RX DMA taskın başlangıcında açılır. NULL handle/handlers veya yanlış HAL DMA bağlantısı reddedilir; handlers içindeki fonksiyon işaretçileri NULL olabilir. Başarılı init sonrası tekrar init HAL_BUSY döner, mevcut nesneleri silmez. `handlers->user` işaretçisinin hedefi kopyalanmaz; modül çalıştığı sürece geçerli uygulama belleği olmalıdır.
- `send_copy`: yalnız task bağlamı, `1 <= len <= FRAME_MAX_SIZE`; NULL, ISR veya sınır dışı uzunluk INVALID. API baytları taşır; protokol geçerliliğini gönderen belirler. `uint16_t` uzunluk, 256 gibi girdilerin doğrulama öncesi 0'a daralmasını önler.
- ACCEPTED yalnız FIFO'ya kopyalanma demektir. `tag` uygulama tarafından seçilir; protokol SEQ alanından bağımsızdır. Gerekirse uygulama benzersizliğini sağlar.
- Kabul kontrolüyle enqueue arasında TX FAULT oluşursa çağrı yine ACCEPTED dönebilir; öğe bölüm 6.5'e göre CANCELLED_FAULT sonucu alır. Bu nedenle ACCEPTED sonucu daha sonra başarı garantisi olarak kullanılamaz. Başka öncelikli task gönderimi API dönmeden işleyebileceği için uygulama `tag` ile ilişkili kayıtlarını çağrıdan **önce** hazırlar.
- `on_rx` ve `on_tx_result` yalnız UartCommTask bağlamında kısa çalışır. Payload/result işaretçisinin ömrü yalnız callback süresidir. Handler send_copy çağırabilir; service/init/abort çağırmaz, beklemez.
- FIFO kabul edilen her TX öğesi normal çalışma devam ettiği sürece **bir kez** sonuç üretir. Sonuç callback'i NULL ise sayaçlar yine güncellenir. COMPLETE, yerel UART TC tamamlanmasıdır; uzaktaki uygulamanın teslim onayı değildir.
- `directions`: `UART_COMM_RECOVER_RX = 1U`, `UART_COMM_RECOVER_TX = 2U`. Yalnız task bağlamından, geçerli maskeyle istek kabul edilir; true başarılı toparlanma değil isteğin kaydıdır. Çalışan yöne zarar verilmez; owner yalnız FAULT yönlerini yeniden değerlendirir. Birleşen istekler bit olarak tutulur.
- `uart_comm_snapshot_t` RX/TX state, hazır bilgisi ve bölüm 5.5 sayaçlarını içerir; getter NULL için false döner. Modül içi mutable parser/handle/tampon işaretçisi dışarı verilmez.

Snapshot tipi F3'te header'da tanımlanır: `uart_rx_phase_t rx_phase`, `uart_tx_state_t tx_state`, `bool initialized`, `bool rx_ready`, `bool rx_quiescent`, `bool tx_accepting`, `uint32_t tx_queue_depth`, `uint32_t tx_queue_high_water` ve bölüm 5.5'te adları verilen `uint32_t` sayaç/hata alanları. Son hata nedeni `uart_comm_tx_code_t last_tx_failure` ve ayrıca `bool has_tx_failure` ile gösterilir; sıfır değerinin yanlışlıkla geçmiş hata varmış gibi okunması önlenir. M1'de iki state enum tanımı da uart_comm.h'ye taşınır, eski header'ları include ederek tutulmaz. Header'daki getter prototipinden önce `typedef struct uart_comm_snapshot uart_comm_snapshot_t;` ileri bildirimi veya tam struct tanımı bulunur.

### 5.3. Durumlar

RX durumları: `STOPPED, STARTING, RUNNING, ABORTING, RETRY_WAIT, FAULT`.

```text
STOPPED -> STARTING -> RUNNING
STARTING/RUNNING -- hata veya tutarsız donanım --> ABORTING
ABORTING -- güvenli duruş --> RETRY_WAIT
RETRY_WAIT -- süre geldi, bütçe var --> STARTING
ABORTING -- 20 ms doldu, güvenli değil --> FAULT
STARTING/RETRY_WAIT -- deneme/zaman sınırı, güvenli duruş var --> FAULT
STARTING/RETRY_WAIT -- deneme/zaman sınırı, duruş belirsiz --> ABORTING(final_stop)
ABORTING(final_stop) -- duruş doğrulandı veya 20 ms doldu --> FAULT
FAULT -- açık kurtarma isteği --> duruş doğrulama -> RETRY_WAIT
```

Soğuk başlangıç başarısızlığında da aynı güvenli toparlanma politikası işletilir; start dönüş kodu ve son hata korunur. Otomatik dönem ilk hatada başlar; bu dönem içindeki bildirimler bütçeyi sıfırlamaz. Dönem, RUNNING sağlık doğrulaması ve 100 ms yeni hatasız çalışma veya yeni oturumda ilk geçerli çerçeveyle kapanır. Yeni bir start'ın HAL_OK dönmesi tek başına sayacı sıfırlamaz. 100 ms yeni deneme sınırı, halen toparlanma gereken durumlar içindir; sağlıklı RUNNING'i sırf zaman geçti diye FAULT yapmaz.

100 ms sınırı **yeni start denemeleri için bütçedir**. Bütçe dolduğunda DMA belirsizse `final_stop=true` ile son durdurma yürütülür; duruş için en fazla 20 ms daha ayrılır ve yeni start yapılmaz. Böylece işlem süresi servis gecikmesi hariç en fazla 120 ms olabilir. FAULT sonucu ile `rx_quiescent` duruş kanıtı ayrı tutulur. Son duruş da başarısızsa ilgili RX DMA istek/kesme kaynakları mümkün olduğunca kapatılır, buffer kilitli kalır ve sessiz yeniden kullanım yapılmaz. Ortak USART IRQ'su tümüyle kapatılarak sağlıklı TX kesilmez.

Bu sağlıklı dönem kontrolü için ayrı periyodik uyanma kurulmaz. Başarılı start zamanı ve hata olayının oluşma zamanı kaydedilir; sonraki servis/yeni hata geldiğinde aradaki sağlıklı süre değerlendirilir. Örneğin 2 saniye sağlıklı sessizlikten sonra gelen hata yeni dönemdir; önceki dönemin 100 ms'si doldu diye doğrudan FAULT yapılmaz.

TX durumları: `IDLE, SENDING, ABORTING, FAULT`. FIFO'dan alınan fakat henüz HAL'e verilmeyen öğe ayrıca `active_valid` ile korunur; bir RX zaman aşımı bu öğeyi silemez.

```text
IDLE -- gönderimi kur --> SENDING
SENDING -- geçerli tamamlanma --> IDLE
SENDING -- DMA hatası / 20 ms --> ABORTING
başlatma başarısız -- temiz donanım --> IDLE + başlangıç hata sonucu
başlatma başarısız -- duruş belirsiz --> ABORTING
ABORTING -- güvenli duruş --> IDLE + başarısız sonuç
ABORTING -- 20 ms, hâlâ güvenli değil --> FAULT + başarısız sonuç
FAULT -- açık kurtarma isteği ve doğrulama --> IDLE
```

Başarısız öğe otomatik tekrar gönderilmez. TX FAULT olduğunda yeni kabul kapatılır; bekleyen FIFO öğeleri sırasıyla CANCELLED_FAULT sonucu alır. RX sağlıklıysa çalışmayı sürdürür. RX FAULT ise sağlıklı TX bağımsız çalışabilir; uygulama komutlara cevap üretirken RX durumunu dikkate alır.

### 5.4. Başlangıç sabitleri ve zamanlama

| Parametre | Değer / anlam |
|---|---|
| RX DMA boyutu | 256 bayt |
| RX kopya parçası | En fazla 32 bayt |
| Bir servis turunun RX bütçesi | 64 bayt |
| Tutarsız producer örneği | Çağrı başına en fazla 3 okuma; sonra 1 ms sonraya bırak, 20 ms boyunca geçerli örnek yoksa RX toparlanması |
| RX aday zaman aşımı | 50 ms, gözlenen üretici ilerlemesiz süre |
| RX yeniden start aralığı | En az 5 ms |
| RX start denemesi | Dönem başına en fazla 5 |
| RX abort sınırı | 20 ms |
| RX toparlanma dönemi | 5 start / 100 ms yeni deneme bütçesi; gerekiyorsa son duruş için en fazla 20 ms ek süre |
| TX aktarım sınırı | Başlangıçtan itibaren toplam 20 ms |
| TX abort sınırı | 20 ms |
| TX FIFO | 8 bekleyen + 1 ayrı aktif öğe |
| Task stack başlangıç değeri | Native API ile 512 `StackType_t` elemanı; F407'de 2048 bayt, ölçümle doğrulanır |
| UART/DMA IRQ hizmet gecikmesi hedefi | En fazla 5 ms; 22,22 ms tur sınırına dayanarak tasarım yapılmaz |
| RX bildiriminden tüketim başlangıcına hedef | En fazla 5 ms, gerçek uygulama yükünde ölçülür |

Bunlar başlangıç mühendislik değerleridir; ölçülmüş kapasite iddiası değildir. Baud/tampon değişirse süre bütçesi yeniden hesaplanır. Zaman karşılaştırmaları `uint32_t` farkıyla yapılır; sayaç taşması test edilir.

### 5.5. Sayaçlar ve sonuçlar

Yeni haberleşme sayaçları `uint32_t` olacak: `rx_bytes_consumed`, `rx_overruns`, `rx_discarded_bytes`, `rx_timeouts`, `rx_start_fails`, `rx_restarts`, `rx_recovery_fails`, `rx_snapshot_defers`, `rx_late_events`; `tx_accepted`, `tx_completed`, `tx_failed`, `tx_cancelled`, `tx_queue_full`, `tx_start_busy`, `tx_start_errors`, `tx_dma_errors`, `tx_timeouts`, `tx_recovery_fails`, `tx_late_events`. `last_rx_error`, `last_tx_error`, son başarısızlık nedeni ve queue yüksek su seviyesi ayrıca tutulur.

`rx_discarded_bytes` ölçülebilen bilinçli düşürmeyi sayar; kesmelerin çok uzun kapalı kaldığı durumda gerçek fiziksel kayıp sayısı diye sunulmaz. Parser'ın mevcut 16 bit sayaçları bu değişiklikte korunabilir; uzun yük testlerinde taşma dikkate alınarak kısa aralıklı delta veya ayrı 32 bit haberleşme sayaçları kullanılır.

Kuyruk boş, aktif öğe yok ve başlamış bütün send_copy çağrıları dönmüşken: `tx_accepted == tx_completed + tx_failed + tx_cancelled` (modüler sayaç hesabıyla). Başarı sayacı, kaynak kopyalama veya DMA TC anında artmaz. Üreticilerin ortak `tx_accepted`/`tx_queue_full` artırmaları kısa koruma altında yapılır; tek yazar varsayılmaz. Enqueue ile sayaç artırma arasına owner girebileceğinden ara snapshot bu muhasebe eşitliği için kabul testi değildir. Queue yüksek su seviyesi aksi kanıtlanmadıkça örneklenen en yüksek derinliktir; tam eşzamanlı tepe olduğu iddia edilmez.

## 6. Yarış koşulları için uygulama kuralları

### 6.1. Olay kutusu ve hata yönlendirme

Olay bitleri: `RX_PROGRESS, UART_ERROR, TX_DONE, RX_ABORT_DONE, TX_ABORT_DONE, TX_REQUEST, RECOVER_REQUEST`. Ham hata bitleri ve RX hata nesli ayrıca saklanır; notification yalnız uyandırma aracıdır.

`HAL_UART_ErrorCallback()` tek tanımlı olacak. Handle eşleşmesi `huart == configured_huart` ile kontrol edilir; aynı Instance üzerinde ikinci handle desteklenmez. `ErrorCode` **bir kez** okunur; DMA handle ErrorCode ve ilgili sağlık bilgileri gerektiğinde aynı kayda eklenir. Yeni HAL çağrılarından sonra eski hatayı `huart->ErrorCode` üzerinden okumaya güvenilmez.

| Hata | RX davranışı | TX davranışı |
|---|---|---|
| PE/FE/NE/ORE, DMA hata biti yok | Durum/hardware kontrolü; kesilen alımı toparla | Sırf bu hata nedeniyle iptal etme |
| HAL_UART_ERROR_DMA | RX durumu ve DMA sağlık kontrolü; gerekiyorsa toparla | SENDING ise belirsiz aktarımı başarısız say ve güvenli durdur |
| Yalnız TX zaman aşımı | Tüketmeye devam et | TX'e özel abort |
| RX tampon taşması | Parser adayını bırak ve yeniden senkron ara | Etkileme |

Yerel HAL `UART_DMAError()` hem TX hem RX sonlandırma yardımcılarını çağırabilir; fiziksel hata kaynağı tek stream olsa da iki yönün yazılım durumu etkilenebilir. `gState == READY` görmek DMA belleği artık okumuyor kanıtı değildir.

Callback kaydı ve owner'ın take-and-clear işlemi aynı kısa korumayı kullanır. Yeni error bits `|=` ile birleştirilir; take-and-clear anında olay maskesi ve hata kodu beraber alınır. Hata/done çakışmasını ayrı bayrak okumalarıyla çözmeye çalışma. Olay kutusu için eski PRIMASK'ı koruyan kısa koruma her iki bağlamda kullanılabilir; RTOS notify çağrısı PRIMASK geri yüklendikten sonra yapılır. Kuyruk kabul durumunun snapshot'ı da kısa korumayla alınır; enqueue bu korumanın dışında kalır.

**Bir aktarımı bitirme/yenisini başlatma sınırı:** Servis başındaki tek snapshot yeterli değildir. Owner yeni TX'i kurmadan ve bir sonucu kesinleştirmeden önce olay kutusunu kısa koruma altında yeniden kontrol eder. İşlenmemiş TX hata/done varsa yeni öğeye geçmez. Temiz sınırda yeni `tx_attempt_id` ve SENDING bilgisi aynı koruma altında yayımlanır; HAL çağrısı koruma dışında yapılır. Callback hata/done kaydına o anda aktif deneme olup olmadığını ve bu kimliği ekler. Aktif TX yokken oluşmuş DMA hatası sonradan başlatılan çerçeveye mal edilmez; RX hata kaydı yine işlenir. Yeni deneme yayımlandıktan sonra gelen hata, HAL çağrısı bitmeden oluşsa da o denemede korunur. TX temizliği RX olaylarını/ham hata geçmişini silemez.

Bu deneme kimliği yalnız yazılımda kaydedilmiş olayların ilişkilendirilmesini sağlar. Gerçek donanımdan gecikmiş callback'i tek başına tanıyamaz; bölüm 6.4'teki eski kaynakların durdurulması şartı devam eder. Tamamlanma ve timeout kararı da son olay kontrolü ile aynı state geçişi sınırında verilir; arada gelen done körlemesine timeout sayılmaz.

### 6.2. RX üretici örneklemesi

Seçilen tasarım, tur sayısını RX callback'inde **yalnız `HAL_UART_RXEVENT_TC` türünde** artırır: `wrap_base += UART_RX_BUF_SIZE`. Olay türü callback'in başında yerel değişkene alınır. Bu projedeki `UART_DMAReceiveCplt()` TC türüyle çağırır; UART IDLE yolu sarım sınırında `Size==256` bildirse bile türü IDLE'dır. HT ve IDLE yalnız bildirim/istatistik üretir. STARTING/RUNNING dışındaki veya DMA abort oturumuna ait callback üretim sayılmaz. Error ile birlikte TC varsa örnek hata nesliyle geçersizleştirilir.

IRQ girişinde ham TCIF okuyup ayrıca tur artırılmaz: giriş kancasıyla HAL'in kendi bayrak okuması arasında TC oluşursa kanca olayı kaçırabilir. Mevcut HAL callback zinciri, gerçekten işlenen TC'yi belirler; HAL driver dosyasını değiştirmek gerekmez.

Owner'da `bool uart_rx_sample_producer(uint32_t *produced)`:

1. Kısa kritik bölümde `wrap_base`, ham TCIF, NDTR, yeniden ham TCIF okunur. CPU ISR çalışamaz, DMA çalışmaya devam eder.
2. TCIF iki okumada değiştiyse veya NDTR 0/geçersizse örnek kabul edilmez; en fazla 3 kısa deneme yapılır. DMA yeniden yükleme anında sonsuz spin yapılmaz.
3. Geçerli `1 <= NDTR <= 256` için `produced = wrap_base + (pending_tc ? 256 : 0) + (256 - NDTR)` hesaplanır. Pending TC henüz ISR'de sayılmamış turdur; örnek fonksiyonu donanım bayrağını temizlemez ve wrap_base'i değiştirmez.
4. Kritik bölüm biter. Üç denemede de örnek tutarsızsa parser/consumer ilerletilmez; örnekleme 1 ms sonraya bırakılır. İlk başarısız örnekten itibaren 20 ms boyunca geçerli örnek alınamazsa RX sağlık hatası kaydedilip toparlanmaya gidilir. Geçerli örnek bu süreyi kapatır. Böylece bozuk NDTR nedeniyle sınırsız meşgul döngü kurulmaz; RX dışındaki işler devam eder.

HAL DMA işleyicisi TCIF'i temizledikten sonra RX TC callback'ini çağırır. Task ISR ortasında çalışamadığından “bayrak temizlendi ama wrap_base henüz artırılmadı” ara durumunu göremez. Bu sözleşme örnek fonksiyonunun ISR'den çağrılmamasına bağlıdır. UART ve RX/TX DMA kesmeleri aynı preemption önceliğinde tutulur; paylaşılan HAL RxEventType değeri iç içe callback'lerle değişmez. DMA start/abort oturum sınırları R2 testinde doğrulanır.

Sayaçlar unsigned modulo çalışır. Meşru farkın 2^31 bayttan küçük olduğu kabul edilir; uzun uyku/çalışmama döneminde eski producer oturumu devam ettirilmez. Her restart yeni üretim oturumudur; sayaç reseti yalnız DMA durdurulup eski IRQ kaynakları temizlendikten sonra yapılır.

Ham bayrakla telafi **en fazla bir bekleyen tur** içindir. Birden fazla tur IRQ kapalı kalırsa sayı yeniden kurulamaz. Bu sınır kapasite ölçümünün parçasıdır; yazılımsal kilit veya `volatile` ile giderilemez. DMA EN/NDTR/TCIF davranışı [RM0090 DMA bölümünden](https://www.st.com/resource/en/reference_manual/rm0090-stm32f4xx-reference-manual-stmicroelectronics.pdf) ve yerel HAL'den doğrulanır.

### 6.3. Kopya sırasında ezilme denetimi

`C = consumed`, `P0 = sample()` alınır. `P0-C >= 256` ise taşma; o tur parser'a veri verilmez. Aksi halde `n = min(32, P0-C, 256-(C%256), kalan_bütçe)` seçilir.

DMA belleğinden n bayt scratch'e alınır. CPU okumaları ve çevre kaydı örneklemelerinin sırası korunur; DMA belleği okuması volatile byte yükleri ve uygun CMSIS bellek bariyerleriyle açık tutulur. F407 için olmayan D-cache bakım kodu eklenmez.

`P1 = sample()` ve RX hata nesli tekrar okunur. P0 veya P1 örnek fonksiyonu false dönerse çıkış değeri kullanılmaz; consumer ilerletilmez, scratch parser'a verilmez ve bölüm 6.2 tekrar zamanı beklenir. Geçerli örnekte `P1-C >= 256`, oturum değişimi veya bekleyen RX hatası varsa scratch düşürülür. Güvenliyse `consumed += n` yapılır, ardından parser scratch ile beslenir. Yeni veri geldiyse bir sonraki tur alınır. Parser/handler sırasında DMA belleğine geri başvurulmaz.

Hata ISR'si kopyanın doğrulama noktasından sonra geldiyse daha önce doğrulanmış çerçevenin teslimi geri alınmaz; sonraki tüketim durdurulur. Tasarım, sınırsız gecikme veya fiziksel hatada hiç çerçeve kaybı olmayacağını vaat etmez.

### 6.4. Güvenli abort ve eski olayların temizlenmesi

TX tekrar kullanılmadan önce stream EN=0, DMAT=0, TXEIE/TCIE kapalı, UART gState READY ve DMA State READY olmalı. İptal sırasında UART kaydırma/veri kaydında kalan baytların bitmesi için TC de gözlenir; hat sessizleşmeden yeni çerçeve başlatılmaz. DMA tamponunun serbestliği ile bütün çerçevenin teslimi ayrı sonuçlardır.

RX restart öncesi stream EN=0, DMAR=0, alımla ilgili kesmeler kapalı, UART RxState ve DMA State READY doğrulanır. Duruş sağlandıktan sonra stream'e ait eski HT/TC/error bayrakları ve ilgili DMA IRQ pending durumu temizlenir; parser adayı bırakılır, SR/DR sırasıyla RX hata bayrakları temizlenir, oturum sayaçları kurulup alım başlatılır.

UART IRQ RX/TX için ortaktır: sırf eski TX tamamlanmasını silmek için USART2 NVIC pending bitini gelişigüzel temizlemek yasaktır. TX'in kendi kaynağı kapatılır; RX hata/IDLE kaynakları korunur. DMA abort hâlâ HAL_STATE_ABORT ise callback işini bitirmeden HAL state alanları elle READY yapılmaz. İptal koşulları sağlanamıyorsa FAULT korunur.

### 6.5. Kuyruğa kabul ile FAULT yarışı

Queue ve notify API'lerini uygulama kritik bölümüne almadan aşağıdaki sıra kullanılır:

1. Üretici girdileri doğrular. Kısa koruma altında `tx_accepting` ve `uint32_t admission_epoch` birlikte okunur. Kapalıysa NOT_READY; açıksa epoch local öğeye kopyalanır.
2. Koruma dışında öğe hazırlanıp `xQueueSendToBack(..., 0)` çağrılır. Queue doluysa QUEUE_FULL; başarılıysa sayaç kısa korumayla artırılır, koruma dışında TX_REQUEST bildirilir ve ACCEPTED döner.
3. Owner her **yeni TX FAULT girişinde** aynı kısa koruma altında kabulü kapatıp admission_epoch'u bir artırır. Queue resetlenmez ve queue nesnesi silinmez; önceden kabul edilmiş öğeler tek tek sonuçlandırılır.
4. Owner FAULT'ta gelen her TX_REQUEST'te de FIFO'yu kontrol eder. Gecikmiş üreticinin FAULT sonrası eklediği öğe uyanma bildirimiyle bulunur ve CANCELLED_FAULT alır; task içinde unutulmaz.
5. Toparlanma olmuş olsa bile kuyruktan alınan öğenin epoch'u güncel değilse gönderilmez, CANCELLED_FAULT üretilir. Böylece FAULT öncesi hazır kontrolü yapan, ancak FAULT+recovery sonrasında enqueue yapan üretici eski işi yeniden canlandıramaz.

`admission_epoch` bir FIFO kabul dönemi kimliğidir; UART callback'ine fiziksel aktarım kimliği yüklemez. Sayaç taşması unsigned hesaplanır; bir send_copy çağrısının 2^32 FAULT dönemi boyunca askıda kalması desteklenmez. Kabul sırasında başka taskın fault/recovery yapabilmesi API'nin asenkron sözleşmesinin parçasıdır.

## 7. Tek task ve düşük CPU için çalışma şekli

### 7.1. Bir servis turu

1. Olay kutusunu atomik al; ham hata kaydını koru.
2. RX/TX hata ve abort durumlarını ilerlet. Geçerli tamamlanmayı zaman aşımı kararından önce işle; hata+done birlikteyse hata öncelikli.
3. RX RUNNING ise en fazla 64 baytı güvenli kopyalarla parser'a ver.
4. Kısa uygulama handler'larını çalıştır; uzun cihaz işlemini UART taskında yürütme.
5. TX IDLE ve aktif öğe yoksa FIFO'dan **beklemeden** bir öğe al. İstek bitinin o turda gelmiş olması şart değil. Aktif tampon hazırlanıp bir kez HAL'e verilir.
6. Sonuçları ve tutarlı snapshot'ı yayımla. RX veri işi/FAULT kuyruk boşaltma gibi hemen yapılabilir iş kaldıysa yeni tur yap; yeni IRQ bekleme.
7. Yapılabilir iş yoksa zamanı yeniden oku, en yakın etkin son tarihe kadar, son tarih yoksa süresiz notification bekle. Tur başında okunan `now` ile uzun handler sonrasındaki bekleme hesaplanmaz.

Kuyruk dolu olsa da TX SENDING/ABORTING iken hemen yapılabilir iş sayılmaz. Aksi halde task sırf kuyruk boş değil diye CPU'yu sürekli kullanır. Her yeni turda RX ve TX'e tekrar sıra gelir; bir yön için içte sonsuz drain yoktur.

### 7.2. Uyku sözleşmesi

Native referans: `xTaskNotify(..., eSetBits)` / gerçek ISR'de `xTaskNotifyFromISR(..., eSetBits, &woken)`. ISR sonunda gerekli yield istenir. Bekleme `xTaskNotifyWait(0, EVENT_MASK, &events, timeout_ticks)` kullanır; girişte olaylar silinmez. Bu notification alanı `ulTaskNotifyTake()` ile paylaşılmaz. Bu mekanizma [FreeRTOS olay biti kullanımına](https://freertos.org/Documentation/02-Kernel/02-Kernel-features/03-Direct-to-task-notifications/04-As-event-group) dayanır.

Boşta RX açık, parser boş, TX boş ve toparlanma yoksa `portMAX_DELAY` ile uyunur; süresiz bekleme için hedef konfigürasyonda `INCLUDE_vTaskSuspend=1` doğrulanır. IRQ olmadan her 1/5/10 ms uyanan sabit bir poll döngüsü kurulmaz.

Süreli bekleme yalnız aktif RX adayının 50 ms son tarihi, TX'in 20 ms son tarihi, abort, RX tekrar denemesi veya geçici producer örnekleme tekrarı varsa hesaplanır. Abort sırasında callback kaybolmasına karşı en fazla bir RTOS tick aralığıyla sağlık kontrolü yapılabilir; bu yalnız geçici abort durumu içindir. Tick hedefi F0'da doğrulanır, mevcut hedef projede rastgele değiştirilmez.

Pozitif milisaniye tick'e yukarı yuvarlanır ve en az 1 tick olur; dolmuş son tarih için 0 verilir. Uyanınca gerçek zaman yeniden kontrol edilir. `HAL_GetTick()` ilerlemesinin RTOS/HAL zaman tabanıyla uyumu doğrulanır. Tickless idle bu tasarım için zorunlu değildir ve ayrı optimizasyon olarak kalır.

Süreli bekleme yalnız ilgili state'in ve o anda uygulanabilir işin son tarihini içerir: RX frame timeout yalnız RUNNING ve dolu parser'da; restart yalnız RETRY_WAIT'te; TX aktarım süresi yalnız SENDING'de. Producer örneklemesi tekrar zamanına ertelenmişse eski frame deadline 0 döndürmez; önce örnekleme tekrarına kadar uyunur. Dolmuş fakat o state'te artık uygulanmayan son tarihin 0 dönmesi taskı sürekli döndüremez. Kesin yazılım üst sınırı nominal süre + en fazla bir tick yuvarlama payı + ölçülen task gecikmesidir. F0'da tick süresi en fazla 5 ms olmalı veya bu başlangıç bütçeleri yeniden hesaplanmalıdır.

Parser timeout işlendiğinde hâlâ bir aday kaldıysa sonraki aday kontrolü için yeni 50 ms son tarihi kurulur; producer ilerlemiş gibi gösterilmez. Parser boşsa bu son tarih tamamen kaldırılır. Böylece önceki dolmuş deadline tekrar tekrar aynı servis turunu tetiklemez.

Uyku öncesi son kontrolden sonra gelen ISR veya üretici isteği notification'da bekler; task beklemeye girer girmez döner. Olay kutusu uyandırma kaydından önce doldurulur. Servis olayları işlerken gelen yeni bildirimler bir sonraki tura kalır.

### 7.3. CPU hedefi nasıl doğrulanacak?

- Boş hatta 10 saniyelik ölçümde UartCommTask'ın periyodik uyanmaması temel kabul koşuludur. FreeRTOS tick/HAL tick IRQ'ları ile UART taskının çalışması ayrı ölçülür.
- İlk performans hedefi: 168 MHz'de her iki yönde 100 Hz × 13 bayt trafikte UART taskı + UART/DMA ISR toplamı ortalama CPU'nun %2'sini aşmasın. Bu ölçüm öncesi hedef, garanti değil.
- Sürekli 115200 tam çift yön trafik de ölçülür; sıfır taşma/CRC kaybı ve bölüm 5.4 gecikme sınırları korunmalıdır. Sonuç yüzdesi ölçülerek belgeye eklenir; tahmin gerçek ölçüm diye yazılmaz.
- Task runtime ölçümü scheduler'da beklenen zamanı CPU zamanı saymamalı. Gerekirse mevcut trace/runtime stats veya GPIO/logic analyzer kullanılır; UART üzerinden yoğun debug log ölçümü bozmamalı.
- Gerçek uygulama yükü hedefi karşılamıyorsa task önceliği, handler süresi ve başka ISR'ların süresi düzeltilir. Körlemesine `vTaskDelay(1)` eklemek RX kapasitesini azaltır. Sürekli backlog, kapasite/takvim sorunudur.

## 8. Uygulama öncesi temel doğrulama

### P0 — Tekrarlanabilir başlangıç ve test düzeni

**Dosyalar:** `Tests/Src/tests.c`, `Tests/Inc/tests.h`, `Core/Src/main.c` USER CODE; gerekirse yeni `Tests/Src/uart_comm_tests.c`, `Tests/Inc/uart_comm_tests.h`.
**Çıktı:** `void uart_comm_tests_run(void)` kontrollü yeni test giriş noktası; üretimde çağrılmaz. Sonuç kaydı PASS/FAIL/NOT_RUN ve test kimliği taşır.

- [ ] Mevcut kaynak değişikliklerini ve build konfigürasyonunu kaydet; kullanıcı değişikliklerini geri alma.
- [ ] CubeIDE Debug **tam rebuild** çalıştır; uart_tx dahil bütün kaynakların build'e katıldığını doğrula. `Debug/subdir.mk` elle düzenlenerek kalıcı kaynak kaydı yapılmaz; IDE yeniden üretir.
- [ ] 29 birim kontrolünü ve T1–T9'u mevcut PA2–PA3 loopback düzeninde çalıştır; gerçek sayıları kaydet. Önceki belgelerdeki ölçümleri yeni çalıştırma sonucu diye kopyalama.
- [ ] Test fonksiyonlarındaki erken `return` durumunu görünür FAIL veya NOT_RUN yap; yalnız `lb_kalan==0` başarı sayılmasın. Beklenen test sayısı da doğrulansın.
- [ ] Kaynak kopyalama/BUSY testleri için başlangıç sayaçlarının delta ölçümlerini kullan. İleride restart sayaçları korunacağından T6 gibi `frames_ok==1` varsayımlarını delta ölçümüne dönüştür.
- [ ] Test kancalarını `UART_COMM_TEST` derleme seçeneğiyle sınırla; üretim derlemesinde kapalı olsun. Kontrollü hata enjeksiyonu ile fiziksel hata deneyini sonuçlarda ayrı etiketle.

**Geçiş koşulu:** Başlangıç build sonucu ve hangi testlerin gerçekten çalıştığı kayıtlı. Kart erişimi yoksa donanım koşulları açık NOT_RUN kalır; ilgili donanım geçiş kapısı tamamlandı işaretlenmez.

## 9. RX uygulama adımları — önce bunlar tamamlanacak

### R1 — Start sahipliği ve açık RX durumları

**Bağımlılık:** P0.  
**Dosyalar:** `Core/Inc/uart_rx.h`, `Core/Src/uart_rx.c`, test dosyaları.  
**Arayüz:** Mevcut `HAL_StatusTypeDef uart_rx_start(UART_HandleTypeDef *)` korunur; `uart_rx_state_t` mevcut joystick yapısının adı olduğundan durum enum'u **`uart_rx_phase_t`** adlandırılır. `uart_rx_phase_t uart_rx_get_phase(void)` eklenir.

- [ ] `RX_START_BUSY_PRESERVES_STATE`: çalışan alımda yarım çerçeve ver, ikinci start HAL_BUSY dönsün; parser adayı, tüketici, sequence ve sayaçlar aynı kalsın; kalan baytlarla çerçeve bir kez çözülsün. Mevcut kodda başarısızlığı göster.
- [ ] `RX_REBIND_ACTIVE`: eski handle/DMA aktifken başka handle ile start reddedilsin; eski sahiplik değişmesin.
- [ ] R1 state enum'unu ve active/same-handle kontrollerini tüm sıfırlamalardan önce ekle. Soğuk parser init yalnız ilk geçerli kurulumda, yeniden başlatma için discard kullan.
- [ ] R1'in mevcut bloklayan recover yolunu da phase alanıyla tutarlı hale getir: normal start ve başarılı eski recover RUNNING, çözülemeyen hata FAULT olsun. `_IT` geçişi R4'e kadar bekler; yeni phase eklenmesi mevcut toparlanmayı devre dışı bırakmasın.
- [ ] STARTING'i HAL çağrısından önce kur; dönen HAL_OK ile birlikte RxState/DMAR/EN ve bekleyen hata kaydını değerlendir. Başarısızlığı istatistik ve phase'te görünür yap. Geçici olarak FAULT'a kapanabilir; otomatik toparlanma R4'te tamamlanacak.
- [ ] `RX_START_FAIL_VISIBLE` ve “HAL çağrısı dönmeden error callback” testlerini ekle; çalışmayan RX RUNNING görünmesin.
- [ ] Build ve P0 regresyonlarını çalıştır; değişen reset sözleşmesini header/test açıklamalarına geçir.

**Geçiş koşulu:** Start reddi hiçbir canlı alım durumunu değiştirmiyor; başarılı/başarısız başlangıç ayrılabiliyor.

### R2 — Gerçek tur sayacı ve tutarlı üretici örneği

**Bağımlılık:** R1.  
**Dosyalar:** `uart_rx.c/.h`, test dosyaları.  
**Arayüz:** İçeride `static bool uart_rx_sample_producer(uint32_t *produced)`; bölüm 6.2 algoritması ve mevcut HAL RX callback'i. Yeni DMA IRQ kancası yok.

- [ ] `RX_PRODUCER_BOUNDARIES` testini kur: başlangıç 0; 127/128/255/256/257/512 üretilen baytta beklenen mutlak konum aynı değer olsun.
- [ ] RX callback'inde yalnız TC olayına ait tur sayacını ekle. HT ve TC kesmelerini açık tut. `HAL_DMA_IRQHandler(&hdma_usart2_rx)` mevcut IRQ'da bir kez çağrılmaya devam etsin.
- [ ] `static void rx_reset_progress_after_stop(void)` ile wrap_base/consumed ve örnekleme zamanlarını tek yerde kur. R4'ten önce çalışan mevcut start/recover yollarını da bu yardımcıya bağla; yalnız ilk start'ı güncelleme. `RX_PRODUCER_RESTART`: birden çok turdan sonra recovery yap; yeni oturumun ilk baytı producer=1 olsun, sahte taşma oluşmasın.
- [ ] Pending TC telafisini, NDTR=0 için sınırlı yeniden örneklemeyi ve sayaç aritmetiğini bölüm 6.2'ye göre uygula.
- [ ] `RX_SAMPLE_STUCK`: sürekli geçersiz NDTR'de 3 okuma sonrası 1 ms erteleme ve 20 ms'de görünür sağlık hatası doğrulansın. R2'de güvenli geçici sonuç FAULT'tur; R4 bu olayı aynı RX toparlanma akışına bağlayacak.
- [ ] `RX_TC_IDLE_SAME_POSITION`: TC+IDLE aynı konumu bildirdiğinde yalnız 256 bayt say; `Size==256` bildirimini yeni tur diye ikinci kez toplama.
- [ ] `RX_PENDING_TC`: TC bayrağı set, ISR henüz çalışmamış, NDTR yeniden yüklenmiş durumda doğru konumu bul; ISR sonrası aynı producer yeniden bulunmalı.
- [ ] `RX_COUNTER_WRAP`: wrap_base ve consumed UINT32 sınırını geçerken fark doğru kalsın. `RX_ABORT_TC_IGNORED`: abort kaynaklı TC normal üretime eklenmesin.
- [ ] Register/HAL modeliyle sınır testlerini ve gerçek kartta sarım testini ayrı çalıştır. Model sonuçları donanım NDTR/TC sırasının kanıtı diye sunulmasın.

**Geçiş koşulu:** Producer geriye sıçramıyor, aynı tur iki kez sayılmıyor; NDTR sınırında CPU sonsuz beklemiyor.

### R3 — Taşma politikası, güvenli scratch ve servis bütçesi

**Bağımlılık:** R2.  
**Dosyalar:** `uart_rx.c/.h`, test dosyaları.  
**Arayüz:** `bool uart_rx_service_budget(uint16_t budget)` eklenir; true hemen işlenebilir RX işi kaldığını belirtir. Örnekleme tekrar zamanı henüz gelmemişse true dönüp spin oluşturmaz. Geçici `uart_rx_service()` bunu 64 baytla çağırabilir. `uart_rx_drain()` iç/destek arayüzüne çekilir; yeni uygulama testleri onu doğrudan çağırmaz.

- [ ] `RX_FULL_LAP_LOSS_VISIBLE`: task tüketimini durdururken IRQ'lar açık kalsın; tam 256 ve 300 bayt giriş ayrı ayrı taşma saysın. `RX_MULTILAP`: 768 baytta da sessiz boş sonucu olmasın.
- [ ] `available>=256` politikasını ekle; parser discard, kayıp/sayaç kaydı ve consumer'ın güncel producer'a taşınması bir kez yapılsın.
- [ ] 32 bayt scratch, bölüm 6.3 ön/son kontrolü ve tur başına 64 bayt bütçeyi uygula. `frame_parser_feed` doğrudan DMA adresi almayı bıraksın.
- [ ] `RX_OVERWRITE_DURING_COPY`: kopya sırasında üreticiyi ilerlet; doğrulama başarısızsa handler hiç çağrılmasın. Hata nesli değişimini de aynı test grubunda sınat.
- [ ] `RX_POST_COPY_SAMPLE_FAIL`: P0 geçerli, P1 başarısızken eski/başlatılmamış P1 değerini kullanma; consumer aynı kalsın, handler çağrılmasın, tekrar son tarihine kadar spin olmasın.
- [ ] `RX_BUDGET_REMAINS`: 100 bayt beklerken ilk tur en fazla 64 tüketilsin ve true dönsün; yeni IRQ olmadan sonraki tur kalanını bitirsin.
- [ ] `RX_RESYNC_AFTER_OVERRUN`: taşma sonrası yeni geçerli çerçeve yalnız bir kez teslim edilsin; önceki yarım çerçeveye eklenmesin.

**Geçiş koşulu:** Tüketici/DMA yarışı için sabit kopya yolu var; kayıp görünür; TX'in servis almasını engelleyen sınırsız RX döngüsü yok.

### R4 — Bloklamayan RX toparlanması

**Bağımlılık:** R3.  
**Dosyalar:** `uart_rx.c/.h`, `stm32f4xx_it.c` USER CODE, test dosyaları.  
**Arayüz:** `void uart_rx_on_error(UART_HandleTypeDef *, uint32_t error)`, `void uart_rx_on_abort_complete(UART_HandleTypeDef *)`, `void uart_rx_on_uart_irq_exit(void)`; durum geçişleri yalnız owner'da.

- [ ] `RX_ERROR_DMA_FE`, `RX_RETRY_EXHAUSTED`, `RX_CALLBACK_MISSING` testlerini kur. Hata sonrası durdurulan RX, başarılı restart sonrası çerçeve almalı; 5 start / 100 ms sınırı yeni denemeleri kapatmalı, varsa son 20 ms durdurma sonrası görünür FAULT üretmeli.
- [ ] `HAL_UART_AbortReceive()` yerine `_IT` durum akışını uygula. ABORTING state/timestamp HAL çağrısından önce yazılsın; her serviste HAL abort tekrar çağrılmasın.
- [ ] R2'deki sürekli producer örnekleme hatasını da bu ortak RX toparlanma akışına bağla; her örnekleme hatası yeni ve sınırsız bir deneme dönemi açmasın.
- [ ] `HAL_UART_IRQHandler()` sonrası USER CODE kancasından RX sağlık bozulmasını bildir. HAL'in başlattığı DMA abort sürüyorsa task onun tamamlanmasını beklesin; callback pointer'ını ikinci abort ile ezmesin.
- [ ] IRQ çıkış sağlık kancası yalnız STARTING/RUNNING oturumunun sağlıksız hale gelmesini bildirsin. FAULT/ABORTING'te her ilgisiz TX TC kesmesinde yeni RX hata dönemi açmasın; aynı oturumun yinelenen sağlık kaydı birleştirilsin.
- [ ] Abort tamamlanma callback'ini ve IRQ çıkış kaydını aynı hata döneminde birleştir. Yeni hata bildirimi deneme bütçesini sıfırlamasın.
- [ ] Bölüm 6.4 güvenli duruşunu doğrula; sonra RX hata bayraklarını, parser adayını ve yeni oturumu hazırla. Çalışan TX'e dokunma. RUNNING dışındayken parser/drain/timeout çalıştırma.
- [ ] `RX_ABORT_SYNC_CALLBACK`, `RX_ABORT_CALLBACK_WITH_EN_SET`, `RX_ERROR_EACH_RESTART` testlerini ekle: doğrudan callback güvenli, EN set ise restart yok, her denemede hata sayacı başa dönmüyor.
- [ ] `RX_NEW_ERROR_AFTER_HEALTHY_IDLE`: restart sonrası 2 saniye hatasız sessizlikten sonra hata ver; periyodik uyanmaya ihtiyaç olmadan yeni deneme dönemi açılsın, eski dönem süresi nedeniyle anında FAULT oluşmasın.
- [ ] “RX FAULT + donanım hâlâ aktif” durumunda recovery yeni buffer/start kurmasın; önce duruş sağlasın. Sağlanamazsa FAULT kalsın.
- [ ] `RX_BUDGET_EXPIRES_ACTIVE_DMA`: beşinci başarısız deneme veya 100 ms sınırında EN=1 bırak; final_stop akışı çalışsın, tekrar start olmasın. Süre sonunda duruş yoksa rx_quiescent=false ve kilitli buffer doğrulansın. `RX_FAULT_TX_IRQ_NO_RETRY`: sağlıklı TX tamamlanmaları RX'te yeni otomatik deneme başlatmasın.

**Geçiş koşulu:** RX hata toparlaması ortak taskı bekletmiyor; dönem sonlu; callback ile fiziksel duruş ayrılmış.

### R5 — Timeout, handler ve RX kapanış testi

**Bağımlılık:** R4.  
**Dosyalar:** `uart_rx.c/.h`, `main.c` USER CODE veya yeni `app_protocol.c/.h`, test dosyaları.  
**Arayüz:** `void uart_rx_set_handler(frame_handler_t handler, void *user)` yalnız start öncesi; `uint32_t uart_rx_next_wait_ms(uint32_t now)` (0: hemen iş; UINT32_MAX: son tarih yok). Bu fonksiyon frame timeout, abort, restart ve varsa producer örnekleme tekrarının en yakın süresini döndürür.

- [ ] Joystick çözme/uygulama sequence takibini kısa uygulama handler'ına taşı. RX taşıma katmanı yalnız parser ve teslimden sorumlu olsun. Sequence gap, kaybolan paket adediyle eşit diye raporlanmasın.
- [ ] Timeout'u en son gözlenen üretici ilerlemesine bağla. Önce bütçeli tüketim; backlog veya yeni producer ilerlemesi varsa timeout verme. Geçerli örnek alınamazsa parser'dan bayt düşürme.
- [ ] Timeout sonrası kalan adayın yeni son tarihini bölüm 7.2'ye göre kur; son üretici ilerleme zamanı ile timeout müdahale zamanını ayrı tut. `RX_TIMEOUT_REARM`: bir aday bırakılıp başka yarım aday kaldığında sonraki kontrol 50 ms sonrasına kurulsun; parser boşsa deadline silinsin.
- [ ] `RX_TIMEOUT_QUIET`: yarım başlık + 50 ms ilerlemesizlikte timeout; `RX_TIMEOUT_CONTINUATION`: 47. ms'de başlayan 57 baytlık devam sırasında geçerli çerçeve düşmesin.
- [ ] `RX_TIMEOUT_NO_CANDIDATE`: parser boşken timeout uyanması planlanmasın. `RX_PAYLOAD_LIFETIME`: handler'ın kopyaladığı değer parser ilerlese de sabit kalsın.
- [ ] R1–R4 ve T1–T6 regresyonlarını çalıştır; yeni idle/deadline sözleşmesini kaydet.

**RX geçiş kapısı:** R1–R5 testleri tamam; gerçek FE/ORE sonrası alım doğrulanmış veya ilgili test açıkça NOT_RUN. TX geliştirmesine geçilirken RX'te tamamlanmamış güvenilirlik işi gizlenmez.

## 10. TX uygulama adımları

### T1 — Hata kapıları ve tutarlı olay alma

2026-10-05: T1–T4 doğrulama kanıtları ve model/kart ayrımı
[`UART_UYGULAMA_DURUMU.md`](docs/archive/UART_UYGULAMA_DURUMU.md) dosyasındadır.
Bu önceki kabul koşusunun kapsamıydı. Kullanıcının son birleşme/FreeRTOS isteğiyle F0–M2 yeniden kapsama alındı; güncel kayıt UART_RTOS_UYGULAMA_PLANI.md içindedir.

**Bağımlılık:** R5.  
**Dosyalar:** `uart_tx.c/.h`, geçici ortak callback sahibi `uart_rx.c`, test dosyaları.  
**Arayüz:** Header'da mevcut `uart_tx_on_error(UART_HandleTypeDef *, uint32_t)` ve `uart_tx_on_abort_complete(UART_HandleTypeDef *)` uygulanır.

- [x] `TX_DMA_ERROR_REPORTED`: HAL DMA hata kaydı TX servisince görülsün. `RX_LINE_ERROR_PRESERVES_TX`: yalnız FE/NE/ORE/PE TX'i iptal ettirmesin.
- [x] Ortak callback ErrorCode'u bir kez yakalayıp RX'e; DMA biti varsa TX'e bildirsin. Yön kararı için callback girişindeki gState tek başına kullanılmasın.
- [x] TX done/error/abort bayrakları ve ham error bits aynı kısa kritik bölümde snapshot olarak alınsın. Callback sıradaki aktarımı başlatmasın.
- [x] Bölüm 6.1'deki aktif deneme kaydını ekle; aktif TX yokken gelen DMA error kaydını sonraki denemenin hatası diye yorumlama. T3 abort akışı yazılana kadar aktif TX hatasında konservatif FAULT ile tamponu kilitle; hata+done başarı sayılmasın.
- [x] `TX_DONE_AND_ERROR`: aynı tur hata+done geldiğinde başarı sayılmasın. `TX_EVENT_DURING_TAKE`: snapshot sırasında/sonrasında gelen olay kaybolmasın.

**Geçiş koşulu:** Olay ile karar ayrılmış; RX başlatmasının HAL ErrorCode'u temizlemesi TX hata bilgisini yok etmiyor. Bu aşamada callback tek tanımlı kalır.

### T2 — Başlatma sözleşmesi ve tampon güvenliği

**Bağımlılık:** T1.  
**Dosyalar:** `uart_tx.c/.h`, test dosyaları.  
**Arayüz:** `uart_tx_send_copy(const uint8_t *data, uint16_t len)`; enum'a `UART_TX_START_BUSY`, `UART_TX_START_ERROR` eklenir. Mevcut `UART_TX_BUSY` yalnız modülün aktif/aborting meşguliyetidir.

- [x] `TX_INVALID_LENGTH`: 0/65/256 ve NULL reddedilsin. `TX_SOURCE_COPY` ve `TX_SECOND_SEND_BUSY` mevcut testlerini koru.
- [x] Init ve send öncesi sahiplik/donanım koşullarını kontrol et. SENDING/ABORTING sırasında init HAL_BUSY; güvenli duruşu doğrulanmamış FAULT'ta reset yok.
- [x] Eski olayları ve yalnız TX'e ait eski donanım kaynaklarını temizleyip buffer/length/start_tick/state'i HAL çağrısından önce tutarlı kur.
- [x] `TX_ERROR_BETWEEN_SERVICE_AND_START`: servis snapshot'ından sonra, yeni TX kurulmadan önce DMA hatası ver; eski olay yeni aktarıma mal edilmesin veya silinmesin. `TX_ERROR_DURING_HAL_START`: yeni deneme yayımlandıktan sonra gelen hata korunsun ve T3'te o denemenin abort'unu doğursun.
- [x] HAL_OK sonrası her TX başlangıcında gerekmeyen TX HT kesmesini atomik tek-bit yazmasıyla kapat; RX HT ve TX TC/error kesmeleri açık kalsın. F407 uygulaması peripheral bit-band kullanır: yerel HAL'in `__HAL_DMA_DISABLE_IT` read-modify-write işlemi aktif stream EN/IRQ bitlerini eski değerle geri yazabilir. HAL çağrısı dönmeden olası HT oluşması güvenli olmalı; TX half callback task bildirimi üretmesin. Amaç gereksiz yarım-gönderim kesme yükünü azaltmak.
- [x] HAL_OK, HAL_BUSY ve HAL_ERROR'u ayrı kaydet. Hata halinde güvenli duruş biliniyorsa IDLE; belirsizse bu adımda konservatif FAULT ile tamponu kilitle. Otomatik ABORTING geçişi T3'te eklenecek; T2 henüz yazılmamış abort fonksiyonuna bağımlı kalmayacak. Üst katman başarısız dönüşte çerçeveyi kendi isteği olmadan tekrar göndermez.
- [x] `TX_COMPLETE_BEFORE_HAL_RETURN`, `TX_HAL_START_BUSY`, `TX_HAL_START_ERROR` testlerini çalıştır. Tampon aktifken değiştirilmesin; HAL başlangıç hatası başarı sayılmasın.

**Geçiş koşulu:** Her dönüşün anlamı açık; aktif veya belirsiz DMA tamponunun üzerine yazılamıyor.

### T3 — Süre sınırı, abort ve otomatik hazır hale gelme

**Bağımlılık:** T2.  
**Dosyalar:** `uart_tx.c/.h`, test dosyaları.  
**Arayüz:** Mevcut `void uart_tx_service(void)`; yeni `uint32_t uart_tx_next_wait_ms(uint32_t now)` ve `bool uart_tx_take_result(uart_tx_result_t *out)`. T3'te tanımlanacak `uart_tx_result_t`: code alanı için `UART_TX_RESULT_COMPLETE`, `UART_TX_RESULT_START_BUSY`, `UART_TX_RESULT_START_ERROR`, `UART_TX_RESULT_DMA_ERROR`, `UART_TX_RESULT_TIMEOUT` değerli enum; ayrıca `uint32_t hal_error` ve `bool recovery_fault`. TX sonucu F2'de ortak sonuç tipine eşlenecek.

- [x] `TX_NO_COMPLETION_TIMEOUT`: 20 ms içinde tamamlanmayan aktarım ABORTING'e geçsin; `TX_DMA_ERROR_ABORT`: hata beklemeden abort başlatsın.
- [x] `HAL_UART_AbortTransmit_IT()` yalnız bir kez çağrılsın. State/abort_tick çağrıdan önce hazır olsun; RX abort veya genel `HAL_UART_Abort()` çağrılmasın.
- [x] T2'de belirsiz başlangıç hatası için kullanılan konservatif FAULT kapanışını bu adımda aynı ABORTING akışına bağla; güvenli duruş sonrası başlangıç hata sonucu korunarak IDLE'a dönülsün.
- [x] Bölüm 6.4 duruş koşullarını callback gelse de gelmese de değerlendir. Koşullar sağlanırsa IDLE'a dön; başarısız çerçeve sayacını bir kez artır, frames_sent artırma.
- [x] 20 ms abort sınırında duruş doğrulanamazsa FAULT. `TX_ABORT_SYNC_CALLBACK`, `TX_ABORT_CALLBACK_WITH_EN_SET`, `TX_ABORT_NO_CALLBACK_SAFE_HW` testlerini çalıştır.
- [x] `TX_LATE_DONE_IN_ABORT`, `TX_LATE_DONE_IN_FAULT`: durum ve başarı sayacı değişmesin. `TX_REUSE_AFTER_ABORT`: güvenli duruş sonrası init gerektirmeden yeni gönderim kabul edilsin; eski event erken bitirmesin.
- [x] Tamamlanma service gecikmesinden önce oluşmuşsa geçerli done önce işlensin; yalnız wall time 20 ms geçti diye başarılı aktarım düşürülmesin. DMAError ile birlikteyse başarısız say.
- [x] `TX_DONE_DURING_TIMEOUT_DECISION`: timeout kararı kurulmadan gelen done son olay kontrolünde görülsün; state geçişiyle tutarlı işlensin. Callback aynı anda geldi diye sonuç iki kez verilmesin.
- [x] HAL'e gerçekten sunulmuş her deneme için sonuç tek öğelik modül sonuç kutusuna yazılsın; başlangıç HAL_BUSY/HAL_ERROR da buna dahil. Başlatma öncesi INVALID/BUSY/NOT_READY reddi sonuç üretmez. `take_result(NULL)` false; geçerli out ile sonuç varsa kopyalayıp tüketir. Sonuç alınmadan yeni deneme BUSY döner; eski sonuç ezilmez. T3 ile beraber mevcut test/ana döngü tüketicilerini sonuç kutusunu boşaltacak şekilde güncelle; F2 bunu callback'e dönüştürecek.
- [x] UINT32 tick sarımı, FAULT recovery ve aktif TX sırasında RX tüketiminin devamını sınat.

**Geçiş koşulu:** TX süresiz BUSY kalmıyor; aktarım başarısızlığı ve modülün yeniden hazır olması ayrı; güvenli duruş yoksa tampon kilitli.

### T4 — Bare-metal RX/TX bütünleşik doğrulama

**Bağımlılık:** T3.  
**Dosyalar:** `tests.c/.h`, `uart_comm_tests.c/.h`, `main.c` USER CODE.

- [x] 40 × 13 = 520 bayt sarım senaryosunu TX DMA ile çalıştır. Her çerçeve normal servis/tamamlanma zincirinden geçsin.
- [x] Tek, birleşik, parçalı, 64 baytlık ve sequence sarımlı çerçeveleri RX/TX eşzamanlı doğrula.
- [x] Aktif TX sırasında gerçek RX FE/ORE; RX sürerken kontrollü TX DMA hatası; iki yön sürerken ortak DMA hata yolunu ayrı ayrı sınat.
- [x] Her helper hem RX hem TX service çalıştırsın; yalnız RX servis eden bekleme helper'ları aktif TX'in tamamlanmasını geciktirmesin.
- [x] HAL'e doğrudan blocking gönderim kullanan eski testler yalnız izole bare-metal test düzeninde çalışsın. RTOS üretim döngüsüne taşınmasın.
- [x] Tam rebuild, callback sembol kontrolü, buffer SRAM adresleri ve sonuç muhasebesini doğrula.

**TX geçiş kapısı:** T1–T4 sonuçları tamam; RX/TX hata toparlaması birbiriyle çakışmıyor. RTOS geçişi bundan sonra başlar.

## 11. Tek FreeRTOS taskına geçiş

### F0 — Hedef proje, CubeMX ve IRQ önkoşulları

**Bağımlılık:** T4.  
**Dosyalar:** Hedef `.ioc`, `FreeRTOSConfig.h`, üretilen RTOS/IRQ/timebase dosyaları ve USER CODE alanları.

- [x] Hedef projenin yolu, kullanılan HAL sürümü, FreeRTOS portu ve native/CMSIS API seçimi kaydedilsin. F0 tamamlanmadan bu dosyaların var olduğu varsayılmasın.
- [x] CubeMX'te mevcut USART2/pin/DMA atamalarını koru. NVIC gruplaması GROUP_4; USART2, DMA1_Stream5 ve DMA1_Stream6 aynı, RTOS API'ye uygun preemption önceliğinde olsun.
- [x] Örnek olarak `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY=5` ise üçünü 5 seç; sayı doğrudan kopyalanmadan gerçek `configPRIO_BITS` ve shifted `configMAX_SYSCALL_INTERRUPT_PRIORITY` ile doğrula. Mevcut 0 önceliğinde FromISR çağrısı etkinleştirme.
- [x] SysTick/SVC/PendSV tek tanımlı olsun. HAL tick kaynağı ve FreeRTOS tick sahibi açıkça belirlensin. HAL timebase için gerekiyorsa boş timer CubeMX'te seçilip açıklansın; kullanılan timer'a sessizce el koyma.
- [x] Task notification, statik allocation, `INCLUDE_vTaskSuspend` ve assert seçeneklerini doğrula. Taskın başka kütüphanece kullanılan notification alanını paylaşmadığından emin ol.
- [x] Ek UART timer/taskı üretme. UartCommTask'ı hem CubeMX hem elle ikinci kez oluşturma; native referansta oluşturucu uart_comm_init olacak.
- [x] Task önceliğini hedef uygulamanın tasklarıyla birlikte belirle. İzole doğrulamada başlangıç `tskIDLE_PRIORITY+2` olabilir; gerçek yükte en fazla 5 ms RX hizmet gecikmesi test edilmeden kalıcı kabul edilmez.

Kesme önceliği kuralları [FreeRTOS Cortex-M3/M4 açıklamasıyla](https://freertos.org/Documentation/02-Kernel/03-Supported-devices/04-Demos/ARM-Cortex/RTOS-Cortex-M3-M4) doğrulanır. Bu adım `FromISR` kodunun etkinleştirilmesinden **önce** tamamlanır.

**Geçiş koşulu:** Hedef RTOS projesi derleniyor, tek interrupt/timebase sahibi var, UART taskı için kaynak ve öncelik bütçesi belli.

### F1 — Tek owner taskı, callback merkezi ve olay bekleme

**Bağımlılık:** F0.  
**Yeni dosyalar:** `Lib/Uart/uart_comm.c`, `Lib/Uart/uart_comm.h`.
**Değişen:** `uart_rx.c/.h`, `uart_tx.c/.h`, `main.c` USER CODE, IRQ USER CODE.  
**Arayüz:** Bölüm 5.2 init; `static void UartCommTask(void *argument)`; iç `comm_notify(uint32_t events)` ve `comm_service_once(uint32_t now)`.

- [x] Init statik task kaynağını kursun; return değeri kontrol edilsin. Queue ve send_copy F2'de eklenecek; F1'de henüz kullanılmaz. 512 StackType_t başlangıç stack'i kullan; CMSIS'e çevrilirse stack biriminin bayt olduğunu varsaymadan arayüzü kontrol et. F1 TX testlerini yalnız owner task içinde kontrollü test adımıyla mevcut TX arayüzünden başlat.
- [x] F1'deki owner test akışı T3 take_result kutusunu da tüketip sonucu kaydetsin; sadece tx_service çağrısı yapıp alınmamış sonuç nedeniyle ikinci gönderimi BUSY bırakmasın. F2 bu aynı tüketimi tag'li public callback'e bağlayacak.
- [x] RX/TX/error/abort HAL callback'lerini tek seferde uart_comm.c'ye taşı; eski tanımları aynı adımda kaldır. Şimdilik mevcut modül bildirim kapılarını çağırabilirler.
- [x] Bu taşımada eksik iki kapıyı aynı adımda tanımla: `void uart_rx_on_event(UART_HandleTypeDef *huart, uint16_t size, HAL_UART_RxEventTypeTypeDef type)` ve `void uart_tx_on_complete(UART_HandleTypeDef *huart)`. Ortak RX callback type'ı hemen okur ve kapıya değer olarak verir. RX tur sayacı/istatistiği kapıda güncellenir; sonra comm_notify çağrılır. Error/abort için R4/T1 kapıları kullanılır. Her ham olay modüle yalnız bir kez verilir; notification dönüşündeki bitlerle ikinci kez enjekte edilmez.
- [x] `comm_notify` gerçek ISR'de FromISR, thread bağlamında normal notify kullansın. Task handle hazır değilse RTOS çağrısı yapmasın; olay kaydı korunmalı. DMA task başlayana kadar açılmasın.
- [x] Ortak servis turunu bölüm 7'ye göre kur. R5/T3 deadline hesaplarını kullan; aktif süre sınırı yoksa süresiz bekle. Bare-metal main service çağrılarını RTOS seçeneğinde kaldır.
- [x] `TASK_EVENT_BEFORE_SLEEP`, `TASK_EVENT_DURING_SERVICE`, `TASK_RX_TX_ERROR_TOGETHER`, `TASK_SYNC_ABORT_CALLBACK` testlerini ekle.
- [x] Tek owner'ı doğrula: başka task aynı huart üzerinde HAL start/abort/transmit çağırmıyor; kısa handler service'e tekrar girmiyor.

**Geçiş koşulu:** Tek UART taskı uyuyup olayla uyanıyor; senkron callback de güvenli; yeni olay uyku geçişinde kaybolmuyor.

### F2 — Kopyalı FIFO, sonuç teslimi ve FAULT politikası

**Bağımlılık:** F1.  
**Dosyalar:** `uart_comm.c/.h`, test dosyaları.  
**Arayüz:** Bölüm 5.2 send_copy, request_recovery ve TX sonuç handler'ı.

```c
typedef struct {
    uint32_t tag;
    uint32_t admission_epoch;
    uint16_t len;
    uint8_t bytes[FRAME_MAX_SIZE];
} uart_comm_tx_item_t;
```

- [x] `uart_comm_init` içine task oluşturulmadan önce `xQueueCreateStatic(8, sizeof(uart_comm_tx_item_t), ...)` ile FIFO kurulumunu ekle. Storage boyutunu `8 * sizeof(...)` hesapla; padding'i göz ardı etme. Kuyrukta kullanıcı tamponunun işaretçisi bulunmasın. F1 taskı ikinci kez oluşturulmasın.
- [x] Bölüm 6.5 sırasıyla gate/epoch snapshot'ı al; local item'ı sıfırlayıp alanlarını/kullanılan baytlarını doldur. `xQueueSendToBack(..., 0)` **kritik bölüm dışında** olsun. Yalnız başarıda TX_REQUEST bildir. Kaynak tamponu API dönüşünde serbesttir; kullanılmayan baytlar/padding nedeniyle rastgele stack içeriği kuyruğa taşınmasın.
- [x] Owner FAULT'a geçerken kısa koruma altında gate'i kapatıp admission_epoch'u artırır. Queue API/notify bu koruma altında çağrılmaz. Normal dequeue ve FAULT boşaltmasında öğenin epoch'unu kontrol et; eski dönem öğesi recovery sonrası gelse de CANCELLED_FAULT alsın.
- [x] Owner bir öğeyi kuyruktan aktif kayda alır; HAL dönüşü başarısız olsa bile kimlik/veri/sonuç kaydı korunur. SENDING/ABORTING sırasında ikinci öğe aktif tampona alınmaz.
- [x] Başarı veya hata sonucu yalnız bir kez üret; abort gerekli ise güvenli çözülme/FAULT sonrası sonucu kesinleştir. Abort failure orijinal DMA/timeout nedenini silmesin, `recovery_fault` ile ayrı bildirilsin.
- [x] T3'ün take_result arayüzünü her tur, yeni öğe almadan önce tüket. Sonuca aktif öğenin tag'ini ekleyip callback'i çağır; callback sırasında yeniden send_copy mümkün olduğundan eski active_valid/result sahipliğini callback'ten önce kapat. INVALID/BUSY/NOT_READY gibi HAL'e hiç girilmemiş beklenmedik alt-modül reddinde sonuç kutusunu sonsuza kadar bekleme: kabul edilmiş aktif öğeyi START_ERROR ile bir kez sonuçlandır, durum tutarsızsa TX kabulünü FAULT politikasıyla kapat.
- [x] TX FAULT'ta kabulü kapat; her servis turunda en fazla bir bekleyen öğeyi CANCELLED_FAULT ile çıkar. Kuyruk boşaltılırken RX servis almaya devam etsin. Recovery kabulü ancak iptaller tamamlandıktan ve duruş doğrulandıktan sonra açsın.
- [x] `QUEUE_EIGHT_PLUS_ACTIVE`, `QUEUE_FULL_NO_OVERWRITE`, `QUEUE_MULTI_PRODUCER_ORDER`, `QUEUE_WAKE_AFTER_TX_DONE`, `QUEUE_ACCEPT_FAULT_RACE`, `QUEUE_ONE_RESULT_PER_ACCEPT` testlerini çalıştır. Çok üreticili FIFO sırası başarılı enqueue sırasıdır; API çağrısına giriş sırası değildir.
- [x] `QUEUE_ENQUEUE_AFTER_FAULT_DRAIN` ve `QUEUE_ENQUEUE_AFTER_RECOVERY`: üreticiyi gate snapshot'ından sonra beklet, owner'a FAULT/boşaltma ve gerekirse recovery yaptır, sonra enqueue'ya devam ettir. Eski epoch hiçbir durumda hatta gönderilmesin; bir kez CANCELLED_FAULT gelsin. `QUEUE_RESULT_BEFORE_SEND_RETURN`: hızlı owner sonucu üretici API dönmeden teslim edebilse de uygulamanın önceden hazırladığı tag kaydı geçerli kalsın.

Statik bellek seçimi task ve kuyruk yaşam süresini sabit tutar; kuyruk öğeleri değer olarak kopyalanır. [FreeRTOS statik allocation yaklaşımı](https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/09-Memory-management/03-Static-vs-Dynamic-memory-allocation) esas alınır.

**Geçiş koşulu:** Kuyruk doluluğu görünür, kabul edilmiş öğeler sessizce kaybolmuyor; TX meşgulken queue var diye task spin yapmıyor.

### F3 — Uygulama teslimi, snapshot ve düşük CPU doğrulaması

**Bağımlılık:** F2.  
**Dosyalar:** `uart_comm.c/.h`, uygulama handler dosyası, test dosyaları.  
**Arayüz:** Bölüm 5.2 `uart_comm_get_snapshot` ve handler sözleşmesi tamamlanır.

- [x] Owner servis sonunda public snapshot'ı kısa kritik bölümde yayımlasın; getter aynı korumayla kopyalasın. Sadece okuyucuyu kilitleyip çok alanlı yazar güncellemesini korumasız bırakma.
- [x] Bölüm 5.2'deki snapshot tipini eksiksiz tanımla; R/T sayaçlarını bölüm 5.5'in alanlarına eşle. Init öncesi snapshot initialized=false; yalnız RX FAULT'ta rx_ready=false ve sağlıklı TX için tx_accepting=true gösterilebilsin.
- [x] RX handler payload'ı gerekiyorsa uygulama belleğine kopyalasın. Handler HAL çağırmasın ve TX tamamlanmasını beklemesin.
- [x] Komut yanıtının ilk örneğini `FRAME_TYPE_RESPONSE`, mevcut SEQ/TYPE/sonuç formatıyla kur; aynı FIFO'ya koy. Kuyruk doluysa sonucu kontrol edip yanıt düşmesini say; başarı raporlama.
- [x] Yanıtı zorunlu ve yan etkili gerçek cihaz komutlarını, yanıt alanı rezervasyonu/komut kabul politikası tasarlanmadan etkinleştirme. İlk test yan etkisiz komut/yanıtla yapılır. Bu sınır ACK/tekrar tasarımının yerine geçmez.
- [x] `TASK_IDLE_NO_POLL` ölçümü: 10 saniye boşta UART taskında süreli uyanma olmasın. `TASK_PARTIAL_FRAME_DEADLINE`: sessizlikte doğru anda timeout işlensin ve parser boşalınca süresiz uykuya dönülsün.
- [x] `TASK_STALE_DEADLINE_NO_SPIN`: RX FAULT/ABORTING durumunda dolmuş eski frame deadline, RUNNING+ertelenmiş producer örneği ve TX IDLE'da eski TX deadline beklemeyi 0'a sabitlemesin. `TASK_WAIT_USES_FRESH_TIME`: handler sonrasında kalan süre güncel zamandan hesaplansın.
- [x] 100 Hz RX+TX ve sürekli tam çift yön testini çalıştır; CPU, stack high-water mark, en uzun ISR/task gecikmesi, queue yüksek su seviyesi ve kayıpları kaydet. Stack için başlangıçta en az %25 kullanılmamış pay hedefle; en ağır hata/handler yolu test edilmeden azaltma.

**Geçiş koşulu:** İşlevsel ve performans sonuçları aynı firmware için kayıtlı; hedef CPU/gecikme bütçesi sağlanıyor veya engel açıkça ölçülmüş.

## 12. Son adım: RX ve TX dosyalarını gerçekten birleştir

### M1 — Doğrulanmış kodu uart_comm içine taşı

**Bağımlılık:** F3.  
**Dosyalar:** `uart_comm.c/.h`, test dosyaları, `stm32f4xx_it.c` USER CODE ve IDE kaynak listesi. Eski `uart_rx.c/.h`, `uart_tx.c/.h` bu adımın sonunda kaldırılır.

- [x] Taşımadan önce R/T/F test sonuçlarını kaydet. Bu adımda yeni davranış/optimizasyon ekleme; önce yalnız sahipliği tek dosyada topla.
- [x] RX state, DMA buffer, üretici sayacı ve yardımcılarını uart_comm.c'deki RX bölümüne; TX state, aktif buffer ve yardımcılarını TX bölümüne taşı. Çakışan `s_huart`, `s_state`, `s_buf`, `s_len` adlarını tek context ve açık `rx_`/`tx_` alanlarıyla çöz.
- [x] İç servisleri `static rx_service_budget`, `static tx_service`, `static rx_sample_producer`, `static rx_next_wait_ms`, `static tx_next_wait_ms` yap. Ortak HAL handle tek yerde kalsın.
- [x] F1'deki on_event/on_complete geçici kapılarını ve T3 sonuç kutusunu da aynı context içine taşı. Yerel TX sonuç kodları ortak sonuç tipine eşlenebilir; kodları/sayaçları ikinci kez üretme. Statik buffer sayısı ve queue'nun 8+1 sahiplik sınırı korunmalı.
- [x] RX sağlık kancasını `void uart_comm_on_uart_irq_exit(void)` olarak değiştir. Sadece UART IRQ bağlantısı için public tut; uygulama API'sinden ayrı yorumla belirt. TC tur sayımı RX callback'inde kalır; yeni DMA IRQ giriş kancası eklenmez.
- [x] Uygulama/test include'larını uart_comm.h'ye geçir. R/T davranış testleri için gerekirse `UART_COMM_TEST` altında özel test arayüzü bırak; eski public RX/TX API'lerini kalıcı wrapper olarak yaşatma.
- [x] Eski dört dosyayı build'den ve kaynak ağacından çıkar. CubeIDE kaynak listesini yeniden üretsin; eski `.o` dosyaları linkte sonucu gizlemesin diye clean rebuild yap.
- [x] Kod ağacında eski include ve çağrı kalmadığını ara. Callback sembollerinin her biri tek tanım; DMA buffer'larının SRAM konumu ve yalnız bir UartCommTask olduğuna bak.
- [x] R/T/F testlerini yeniden çalıştır; davranış/sayaç sonuçlarını taşımadan önceki firmware ile karşılaştır.

**Nihai dosya düzeni (6 Ekim 2026 sadeleştirmesi):**

```text
Lib/Uart/uart_comm.h         Tek public arayüz + IRQ kancaları
Lib/Uart/uart_comm.c         Task, queue, RX/TX state, DMA buffer; proje olay kapıları
Core/Inc/protocol.h          HAL/RTOS bağımsız çerçeve/CRC/parser API
Core/Src/protocol.c
Core/Inc/app_protocol.h      Snapshot ve uygulama handler arayüzü
Core/Src/app_protocol.c      SEQ ve joystick uygulaması
Lib/Uart/uart_comm_internal.h Private çekirdek/test erişimi
Lib/Uart/uart_comm_config.h   Tampon/task/süre ayarları
Lib/Uart/uart_comm_port.c/.h  STM32F4 register/DMA bağlantısı
Core/Inc/protocol_uart.h     İsteğe bağlı byte/parser adaptörü
Core/Src/protocol_uart.c     DATA/TIMEOUT/RESET -> protocol
Tests/Inc/*.h                Kart test arayüzleri
Tests/Src/*.c                Yalnız test yapılandırmasında etkin testler
```

Tek dosya içinde önerilen bölüm sırası: sabitler/tipler → context ve buffers → kritik bölüm/olay yardımcıları → RX producer/kopya → RX toparlanma/timeout → TX başlatma/toparlanma → queue/public API → task → HAL callback ve IRQ kancaları. RX/TX algoritmaları tek dev fonksiyonda iç içe geçirilmez.

### M2 — Son kabul ve gerçek ölçüm kaydı

- [x] CubeIDE Debug ve üretim/test kancaları kapalı yapılandırmayı tam derle ve bağla; yeni derleyici uyarısı/callback çakışması yok.
- [x] Normal çift yön, parçalı çerçeve, sarım, taşma, başlangıç BUSY/ERROR, UART/DMA hata, kayıp/geç/senkron callback, queue dolu ve iki üreticili testler geçsin.
- [x] Task/IRQ sahipliği, kritik bölüm süreleri ve “bildirim gelirken uykuya geçiş” tekrar incelensin.
- [x] `Core/Src` üretim yollarında blocking transmit/receive, `HAL_Delay`, bloklayan abort beklemesi ve service busy-wait kalmadığını kontrol et. Deadline ile uyanıp bir kez yapılan durum kontrolü bu yasağın kapsamında değildir. HAL'in kendi ISR içindeki sınırlı işlemleri ayrıca ölç; uygulama callback'leriyle karıştırma.
- [x] CPU/stack/latency ölçümlerini hedef kart, baud, trafik, build optimization ve RTOS tick bilgisiyle kaydet.
- [x] Bu belgedeki kutuları yalnız ilgili test sonucu varsa işaretle. Donanımda koşulmayan testleri açık bırak. `MIMARI.md` ve önceki yol haritasına yeni belgenin nihai durumunu işaret eden kısa referans ekle.

**Tamamlanma ölçütü:** UART için tek task, nihai tek RX/TX modülü, olayla uyuma, görünür hata/kayıp sonuçları ve bütün geçiş kapılarının gerçek sonuçlarla kapanması.

## 13. Sıra, bağımlılıklar ve her adımın çalışma yöntemi

```text
P0 -> R1 -> R2 -> R3 -> R4 -> R5
   -> T1 -> T2 -> T3 -> T4
   -> F0 -> F1 -> F2 -> F3
   -> M1 -> M2
```

Her adımda:

1. Hangi davranışın değişeceği ve hangi testin bunu göstereceği belirlenir.
2. Mevcut hatayı yakalayan test mümkünse önce başarısız çalıştırılır. Register/IRQ sınır testleri donanım modeliyle ve kartla ayrı işaretlenir; yeni test framework kütüphanesi eklenmez.
3. Yalnız o adımın kodu yazılır; header, uygulama ve test sözleşmeleri beraber güncellenir.
4. Derleme/bağlama ve ilgili regresyonlar çalıştırılır; ISR/task/DMA yarış pencereleri gözden geçirilir.
5. Sonuç kayıtlıysa geçiş koşulu kapatılır; istenirse yalnız bu adımın değişiklikleri ayrı commit edilir. Başka kullanıcı değişiklikleri commit'e karıştırılmaz.

**Doğrulama işlemleri:** CubeIDE'de `Project > Clean` ardından `Build Project` tam derleme/bağlama kontrolüdür. Test firmware'i kartta çalıştırıldığında `test_sayisi/test_gecen/test_kalan`, `lb_sayisi/lb_gecen/lb_kalan` ve yeni adlandırılmış test sonuçları debugger'da okunur; beklenen sayıda testin çalışması ve her birinin PASS olması birlikte aranır. RTOS testinde test sürücüsü owner taskın işini dışarıdan service çağırarak yapmaz. Donanım modeli kullanılıyorsa hangi HAL/register davranışlarının taklit edildiği test kaydına yazılır.

En kritik bağımlılıklar: R2 olmadan sağlam taşma kontrolü; T3 olmadan güvenli FIFO; F0 olmadan FromISR; F3 olmadan düşük CPU iddiası; M1 olmadan dosyaların birleştiği iddiası yapılamaz.

## 14. Planın kendi kontrolü ve doğrulama sınırları

| Kontrol | Tasarımda karşılığı / test sahibi |
|---|---|
| Aktif RX'e ikinci start | R1 |
| Tam tur, pending TC, IDLE tekrar bildirimi | R2 |
| Kopya sırasında DMA ezmesi | R3 |
| Hata sırasında yeniden start ve deneme sayacının sıfırlanması | R4 |
| DMA ilerlerken yanlış frame timeout | R5 |
| DMA hatasının iki yönü etkilemesi | T1, T4 |
| HAL callback'in senkron gelmesi | R4, T3, F1 |
| FAULT'tan sahte init ile çıkma / geç callback | T2, T3 |
| Queue kabulü ile FAULT yarışı | F2 |
| TX meşgulken kuyruk nedeniyle sürekli dönme | F2, F3 |
| Uykuya girerken olayın gelmesi | F1 |
| Aynı kaynağın iki task/iki callback tarafından sahiplenilmesi | F1, M1 |
| Kaynak buffer/payload ömrü | R3, R5, T2, F2 |
| Tick/producer sayaç sarımı | R2, T3 |
| Gerçekten tek dosya ve tek UART taskı | M1, M2 |

Bu mimari kaynak incelemesine ve açık sözleşmelere dayanır. Donanım zamanlaması, IRQ gecikmesi, gerçek hata enjeksiyonu ve CPU yüzdesi belge yazılarak kanıtlanamaz. Bu nedenle her risk için somut kabul testi bulunur; “adımları uygulamak hiç sorun çıkarmayacak” garantisi yerine, sorunu sonraki adıma taşımadan yakalayacak geçiş koşulları konmuştur.

## 15. Kaynaklar

- Yerel `Core/Src/uart_rx.c`: start, drain, timeout, recover ve HAL RX/error callback yolları.
- Yerel `Core/Src/uart_tx.c` ve `Core/Inc/uart_tx.h`: çalışan TX uygulaması ile henüz uygulanmamış sözleşmeler.
- Yerel `Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c`: `HAL_UARTEx_ReceiveToIdle_DMA`, `HAL_UART_IRQHandler`, `UART_DMAError`, `HAL_UART_AbortTransmit_IT`, `HAL_UART_AbortReceive_IT`, `UART_EndTxTransfer`, `UART_EndRxTransfer`.
- Yerel `Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c`: `HAL_DMA_Abort_IT` ve `HAL_DMA_IRQHandler` (abort callback ve state sırası).
- [Mevcut UART RX/TX/RTOS referans planı](docs/archive/UART_RX_TX_RTOS_YOL_HARITASI.md).
- [Önceki güvenilirlik planı](docs/superpowers/plans/2026-10-02-uart-guvenilirlik.md).
- Resmî ST/FreeRTOS bağlantıları ilgili tasarım bölümlerinde verilmiştir; kullanılan HAL sürümünün gerçek davranışı için yerel kaynak esas alınır.

## 16. İkinci mimari incelemesi — revizyon 2

5 Ekim 2026'da plan yeniden mevcut HAL kaynakları ve FreeRTOS kritik bölüm sözleşmesiyle karşılaştırıldı. Aşağıdaki plan eksikleri düzeltildi; firmware henüz uygulanmadı:

| Bulgu | Düzeltilen karar | Doğrulama adımı |
|---|---|---|
| Queue API uygulama kritik bölümü içindeydi | API dışarı alındı; gate/epoch ve FAULT sırasında geç enqueue sözleşmesi eklendi | F2 |
| RX deneme bütçesi dolarken DMA aktif kalabilirdi | Yeni deneme bütçesi ile son duruş süresi ayrıldı; final_stop ve rx_quiescent eklendi | R4 |
| Servis snapshot'ından sonra yeni TX'e geçerken olay karışabilirdi | Son olay kontrolü ve yazılım deneme kaydı; eski donanım kaynağı temizliği zorunlu | T1–T3 |
| R2 sayacı eski recover yolunda sıfırlanmayabilirdi | Bütün start/recover yolları tek ilerleme resetine bağlandı | R2 |
| Scratch sonrası sample=false davranışı açık değildi | Çıkış değeri kullanılmadan kopya reddi, consumer korunması | R3 |
| Callback taşıma ve TX sonuç teslimi için arayüz eksikti | on_event/on_complete ve take_result tanımları, tüketim kuralları eklendi | T3, F1, F2, M1 |
| Geçersiz state'in eski deadline'ı taskı döndürebilirdi | State'e göre deadline seçimi ve bekleme öncesi yeni zaman okuması | F3 |

Bu revizyonda 20 küçük soyut kontrol çalıştırıldı: 10 producer/sınır hesabı, 6 gate–enqueue–FAULT/recovery sıralaması, 2 RX deneme sonlandırma kararı ve 2 deadline kararı beklenen sonucu verdi. Bunlar karar kurallarının modelleridir; HAL register modelini veya gerçek firmware'i çalıştırmaz. Register modeli ve kart kabul testleri ilgili adımlarda çalıştırılacak; yeni tasarım için derleme veya donanım testi geçmiş kabul edilmedi.

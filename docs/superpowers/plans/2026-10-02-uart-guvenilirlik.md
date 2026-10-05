# UART güvenilirliği uygulama planı

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** TX hata kilitlenmesini, RX başlatmada durum bozulmasını ve sessiz DMA tampon taşmasını gidermek.

**Architecture:** Tek ana bağlam UART modüllerini yönetir; kesmeler olay bildirir. TX aktarımının sonucu ile modülün yeni gönderime hazır olması ayrı tutulur: başarısız aktarım güvenli şekilde iptal edilirse otomatik IDLE'a dönülür. FAULT yalnız güvenli toparlanmanın tamamlanamadığı durumu temsil eder. RX üretici ilerlemesi tur bilgisiyle takip edilir; taşmada parser yeniden senkronize edilir.

**Tech Stack:** STM32F407VG, STM32 HAL, STM32CubeIDE; mevcut kütüphaneler.

**Spec:** Kullanıcının proje talimatları ve bu sohbetin UART incelemesindeki üç bulgu.

## Global Constraints

- Önemli değişiklikleri Türkçe açıkla.
- .ioc, pinler ve çevrebirim ayarlarını koru.
- Üretilen kodda gerekli değişiklikleri USER CODE bölümlerinde tut.
- Yeni kütüphane ekleme; FreeRTOS görevlerinde bloklayan gecikme kullanma.
- Mevcut çalışma ağacı değişikliklerini koru.

## Review Focus

- TX DMA hatası sırasında RX de HAL tarafından durdurulabilir: iki modül de toparlanmalı.
- İptal tamamlanmazsa init ve yeni gönderim tamponu serbest bırakamaz.
- Başarılı iptal sonrasında init gerekmeksizin yeni gönderim kabul edilmeli; eski tamamlanma bildirimi yeni aktarımın sonucu sayılmamalı.
- HAL_BUSY dönen RX başlangıcı mevcut parser ve okuma konumunu değiştirmemeli.
- DMA turu veya tüketim sırasında üzerine yazma sessizce boş tampon sayılamaz.

## Görev 1: TX hata yaşam döngüsü

**Files:** Core/Inc/uart_tx.h, Core/Src/uart_tx.c, Core/Src/uart_rx.c, Core/Src/tests.c, Core/Inc/tests.h.

**Interfaces:** Yeni `void uart_tx_on_error(UART_HandleTypeDef *huart, uint32_t error)` ve `void uart_tx_on_abort_complete(UART_HandleTypeDef *huart)` bildirimleri; mevcut public API korunur. `UART_TX_ABORTING` eklenir.

**Durum sözleşmesi:** Normal tamamlanma SENDING -> IDLE; aktarım hatası veya zaman aşımı SENDING -> ABORTING; güvenli iptal ABORTING -> IDLE; süre sınırında güvenli duruş doğrulanamazsa ABORTING -> FAULT. IDLE, DMA tamponunun serbest ve HAL TX yolunun yeni gönderime hazır olduğunu garanti eder. ABORTING sırasında send_copy BUSY, FAULT sırasında NOT_READY döner. Son aktarımın hata nedeni ve toparlanma hatası ayrı kaydedilir; hata geçmişi IDLE'a dönünce silinmez. FAULT tek başına fiziksel donanım arızasını kanıtlamaz.

- [ ] Önce DMA hatası, kayıp tamamlanma, geç callback ve iptal başarısızlığı için başarısız testleri kur.
- [ ] Ortak HAL hata callback'inde ErrorCode'u temizlenmeden yakala; DMA hatasını aktif TX'e ve mevcut RX toparlanmasına bildir. Normal RX FE/NE/ORE hatasını otomatik TX hatası sayma.
- [ ] ISR bayraklarını kısa kritik bölümde alıp temizle; önceki PRIMASK değerini geri yükle. HAL çağrılarını kritik bölümün dışında yap.
- [ ] TX hata veya süre aşımında ABORTING durumuna geç; HAL_UART_AbortTransmit_IT ile iptali başlat. Güvenli duruş ve HAL hazırlığı doğrulanınca başarısız aktarımı bir kez kaydet, eski bildirimleri temizle ve otomatik IDLE'a dön. frames_sent artırma; uart_tx_init gerektirme.
- [ ] İptal için ayrı 20 ms servis tabanlı üst sınır koy. DMA stream EN, DMAT, TX kesmeleri ve UART/DMA HAL durumlarını birlikte değerlendir. Süre dolmadan işlenebilir tamamlanmayı önce ele al; süre dolduğunda güvenli yeniden kullanım doğrulanamazsa FAULT'a geç. init/send bu durumu yalnız yazılım alanlarını sıfırlayarak aşamasın. Bu HAL sürümünde AbortTransmit_IT HAL_OK dönerken DMA iptalinde hata oluşursa tamamlanma callback'ini doğrudan çağırabilir; yalnız dönüş değeri veya callback donanım duruşunun kanıtı sayılmamalı. Callback'in fonksiyon dönmeden çalışabileceğini hesaba kat.
- [ ] Başlangıçta HAL_OK sonrası sabit toplam 20 ms TX zaman aşımı kur: mevcut 115200 8N1 ve en fazla 64 bayt için normal süre yaklaşık 5,56 ms. NDTR ilerlemesi bu toplam süreyi uzatmasın. Servis gecikirse tamamlanma bildirimini süre aşımından önce değerlendir.
- [ ] Yalnız SENDING durumunda tamamlanmayı başarılı gönderim say; hata bildirimiyle çakışmada güvenli hata yolunu seç. İptal sonrası eski bildirimleri yeni gönderim kurulmadan önce temizle.
- [ ] Aktarım hatasını/zaman aşımını ve toparlanma başarısızlığını ayrı nedenler ve sayaçlarla kaydet. NDTR farkına fiziksel teslim anlamı yükleme. Başarısız veya sonucu belirsiz çerçeveyi otomatik tekrar gönderme; yeni istek IDLE'da kabul edilsin.
- [ ] Yukarıdaki testleri ve mevcut T7–T9'u çalıştır. DMA hatası ve kayıp tamamlanma senaryolarında başarılı iptal sonrası init çağırmadan yeni gönderimin kabul edilip çözüldüğünü doğrula; eski aktarım frames_sent sayacına eklenmesin. Güvenli duruş sağlanamayan iptal senaryosunda FAULT ve yeni gönderimin reddedildiğini; geç callback'in FAULT'u kaldırmadığını ve yeni aktarımı erken tamamlamadığını doğrula.

## Görev 2: RX başlatma güvenliği

**Files:** Core/Src/uart_rx.c, Core/Src/tests.c.

**Interfaces:** `HAL_StatusTypeDef uart_rx_start(UART_HandleTypeDef *huart)` korunur.

- [ ] Çalışan RX'te ikinci start çağrısının HAL_BUSY döndürürken parser, okuma konumu ve sayaçları koruduğunu sınayan testi yaz.
- [ ] Aktif alım ve modül sahipliğini bütün sıfırlamalardan önce kontrol et. Başka handle'a geçerken eski DMA çalışıyorsa reddet.
- [ ] Durdurulmuş alımı hazırladıktan sonra DMA'yı başlat. İlk başlatma başarısızlığını faulted ile görünür kıl; DMA başlamadan önce callback'in kullanacağı handle ve olay durumları kurulmuş olsun.
- [ ] Yarım çerçeve varken ikinci start çağrısı yap; kalan baytlar geldiğinde çerçevenin tek kez çözüldüğünü doğrula.

## Görev 3: RX taşma ve üzerine yazma denetimi

**Files:** Core/Inc/uart_rx.h, Core/Src/uart_rx.c, Core/Src/tests.c, Core/Inc/tests.h.

**Interfaces:** Mevcut RX API korunur; stats'a overrun_events eklenir.

- [ ] Tüketimi durdurup tam 256 bayt ve 256'dan fazla bayt göndererek mevcut sessiz kaybı gösteren testler kur; RX kesmeleri çalışmaya devam etsin.
- [ ] HT/TC olaylarını tek bayrak yerine monoton üretici ilerlemesinin hesabına kat. NDTR ve tur bilgisini tutarlı örnekle; bekleyen TC ve NDTR yeniden yükleme sınırını ayrıca ele al.
- [ ] Üretici ve tüketici farkıyla okunmamış bayt miktarını hesapla. En az bir tampon kadar birikme durumunu konservatif taşma say; parser adayını bırak, overrun_events artır ve güncel üretici konumundan tekrar senkron ara.
- [ ] Parser'ı DMA belleği üzerinden uzun süre çalıştırma: küçük aralıkları çalışma tamponuna kopyala. Kopyalama sonrasında üretici ilerlemesini tekrar kontrol et; kaynak aralık ezilmişse kopyayı parser'a verme.
- [ ] Tam tur, çoklu tur, HT/TC-IDLE çakışması, NDTR=0 sınırı, kopyalama sırasında üzerine yazma ve taşma sonrası geçerli çerçeve testlerini çalıştır.
- [ ] Kesme bayrakları olay kuyruğu değildir: kesmeler tam DMA turundan uzun engellenirse tüm turlar sayılamaz. Mevcut tamponla yaklaşık 22,2 ms tur süresi sınırını ve tüketim süresi bütçesini belgeleyip ölç. Bu sınırı sağlayamayan kullanım için tampon/akış kontrolü tasarımını ayrıca değerlendir.

## Görev 4: Bütünleşik doğrulama

**Files:** Core/Src/tests.c, MIMARI.md, ACIK_BULGULAR.md.

- [ ] CubeIDE Debug yapılandırmasını tam derle ve bağla; hata ve yeni uyarı kalmamalı.
- [ ] Mevcut birim ve loopback testlerini kartta çalıştır; sonuç sayaçlarını kaydet.
- [ ] Eşzamanlı TX/RX sırasında TX DMA hatası ve RX hatası uygula; iki yönün doğru durumlara geçtiğini doğrula.
- [ ] Süre aşımı, iptal başarısızlığı, geç callback, ikinci RX start ve taşma testlerini kart veya kontrollü HAL test ortamında doğrula; yalnız test kancasıyla doğrulananları açıkça belirt.
- [ ] Paylaşılan bayrakları, DMA tampon sahipliğini ve yeniden başlatma sırasını yarış koşulları açısından tekrar incele.
- [ ] Belgeleri yalnız elde edilen gerçek sonuçlarla güncelle; donanımda sınanmayan iddiaları tamamlandı sayma.

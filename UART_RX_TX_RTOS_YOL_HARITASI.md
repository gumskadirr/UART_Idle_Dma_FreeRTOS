# UART RX + TX ve ortak FreeRTOS taskı yol haritası

> 6 Ekim 2026 güncellemesi: Bu belgenin eski alım/sıra anlatımı tarihsel kayıttır. Güncel birleşik RX/TX ve FreeRTOS kullanımı [UART_COMM_KULLANIM.md](UART_COMM_KULLANIM.md), mimari ve son kabul [UART_BIRLESIK_MIMARI_VE_UYGULAMA_YOL_HARITASI.md](UART_BIRLESIK_MIMARI_VE_UYGULAMA_YOL_HARITASI.md) ve [UART_RTOS_UYGULAMA_PLANI.md](UART_RTOS_UYGULAMA_PLANI.md) içindedir.

Tarih: 30 Eylül 2026  
Durum: Uygulama öncesi geliştirme planı. Bu belge hazırlanırken kaynak kod veya `.ioc` değiştirilmedi.

## 1. Yeni hedef ve önceki planla ilişkisi

Mevcut RX altyapısını koruyarak önce DMA tabanlı TX eklenecek. RX ve TX temel olarak doğrulandıktan sonra ikisi tek bir FreeRTOS haberleşme taskında birleştirilecek.

Bu belge, `UART_GELISTIRME_PLANI.md` içindeki sonraki işlerin sırasını günceller. Paket formatı, CRC, pinler ve UART hızı korunur. Joystick kontrol mantığı ve ayrıntılı performans optimizasyonu, ortak haberleşme altyapısından sonra tamamlanır.

Temel kural:

> Ortak task TX'i başlatır, bitmesini bekleyerek RX işlemesini durdurmaz. TX tamamlandığında bildirim alıp sıradaki gönderime geçer.

Tek task, donanımın aynı anda gönderip almasına engel değildir. USART2 TX ve RX ayrı DMA stream'leriyle çalışacaktır.

## 2. Başlangıç durumu

| Bileşen | Mevcut durum |
|---|---|
| `crc16.c/.h` | CRC hesabı mevcut |
| `frame.c/.h` | En fazla 64 baytlık çerçeve oluşturma mevcut |
| `parser.c/.h` | Akıştan çerçeve çıkarma mevcut; kısmi paket zaman aşımı henüz yok |
| `uart_rx.c/.h` | 256 bayt Circular DMA, NDTR ile konum, sarım ve tüketim mevcut |
| `tests.c/.h` | Birim testleri ve bloklayan TX kullanan loopback mevcut |
| TX DMA | Henüz yok |
| FreeRTOS | Bu alt projede henüz yok |
| IRQ ayarları | Öncelikler 0, gruplama GROUP_0; RTOS öncesi değişecek |

İlk aşamada RX boyutu 256 bayt, çerçeve boyutu en fazla 64 bayt kalacak. Bunlar ölçülmüş nihai kapasite sınırları değildir.

## 3. Hedef mimari ve sahiplik

```text
Diğer uygulama görevleri / ortak taskın ürettiği cevaplar
                         |
                 Tam çerçeveyi kopyala
                         |
                    TX FIFO kuyruğu
                         |
                Tek UartCommTask
                  /            \
     RX DMA'yı tüket             TX boşsa bir çerçeve başlat
           |                               |
       Ayrıştırıcı                    Normal TX DMA
           |                               |
      Mesaj işleme                  Tamamlanma bildirimi

RX IDLE / HT / TC ──────> UartCommTask <────── TX tamamlandı / hata
```

| Kaynak | Sahibi / kullanım kuralı |
|---|---|
| RX DMA tamponu | DMA yazar; yalnızca haberleşme taskı tüketir |
| RX okuma konumu ve ayrıştırıcı | Yalnızca haberleşme taskı değiştirir |
| TX kuyruğu | Üreticiler FreeRTOS kuyruğuna kopyalar; yalnızca haberleşme taskı çıkarır |
| Aktif TX tamponu ve TX durumu | Haberleşme taskı yönetir; DMA okuyabilir |
| HAL alım/gönderim başlatma ve kurtarma | Haberleşme taskı yönetir |
| HAL IRQ işleyicileri | Donanım/HAL işlemlerini yapar; uygulama callback'leri kısa bildirim üretir |

Diğer tasklar aynı `huart2` üzerinde doğrudan `HAL_UART_Transmit*` veya yeniden başlatma çağrısı yapmayacak. Ortak task kendi kuyruğunda yer açılmasını veya kendi tamamlanma olayını bloklanarak beklemeyecek.

## 4. Aşama 1 — TX DMA donanım ayarını ekle

Önce FreeRTOS eklemeden tek çerçevelik TX DMA doğrulanacak.

CubeMX ayarları:

| Ayar | Değer |
|---|---|
| USART2 | Mevcut 115200, 8N1, TX/RX, PA2/PA3 korunur |
| USART2_RX DMA | Mevcut DMA1 Stream5, Channel4, Circular korunur |
| USART2_TX DMA | DMA1 Stream6, Channel4 |
| TX yönü | Memory to Peripheral |
| TX modu | Normal |
| TX veri genişliği | Memory ve Peripheral: Byte |
| TX artış | Memory increment açık, Peripheral increment kapalı |
| TX FIFO | İlk sürümde kapalı |
| Kesmeler | DMA1 Stream6 ve mevcut USART2 global interrupt açık |

Asıl FreeRTOS projesinde Stream6 başka bir çevrebirim tarafından kullanılıyorsa önce kaynak çakışması çözülecek. Var olan çevrebirim atamaları sessizce değiştirilmeyecek.

- [ ] `hdma_usart2_tx` oluştuğunu ve `huart2.hdmatx` bağlantısını kontrol et.
- [ ] TX DMA IRQ'nun doğru HAL DMA handle'ını işlediğini doğrula.
- [ ] USART2 IRQ içindeki `HAL_UART_IRQHandler(&huart2)` çağrısını koru.
- [ ] Tek bir bilinen çerçeveyi `HAL_UART_Transmit_DMA()` ile gönder; çağrının dönüşünü kontrol et.
- [ ] Mevcut bloklayan loopback testini bu ilk doğrulamadan ayrı tut.

Önemli: Projedeki HAL'de Normal TX DMA tamamlandığında UART TC kesmesi etkinleştirilir. Kullanıcıya ait `HAL_UART_TxCpltCallback()` UART aktarımı tamamlanınca çağrılır. TX'i serbest bırakma kararını ham DMA TC olayına bağlama; USART2 IRQ bu nedenle TX için de gereklidir.

Kabul ölçütü: Karşı tarafta doğru baytlar alınır, TX tamamlanma callback'i gelir, RX açık kalır. TX DMA çerçeveyi durmadan tekrarlamaz.

## 5. Aşama 2 — Tek aktif gönderimi yöneten `uart_tx` modülünü yaz

Bu aşamada henüz çok üreticili FIFO kurmak zorunlu değil. Önce tek aktif gönderimin ömrünü ve durumlarını doğrula.

- [ ] `uart_tx.c/.h` oluştur; başlangıç, gönderim başlatma, tamamlanma ve hata durumlarını ayır.
- [ ] DMA erişimine uygun SRAM'de, modül ömrü boyunca yaşayan 64 baytlık aktif TX tamponu ayır; CCM kullanma.
- [ ] Gönderilecek çerçeveyi aktif tampona kopyala.
- [ ] TX hazır değilse yeni başlatmayı reddet; çalışan DMA tamponunu değiştirme.
- [ ] `HAL_OK`, `HAL_BUSY` ve `HAL_ERROR` sonuçlarını ayrı ele al.
- [ ] IRQ callback'i yalnızca olay bildirsin; sıradaki gönderimi kesme içinde başlatma.
- [ ] Tamamlanma durumunu ana döngüde işle; RTOS'a geçince aynı işi ortak task üstlenecek.

Durum akışı:

```text
BOŞ → ÇERÇEVE HAZIR → GÖNDERİLİYOR → TAMAMLANDI → BOŞ
                           |
                       HATA / İPTAL
```

Aktif durum, tamamlanma kesmesinin çok erken gelmesi ihtimaline karşı HAL çağrısından önce tutarlı hâle getirilmeli; başlatma başarısız olursa geri alınmalı. Hata/iptalde DMA durduğu doğrulanmadan tampon yeniden kullanılmamalı.

`HAL_BUSY` döngü içinde tekrar tekrar denenmeyecek. Aktif çerçeve korunacak; tutarsız durum veya süre aşımı ayrı olarak yönetilecek. Başarısız çerçeve sessizce düşürülmeyecek.

Kabul ölçütü: Bir gönderim sürerken ikinci başlatma ilk paketi bozamaz. Çağıranın kaynak tamponu gönderim kabulünden sonra değişse bile DMA'nın gönderdiği kopya değişmez. RX tüketimi TX boyunca devam eder.

## 6. Aşama 3 — Bare-metal ortamda RX ve TX'i birlikte doğrula

- [ ] Ana döngü hem RX tüketimini hem TX tamamlanma durumunu servis etsin.
- [ ] Art arda farklı sıra numaralı çerçeveler gönder; her yeni TX'i öncekinin tamamlanmasından sonra başlat.
- [ ] 40 adet 13 baytlık çerçeveyle mevcut 520 baytlık sarım senaryosunu TX DMA kullanarak tekrarla.
- [ ] Tek, birleşik ve parçalı RX paketlerini TX sürerken de dene.
- [ ] Test göndericisi içinden doğrudan `uart_rx_drain()` çağırarak asıl servis yolunu atlama; normal callback/bildirim/servis zincirini sınayan ayrı test yap.
- [ ] PA2–PA3 loopback ile PC bağlantısını ayrı test düzenekleri olarak kullan. PC'nin TX çıkışı ile kart TX'ini jumper üzerinden birbirine bağlama.

Kabul ölçütü: Gönderim sırasında alım paketleri işlenir; veri sırası, CRC ve tampon sarımı doğru kalır. Bu doğrulama, henüz FreeRTOS zamanlama doğrulaması sayılmaz.

## 7. Aşama 4 — Ortak FreeRTOS altyapısını ve TX kuyruğunu kur

`UartCommTask` adında tek haberleşme görevi oluştur. Projenin FreeRTOS/CMSIS kullanım biçimini esas al. Aşağıdaki örnek API adları native FreeRTOS içindir; CMSIS kullanılıyorsa eşdeğer mekanizma bilinçli seçilmeli, aynı bildirim alanı farklı yöntemlerle paylaşılmamalıdır.

- [ ] Tek haberleşme taskını ve TX kuyruğunu oluştur; oluşturma hatalarını kontrol et.
- [ ] İlk tasarım değeri olarak 8 bekleyen çerçevelik FIFO seç; gerçek ihtiyaçla sonradan doğrula.
- [ ] Kuyruk öğesi uzunluk ve en fazla 64 baytlık tam çerçeveyi değer olarak taşısın; payload işaretçisi taşımasın.
- [ ] Kuyruk dışında bir aktif TX tamponu kullan. Böylece 8 bekleyen ve 1 aktif çerçeve olabilir.
- [ ] Üretici için `uart_comm_send_copy(data, len)` benzeri, beklemeden dönen bir API tanımla.
- [ ] API sonuçları: kabul edildi, kuyruk dolu, geçersiz argüman ve henüz hazır değil.
- [ ] Yalnızca kuyruğa başarıyla eklendikten sonra ortak taska TX isteği bildir.
- [ ] İlk sürümde API'yi task bağlamıyla sınırla. Kesmeden gönderim gerçekten gerekirse ayrıca `FromISR` arayüzü tasarla.

“Kabul edildi” yalnızca verinin kuyruğa kopyalandığı anlamına gelir; hatta gönderildiği veya PC tarafından işlendiği anlamına gelmez.

Kuyruk doluysa çağıran sonucu ele alır. Başlangıçta sessiz üzerine yazma uygulanmaz. İleride telemetri/joystick için yalnızca en güncel örneği tutma politikası eklenebilir; komut cevapları aynı politikayla düşürülmez.

TX kuyruğundan çıkarılan öğe aktif tampona taşındıktan sonra, başlatma başarısız olsa bile sonucu belirlenene kadar sahipliği ortak taskta kalır.

## 8. Aşama 5 — RX ve TX olaylarını tek taskta birleştir

Bir task notification değeri üzerinde ayrı olay bitleri kullan:

| Olay | Kaynak | Anlam |
|---|---|---|
| RX_READY | UART IDLE veya RX DMA HT/TC callback'i | RX'te yeni veri olabilir |
| TX_REQUEST | Kuyruğa ekleyen task | Gönderilmeyi bekleyen çerçeve olabilir |
| TX_DONE | `HAL_UART_TxCpltCallback()` | Aktif gönderim tamamlandı |
| UART_ERROR | UART/DMA hata yolu | Kaydedilen hata durumunu değerlendir |

Kesmeden `xTaskNotifyFromISR(..., eSetBits, ...)`, task bağlamından `xTaskNotify(..., eSetBits)` kullanılabilir. Olaylar aynı anda gelebilir; `else if` ile birbirini dışlayan olaylar gibi işlenmemeli.

Task beklemesinde `xTaskNotifyWait(0, EVENT_MASK, &events, timeout)` yaklaşımı kullanılabilir: girişte olay silme, alınan bitleri çıkışta temizle. Aynı notification alanında `ulTaskNotifyTake()` ile bit tabanlı yöntemi karıştırma. Task çalışırken gelen olaylar sonraki bekleme çağrısı için beklemede kalmalı.

Olay bitleri sayaç değildir. Üç RX olayı tek bite birleşebilir; gerçek okunacak veri NDTR ve okuma konumundan bulunur. Birden fazla TX_REQUEST tek bite birleşebilir; bekleyen çerçeveler kuyruğun içinde korunur.

### Taskın çalışma sırası

1. Task handle'ı ve kuyruk hazırken RX DMA'yı başlat; başlangıç yalnızca bir kez yapılsın.
2. Hata ve TX tamamlanma olaylarını işleyip aktif aktarım durumunu güncelle.
3. RX'teki yeni baytları ayrıştırıcıya aktar.
4. Ayrıştırılan mesajların kısa işlemlerini gerçekleştir; gerekiyorsa yanıtı TX kuyruğuna ekle.
5. TX boşsa kuyruktan bir çerçeveyi aktif tampona alıp DMA'yı başlat.
6. Yapılabilir iş kaldıysa devam et; kalmadıysa yeni olay veya en yakın zaman aşımına kadar bekle.

TX sürerken task yalnızca TX_DONE bekleyen ayrı bir bekleme döngüsüne girmeyecek. Aynı bekleme RX_READY ve hata olaylarıyla da uyanabilecek. Kuyruktan alma işlemi beklemeden yapılacak; task doğrudan boş TX kuyruğu üzerinde sonsuza kadar beklemeyecek.

Her turda TX boşsa kuyruk kontrol edilmeli. Yalnızca TX_REQUEST geldiği turda kontrol etmek, TX meşgulken biriken çerçevelerin TX_DONE sonrasında kuyrukta kalmasına neden olabilir.

Sürekli RX, TX'in servis edilmesini sonsuza kadar ertelememeli. Mevcut `uart_rx_drain()` döngüsü gerekirse tur başına bayt/iş bütçesiyle sınırlandırılacak. Bütçe bitip veri kaldığında task RX işinin kaldığını hatırlayıp tekrar servis edecek; yeni IRQ bekleyerek uyumayacak. Bütçe, RX taşmasına yol açacak kadar küçük seçilmeyecek.

Kabul ölçütü: Aynı anda RX_READY, TX_DONE ve TX_REQUEST geldiğinde hepsi doğru işlenir. Task, TX sürerken RX alır; boşta sürekli dönmez; kuyrukta bekleyen çerçeve bildirimin birleşmesi yüzünden takılı kalmaz.

## 9. Aşama 6 — Kesme önceliklerini ve callback sahipliğini tamamla

- [ ] NVIC gruplamasını `NVIC_PRIORITYGROUP_4` yap.
- [ ] USART2, RX DMA ve TX DMA kesmelerini `FromISR` çağrılarına uygun önceliğe ayarla.
- [ ] Örneğin `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5` ise başlangıçta üçünü de 5 seçmek uygundur; gerçek FreeRTOS yapılandırmasıyla doğrula.
- [ ] SVC, PendSV, SysTick ve HAL zaman tabanını mevcut asıl FreeRTOS projesiyle uyumlu kur; eski bare-metal handler'ları çakışacak biçimde taşıma.
- [ ] Task handle'ı hazır olmadan RTOS bildirimi üretilememesini sağla; UART DMA başlatmasını task başlangıcına taşı.
- [ ] Tek bir HAL RX callback, tek TX callback ve tek error callback tanımı bırak.

Önerilen modül düzeni:

| Dosya | Sorumluluk |
|---|---|
| `frame.c/.h`, `crc16.c/.h` | Mevcut çerçeve oluşturma ve CRC |
| `parser.c/.h` | HAL/RTOS'tan bağımsız akış ayrıştırma |
| `uart_rx.c/.h` | RX DMA tamponu ve bayt tüketimi |
| `uart_tx.c/.h` | Aktif TX tamponu, TX başlatma ve durum yönetimi |
| Yeni `uart_comm.c/.h` | Tek task, TX kuyruğu, olaylar, ortak HAL callback yönlendirmesi |
| `tests.c/.h` | Test senaryoları |

HAL callback'leri `uart_comm.c` içinde merkezi tutulabilir. Şu anda `uart_rx.c` içinde olan callback'ler taşınırsa eski tanımları kaldır; aynı sembolü iki dosyada bırakma. RX istatistikleri kısa yardımcı fonksiyonlarla güncellenebilir.

`frame_received()` içindeki uygulama davranışını kayıtlı handler üzerinden ortak task/uygulama katmanına ayır. Böylece RX modülü joystick veya cevap politikasının sahibi olmak zorunda kalmaz.

## 10. Aşama 7 — Yanıt gönderimini ve hata durumlarını bağla

- [ ] Mevcut `FRAME_TYPE_RESPONSE = 0x80` ile ilk komut yanıtını oluştur.
- [ ] Yanıt SEQUENCE alanında ilgili komutun kimliğini taşısın; payload yanıtlanan TYPE ve sonuç kodunu içersin.
- [ ] Ortak taskın ürettiği yanıt da aynı TX FIFO'ya kopyalansın; handler doğrudan HAL TX çağırmasın.
- [ ] Kuyruk doluysa aynı task kuyruğun boşalmasını beklemesin. Komut kabul sınırı ve yanıt kaybı davranışını tanımla; gerekirse bekleyen yanıt için yer ayır veya isteği kabul etme.
- [ ] UART hata kodunu callback sırasında kaydet. Daha sonraki HAL çağrısının `huart->ErrorCode` alanını değiştirebileceğini dikkate al.
- [ ] RX hatası ile TX hatasını ayır; her UART hatasını otomatik olarak aktif TX'in başarısızlığı sayma. Ortak HAL DMA hata yolunun iki yönün durumuna etkisini incele.
- [ ] Yalnızca RX kurtarılacaksa çalışan TX'i gereksiz yere durdurma. İki yön de etkilenmişse ikisini de tutarlı biçimde kurtar.
- [ ] TX için uzunluğa, 115200/8N1 iletim süresine ve task gecikmesine göre tamamlanma son tarihi belirle.
- [ ] Zaman aşımı/iptalde DMA ve UART aktarımının durduğu kesinleşmeden yeni çerçeve başlatma. Eski tamamlanma/abort olayının yeni çerçeveyi tamamlanmış saymasını engelle.
- [ ] Kısmen gönderilmiş çerçeveyi körlemesine otomatik tekrarlama; protokol tekrar/ACK politikasıyla karar ver.

64 baytın hatta aktarımı yaklaşık 5,56 ms'dir. Bu değer tek başına yazılım zaman aşımı değildir; task gecikmesi ve zaman çözünürlüğü için pay gerekir. Son tarih kontrolü ayrı bloklayan gecikmeyle değil, ortak taskın süreli bildirim beklemesiyle yapılır.

RX kısmi paket zaman aşımı da eklendiğinde bekleme süresi, TX ve RX son tarihlerinin en yakınına göre hesaplanır. RX zaman aşımı kontrolü yalnızca yeni RX olayı geldiğinde çalıştırılmaz; hat sessizken de çalışmalıdır.

Kabul ölçütü: Komut alındığında yanıt diğer RX verilerini durdurmadan gönderilir. Kuyruk doluluğu, başlatma hatası ve tamamlanmama durumu görünür bir sonuç üretir; kilitlenme veya sessiz tampon bozulması oluşmaz.

## 11. Aşama 8 — Ortak taskı doğrula

- [ ] Mevcut CRC/çerçeve/ayrıştırıcı testlerini koru.
- [ ] Tek TX ve art arda TX testlerini Normal DMA ile çalıştır.
- [ ] Kaynak tamponu kuyruk kabulünden hemen sonra değiştir; gönderilen kopyanın doğru kaldığını doğrula.
- [ ] Farklı üretici görevlerden gönderim iste; çerçeve baytlarının birbirine girmediğini doğrula.
- [ ] FIFO sırasını kuyruğa başarılı eklenme sırasına göre kontrol et.
- [ ] TX meşgulken yeni çerçeveler ekle; tamamlanma sonrasında kuyruk kendiliğinden ilerlesin.
- [ ] 100 Hz joystick RX sürerken komut yanıtları gönder; RX sarımı ve CRC sonuçlarını kontrol et.
- [ ] RX ve TX tamamlanma olaylarını yakın zamanlı üret; olay kaybı ve çift işlemeyi kontrol et.
- [ ] Kuyruk doluluğu, HAL_BUSY/HAL_ERROR ve TX zaman aşımı yollarını sınırlandırılmış senaryolarla doğrula.
- [ ] RX hatası sırasında TX'in, TX hatası sırasında RX'in gerçek davranışını kontrol et.
- [ ] Bildirimle uyanma yolunu doğrudan `drain()` çağrısıyla atlamadan test et.
- [ ] Task stack kullanımını, en uzun RX tüketim gecikmesini ve TX bekleme süresini gerçek uygulama yükünde ölç.
- [ ] Derleyici hatalarını, callback çakışmalarını ve paylaşılan durum yarışlarını kontrol et.

İlk kabul için yoğun trafik zorunlu değildir. Önce basit senaryolarda doğru sahiplik, sıra, uyanma ve hata davranışını göster; sonra yük sınırlarını ölç.

## 12. İlk sürüme dahil olmayan sonraki işler

- Joystick aç/kapat durum makinesinin cihaz kontrolüne bağlanması.
- Komutlara özgü uzun işlemler ve iş tamamlama sonuçları.
- ACK zaman aşımı, tekrar gönderim ve tekrar komut ayıklama politikasının tamamlanması.
- Telemetri için en güncel örnek politikası veya öncelikli cevap kuyruğu.
- Gerçek trafik gerektirirse tampon/kuyruk boyutlarının artırılması.

Ortak task kullanımı bu özelliklerin sonradan eklenmesini engellemez. İşleme süreleri uzarsa cihaz işi ilgili göreve aktarılır; UART'ın RX/TX sahipliği tek haberleşme taskında kalır.

## 13. Uygulama sırası özeti

1. **TX DMA ayarı:** Normal DMA ve doğru IRQ zinciri.
2. **TX modülü:** Tek aktif çerçeve, kalıcı tampon, tamamlanma ve hata durumları.
3. **RX + TX birlikte test:** Henüz RTOS eklemeden donanım akışını doğrula.
4. **FreeRTOS kuyruğu ve task:** Tam çerçeve kopyası, tek UART sahibi.
5. **Ortak olay döngüsü:** RX, TX isteği, TX tamamlandı ve hata bildirimleri.
6. **IRQ/callback entegrasyonu:** RTOS öncelikleri ve tek callback sahipliği.
7. **Yanıt ve toparlama:** Kuyruk doluluğu, süre aşımı, yönlere uygun kurtarma.
8. **Doğrulama:** Önce işlevsel, ardından gerçek yük altında zamanlama.

Pratikte 4–6. aşamalar aynı entegrasyon çalışmasının parçalarıdır; RTOS `FromISR` bildirimleri etkinleştirilmeden 6. aşamanın öncelik ve başlangıç koşulları tamamlanmış olmalıdır.

## 14. Dayanaklar

- [STM32F407 DMA eşlemeleri — RM0090](https://www.st.com/resource/en/reference_manual/dm00031020-stm32f405-415-stm32f407-417-stm32f427-437-and-stm32f429-439-advanced-arm-based-32-bit-mcus-stmicroelectronics.pdf)
- [FreeRTOS — Task notification ile olay bitleri](https://freertos.org/RTOS_Task_Notification_As_Event_Group.html)
- [FreeRTOS — xTaskNotify](https://www.freertos.org/Documentation/02-Kernel/04-API-references/05-Direct-to-task-notifications/04-xTaskNotify)
- Projedeki `Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c`: `HAL_UART_Transmit_DMA`, `UART_DMATransmitCplt` ve `UART_EndTransmit_IT` akışı.

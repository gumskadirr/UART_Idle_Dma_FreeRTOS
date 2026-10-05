# UART RX geliştirme planı

Tarih: 29 Eylül 2026  
Hedef: STM32F407VG, STM32 HAL, STM32CubeIDE ve FreeRTOS  
Durum: Bu belge sohbetten çıkarılmış geliştirme planıdır. Aşağıdaki işler henüz uygulanmamış ve donanım üzerinde doğrulanmamıştır.

## 1. Amaç ve kapsam

PC arayüzü, joystick etkin olduğunda STM32'ye 100 Hz hızında X–Y verisi gönderecek. Joystick akışı devam ederken butonlardan başka cihaz komutları da gönderilebilecek. Joystick kapatıldığında UART alımı çalışmaya devam edecek; sadece joystick verisinin uygulanması duracak.

Hedefler:

- Bayt başına kesme ve sürekli sorgulama yükünden kaçınmak.
- Farklı türde paketleri aynı UART akışından güvenilir biçimde ayırmak.
- Joystick için en güncel örneği kullanmak; cihaz komutlarının sırasını korumak.
- Başlangıçta tek bir UART FreeRTOS taskı kullanmak.
- Hatalı, eksik, birleşik ve tampon sınırına denk gelen paketleri yönetmek.
- Komut kaybını veya alım hatasını sessizce gizlememek.

Paket sınırını IDLE belirlemeyecek. IDLE, HT ve TC yalnızca yeni verinin işlenmesi gerektiğini bildirecek.

## 2. Mevcut proje durumu

Belge hazırlanırken kontrol edilen durum:

| Konu | Mevcut durum |
|---|---|
| MCU | STM32F407VG |
| UART | USART2, 115200 baud, 8 veri biti, parity yok, 1 stop biti |
| Pinler | PA2: TX, PA3: RX |
| Akış kontrolü | Donanımsal akış kontrolü kapalı |
| RX DMA | DMA1 Stream5, Channel4, Circular |
| DMA kesmesi | Handler mevcut; öncelik 0 |
| USART2 kesmesi | USART2 IRQ handler henüz yok |
| NVIC gruplaması | NVIC_PRIORITYGROUP_0 |
| Alımın başlatılması | ReceiveToIdle DMA çağrısı henüz yok |
| FreeRTOS | İncelenen bu alt projede henüz yok; asıl uygulama FreeRTOS kullanacak |
| Uygulama protokolü | Henüz uygulanmamış |

İlgili dosyalar: `Core/Src/main.c`, `Core/Src/stm32f4xx_hal_msp.c`, `Core/Src/stm32f4xx_it.c`, `UART_IDLE_DMA.ioc`.

Uygulama sırasında pin atamaları ve 115200/8N1 ayarı korunacak. Üretilmiş kodda gereksiz değişiklik yapılmayacak; uygulama kodu USER CODE alanları ve ayrı uygulama dosyalarında tutulacak. Yeni kütüphane eklenmeden önce kullanıcıya sorulacak.

## 3. Hedef mimari

```text
PC
  Joystick zamanlayıcısı ─┐
                         ├─ Paketleme → Ortak gönderim yöneticisi → UART
  Buton olayları ─────────┘

STM32
  UART → Circular DMA tamponu
                  ↓
        IDLE / HT / TC kesmesi
                  ↓ task bildirimi
             Tek UART taskı
                  ↓
       Yeni baytları tüketme ve ayrıştırma
                  ↓
        Uzunluk / tür / CRC kontrolü
                  ↓
       Joystick durumu veya cihaz komutu
```

DMA tamponunun kendisi ring buffer olarak kullanılacak. Başlangıçta ikinci bir ham bayt ring buffer eklenmeyecek. Ayrıştırıcının yarım paketi saklamak için küçük, ayrı bir paket çalışma alanı olabilir.

UART taskı kısa komutları geliş sırasıyla uygulayabilecek. Uzun işlemler bir durum makinesiyle başlatılıp ilerletilecek veya asıl projedeki ilgili göreve aktarılacak. Motorun hedefe ulaşması gibi bir işlemin tamamlanması UART taskında beklenmeyecek.

## 4. Başlangıç değerleri ve açık kararlar

| Konu | Başlangıç kararı / önerisi |
|---|---|
| Joystick gönderim frekansı | 100 Hz; her 10 ms'de bir örnek |
| RX DMA tamponu | 256 bayt başlangıç değeri; ölçümlerle doğrulanacak |
| İlk değerlendirmedeki 512 bayt | Daha geniş gecikme payı için önerilmişti; zorunlu boyut değil |
| Paket formatı | 2 bayt başlangıç + sürüm + tür + uzunluk + sıra numarası + veri + CRC16 |
| Maksimum payload | Başlangıç önerisi 55 bayt |
| Maksimum toplam paket | Başlangıç önerisi 64 bayt |
| Joystick payload | İki adet işaretli 16 bit sayı; toplam 4 bayt |
| Joystick değer aralığı | Başlangıç önerisi −1000…+1000; arayüzle kesinleştirilecek |
| Çok baytlı alanlar | Little-endian; düşük bayt önce |
| UART task sayısı | Başlangıçta 1 |

Henüz belirlenmeyenler: Gerçek cihaz komutları ve süreleri, komut kuyruğu kapasitesi, kısmi paket zaman aşımı, joystick veri zaman aşımı, ACK zaman aşımı ve tekrar sınırı, task önceliği ve stack boyutu.

Bu değerler başlangıçta sınırlı ve açık bir sözleşmeyle seçilecek. Büyük trafik ve ayrıntılı performans optimizasyonu sonraki aşamada ele alınacak; temel veri bütünlüğü ilk sürümden itibaren korunacak.

## 5. Adım 1 — PC ve STM32 için protokol sözleşmesini kesinleştir

- [ ] Alan sırasını, boyutlarını, bayt sırasını ve maksimum uzunluğu iki tarafta aynı tanımla.
- [ ] Mesaj türlerini ve her türün kabul ettiği payload uzunluğunu listele.
- [ ] Joystick aralığını, merkez değerini ve eksen yönlerini belirle.
- [ ] Sıra numarasının hangi akışta arttığını belirle: tüm mesajlar için ortak mı, mesaj sınıfına göre ayrı mı? Ortak sayaçta joystick aralarındaki başka komutları kayıp sanma.
- [ ] 16 bit sayacın taşmasını ve PC/STM32 yeniden başladığında oturumun nasıl yenileneceğini tanımla.
- [ ] Komut yanıtlarının “kabul edildi”, “tamamlandı”, “meşgul” ve “geçersiz” anlamlarını belirle.

### Önerilen paket

```text
AA 55 | VERSION | TYPE | LENGTH | SEQUENCE | PAYLOAD | CRC16
  2        1       1      1         2          N        2    bayt
```

`LENGTH` yalnızca payload boyutudur. Toplam paket boyutu `9 + LENGTH` olur. `VERSION` başlangıçta `1` olacak.

| TYPE | Mesaj | Payload |
|---|---|---|
| 0x10 | Joystick verisi | X: int16, Y: int16; 4 bayt |
| 0x11 | Joystick modu | 0: kapalı, 1: açık; 1 bayt |
| 0x20 | Çıkış ayarla — örnek cihaz komutu | Çıkış numarası ve istenen durum; 2 bayt |
| 0x80 | Komut yanıtı | Yanıtlanan TYPE ve sonuç kodu; 2 bayt |

Yanıtın `SEQUENCE` alanı, yanıtlanan komutun numarasını taşıyacak. Diğer komut türleri gerçek cihaz gereksinimleriyle eklenecek.

### CRC sözleşmesi

CRC-16/IBM-3740, diğer adıyla CRC-16/CCITT-FALSE:

```text
Polynomial : 0x1021
Initial    : 0xFFFF
Reflect in : false
Reflect out: false
XOR out    : 0x0000
```

CRC, VERSION alanından payload sonuna kadar hesaplanacak. Başlangıç baytları ve CRC alanının kendisi hesaba katılmayacak. Sonuç düşük bayt önce gönderilecek.

Kontrol örnekleri:

```text
ASCII "123456789" → CRC = 0x29B1

X = +1000, Y = −500, SEQUENCE = 1:
AA 55 | 01 | 10 | 04 | 01 00 | E8 03 0C FE | 46 59
CRC = 0x5946, toplam = 13 bayt
```

Paketler açık bayt dönüşümleriyle oluşturulacak. C struct belleği doğrudan UART'a gönderilmeyecek; padding ve bayt sırası varsayımlarına dayanılmayacak.

Çıktı: PC ve STM32'nin aynı örnek paketleri üretip çözebildiği protokol tanımı.

## 6. Adım 2 — PC gönderimini tek noktada sırala

- [ ] Joystick ve buton olaylarının doğrudan seri porta yazmasını kaldır; tek gönderim yöneticisi kullan.
- [ ] Başlayan bir paketin tüm baytlarını, gerekirse kısmi yazmaları tamamlayarak, diğerine geçmeden gönder.
- [ ] Cihaz komutlarını FIFO mantığıyla koru.
- [ ] Bekleyen joystick örnekleri birikirse henüz gönderilmeyen örneği en güncel değerle değiştir.
- [ ] Komutlara öncelik verilecekse bunu yalnızca paket sınırlarında uygula.
- [ ] Gönderim kuyruğu doluluğu için açık davranış tanımla; buton komutlarını sessizce düşürme.
- [ ] Joystick aç/kapat işlemini açık bir mod paketiyle gönder.

Beklenen hat akışı:

```text
[Joystick 1] [Joystick 2] [Buton komutu] [Joystick 3]
```

Gönderilmeye başlamış bir joystick paketinin arasına başka paketin baytları girmeyecek. İşletim sistemi veya USB–UART, paketleri parçalı ya da birleşik ulaştırabilir; STM32 buna bağımlı olmayacak.

Çıktı: Eşzamanlı buton ve joystick üretiminde paket baytlarının birbirine girmediği gönderici.

## 7. Adım 3 — CubeMX ve FreeRTOS altyapısını hazırla

- [ ] Asıl FreeRTOS projesine entegrasyon noktasını belirle; mevcut kernel ve HAL zaman tabanı düzenini kontrol et.
- [ ] USART2 115200/8N1, PA2/PA3 ve mevcut RX DMA eşlemesini koru.
- [ ] RX DMA Circular, byte veri genişliği, memory increment açık, peripheral increment kapalı olsun.
- [ ] USART2 global interrupt'ı etkinleştir; handler içinde `HAL_UART_IRQHandler(&huart2)` çağrılsın.
- [ ] DMA handler içindeki `HAL_DMA_IRQHandler(&hdma_usart2_rx)` çağrısını koru.
- [ ] NVIC gruplamasını FreeRTOS Cortex-M4 kullanımına uygun `NVIC_PRIORITYGROUP_4` olarak düzenle.
- [ ] USART2 ve RX DMA kesme önceliklerini `FromISR` çağrılarına uygun seç. Mevcut öncelik 0 kullanılmayacak.
- [ ] Örneğin `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5` ise bu iki IRQ için sayısal olarak 5 veya daha büyük, uygulanabilen bir değer kullan. Gerçek projedeki ayarı esas al.
- [ ] Task ve bildirim altyapısı hazır olduktan sonra RX DMA'yı başlat; dönüş değerini kontrol et.

Gerekli CubeMX değişiklikleri yukarıdaki gibidir. `.ioc` düzenlemesi yapılırken bunlar uygulanıp açıklanacak; bu belge oluşturulurken `.ioc` değiştirilmedi.

Çıktı: FreeRTOS altında kesmeleri doğru öncelikle çalışan UART RX altyapısı.

## 8. Adım 4 — Circular DMA ve okuma konumunu uygula

- [ ] Ömrü alım boyunca süren, DMA erişimine uygun SRAM'de bulunan 256 baytlık tampon ayır. STM32F407'nin CCM belleğini RX DMA tamponu için kullanma.
- [ ] `HAL_UARTEx_ReceiveToIdle_DMA()` ile normal çalışma başlangıcında alımı bir kez başlat.
- [ ] HT, TC ve IDLE olaylarını açık tut. Normal callback'lerde DMA'yı durdurup yeniden başlatma.
- [ ] Callback içinde yalnızca ilgili UART'ı kontrol edip `vTaskNotifyGiveFromISR()` ve gerektiğinde `portYIELD_FROM_ISR()` kullan.
- [ ] Bildirim sayısını paket veya bayt sayısı olarak yorumlama.
- [ ] Okuma konumunu yalnızca UART taskı değiştirsin; ayrıştırıcı durumu da bu taska ait olsun.

Konumların anlamı:

```text
write_pos = (tampon_boyutu - DMA_kalan_sayacı) % tampon_boyutu
read_pos  = taskın bir sonraki okuyacağı konum
```

Task, `read_pos` ile yakaladığı `write_pos` arasındaki baytları tüketir. Sarım varsa önce tampon sonuna, sonra başına kadar olan iki aralık işlenir. İşleme sırasında gelen veriler için yazma konumu yeniden kontrol edilir; elde veri kalmayınca task bildirimi bekler.

Tampon okununca sıfırlanmayacak; `read_pos` ilerletilecek. Başka görevlere DMA tamponuna ait kalıcı veri işaretçisi verilmeyecek; gerekiyorsa doğrulanmış mesaj kopyalanacak.

Yalnızca modüler konumlar tam tur taşmasını tespit edemez. DMA okunmamış veri üzerinden tam tur geçerse konumlar eşit görünebilir. Bu nedenle zamanında tüketim zorunludur. Ayrıca taşma tespiti gerekiyorsa toplam üretim/tüketim veya tur sayacı tasarlanacak; kesme gecikmeleri ve sayaç yarışları da hesaba katılacak. `volatile` tek başına senkronizasyon çözümü sayılmayacak.

Çıktı: Her yeni baytın normal çalışma koşullarında bir kez ve doğru sırayla ayrıştırıcıya verilmesi.

## 9. Adım 5 — Akış üzerinde paket ayrıştırıcısını uygula

- [ ] Başlangıç ara → başlığı al → uzunluğu kontrol et → payload/CRC bekle → doğrula → teslim et durumlarını oluştur.
- [ ] Bir uyanmada birden fazla paketi işle.
- [ ] Yarım paketi sonraki okumaya sakla; IDLE'da ayrıştırıcıyı sıfırlama.
- [ ] Maksimum uzunluk, sürüm ve mesaj türüne özgü uzunlukları kontrol et.
- [ ] CRC doğru olmadan komutu uygulama.
- [ ] Payload içindeki `AA 55` dizisini normal veri kabul et.
- [ ] Hatalı aday paket sonrası adayın başlangıcından bir bayt ilerleyerek yeniden taranabilecek veriyi koru; arkadaki geçerli paketi körlemesine silme.
- [ ] Eksik paket için PC aktarım davranışıyla uyumlu, sınırlandırılmış bir tamamlama zaman aşımı tanımla.
- [ ] Aralık dışı X–Y, geçersiz mod ve bilinmeyen komut için tanımlı hata davranışı uygula.

Paket ayrıştırıcısı HAL ve FreeRTOS'tan mümkün olduğunca bağımsız tutulacak; giriş olarak bayt veya bayt aralığı alacak. Böylece donanım olmadan da anlamlı test yapılabilecek.

Çıktı: Parçalanma, birleşme ve tampon sarımından bağımsız paket çözümleme.

## 10. Adım 6 — Tek task içinde joystick ve komut davranışını kur

- [ ] Başlangıçta joystick kontrolünü kapalı tut.
- [ ] Joystick açıkken doğrulanmış en güncel X–Y örneğini, sıra numarasını ve alım zamanını birlikte kaydet.
- [ ] Joystick kapatma komutu işlendiğinde kayıtlı örneği geçersiz yap; kapalı modda gelen örneği hareket için uygulama.
- [ ] Yeniden açıldığında eski örneği kullanma; yeni geçerli örneği bekle.
- [ ] Paket sırasına bağlı mod geçişlerini aynı taskın durum makinesinde sırayla işle. Aç/kapat sınırlarının üzerinden eski joystick verisi taşınmasın.
- [ ] Kısa cihaz komutlarını geliş sırasıyla uygula. Ertelenen işlemler için gerekirse sınırlı FIFO oluştur.
- [ ] UART taskını dolu kuyruk, motor hareketi, bloklayan haberleşme veya uzun işlem beklemesiyle durdurma.
- [ ] Uzun işlemleri küçük adımlarla ilerlet veya ilgili mevcut göreve aktar.
- [ ] Başka görev joystick okuyacaksa tutarlı örnek aktarımı sağla; örneğin uzunluğu 1 olan kuyruk ve `xQueueOverwrite()` kullan. Salt paylaşılan X ve Y değişkenlerine güvenme.
- [ ] Joystick veri zaman aşımı ve uygun çıkış davranışını belirle. Yalnızca geçerli joystick örneği bu süreyi yenilesin.

Task, yeni veri yokken FreeRTOS bildirimi bekleyecek; sabit 100 Hz sorgulama döngüsü kurulmayacak. Aynı task zaman aşımı veya işlem ilerlemesi takip ediyorsa bir sonraki son tarihe kadar süreli bekleyecek. `HAL_Delay()` kullanılmayacak; araya konan `vTaskDelay()` çağrısının da alım tüketimini geciktirdiği unutulmayacak.

Çıktı: Tek task içinde doğru mod sırası, güncel joystick ve sıralı kısa komut işleme.

## 11. Adım 7 — Yanıtları, tekrarları ve hata toparlamayı ekle

- [ ] Joystick örnekleri için varsayılan olarak tek tek ACK/yeniden gönderim uygulama; güncel veri ve zaman aşımını esas al.
- [ ] Teslimi takip edilecek buton komutlarında SEQUENCE ve sonuç yanıtını kullan.
- [ ] Zaman aşımında aynı komut kimliğiyle yeniden gönder; STM32 aynı komutu tekrar uygulamak yerine önceki durumunu/sonucunu dönebilsin.
- [ ] Tekrar tespitinin sınırlarını, sıra numarası taşmasını ve oturum yenilenmesini tanımla.
- [ ] Komut kabul edilemiyorsa “meşgul” gibi açık sonuç dön; sessizce düşürme.
- [ ] Yanıt TX yolunda tek sahip ve tamamlanana kadar geçerli tampon kullan. DMA/interrupt tabanlı gönderimi tercih et; `HAL_BUSY` ve kuyruk doluluğunu yönet.
- [ ] UART/DMA hata callback'lerinden taska hata bildir; normal veri bildirimiyle hata durumunu ayırt et.
- [ ] HAL alımı sonlandırmışsa, taskın sahip olduğu kontrollü toparlama akışıyla yeniden başlat. Kurtarma sırasında indeksleri ve yarım paket durumunu tutarlı biçimde sıfırla.
- [ ] CRC hatası, geçersiz uzunluk, UART/DMA hatası, kuyruk doluluğu ve tespit edilen kayıplar için sayaç tut.

RX ve TX aynı UART'ı paylaşırken RX hata toparlaması sürmekte olan TX'i istemeden kesmemeli. Gerekli TX DMA ayarı eklenirse CubeMX değişikliği ayrıca açıklanacak.

Çıktı: Komut sonucunun takip edilebildiği, tekrar uygulamanın önlendiği ve hatadan toparlanan iletişim.

## 12. Adım 8 — Kademeli doğrulama yap

### İlk işlevsel doğrulamalar

- [ ] CRC test vektörlerini PC ve STM32 tarafında doğrula.
- [ ] Tek paket, her olası noktadan bölünmüş paket ve art arda birleşmiş paketleri dene.
- [ ] `AA 55` içeren payload, hatalı CRC, aşırı uzunluk ve eksik paket senaryolarını dene.
- [ ] Paket başlığını ve payload'ı DMA tamponunun sonuna denk getirerek sarımı dene.
- [ ] IDLE ve HT/TC'nin yakın oluştuğu durumda aynı baytların iki kez işlenmediğini doğrula.
- [ ] 100 Hz joystick sürerken buton komutları gönder; kimlik ve sonuçları karşılaştır.
- [ ] Joystick aç/kapat/yeniden aç sırasını ve eski örneğin uygulanmadığını kontrol et.
- [ ] Hat sessizleştiğinde zaman aşımının çalıştığını kontrol et.
- [ ] Tekrar komut, kayıp yanıt, dolu kuyruk ve bağlantı yeniden kurulması senaryolarını dene.

### Sonraki aşama: Gerçek yük ve zaman bütçesi

115200/8N1 için teorik maksimum giriş hızı 11.520 bayt/s'dir:

| DMA tamponu | Yarım tampon süresi | Tam tampon süresi |
|---|---:|---:|
| 128 bayt | 5,56 ms | 11,11 ms |
| 256 bayt | 11,11 ms | 22,22 ms |
| 512 bayt | 22,22 ms | 44,44 ms |

Bu süreler kesintisiz tam hızlı trafik içindir. 13 baytlık önerilen joystick paketi 100 Hz'de 1300 bayt/s, yaklaşık %11,3 hat doluluğu oluşturur.

IDLE, son alınan karakterden sonra yaklaşık bir karakter süresi boşlukta oluşur; mevcut ayarda bu süre yaklaşık 86,8 µs'dir. 100 Hz küçük paket akışı bunu engellemez. Ancak PC paketleri birleştirebilir; her paket için ayrı IDLE beklenmeyecek.

Tampon boyutu, task tüketene kadar birikebilecek en büyük veri miktarına göre seçilecek. HT/TC ile her yarım tamponun, DMA yeniden üzerine yazmaya başlamadan tüketilmesi muhafazakâr bir hedef olacak. Kesme gecikmesi, taskın beklemesi, diğer işlerin araya girmesi ve tüketme süresi birlikte ölçülecek.

Sohbetteki 5 ms tüketim bütçesi yalnızca bir tasarım hedefiydi; ölçülmüş sonuç veya kesin garanti değildir. 256 bayt tamponla yarım tampon süresi 11,11 ms olduğundan bu hedefin gerçek uygulamada sağlanabildiği kontrol edilecek. Gerekiyorsa tampon büyütülecek veya uzun işler ayrılacak; tampon büyütmenin sürekli yavaş tüketiciyi çözemeyeceği dikkate alınacak.

- [ ] Gerçek FreeRTOS görevleri çalışırken en kötü alım gecikmesini ölç.
- [ ] Sürekli tam hızlı trafik ve kısa trafik patlamalarıyla HT/TC yolunu doğrula.
- [ ] Kontrollü task gecikmesinde kayıp/taşma davranışının görünür olduğunu doğrula.
- [ ] Kritik bölüm, kesme önceliği, DMA bellek erişimi ve paylaşılan durum yarışlarını gözden geçir.
- [ ] STM32CubeIDE derlemesini tamamla; derleyici hatalarını ve ilgili uyarıları gider.

Çıktı: Tanımlanan kullanım sınırlarında test edilmiş iletişim. Donanım testi yapılmadan kayıpsızlık veya belirli CPU kullanım oranı iddia edilmeyecek.

## 13. Önerilen uygulama sırası

1. Protokolü ve örnek bayt dizilerini kesinleştir.
2. PC paketleyicisini ve tek göndericiyi oluştur.
3. FreeRTOS, UART ve DMA kesme ayarlarını hazırla.
4. Circular DMA'dan yeni baytları tek taska güvenilir biçimde aktar.
5. Paket ayrıştırıcısını ekle ve temel akış testlerini yap.
6. Joystick modu, güncel örnek ve kısa komut işleme davranışını ekle.
7. Komut yanıtı, tekrar ayıklama ve hata toparlamayı tamamla.
8. Gerçek uygulama yükünde süreleri ölç; tampon ve task ayarlarını doğrula.

İlk hedef, büyük bir iletişim altyapısı kurmak değil; küçük, açık kuralları olan ve test edilebilen bir UART modülü oluşturmaktır. Mevcut komutlar büyüdükçe aynı paket yapısına yeni mesaj türleri eklenebilir.

## 14. Başvuru kaynakları

- [ST — UART ReceptionToIdle Circular DMA örneği](https://github.com/STMicroelectronics/STM32CubeF4/blob/master/Projects/STM32446E-Nucleo/Examples/UART/UART_ReceptionToIdle_CircularDMA/readme.txt)
- [FreeRTOS — Cortex-M kesme öncelikleri](https://freertos.org/Documentation/02-Kernel/03-Supported-devices/04-Demos/ARM-Cortex/RTOS-Cortex-M3-M4)
- [CRC-16/IBM-3740 parametreleri](https://reveng.sourceforge.io/crc-catalogue/16.htm#crc.cat.crc-16-ibm-3740)

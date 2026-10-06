# Taşınabilir UART ve protokol ayrımı

Tarih: 6 Ekim 2026. Durum: kullanıcı tarafından `devam et` yanıtıyla onaylandı; uygulama planı inceleme aşamasında.
Başlangıç: GitHub `main`, `15df55a97748ced5ae6ebfcedbce26a86f5ff9a5`.

## Amaç ve kapsam

UART kaynakları yeni bir STM32F4 + HAL + FreeRTOS projesine çekirdek kod değiştirilmeden kopyalanabilmeli. Kullanıcı mevcut RX/TX güvenilirliğini ve düşük boşta CPU tüketimini koruyan, anlaşılır ve az dosyalı bir yapı istiyor.

İlk sürüm tek UART ve tek statik owner task kullanır. Çoklu UART, başka MCU aileleri, RTOS'suz kullanım, ACK veya otomatik yeniden gönderim kapsam dışındadır. Harici kütüphane eklenmez. Mevcut STM32F407VG, USART2, PA2/PA3, DMA ve FreeRTOS ayarları korunur; `.ioc` değişmez.

## Katmanlar ve dosyalar

```text
Lib/Uart/
    uart_comm.c             RX/TX, FIFO, toparlanma, owner task
    uart_comm.h             uygulama API'si ve byte teslim sözleşmesi
    uart_comm_internal.h    özel çekirdek/test erişimi
    uart_comm_config.h      derleme zamanı tampon, task ve süre ayarları
    uart_comm_port.c        STM32F4 register/DMA erişimi ve doğrulama
    uart_comm_port.h        çekirdeğin kullandığı özel donanım arayüzü
    README.md               başka projeye taşıma ve ham byte örneği

Core/Inc + Core/Src:
    protocol.h/.c           mevcut format, CRC ve parser; HAL/RTOS bağımsız
    protocol_uart.h/.c      isteğe bağlı UART–parser bağlantısı
    app_protocol.h/.c       joystick ve SEQ gibi uygulama davranışı
```

Bu düzen önceki UART dosyalarını taşır; aynı işi yapan kopya sürücüler bırakılmaz. İki dosyalık isteğe bağlı adaptör, protokolü kullanan üretim ve test kodunun aynı timeout/reset bağlantısını kullanmasını sağlar. Yeni projeye yalnız `Lib/Uart` alınarak ham byte haberleşmesi yapılabilir. Aynı paket formatı gerekiyorsa `protocol` ve adaptör de alınır. `app_protocol` yeni uygulamanın ihtiyacına göre kullanılır veya değiştirilir.

Ek task, RX kuyruğu veya dinamik bellek oluşturulmaz. Adaptör ve uygulama handler'ı mevcut UART taskında kısa ve beklemeden çalışır.

## RX bağlantı sözleşmesi

UART artık `protocol.h`, `frame_parser_t`, `frame_info_t`, CRC veya `FRAME_MAX_SIZE` bilmez. UART'a ait gönderim kapasitesi bağımsız bir ayar olur; varsayılanı 64 bayttır.

Tek RX callback'i üç olay alır: `DATA`, `TIMEOUT`, `RESET`. DATA, kopyalanması ve oturum/hata nesli doğrulanması tamamlanmış byte'ları verir. Veri işaretçisi yalnız callback süresince geçerlidir. TIMEOUT ve RESET veri taşımaz. Callback IRQ veya kritik bölüm içinde çağrılmaz.

Callback dönüşü iki bağımsız bilgi taşır: bekleyen eksik mesaj var mı; bu çağrıda doğrulanmış mesaj bulundu mu. Ham byte kullanan uygulama ikisini de kapalı döndürür. Protokol adaptörü bunları parser'ın bekleyen adayı ve gerçekten CRC/format doğrulanmış çerçevesine göre üretir. Ham byte alınması doğrulanmış protokol mesajı sayılmaz.

- Parser ve frame handler'ının sahibi `protocol_uart` context'idir. Adaptör context'i ve kullanıcı hedefi UART modülü ömrü boyunca yaşar; başlangıç scheduler öncesidir.
- RESET, taşma veya yeni RX oturumu öncesinde eksik adayı atar; protokol istatistiklerini korur. İlk kurulumda parser ayrıca initialize edilir. Reset dönüşü bekleyen aday bırakmaz.
- Eksik aday varsa, varsayılan 50 ms timeout son **DMA üretici ilerlemesine** göre hesaplanır. Scratch tesliminin veya taskın geç çalışmasının zamanı deadline'ı uzatmaz.
- TIMEOUT öncesinde üretici tekrar örneklenir. Yeni byte veya ertelenmiş örnekleme varsa timeout işlenmez. Timeout ikinci RX bütçesi açmaz.
- TIMEOUT sonrası hâlâ aday varsa bir sonraki timeout yeni zamanla kurulur; aday yoksa periyodik uyanma bırakılmaz. `uint32_t` tick sarımı korunur.
- Doğrulanmış mesaj, aktif RX toparlanmasını yalnız donanım sağlıklıyken ve bekleyen hata yokken kapatabilir. Mevcut 100 ms sağlıklı çalışma ile kapatma yolu da korunur.

Kullanıcı API'sindeki `on_frame` yerine bu genel `on_rx` bağlantısı gelir. Mevcut frame handler'ları adaptöre bağlanır. Init/send/recovery/snapshot kullanım şekli korunur; gerekiyorsa protokole ait timeout sayaç adı UART tarafında genel RX timeout olarak güncellenir. Parser istatistikleri UART snapshot'ından değil adaptörün/protokolün durumundan okunur.

## STM32F4 bağlantısı

`uart_comm_init` seçilen HAL UART handle'ını almaya devam eder. Port, RX/TX DMA handle'larından stream ve ilgili IRQ'yu çıkarır; sabit USART2, DMA1 Stream5/6 ve Channel4 şartı kaldırılır. STM32F4 için DMA1/DMA2 stream IRQ eşlemesi doğrulanır. HAL parent, ayrı RX/TX stream, RX circular/peripheral-to-memory ve TX normal/memory-to-peripheral şartları korunur. UART ile DMA kanalının gerçek donanım yönlendirmesi CubeMX/proje sorumluluğundadır; her aileye uygunluk iddia edilmez.

Port; donanımın çalışıyor/sağlıklı/durmuş olması, TC/NDTR örnekleme, eski TX kaynaklarını temizleme ve HTIE atomik temizleme işlemlerini toplar. F407'de HTIE için mevcut bit-band güvenliği korunur; aktif DMA CR register'ı bütün olarak read-modify-write yapılmaz. Eski TX olayları temizlenirken yalnız seçilen TX DMA IRQ pending kaydı temizlenir; ortak UART IRQ korunur.

Genel HAL callback tanımları projeye ait USER CODE alanlarında bulunur ve kütüphanenin `uart_comm_on_*` kapılarına handle ile yönlendirilir. Kütüphane başka UART handle'larını işlemez. Error callback RX ve TX hata kayıtlarını birlikte yaptıktan sonra taskı uyandırır. UART IRQ çıkış kapısı HAL handler'dan sonra çağrılır; STARTING sırasında sahte hata üretmez.

`uart_comm_config.h` varsayılanları: RX halka 256, scratch 32, RX tur bütçesi 64; TX öğesi 64, kuyruk 8; task stack 512 word, native priority 25. Mevcut sample/abort/TX timeout ve sınırlı RX restart politikaları aynı kalır. Desteklenen ayar aralıkları derleme zamanında kontrol edilir; kuyruk/dizi boyutları birbirinden bağımsız sabitlerle ayrışmaz.

## Değişmeyen güvenilirlik koşulları

Mutlak üretici aritmetiği, gerçek TC ile wrap sayımı, çift örnekleme ile doğrulanmış kopya, RX tur bütçesi, PRIMASK'ın korunması, bounded abort/restart, TX FIFO admission epoch ve kabul edilmiş öğe başına tek sonuç korunur. Callback'ten yeniden send yapılabilir. Kuyruk doluysa beklenmez. İş/deadline yokken task notification ile süresiz uyur; periyodik polling eklenmez.

## Kabul ve doğrulama

1. UART paketinin yalnız HAL/FreeRTOS ve kendi header'larıyla, protokol kaynakları olmadan derlendiği ham byte örneği doğrulanır. Mevcut projede kaynak/include listeleri CubeIDE üzerinden yenilenir; üretilmiş makefile elle düzenlenmez.
2. RX 31, TX 23 ve RTOS 16 mevcut model testi adaptör üzerinden aynı davranışı doğrulamaya devam eder. Yeni testler ham byte teslimini, RESET sonrası eski/yeni mesaj birleşmemesini, eksik aday deadline'ını, geç servis/tick sarımını ve farklı handle/stream IRQ yönlendirmesini kapsar.
3. CubeIDE Debug/Release ve native test, CH340 test, üretim derlemeleri hatasız ve uyarısız olmalıdır. Üretimde test sembolleri ve ikinci callback seti bulunmamalıdır.
4. Mevcut CH340 COM18 bağlantısıyla veri/yük, timeout, CRC resync, kuyruk doluluğu, boşta uyuma, physical break toparlanması ve sıra kontrolleri tekrar çalıştırılır. Sonunda doğrulanan üretim firmware'i karta geri yüklenir.
5. Aynı UART kaynakları USART1 veya USART3 için hazırlanmış ikinci bir entegrasyon derlemesinde değiştirilmeden kullanılır. Bu ayrı build kontrolü mevcut kartın pin/.ioc ayarlarını değiştirmez. Başka UART üzerinden fiziksel çalışma, uygun bağlantı ile ayrıca doğrulanmadıkça donanımda kanıtlanmış diye raporlanmaz.

Servis+ISR CPU ölçümü ile bütün task runtime ölçümü ayrılır. Önceki planın tam task CPU hedefi bu refaktörle kendiliğinden kapanmaz. Pushlanan başlangıç commit'i geri dönüş noktasıdır; katmanlı kodun sonradan pushlanması bu ilk push ile yapılmış sayılmaz.

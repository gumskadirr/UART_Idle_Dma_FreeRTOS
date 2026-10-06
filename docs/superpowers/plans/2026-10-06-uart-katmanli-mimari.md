# Taşınabilir UART Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans for inline execution, or superpowers:subagent-driven-development if the user selects delegation. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** UART kaynaklarını protokolden ayırıp STM32F4 + HAL + FreeRTOS projelerine kopyalanabilir hale getirmek.

**Architecture:** Tek statik UART taskı byte teslimi, DMA, FIFO ve toparlanmayı yönetir. İsteğe bağlı `protocol_uart` adaptörü mevcut parser/frame handler'ını bağlar. STM32F4 register işlemleri port dosyasında, boyut/süre seçenekleri config header'ında tutulur.

**Tech Stack:** STM32F407VG, STM32F4 HAL 1.8.5, mevcut FreeRTOS 10.3.1, CubeIDE 1.19.0, mevcut PC GCC/HAL-RTOS modelleri ve CH340/pyserial araçları.

**Spec:** [Onaylanan tasarım](../specs/2026-10-06-uart-katmanli-mimari-design.md).

## Global Constraints

- İlk sürüm tek UART ve tek statik owner task kullanır.
- Harici kütüphane eklenmez; dinamik bellek, ek task/RX kuyruğu veya periyodik polling eklenmez.
- Mevcut STM32F407VG, USART2, PA2/PA3, DMA ve FreeRTOS ayarları korunur; `.ioc` değişmez.
- RX halka 256, scratch 32, RX tur bütçesi 64; TX öğesi 64, kuyruk 8; task stack 512 word, native priority 25.
- Eksik aday timeout'u 50 ms, üretici ilerlemesine göre; sample/abort/TX ve RX restart süreleri mevcut değerlerle korunur.
- PRIMASK korunur; HAL/RTOS API ve uygulama callback'leri kritik bölüm dışında kalır. Kabul edilmiş TX öğesine tek sonuç, FIFO admission epoch ve bounded recovery korunur.
- Commit yazarı ve kaydeden kişi `gumskadirr <kg083682@gmail.com>`; otomatik araç atfı eklenmez. Başlangıç projesi `15df55a` ile pushlandı. Yeni kodun push'u ayrı bir işlem olarak raporlanır.

## Review Focus

- RX reset/taşma sonrasında önceki oturumun yarım paketi yeni byte'larla birleşmemeli.
- Uzun süren handler, geç servis veya tick sarımı eksik aday timeout'unu uzatmamalı ve boşta tekrar uyanma üretmemeli.
- Callback sırasında yeni RX hatası gelirse aynı callback'in doğruladığı mesaj bu hatayı affetmemeli.
- Seçili olmayan UART callback'i hiçbir sayaç, notification veya donanım kaydını değiştirmemeli; TX temizliği doğru DMA IRQ'ya gitmeli.
- Protokol kaynakları olmadan ham byte derlemesi ve farklı UART/DMA entegrasyonu aynı çekirdeği kullanmalı.

## Task 1: Byte arayüzü ve protokol adaptörü

**Files:** `Core/Inc/uart_comm.h`, `Core/Inc/uart_comm_internal.h`, `Core/Src/uart_comm.c`; yeni `Core/Inc/protocol_uart.h`, `Core/Src/protocol_uart.c`; `Core/Src/main.c` USER CODE; `Tests/Src/tests.c`, `uart_comm_tests.c`, `uart_rtos_tests.c`; `tools/tests/test_uart_rx.c`, `test_uart_tx.c`, `test_uart_comm.c` ve `tools/test_uart_rx.py`, `test_uart_tx.py`, `test_uart_comm.py`.

**Interfaces:**

- `uart_comm_rx_event_t`: `UART_COMM_RX_DATA`, `UART_COMM_RX_TIMEOUT`, `UART_COMM_RX_RESET`.
- `uart_comm_rx_handler_t`: `uint32_t (*)(uart_comm_rx_event_t event, const uint8_t *data, uint16_t len, void *user)`; dönüş bitleri `UART_COMM_RX_PENDING=1U`, `UART_COMM_RX_VALIDATED=2U`. RESET sonrası çekirdek pending'i koşulsuz temizler; DATA/TIMEOUT sonrası yalnız bu iki bit kullanılır.
- `uart_comm_handlers_t`: `on_rx`, mevcut `on_tx_result`, `rx_user`, `user`. RX ve TX hedefleri ayrılır; designated initializer kullanılır. Init/send/recovery fonksiyon imzaları korunur; snapshot `rx_frame_timeouts` adı `rx_timeouts` olur.
- `protocol_uart_t`: `frame_parser_t parser`, `frame_handler_t on_frame`, `void *user`, callback'e ait doğrulama bayrağı. `void protocol_uart_init(protocol_uart_t *, frame_handler_t, void *)`; `uint32_t protocol_uart_on_rx(uart_comm_rx_event_t, const uint8_t *, uint16_t, void *)`.
- Test-only `rx_set_handler` genel RX handler'ını alır. `rx_get_parser` kaldırılır; test fixture'i sahip olduğu adaptörün `parser` alanını okur. Üretimde parser'a dış tasktan erişilmez.
- Test-only `void comm_test_frame_enter(void)` / `void comm_test_frame_exit(void)` adaptörün frame handler süresini mevcut DWT/IRQ çıkarımıyla ölçer; üretimde tanım/çağrı bulunmaz. PC modellerinde bu ölçüm kullanılmaz.

- [x] `raw_bytes`, `reset_discards_partial`, `timeout_progress_and_wrap`, `validated_with_new_error` regresyonlarını RX modeline ekle ve çalıştır. Beklenen ilk RED: yeni callback/adaptör sembolleri henüz yok. Assertion'lar: ham byte'lar bire bir teslim edilir ve wait `UINT32_MAX`; reset parser.len'i sıfırlar fakat frames_ok'u korur; yeni üretici ilerlemesi 50 ms deadline'ı yeniler, tick sarımı aşılır; callback içindeki error recovery'yi açık bırakır.
- [x] Arayüz ve adaptörü uygula. Parser/handler/user RX context'inden çıkartılır; context yalnız pending bilgisi ve üretici ilerleme zamanı tutar. DATA mevcut doğrulanmış scratch'ten gelir; RESET start/restart/overrun sırasında çağrılır. TIMEOUT mevcut sample/bütçe akışını korur; kalan aday için zaman yeniden kurulur. VALIDATED dönüşünden sonra kilit içinde health+fault kontrolü yapılır; callback sırasında gelmiş yeni hata affedilmez. `protocol.c/.h` algoritmaları değişmez.
- [x] Üretim/native/RTOS/CH340 fixture'larını adaptör context'lerine bağla; parser sorgularını fixture erişimine taşı. Task öncesi native testlerin adaptörü ayrıca initialize edilir. Mevcut test senaryolarının koşulları ve sayıları azaltılmaz. Test-only frame ölçüm kapıları adaptörün frame teslimini çevreler; byte callback maliyeti servis ölçümüne dahil olur.
- [x] Üç PC suite'i çalıştır: `python tools/test_uart_rx.py`, `python tools/test_uart_tx.py`, `python tools/test_uart_comm.py`. Mevcut 70 senaryo ve dört yeni RX senaryosu PASS olmalı; UART header/source dosyalarında protokol tipi/include/sabitine başvuru olmamalı.
- [x] Değişiklikleri `gumskadirr` kimliğiyle yerel commit olarak kaydet; kanıtları planda işaretle.

## Task 2: STM32F4 portu ve proje callback sahipliği

**Files:** yeni `Lib/Uart/uart_comm_port.c`, `uart_comm_port.h`; UART çekirdeği; `Core/Src/main.c`, `stm32f4xx_it.c` USER CODE; `tools/tests/hal_model/stm32f4xx_hal.h`, yeni model callback bağlantı dosyası `tools/tests/hal_model/uart_callbacks.c`; üç PC suite/runner ve yeni `tools/tests/test_uart_port.c`, `tools/test_uart_port.py`.

**Interfaces:**

- `bool uart_port_dma_irq(DMA_Stream_TypeDef *, IRQn_Type *)`, `bool uart_port_validate(const UART_HandleTypeDef *)`.
- `bool uart_port_rx_active/uart_port_rx_healthy/uart_port_rx_stopped/uart_port_tx_stopped(const UART_HandleTypeDef *)`.
- `uart_port_rx_sample_t { uint32_t tc_before, ndtr, tc_after; }`; `uart_port_rx_sample_t uart_port_rx_sample(const UART_HandleTypeDef *)`. Çağıran mevcut kilidi tutar; mutlak sayaç/session çekirdekte kalır.
- `void uart_port_rx_clear_tc/uart_port_rx_mask_sources/uart_port_rx_clear_errors/uart_port_tx_disable_half_irq/uart_port_tx_clear_sources/uart_port_tx_mask_sources(UART_HandleTypeDef *)`. Kaynak maskeleme ve TX flag temizleme mevcut kritik bölüm sınırlarını korur.
- Public yönlendirme kapıları: `uart_comm_on_rx_event(handle,size)`, `uart_comm_on_error(handle)`, `uart_comm_on_rx_abort_complete(handle)`, `uart_comm_on_tx_complete(handle)`, `uart_comm_on_tx_abort_complete(handle)`, `uart_comm_on_uart_irq_exit(handle)`; hepsi `void`.

- [x] `dma_irq_mapping` testi 16 DMA1/DMA2 stream için doğru IRQ'yu, bilinmeyen/null stream için reddi doğrulasın. `foreign_uart_events` RTOS modeli seçili olmayan ve null handle için notification/sayaç/deadline'ın değişmediğini doğrulasın. `selected_tx_irq_clear` farklı TX stream seçildiğinde yalnız onun pending IRQ'sunun temizlendiğini doğrulasın. Yeni kapılar/eşleme yokken RED'i gör.
- [x] Donanım register erişimlerini porta taşı; mevcut HAL start/abort çağrıları owner çekirdeğinde kalır. Sabit USART2/stream/channel şartlarını kaldır; parent/yön/mode/ayrı stream kontrolünü koru. DMA IRQ map'i gerçek STM32F4 macro'ları ve modelin 16 stream temsilcisiyle test edilir; eşleme için testte algoritma kopyası yazılmaz.
- [x] HAL callback tanımlarını `main.c` USER CODE0 alanına yönlendirme wrapper'ları olarak taşı. HAL UART handler sonrası çıkış hook'una `&huart2` ver. PC fixture wrapper'ları aynı public kapıları çağırır. Error hook iki yönü kaydetmeden notification vermez; eski `combined_error`, `suspended_notify`, `healthy_start_irq`, `ht_clear_completion_race` testlerini koru.
- [x] RX/TX/RTOS ve yeni port suite'ini çalıştır; tüm senaryolar PASS. IRQ/lock/handle kontrolü kod incelemesini yap ve yerel commit kaydet.

## Task 3: Kopyalanabilir paket, ayarlar ve bağımsız entegrasyon

**Files:** üç UART dosyasını `Core`'dan `Lib/Uart/`'a taşı; yeni `Lib/Uart/uart_comm_config.h`, `README.md`; `.cproject`; PC runner source/include listeleri; yeni `tools/tests/test_uart_portable.c`, `tools/test_uart_portable.py`; `UART_COMM_KULLANIM.md`, `Core/Inc/app_protocol.h` kullanım açıklamaları.

**Interfaces:** Config sabitleri `UART_RX_BUF_SIZE`, `UART_RX_SCRATCH_SIZE`, `UART_RX_SERVICE_BUDGET`, `UART_RX_TIMEOUT_MS`; `UART_TX_BUF_SIZE=64U`, `COMM_QUEUE_SIZE=8U`, `COMM_STACK_SIZE=512U`, `COMM_TASK_PRIORITY=25U`; mevcut retry/sample/abort/recovery süre isimleri ve değerleri korunur. Derleme zamanı override'lar `#ifndef` ile desteklenir. RX halka 2'nin kuvveti 2..32768 (uint32 sayaç sarımında DMA konumu korunur), scratch 1..halka, bütçe 1..65535, TX öğesi 1..65535; kuyruk/stack/süreler sıfır olamaz, süreler uint32 yarım aralığından küçük olmalı; RTOS priority `configMAX_PRIORITIES` altında olmalı.

- [x] `test_uart_portable.py` protokol kaynakları/include dizini olmadan UART çekirdeğini ve portu gerçek HAL/RTOS header'larıyla ARM için derlesin; ham RX/TX uygulama örneğini USART1/DMA2 Stream2 RX Channel4, DMA2 Stream7 TX Channel4 ile bağlasın. Alternatif proje context'i ayrı/ignored `.build/portable/` altında olsun; mevcut UART kaynakları kopyalanmadan/doğrudan aynı dosyalardan derlensin. Eksik paket/config/arayüz nedeniyle ilk RED'i kaydet.
- [x] Dosyaları taşı, config ve boyut kontrollerini uygula; public header config'i include etsin, internal header protokolü include etmesin. `.cproject` Debug/Release sourceEntry `Lib/Uart` ve include `../Lib/Uart` ekle. Eski UART dosyalarını bırakma. CubeIDE makefile'ları headless derleme ile yeniden üretsin.
- [x] Bağımsız örneği derle; UART paketinin unresolved symbol listesinde `frame_`, `crc16_`, `protocol_`, `app_protocol_` olmayacağını doğrula. Farklı UART örneği compile/link entegrasyon kanıtıdır; pin ve fiziksel test kanıtı olarak sunulmaz. Sıfır/geçersiz boyut ve desteklenmeyen priority ayarlarını derleyicinin reddettiğini denetle; varsayılan konfigürasyon hatasız derlensin.
- [x] README'de kopyalama, CubeIDE include/source ekleme, RX circular/TX normal DMA, RTOS güvenli IRQ priority, HAL callback/IRQ forwarding, init/send/raw RX ve isteğe bağlı protokol adaptör örneğini yaz. Yeni UART taskı oluşturulmayacağını ve handler'ların beklemeyeceğini belirt. Kendi source paketimiz dış bağımlılık değildir; yeni kütüphane indirme/kurma işlemi yapılmaz.
- [x] Tüm PC suite'leri ve bağımsız entegrasyon derlemesini çalıştır; yerel commit kaydet.

## Task 4: CubeIDE, kart ve son kabul

**Files:** `UART_COMM_KULLANIM.md`, `UART_RTOS_UYGULAMA_PLANI.md`, bu planın kanıt/kontrol kutuları; build/test çıktıları yalnız ignored `.build/` altında.

- [x] CubeIDE headless `-cleanBuild UART_IDLE_DMAv2/Debug` ve `-cleanBuild UART_IDLE_DMAv2/Release` çalıştır; loglarda iki tam derleme 0 error/0 warning olmalı. IDE env-hash gürültüsünü ayıkla. `.ioc`, pin/peripheral/generated USER CODE dışı değişikliklerini diff ile denetle.
- [x] `tools/build.sh test`, `serial`, üretim kiplerini sırayla derle; her ELF'i `.build/board-tests/layered-native.elf`, `layered-ch340.elf`, `layered-production.elf` olarak ayrı kaydet. Kipler aynı çıktı klasörünü kullandığından paralel çalıştırma. Her kipte 0 error/0 warning; üretimde test kancaları ve HAL callback çiftleri yok.
- [x] `python tools/run_board_tests.py --stop loopback_testi_kosur --elf .build/board-tests/layered-native.elf`: scheduler öncesi birim29/29 kontrolü. PA2–PA3 jumper çıkarılmış olduğundan native loopback kabulünü koşulmuş diye raporlama.
- [x] CH340 firmware'i `--serial --elf .../layered-ch340.elf` ile yükle. `python tools/test_uart_serial.py --port COM18 --output .build/board-tests/layered-ch340-results.json`, ardından `--metrics-only` ve `--sink-order-probe` kipleriyle ayrı JSON'lar üret. 11 veri/yük +1 metrics +1 sıra, toplam13 kontrol PASS; idle10sn UART servis turu0 olmalı. CPU servis+ISR ölçümü olarak etiketlenir; tam task runtime hedefi ayrıca açık kalır.
- [x] IRQ/owner sınırları, error+done önceliği, callback içi enqueue, timeout/reset, foreign handle ve DMA temizliği için bağımsız final review iste. Önemli bulgu varsa tetikleyici regresyon ile RED→GREEN; sadece etkilenen testleri ve gerekli kabulü tekrar koş.
- [x] `python tools/run_board_tests.py --production --elf .build/board-tests/layered-production.elf` ile üretimi geri yükle; initialized=1, rx_ready=1, tx_accepting=1, TX_IDLE kontrolü. ELF SHA256, gerçek test sayıları ve açık kalan sınırları mevcut belgelere kaydet. Sonuç/kısıtları Türkçe bildir; bu aşamadaki kodun henüz remote'a pushlanmadığını açıkça belirt.

## Uygulama yöntemi ve başlangıç durumu

Öneri: Aynı oturumda ana ajan uygulasın; tasklar birbirinin callback/context/port arayüzüne bağlıdır. Sonunda tek bağımsız inceleme yapılır. Delegasyon tercih edilirse kullanıcı bunu plan incelemesinde seçebilir.

Tasarım 6 Ekim 2026'da kullanıcı tarafından `devam et` yanıtıyla onaylandı. Kullanıcı `planı uygula` yanıtıyla uygulamayı onayladı. Task1–Task4 tamamlandı; model, protokolsüz entegrasyon, derleme ve CH340 kart kabulü kanıtları aşağıda kayıtlıdır. Başlangıç push'u `15df55a`, yerel tasarım commit'i `226eaf6`.


## Son kabul — 6 Ekim 2026

- RX35/35, TX23/23, RTOS17/17, port3/3: toplam78 model senaryosu PASS. Protokolsüz USART1/DMA2 ARM compile/link ve konfigürasyon kontrolü2/2 PASS; ikinci kontrolde10 hatalı seçenek derlemede reddedildi. Mevcut70 senaryo korunmuştur.
- CubeIDE Debug/Release tam derlemeleri ve ayrı native/CH340/üretim derlemeleri0 hata/0 uyarı. `.ioc`, pin/DMA/NVIC, FreeRTOSConfig ve generated USER CODE dışı HAL ayarları değiştirilmedi. Üretim ELF'inde test sembolü yok; tek UART taskı ve proje callback seti var, DMA tamponu normal SRAM'de.
- Kartta native protocol birim29/29 geçti; loopback/diğer native kabul testleri jumper çıkarıldığı için tekrar koşulmadı. CH340:11 veri/yük +1 metrics +1 sıra,13/13 PASS. Physical BREAK RX restart delta1; TX kuyruğu8 kabul/1 görünür drop; DMA/SEQ sarımı520 çerçeve.
- Idle10sn/0 UART servis turu.100Hz çift yön1000TX/1000RX,10087ms, servis+ISR CPU%1,521. Sürekli64bayt1000TX/1000RX,5697ms,%7,908. Stack boş379/512word; kritik1228cycle=7,31µs, IRQ1852cycle=11,02µs, RX latency7659cycle=45,59µs; frame handler2850cycle, result handler153cycle, max service45287cycle. Ölçüm bütün task runtime'ı değildir.
- Bağımsız `layered_uart_review`: yeni Critical/Important regresyon yok. İnceleme read-only kaynak/log kontrolüne dayanıyor; reviewer'ın PC binary tekrar çalıştırması Windows DLL relocation623 nedeniyle başlamadı. Ana ajanın fresh model/kart koşuları yukarıdadır.
- Minor, ertelendi: `uart_comm.h` ve internal header send açıklamaları varsayılan1..64 byte diyor; gerçek ayarlanabilir sınır `UART_TX_BUF_SIZE`. README bunu doğru anlatıyor, çalışmayı etkilemiyor. Başka kod değişikliği gerektiren bulgu yok.

### Kararlar ve kabul sınırları

- Mevcut `uart-birlesik` checkout kullanıldı; CubeIDE ve kart araçlarının bu projeye bağlı yolu korundu. Yeni kod yerel checkout/commit'lerdedir; başlangıç push'u `15df55a` geri dönüş noktasıdır.
- RX halka2'nin kuvveti2..32768: uint32 mutlak sayaç sarımında `% ring` hizası korunur. Bedeli: keyfi halka boyutu desteklenmez.
- Init8N1, byte hizalı ve memory increment açık DMA ister; daha geniş DMA elemanı byte tamponunu aşmamalı. Bedeli:9bit/parity ve farklı DMA hizalaması bu sürümde reddedilir.
- Timeout son **gözlenen** üretici ilerlemesine bağlıdır. Scratch teslimi/uzun handler sonraki deadline'ı ilerletmez; ilk ilerlemenin task tarafından gözlenmesine kadarki gecikme için yeni bir donanım timestamp'i eklenmedi. Mevcut baseline davranışı korunur; gerçek wire zamanından kesin50ms sınırı iddia edilmez.
- Tam UART task CPU hedefi ve global iki-üreticili enqueue trace kabulü önceki planın açık işleri olarak kalır. Bu refaktörün PASS sonuçları bu açıkları kapatmaz.
- USART1/DMA2 fixture'i aynı çekirdeğin compile/link taşınabilirliğini doğrular; fiziksel USART1 kabulü değildir. Kartta bu çalışmada USART2/CH340 doğrulandı.
- Kart kabulü ve son üretim geri yükleme ana ajan tarafından yapıldı; `PRODUCTION initialized=1 rx_ready=1 tx_accepting=1 tx_state=0 tasks=4` doğrulandı.

### Firmware ve kanıtlar

- Native `layered-native.elf`: `12317b9824804f802f37f7c056ed6add5181cf6d8f9a97c6b54a8ecbbe2bf99c`.
- CH340 `layered-ch340.elf`: `591a0b587df198780babb78961da61c1f7c62d926ade0c2e744c58dfa1f1a463`.
- **Kartta bırakılan üretim** `layered-production.elf`: `efc310c178a976222dc476013412bd533c1a84eb8c276778e07da364277d0acb`; text47236/data96/bss25104. Önceki readability üretimine göre text+996bayt, bss+16bayt; stack ve kuyruk boyutları aynı.
- `.build/layered-final-{rx,tx,rtos,port,portable}.log`, `layered-final-ide-build.log`, `layered-{native,serial,production}-build.log`, `layered-production-symbols.log`.
- `.build/portable/{build.log,symbols.log,uart-undefined.log,uart_usart1.elf}`; Core/Inc veya protokol kaynakları include/source listesinde yok, yalnız HAL/FreeRTOS konfigürasyon header'ları fixture alanına kopyalanır.
- `.build/board-tests/layered-{native,serial,production}-smoke.log`, `layered-ch340-{results,metrics,order}.json`.

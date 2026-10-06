# Taşınabilir UART Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans for inline execution, or superpowers:subagent-driven-development if the user selects delegation. Steps use checkbox (`- [ ]`) syntax for tracking.

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

- [ ] `raw_bytes`, `reset_discards_partial`, `timeout_progress_and_wrap`, `validated_with_new_error` regresyonlarını RX modeline ekle ve çalıştır. Beklenen ilk RED: yeni callback/adaptör sembolleri henüz yok. Assertion'lar: ham byte'lar bire bir teslim edilir ve wait `UINT32_MAX`; reset parser.len'i sıfırlar fakat frames_ok'u korur; yeni üretici ilerlemesi 50 ms deadline'ı yeniler, tick sarımı aşılır; callback içindeki error recovery'yi açık bırakır.
- [ ] Arayüz ve adaptörü uygula. Parser/handler/user RX context'inden çıkartılır; context yalnız pending bilgisi ve üretici ilerleme zamanı tutar. DATA mevcut doğrulanmış scratch'ten gelir; RESET start/restart/overrun sırasında çağrılır. TIMEOUT mevcut sample/bütçe akışını korur; kalan aday için zaman yeniden kurulur. VALIDATED dönüşünden sonra kilit içinde health+fault kontrolü yapılır; callback sırasında gelmiş yeni hata affedilmez. `protocol.c/.h` algoritmaları değişmez.
- [ ] Üretim/native/RTOS/CH340 fixture'larını adaptör context'lerine bağla; parser sorgularını fixture erişimine taşı. Task öncesi native testlerin adaptörü ayrıca initialize edilir. Mevcut test senaryolarının koşulları ve sayıları azaltılmaz. Test-only frame ölçüm kapıları adaptörün frame teslimini çevreler; byte callback maliyeti servis ölçümüne dahil olur.
- [ ] Üç PC suite'i çalıştır: `python tools/test_uart_rx.py`, `python tools/test_uart_tx.py`, `python tools/test_uart_comm.py`. Mevcut 70 senaryo ve dört yeni RX senaryosu PASS olmalı; UART header/source dosyalarında protokol tipi/include/sabitine başvuru olmamalı.
- [ ] Değişiklikleri `gumskadirr` kimliğiyle yerel commit olarak kaydet; kanıtları planda işaretle.

## Task 2: STM32F4 portu ve proje callback sahipliği

**Files:** yeni `Lib/Uart/uart_comm_port.c`, `uart_comm_port.h`; UART çekirdeği; `Core/Src/main.c`, `stm32f4xx_it.c` USER CODE; `tools/tests/hal_model/stm32f4xx_hal.h`, yeni model callback bağlantı dosyası `tools/tests/hal_model/uart_callbacks.c`; üç PC suite/runner ve yeni `tools/tests/test_uart_port.c`, `tools/test_uart_port.py`.

**Interfaces:**

- `bool uart_port_dma_irq(DMA_Stream_TypeDef *, IRQn_Type *)`, `bool uart_port_validate(const UART_HandleTypeDef *)`.
- `bool uart_port_rx_active/uart_port_rx_healthy/uart_port_rx_stopped/uart_port_tx_stopped(const UART_HandleTypeDef *)`.
- `uart_port_rx_sample_t { uint32_t tc_before, ndtr, tc_after; }`; `uart_port_rx_sample_t uart_port_rx_sample(const UART_HandleTypeDef *)`. Çağıran mevcut kilidi tutar; mutlak sayaç/session çekirdekte kalır.
- `void uart_port_rx_clear_tc/uart_port_rx_mask_sources/uart_port_rx_clear_errors/uart_port_tx_disable_half_irq/uart_port_tx_clear_sources/uart_port_tx_mask_sources(UART_HandleTypeDef *)`. Kaynak maskeleme ve TX flag temizleme mevcut kritik bölüm sınırlarını korur.
- Public yönlendirme kapıları: `uart_comm_on_rx_event(handle,size)`, `uart_comm_on_error(handle)`, `uart_comm_on_rx_abort_complete(handle)`, `uart_comm_on_tx_complete(handle)`, `uart_comm_on_tx_abort_complete(handle)`, `uart_comm_on_uart_irq_exit(handle)`; hepsi `void`.

- [ ] `dma_irq_mapping` testi 16 DMA1/DMA2 stream için doğru IRQ'yu, bilinmeyen/null stream için reddi doğrulasın. `foreign_uart_events` RTOS modeli seçili olmayan ve null handle için notification/sayaç/deadline'ın değişmediğini doğrulasın. `selected_tx_irq_clear` farklı TX stream seçildiğinde yalnız onun pending IRQ'sunun temizlendiğini doğrulasın. Yeni kapılar/eşleme yokken RED'i gör.
- [ ] Donanım register erişimlerini porta taşı; mevcut HAL start/abort çağrıları owner çekirdeğinde kalır. Sabit USART2/stream/channel şartlarını kaldır; parent/yön/mode/ayrı stream kontrolünü koru. DMA IRQ map'i gerçek STM32F4 macro'ları ve modelin 16 stream temsilcisiyle test edilir; eşleme için testte algoritma kopyası yazılmaz.
- [ ] HAL callback tanımlarını `main.c` USER CODE0 alanına yönlendirme wrapper'ları olarak taşı. HAL UART handler sonrası çıkış hook'una `&huart2` ver. PC fixture wrapper'ları aynı public kapıları çağırır. Error hook iki yönü kaydetmeden notification vermez; eski `combined_error`, `suspended_notify`, `healthy_start_irq`, `ht_clear_completion_race` testlerini koru.
- [ ] RX/TX/RTOS ve yeni port suite'ini çalıştır; tüm senaryolar PASS. IRQ/lock/handle kontrolü kod incelemesini yap ve yerel commit kaydet.

## Task 3: Kopyalanabilir paket, ayarlar ve bağımsız entegrasyon

**Files:** üç UART dosyasını `Core`'dan `Lib/Uart/`'a taşı; yeni `Lib/Uart/uart_comm_config.h`, `README.md`; `.cproject`; PC runner source/include listeleri; yeni `tools/tests/test_uart_portable.c`, `tools/test_uart_portable.py`; `UART_COMM_KULLANIM.md`, `Core/Inc/app_protocol.h` kullanım açıklamaları.

**Interfaces:** Config sabitleri `UART_RX_BUF_SIZE`, `UART_RX_SCRATCH_SIZE`, `UART_RX_SERVICE_BUDGET`, `UART_RX_TIMEOUT_MS`; `UART_TX_BUF_SIZE=64U`, `COMM_QUEUE_SIZE=8U`, `COMM_STACK_SIZE=512U`, `COMM_TASK_PRIORITY=25U`; mevcut retry/sample/abort/recovery süre isimleri ve değerleri korunur. Derleme zamanı override'lar `#ifndef` ile desteklenir. RX halka 2..65535, scratch 1..halka, bütçe 1..65535, TX öğesi 1..65535; kuyruk/stack/süreler sıfır olamaz, süreler uint32 yarım aralığından küçük olmalı; RTOS priority `configMAX_PRIORITIES` altında olmalı.

- [ ] `test_uart_portable.py` protokol kaynakları/include dizini olmadan UART çekirdeğini ve portu gerçek HAL/RTOS header'larıyla ARM için derlesin; ham RX/TX uygulama örneğini USART1/DMA2 Stream2 RX Channel4, DMA2 Stream7 TX Channel4 ile bağlasın. Alternatif proje context'i ayrı/ignored `.build/portable/` altında olsun; mevcut UART kaynakları kopyalanmadan/doğrudan aynı dosyalardan derlensin. Eksik paket/config/arayüz nedeniyle ilk RED'i kaydet.
- [ ] Dosyaları taşı, config ve boyut kontrollerini uygula; public header config'i include etsin, internal header protokolü include etmesin. `.cproject` Debug/Release sourceEntry `Lib/Uart` ve include `../Lib/Uart` ekle. Eski UART dosyalarını bırakma. CubeIDE makefile'ları headless derleme ile yeniden üretsin.
- [ ] Bağımsız örneği derle; UART paketinin unresolved symbol listesinde `frame_`, `crc16_`, `protocol_`, `app_protocol_` olmayacağını doğrula. Farklı UART örneği compile/link entegrasyon kanıtıdır; pin ve fiziksel test kanıtı olarak sunulmaz. Sıfır/geçersiz boyut ve desteklenmeyen priority ayarlarını derleyicinin reddettiğini denetle; varsayılan konfigürasyon hatasız derlensin.
- [ ] README'de kopyalama, CubeIDE include/source ekleme, RX circular/TX normal DMA, RTOS güvenli IRQ priority, HAL callback/IRQ forwarding, init/send/raw RX ve isteğe bağlı protokol adaptör örneğini yaz. Yeni UART taskı oluşturulmayacağını ve handler'ların beklemeyeceğini belirt. Kendi source paketimiz dış bağımlılık değildir; yeni kütüphane indirme/kurma işlemi yapılmaz.
- [ ] Tüm PC suite'leri ve bağımsız entegrasyon derlemesini çalıştır; yerel commit kaydet.

## Task 4: CubeIDE, kart ve son kabul

**Files:** `UART_COMM_KULLANIM.md`, `UART_RTOS_UYGULAMA_PLANI.md`, bu planın kanıt/kontrol kutuları; build/test çıktıları yalnız ignored `.build/` altında.

- [ ] CubeIDE headless `-cleanBuild UART_IDLE_DMAv2/Debug` ve `-cleanBuild UART_IDLE_DMAv2/Release` çalıştır; loglarda iki tam derleme 0 error/0 warning olmalı. IDE env-hash gürültüsünü ayıkla. `.ioc`, pin/peripheral/generated USER CODE dışı değişikliklerini diff ile denetle.
- [ ] `tools/build.sh test`, `serial`, üretim kiplerini sırayla derle; her ELF'i `.build/board-tests/layered-native.elf`, `layered-ch340.elf`, `layered-production.elf` olarak ayrı kaydet. Kipler aynı çıktı klasörünü kullandığından paralel çalıştırma. Her kipte 0 error/0 warning; üretimde test kancaları ve HAL callback çiftleri yok.
- [ ] `python tools/run_board_tests.py --stop loopback_testi_kosur --elf .build/board-tests/layered-native.elf`: scheduler öncesi birim29/29 kontrolü. PA2–PA3 jumper çıkarılmış olduğundan native loopback kabulünü koşulmuş diye raporlama.
- [ ] CH340 firmware'i `--serial --elf .../layered-ch340.elf` ile yükle. `python tools/test_uart_serial.py --port COM18 --output .build/board-tests/layered-ch340-results.json`, ardından `--metrics-only` ve `--sink-order-probe` kipleriyle ayrı JSON'lar üret. 11 veri/yük +1 metrics +1 sıra, toplam13 kontrol PASS; idle10sn UART servis turu0 olmalı. CPU servis+ISR ölçümü olarak etiketlenir; tam task runtime hedefi ayrıca açık kalır.
- [ ] IRQ/owner sınırları, error+done önceliği, callback içi enqueue, timeout/reset, foreign handle ve DMA temizliği için bağımsız final review iste. Önemli bulgu varsa tetikleyici regresyon ile RED→GREEN; sadece etkilenen testleri ve gerekli kabulü tekrar koş.
- [ ] `python tools/run_board_tests.py --production --elf .build/board-tests/layered-production.elf` ile üretimi geri yükle; initialized=1, rx_ready=1, tx_accepting=1, TX_IDLE kontrolü. ELF SHA256, gerçek test sayıları ve açık kalan sınırları mevcut belgelere kaydet. Sonuç/kısıtları Türkçe bildir; bu aşamadaki kodun henüz remote'a pushlanmadığını açıkça belirt.

## Uygulama yöntemi ve başlangıç durumu

Öneri: Aynı oturumda ana ajan uygulasın; tasklar birbirinin callback/context/port arayüzüne bağlıdır. Sonunda tek bağımsız inceleme yapılır. Delegasyon tercih edilirse kullanıcı bunu plan incelemesinde seçebilir.

Tasarım 6 Ekim 2026'da kullanıcı tarafından `devam et` yanıtıyla onaylandı. Bu plan uygulama öncesi inceleme için hazırdır; henüz ürün kodu değiştirilmedi. Başlangıç push'u `15df55a`, yerel tasarım commit'i `226eaf6`.

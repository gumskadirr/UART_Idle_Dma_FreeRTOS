# UART birleşme ve FreeRTOS uygulama planı

Amaç: Doğrulanmış RX/TX çekirdeklerini tek uart_comm.c/.h modülüne taşıyıp uygulamaya kopyalı send, callback ve snapshot arayüzü sağlamak. Boş hatta UART taskı notification ile süresiz uyur.

Şartname: UART_BIRLESIK_MIMARI_VE_UYGULAMA_YOL_HARITASI.md, bölümler 1–7; işler F0–F3, M1–M2. Kullanıcının 2026-10-05 yeni isteği önceki TX aşamasında durma sınırını kaldırır.

Mevcut CubeMX ayarları: STM32F407VG, 168 MHz, USART2 PA2/PA3 115200 8N1; RX DMA1/Stream5 circular 256, TX DMA1/Stream6 normal; üç IRQ priority 5, GROUP4; FreeRTOS 10.3.1 ARM_CM4F/CMSIS-v2, 1 ms tick, HAL TIM6 timebase. Bu ayarlar ve kullanıcı tarafından eklenmiş dosyalar korunur. Modül native FreeRTOS API kullanır, kendi statik task/kuyruğunu oluşturur.

## Sıra ve kanıt

- [x] F0: Güncel CubeIDE kaynak listesini üret; test/üretim tam derleme, tek SVC/PendSV/SysTick, uygun IRQ ve zaman tabanı. Native UART taskı 512 stack elemanı; CMSIS default task priority 24 olduğundan UART priority 25. Default taskın USER CODE'u süresiz suspend olur.
- [x] M1 ilk taşıma: Kullanıcının birleşme isteği doğrultusunda saf kaynak taşımasını önce yap; eski R/T testlerini yeni test-only kapılara geçir ve aynı sonuçları doğrula. Üretimde eski RX/TX API/dosyası kalmasın. Yeni davranış F aşamalarında ayrı doğrulanır.
- [x] F1: Tek owner; callback olay kaydından sonra gerçek ISR/thread ayrımlı notification; RX yalnız task başlayınca açılsın. Bekleme girişinde notification silinmesin; aktif deadline yoksa portMAX_DELAY. Kayıp abort callback için yalnız abort sırasında 1 tick sağlık kontrolü.
- [x] F2: 8 statik kopyalı FIFO + aktif öğe; tag/epoch, FAULT gate, bir sonuç/öğe. Kuyruk API ve notify PRIMASK dışında; gecikmiş eski epoch işi CANCELLED_FAULT. Sonuç callback'i öncesi aktif sahipliği kapat; callback yeniden enqueue yapabilir.
- [x] F3: Korunan snapshot, uint32 sayaçlar; tek yan etkisiz response örneği; 10 sn boşta uyanmama, partial timeout, 100 Hz ve sürekli loopback trafik; UART CPU/stack/IRQ gecikmesini gerçek kartta ölç.
- [x] M2: R/T/F PC model ve kart regresyonu; üretim kancaları kapalı ELF, buffer SRAM, callback tekilliği, generated USER CODE sınırları; kullanım örneği ve dokümantasyon.
- [x] Tek bağımsız final review; önemli bulgular testle RED→GREEN, ardından tüm regresyon.

## Kararlar ve riskler

- Ruling: M1 saf taşıması F1'den önce — kullanıcı önce dosyaların birleşmesini istedi; mevcut çekirdeği taşımak ile yeni RTOS davranışını ayrı doğrulayacağız — yanlışsa test-only kapı uyarlaması taşıma hatasını gizleyebilir, R/T modeli ve kart regresyonu aynı sonuçları arar.
- FreeRTOS zaten kullanıcı tarafından CubeMX ile eklenmiştir; başka kütüphane eklenmeyecek. .ioc elle değiştirilmeyecek. TIM6 kullanıcı seçimi korunur.
- Model scheduler/IRQ donanımı kanıtı sayılmaz. Kart test sürücüsü owner servislerini dış tasktan çağırmaz. Başarılı queue kabulü uzak cihaz onayı değildir; failed öğe otomatik yeniden gönderilmez.
- T3'te ilk bayttan önceki DMA TE/TC=0 güvenli FAULT politikası korunur. Public recovery yalnız fiziksel duruş sağlanınca kabul kapısını açabilir.

İlerleme/kanıt bu dosyaya işlenecek; eski UART_UYGULAMA_DURUMU.md önceki T1–T4 koşularının tarihsel kaydıdır.

## Uygulama ve inceleme kaydı

- F0: CubeIDE kaynak listesi güncellendi; HAL 1.8.5, FreeRTOS 10.3.1, TIM6/SysTick sahipliği ve IRQ priority5 doğrulandı.
- M1 saf taşıma: PC RX30/30, TX23/23; kart49/49 + birim29/29 + loopback15/15. Eski dosyalar kaldırıldı; tek HAL handle/context ve callback merkezi.
- F1–F3: Native kart kabulü 71/71 (49 eski +22 RTOS/public API kontrolü), birim29/29/loopback15/15; bu koşu final inceleme düzeltmelerinden önceki kanıttır. Son firmware koşusu aşağıda ayrıca kaydedilir.
- PC final: RX31/31 (uygulama snapshot dahil), TX23/23, RTOS16/16.
- Kritik bölüm hedefi ilk koşuda 11,3 µs ile düştü; ELF adresi snapshot publish olarak çözüldü. Owner alanları koruma dışında hazırlandı; korunan sayaç kopyası/yayın bırakıldı. Sonraki kart koşusunda ~5,8 µs ve PASS.
- Profil ölçümünde IRQ sayacı ile cycle sayacının ayrı okunması uint32 çıkarma taşmasına yol açtı. İki değer atomik çift olarak örneklendi; aynı native handler budget kontrolü RED→GREEN. Ölçüm kodu üretime girmez.
- Uygulama X/Y/sequence verisi için korunan app_protocol_get_snapshot eklendi; model app_snapshot önce eksik sembolle RED, sonra31/31 GREEN. Uygulama writer/getter PRIMASK koruması incelemede doğrulandı; kendi kritik bölüm profili de son test firmware'ine eklendi.
- Tek bağımsız inceleme: uart_rtos_final_review; Critical yok, iki Important bulgu.
- Final: fixed scheduler suspended iken kaybolan ISR notification — suspended_notify RED→GREEN; kernel pending-ready desteği kullanılıyor, yalnız NOT_STARTED engelleniyor.
- Final: fixed FAULT kuyruğunun son iptalinden sonra ertelenmiş recovery ile süresiz uyku — deferred_recovery_one/eight RED→GREEN; pending recovery yeni tur gerektiriyor. RTOS suite16/16.
- İnceleme kanıt notları: eski üretim logu kullanılmadı, güncel üretim firmware'i tekrar derlendi; UART ve uygulama kritik bölüm profilleri ayrı tutuldu.
- Ruling: F1/F2 geçici ayrı-modül geçiş kapıları kurulmadan birleşik owner katmanında birlikte uygulandı — saf taşıma önce kanıtlandı ve geçici API tekrarını önler — yanlışsa adımlar arası hata ayrımı zayıflar; ayrı model senaryoları ve native regresyon bu riski sınar.
- Final: Ruling: main.c örnek sonuç callback'i owner'ın debugger amaçlı globals alanlarına yazar; uygulama taskı okuyucusu yok — şu an yarış yok; gerçek task paylaşımı için değer kopyası/uygulama kuyruğu gereği kullanım belgesinde açıklanır — yanlış kullanımda tutarsız okuma olabilir.

## Son kabul — 6 Ekim 2026

**Son firmware geçti:** kart kabulü **71/71**, birim **29/29**, loopback **15/15**; FAIL/NOT_RUN/SKIP=0. İnceleme düzeltmeleri ve uygulama snapshot koruması bu firmware'de mevcut.

- Test ELF: `.build/board-tests/rtos-final-test.elf`; SHA256 `46a37312173915c9d78efead1cde3701ed1e3a8b6d1dd1e7ae761e8e069ab2d0`.
- Aynı ELF'in sonuç kaydı: `.build/board-tests/rtos-final-accepted.log`.
- Son PC regresyonu: `python tools/test_uart_rx.py` **31/31**; `python tools/test_uart_tx.py` **23/23**; `python tools/test_uart_comm.py` **16/16**.
- Güncel tam üretim derlemesi: **0 hata / 0 uyarı**, UART_COMM_TEST geçen derleme satırı=0. Text46192, data96, bss25088 bayt. `.build/production-build.log`, `.build/production-symbols.log`.
- CubeIDE Debug son derleme: **0 hata / 0 uyarı**, `.build/final-ide-build.log`.
- Üretim ELF: `.build/board-tests/final-production.elf`; SHA256 `d26496142dca0182616dd9137af790901a666905f61b563dd7108cc5b585f1bd`.
- Üretim kart başlangıcı da doğrulandı: initialized=1, rx_ready=1, tx_accepting=1, TX_IDLE, toplam4 task (UART, default, Idle, timer). `.build/board-tests/final-production-smoke.log`. Karta sonunda üretim firmware'i bırakıldı.
- Üretim sembollerinde tek UartCommTask; SVC/PendSV/SysTick ve her aktif HAL callback tek tanım. RX DMA buffer `0x20000f8c`/256 bayt; context/TX buffer `0x20000d4c`; owner stack `0x2000029c`/2048 bayt: SRAM, CCM dışında. Test/enjeksiyon/profil sembolü yok.

### Aynı test firmware'inin performansı

Kart STM32F4DISCOVERY/STM32F407VG, HCLK168MHz, HAL1.8.5, FreeRTOS10.3.1 ARM_CM4F, 1ms tick; GCC13.3.1 **-O0**, USART2/115200/8N1, PA2–PA3 loopback. Ölçüm DWT CYCCNT kullanır; taskta beklenen süre CPU sayılmaz. UART servis süresi ve UART/DMA ISR süreleri toplanır. Kapsam düzeltmesi: servis dışındaki bekleme hesabı/notification maliyeti ölçülmez; değer bütün UART taskı için üst sınır değildir. Toplam sistem CPU/power ölçümü değildir.

| Ölçüm | Sonuç | Kabul |
|---|---:|---|
| 10sn boş hat | 0 UART servis turu, 0 UART/DMA cycle, süreli uyanma yok | PASS |
| 10sn, her yönde100Hz×13 bayt | 1000 TX/1000 RX; yaklaşık **%1,6 UART CPU** | ≤%2 PASS |
| Sürekli64 bayt loopback | 187 çerçeve; kayıp/overrun artışı0; yaklaşık **%7,4 UART CPU** | PASS |
| UART kritik bölüm maksimum | 972 cycle ≈ **5,79µs** | <10µs PASS |
| Uygulama snapshot kritik bölüm maksimum | 198 cycle ≈ **1,18µs** | <10µs PASS |
| UART/DMA ISR maksimum | 1779 cycle ≈ **10,59µs** | PASS |
| RX bildiriminden hizmete maksimum | 31119 cycle ≈ **185µs** | <5ms PASS |
| UART servis turu maksimum | 42158 cycle ≈ **251µs** | Kayıtlı |
| Frame/result handler maksimum | ≈27,67µs /4,88µs | <100µs PASS |
| UART stack kullanılmamış pay | 332/512 word =1328 bayt, **%64,8** | ≥%25 PASS |
| Kuyruk gözlenen yüksek su seviyesi | 8 bekleyen +1 aktif sınırı | PASS |

CPU yüzdeleri runner'ın permille değerlerinden gelir (16 ve74); daha hassas yüzde iddiası yok. Hata enjeksiyonları son profiling/stack örneğine dahil. Gerçek CCM DMA TE ve önceki çekirdek FE kontrolleri kayıt defterinde PHYSICAL; kontrollü hata kapıları test-only. RTOS'taki FE deneyinin fiziksel SBK yöntemi test kaynağında açıktır. HAL/register modelleri ve scheduler modeli donanım kanıtı yerine kullanılmadı.

### Son inceleme kararları

İki Important kusur RED→GREEN testleriyle düzeltildi ve bütün PC/kart regresyonu geçti. Üretim logunun eskiliği ve uygulama kritik bölüm ölçüm kapsamı notları son derleme/ayrı profil ile kapandı; ertelenmiş bulgu yok.

Kararlar: (1) Kullanıcının istediği sırayla saf dosya birleşmesi task entegrasyonundan önce yapıldı; taşıma hatası riski R/T regresyonuyla sınandı. (2) F1/F2 geçici wrapper'ları kurulmadan ortak owner'da birlikte uygulandı; adımlar arası hata ayrımı riski ayrı senaryolarla sınandı. (3) Main sonuç globals'ı debugger örneği olarak korundu; uygulama taskı okuyucusu yok. İleride paylaşım yanlış yapılırsa tutarsız okuma riski vardır; uygulama kuyruğu/değer kopyası gerekir.

Kullanıcının CubeMX/FreeRTOS kurulumuna yeni kütüphane eklenmedi ve `.ioc` elle değiştirilmedi. Kaynak/IRQ/main eklemeleri USER CODE sınırlarında kaldı. Tarihsel `UART_UYGULAMA_DURUMU.md` korunmuştur. Commit/push yapılmadı; çalışma ağacındaki kullanıcı değişiklikleri korunur.

## CH340 ile bağımsız RX/TX kabulü — 6 Ekim 2026

Kullanıcı CH340 TTL dönüştürücüsünü TX→PA3, RX→PA2, GND→GND, 3,3 V olarak bağladı; eski loopback jumper'ı çıkarıldı. COM18 / VID1A86 PID7523, 115200 8N1. ST-LINK yalnız firmware yüklemesi/başlangıç kontrolü içindir; aşağıdaki veri trafiği CH340 ile gerçek seri hat üzerinden çalıştırıldı.

**Son test firmware'i:** `.build/board-tests/ch340-test.elf`, SHA256 `e691d26495440016510e8d2fc5eb63a57891d82be6cda3b6673042ad65a9bb97`. GCC13.3.1 -O0, HCLK168MHz, FreeRTOS1ms. `UART_COMM_TEST` ve `UART_COMM_SERIAL_TEST` açık; production sürücüsü aynı public API ile kullanıldı. **11/11 veri/yük kontrolü +1/1 süre kontrolü +1/1 negatif RX sıra kontrolü =13/13 PASS.** Sonuçlar sırasıyla `.build/board-tests/ch340-results.json`, `ch340-metrics.json`, `ch340-order-green.json`.

Kontroller: gerçek echo/payload; parçalı64 bayt; birleşik çerçeveler; bozuk CRC/yeniden senkron; yarım çerçeve timeout; 520 çerçeveyle DMA ve uint16 SEQ sarımı; kuyruk doluluğunda8 kabul +1 görünür yanıt düşmesi; 10sn boşta uyanmama; bağımsız100Hz ve sürekli64 bayt RX/TX; fiziksel CH340 break sonrası RX restart/echo; IRQ/handler/stack sınırları; kasıtlı RX tekrarı ve yanlış uzunluğun sayılması.

| Son firmware ölçümü | Sonuç |
|---|---|
| 10sn sessizlik | 0 UART servis turu, 0 UART/DMA cycle, süreli uyanma yok |
| Her yönde100Hz×13 bayt | 1000 gönderim/1000 alım; ölçülen servis+ISR payı %1,471 |
| Bağımsız sürekli64 bayt | Her yönde1000 çerçeve, sıra/içerik/uzunluk/CRC doğrulandı; overrun/kayıp artışı0; ölçülen servis+ISR payı %7,76 |
| UART kritik bölüm / ISR maksimum | 965cycle=5,74µs /1821cycle=10,84µs |
| RX bildiriminden servise maksimum | 8191cycle=48,76µs |
| Frame/result handler maksimum | 2928cycle=17,43µs /475cycle=2,83µs |
| Servis turu maksimum | 44023cycle=262,04µs |
| UART stack boş payı | 360/512word=1440bayt, %70,31 |
| Fiziksel break | RX restart delta1; yeni geçerli echo alındı |

**CPU kapsamı:** Bu yüzde DWT ile ölçülen UART servis kodu + UART/DMA ISR payıdır. Servis dışındaki bekleme/notification yönetimi, fixture taskının frame/CRC hazırlığı ve diğer sistem taskları dahil değildir. Dolayısıyla plandaki bütün UART taskı+ISR için ≤%2 hedefinin kesin kapanış kanıtı değildir; tam task runtime ölçümü açık kalır. Sessiz hat için sıfır UART servis/IRQ ve süreli uyanma olmaması ayrıca doğrudan doğrulandı.

İnceleme ve RED→GREEN: İlk fixture, tek handler'da9 yanıt üretirken206µs; toplu yanıt fixture taskına taşındı. 64 bayt echo frame/CRC hazırlığı da106µs olduğundan callback yalnız payload kopyalar, hazırlığı fixture taskı yapar; son handler17,43µs. RED kayıtları `ch340-handler-limit-red.json`, `ch340-echo-limit-red.json`.

Bağımsız `ch340_review`: yeni Critical yok; Important RX test açığı tekrar/uzunluk kontrolüyle kapatıldı. Aynı gerçek hat girişinde [SEQ0/len4, SEQ1/len4, SEQ1/len4, SEQ3/len1] eskiden0, son firmware'de2 sapma üretti (`ch340-order-red.json`→`ch340-order-green.json`). Host ayrıca çerçeveler arasındaki ve kapanıştaki artık TX baytlarını hata sayar. Test kuyruğuna payload kopyası ve sabit task önceliklerindeki sayaç sahipliği incelendi. Üretim sürücüsü bu incelemede yeniden değerlendirilmedi.

Derleme: son normal test ve üretim tam derlemeleri0 hata/0 uyarı; üretimde test tanımlı derleme satırı0. Üretim text46192/data96/bss25088; test ve CH340 sembolleri üretimden çıkar. PC modelleri RX31/31, TX23/23, RTOS16/16 geçti. Eski71/71 native loopback koşusu tarihsel kanıttır; jumper çıkarıldığı için bu oturumda yeniden koşturulmadı.

**Kartta tekrar üretim firmware'i var:** `.build/board-tests/ch340-final-production.elf`, SHA256 `93c819a3fa068bb68d6ed5d5298eaf8ecf378ded2626de1d269deb244edd682f`. Son başlangıç kaydı `.build/board-tests/ch340-production-smoke.log`: initialized1, rx_ready1, tx_accepting1, TX_IDLE, tasks4. `.ioc`, pinler, peripheral ayarları ve üretim `uart_comm.c` bu test çalışmasında değiştirilmedi. Pyserial önceden kurulu; yeni kütüphane eklenmedi. Commit/push yapılmadı.

Kapsam dışında kalan plan açıkları: eski native yanıt örneğinin ret sayacı, iki üreticinin ortak başarılı-enqueue sırasının doğrudan izi, ana RX kontrol kutularının senkronizasyonu ve eski native hata kaynak etiketleri bu CH340 çalışmasıyla tamamen kapatılmış sayılmaz. Yeni CH340 yanıt örneği düşmeyi açık sayar/test eder; iki yönlü hat sırası, iki taskın ortak TX enqueue sırası için kanıt yerine geçmez.

## 6 Ekim 2026 — dosya düzeninin sadeleştirilmesi

Frame/parser/CRC altı kaynak/başlık yerine `protocol.c/.h` altında birleştirildi; fonksiyon gövdeleri, sabitler ve API imzaları korunur. Kart testlerinin altı dosyası `Tests/Src` ve `Tests/Inc` altına taşındı. Private başlık `uart_comm_internal.h` oldu; uygulama durumu dosya içinde, bütün test tüketicileri snapshot API'sinde. Core C/H sayısı29→19, gerçek proje dosyası azalması4. Beş tarihsel belge `docs/archive` altında, kökte üç güncel MD kaldı. `.ioc`, pin/peripheral/RTOS ayarları ve UART algoritmaları değişmedi.

Doğrulama: PC RX31/31 + TX23/23 + RTOS16/16; kart protokol birim testleri29/29; CH340 veri/yük11/11 + ölçüm1/1 + tekrar sıra/yanlış boyut1/1, toplam13/13. CubeIDE Debug/Release, komut satırı normal test/serial/üretim tam derlemeleri0 hata/0 uyarı. Tek UART taskı ve callback seti; üretim ELF'inde test/serial sembolleri yok. Bağımsız incelemede kritik/önemli regresyon yok; yerel Markdown bağlantıları geçerli. Eski native loopback15/15 ve kabul71/71 koşusu jumper çıkarılmış olduğu için tekrar edilmedi; önceki kaydı geçerliliğinin sınırlarıyla korunur.

CH340: boş hatta10sn/0 UART turu;100Hz çift yönde1000TX/1000RX,10074ms, ölçülen servis+ISR%1,476. Sürekli64bayt trafikte1000TX/1000RX,5708ms,%7,773. Kritik965cycle, IRQ1818cycle, RX latency7795cycle, frame handler2819cycle, result handler470cycle, max service44609cycle; boş stack360/512word. Bu ölçüm bütün UART taskının runtime'ı değildir; önceki tam CPU ölçümü ve diğer plan açıkları kapanmış sayılmaz.

Kanıtlar `.build/board-tests/simplify-ch340-results.json`, `simplify-ch340-metrics.json`, `simplify-ch340-order.json`, `simplify-unit-board.log`, `simplify-production-smoke.log`; derlemeler `.build/simplify-ide-build.log`, `simplify-native-build.log`, `simplify-serial-build.log`, `simplify-production-build.log`.

Firmware SHA256:
- Normal test `simplify-native.elf`: `cf99d474e9e55af9367f0f4e9869e1a2ea15311f82667f6fc661721dc5090821`.
- CH340 test `simplify-ch340.elf`: `02a412664b72411867c338e296a6502fb41d435445435d054ce72244c6b933ac`.
- **Son kart firmware'i** üretim `simplify-production.elf`: `3f5f8410563ac0337fcc1f0f298c5e08673414bdc52bdcaccfb3a2d6dab1c227`; initialized1, rx_ready1, tx_accepting1, TX_IDLE, tasks4. Üretim text46192/data96/bss25088.

Yeni kütüphane, commit veya push yapılmadı. Kullanım ve güncel dosya düzeni [UART_COMM_KULLANIM.md](UART_COMM_KULLANIM.md) içindedir.


## 6 Ekim 2026 — uart_comm iç akışının okunabilirliği

Private helper adları `rx_`/`tx_`/`comm_` sorumluluğuyla netleştirildi. `comm_service_once` sırası korunarak TX sonuç teslimi, recovery istekleri ve tek queue öğesinin hizmeti üç yardımcıya ayrıldı. Callback/IRQ tanımları dosyanın sonunda bir arada; kritik bölümlerde sıkıştırılmış işlemler ayrı satırlarda. Kullanılmayan `rx_drain` tanımı ve private prototipi kaldırıldı. Public API/timeout/task/IRQ/DMA/queue ayarı değişmedi, yeni dosya/kütüphane yok. Ana servis67→27 satır, toplam C dosyası1303→1364; bu adım okunabilirlik düzenlemesidir.

PC RX31/31, TX23/23, RTOS16/16. CubeIDE Debug/Release ve komut satırı normal test/serial/üretim tam derlemeleri0 hata/0 uyarı. Üretim test/serial sembollerinden arındırılmış, tek UART taskı ve callback seti doğrulandı. Bağımsız incelemede kritik/önemli regresyon yok: ortak102 fonksiyondan101'i isim/yorum/boşluk dışında aynı, servis gövdesindeki üç helper eski işlem/kilit/callback sırasını koruyor.

CH34011 veri/yük +1 ölçüm +1 tekrar sıra/yanlış boyut kontrolü, toplam13/13 geçti. Boş hatta10sn/0 UART servis turu.100Hz çift yön1000TX/1000RX,10073ms, ölçülen servis+ISR%1,481; sürekli64bayt1000TX/1000RX,5707ms,%7,8. Stack boş386/512word; max kritik968cycle, IRQ1802cycle, RX latency9134cycle, frame handler2820cycle, result handler717cycle, service44345cycle. Önceki tam task CPU ölçümü ve native test/plan açıkları bu çalışmayla kapanmış sayılmaz; jumper çıkarılmış olduğundan native loopback kabulü yeniden koşulmadı.

Kanıtlar `.build/readability-ide-build.log`, `readability-native-build.log`, `readability-serial-build.log`, `readability-production-build.log`; `.build/board-tests/readability-ch340-results.json`, `readability-ch340-metrics.json`, `readability-ch340-order.json`, `readability-serial-smoke.log`, `readability-production-smoke.log`.

Firmware SHA256:

- Native test `readability-native.elf`: `39d7f92f5bf11394406bd81af85cbb5e1538f3722c94057425af94d33301f4f6`.
- CH340 test `readability-ch340.elf`: `6d414151be6f2551b9eef5329f9d5efc3b1fb6a4dcd0559ba3a5d00359b8c8c8`.
- **Güncel kart firmware'i** üretim `readability-production.elf`: `bdd5f6d7f25e0b8b6d2f4b3adbbf326ed3b194b70ec6ba9500619de502c41f65`; initialized1, rx_ready1, tx_accepting1, TX_IDLE, tasks4. Üretim text46240/data96/bss25088; önceki düzenlemeye göre text+48bayt, RAM aynı.

Son kartta üretim firmware'i var. `.ioc`/generated donanım ayarları korunur. Commit/push yapılmadı.

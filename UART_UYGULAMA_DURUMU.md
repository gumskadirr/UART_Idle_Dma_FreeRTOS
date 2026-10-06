# UART uygulama ve doğrulama kaydı

Plan: `UART_BIRLESIK_MIMARI_VE_UYGULAMA_YOL_HARITASI.md` (revizyon 2).
Başlangıç: 2026-10-05, `ba96216`, mevcut `uart-birlesik` çalışma dalı.

## Uygulama sırası

| Adım | Durum | Kanıt / kalan iş |
|---|---|---|
| P0 / R1–R5 | Kapanış koşusu geçti | Kart: 29/29 birim, 15/15 loopback, 37/37 önceki kabul; PC C/HAL: 28/28. T4'te gerçek FE/ORE sonrası alım da PASS. |
| T1 | Doğrulandı | Eksik kapı RED → 5/5 C/HAL PASS; ARM build uyarısız; kart regresyonu 29/29 + 15/15 + 37/37. |
| T2 | Doğrulandı | Yeni dönüş tipleri RED; davranış 9/12 → 12/12 PASS; ARM build uyarısız; kart 29/29 + 15/15 + 37/37. |
| T3 | Doğrulandı | Eksik sonuç/deadline API RED → 22/22 TX ve 28/28 RX PASS; ARM test build uyarısız; kart regresyonu 29/29 + 15/15 + 37/37. |
| T4 | Doğrulandı, son inceleme düzeltmeleri tamam | Kart: 29/29 birim + 15/15 loopback + 49/49 kabul; 0 FAIL/NOT_RUN/SKIP. PC: 30/30 RX, 23/23 TX. Üretim ve test build: 0 uyarı/hata. |
| F0 | Kullanıcı kapsamı dışında | Kullanıcı FreeRTOS eklenmesini reddetti; TX aşamasında durulacak. |
| F1–F3 | Kullanıcı kapsamı dışında | F0 uygulanmayacak. |
| M1–M2 | Kullanıcı kapsamı dışında | Plan bağımlılıkları nedeniyle birleşme yapılmayacak. |

## Çalışma kararları

- Mevcut çalışma dalında devam ediliyor: kullanıcı bu projedeki önceki değişikliklerden ilerlenmesini istedi. RX sadeleştirmesi ve kullanıcının `.settings/language.settings.xml` değişikliği korunuyor; ayrı worktree'ye aktarılıp geride bırakılmıyor.
- Planın 1–7. bölümleri şartnamedir. Uygulama adımları P0/R1–R5 zaten mevcut olduğundan yeniden yazılmayacak; kabul sonuçları denetlenecek.
- PC modeli gerçek C kodunu çalıştırır; fiziksel DMA/IRQ zamanlamasının kanıtı değildir. Kartta koşulmayan deney `NOT_RUN` kalır.
- Kart/jumper kullanıcı tarafından hazır bildirildi. Yerel CubeIDE ST-LINK araçları kullanılabilir; tanımlanan probe STM32F4DISCOVERY / `0665FF495751826687091736`.
- Pre-flight: R5 deadline ve T3 sonuç kutusu F1'de tüketilecek; F2 epoch/gate ile FIFO'ya bağlanacak; M1'de public RX/TX API kaldırılacak. Mevcut TX header'ındaki hedef açıklamalar uygulanmış kod kabul edilmiyor.
- Kapsam güncellemesi: Kullanıcı açıkça “FreeRTOS ekleme; TX aşamasında kal” dedi. F0–F3 ve M1–M2 uygulanmayacak; T4 son doğrulanacak adımdır. `.ioc`, NVIC ve SysTick ayarları korunur.

## RX kapanış kanıtı

- İlk gerçek kart koşusu: birim 29/29, loopback 12/15, kabul 18/37. Gerekli test önkoşulları düzeltilmeden başarı iddia edilmedi.
- Test düzeltmeleri: dışarıdan HAL abort yerine owner FAULT/recovery akışı; FAULT'ta aktif DMA varsa açık recovery isteği; boş parser'da sample fault için gerçek RX olayı; gerçek FE'de error bildirimi yanında restart tamamlanması; yeni bütçe testinden önce 100 ms sağlıklı dönem; sağlıklı sessizlik testi gerçekten 2 saniye servis çağırmıyor.
- Son koşu: `.build/board-tests/rx-accepted.log`, UART_COMM_TEST açık ELF; sıfır FAIL/NOT_RUN/SKIP kayıtlı 37 mevcut kontrol. Ek ORE deneyi bu tabloda bulunmadığından ayrıca NOT_RUN olarak tutuluyor.
- RX kabul ELF SHA256: `e95c36989c3d8c7f9af0852b9fd7e0234f39406666bbea23ac588114365c7a9e`.

## TX kapanış kanıtı

- T4 testleri önce eksik `uart_tx_test_faults` kapısıyla linkte RED oldu. Kapı yalnız `UART_COMM_TEST` altında eklendi. İlk kart koşusu 43/47; düzeltme ve donanım davranışını ayrı değerlendirdikten sonra 47/47 PASS.
- 40 × 13 = 520 bayt gerçek TX DMA zinciri, RX producer 520, 40 handler teslimi, sequence 65534 → 65535 → 0 → ... → 37 ve sıfır sequence gap doğrulandı. Birleşik, parçalı ve 64 bayt çerçeveler ayrıca geçti.
- Gerçek FE: TX sırasında SBK break. Gerçek ORE: izole test düzeneğinde RX DMA `HAL_DMA_Abort_IT` ile durdurulup DMAR açık tutuldu; ikinci loopback baytı donanım ORE üretti. Her iki durumda TX COMPLETE, RX restart ve sonraki geçerli çerçeve teslimi doğrulandı.
- Ruling: ORE testinde DMAR'ı elle kapatan ilk düzenek değiştirildi — native HAL, DMAR kapalıysa DMA abort'u atlıyor ve aktif DMA geride kalıyordu; bu ORE toparlanmasını sınamıyordu — yanlışsa yeni düzeneğin gerçek ORE kanıtı geçersiz olur; `last_error & ORE` koşulu ayrıca denetleniyor.
- Gerçek TX DMA TE: yalnız test build'inde DMA kaynağı CCM `0x10000000`; CPU bu adresi okumaz. [RM0090](https://www.st.com/resource/en/reference_manual/rm0090-stm32f407-advanced-armbased-32bit-mcus-stmicroelectronics.pdf) STM32F407 bus matrix/CCM erişim sınırını açıklar. Native DMA IRQ → HAL UART DMA error → iki modül kapısı çalıştı, RX yeniden alıma döndü.
- Ruling: İlk bayttan önceki TE için başarı ölçütü DMA_ERROR + recovery_fault + FAULT'tur — HAL başlangıçta TC'yi temizler, ilk DR yüklemesi gerçekleşmezse TC kendiliğinden set olmaz; planın güvenli duruş şartı TC=1'dir — yanlışsa güvenli TX gereksiz kilitli kalır; veri/tampon güvenliği açısından konservatif seçimdir. Init TC=0 iken reddedildi. Yalnız test düzeneğinde EN=0/DMAT=0 sonrası TE off/on ile idle frame üretildi, TC gözlendi, init ve yeni aktarım doğrulandı. Üretim servisi TE/pin/.ioc değiştirmez.
- Ortak DMA hatası ayrıca iki yön aktifken callback enjeksiyonuyla sınandı. Kayıp TX done + kayıp abort callback deneyinde gerçek HAL donanımı tamamladı; modül 20 ms timeout sonucu verdi, fiziksel duruşla IDLE'a döndü ve yeni TX/RX geçti. Başlangıç BUSY/ERROR birer sonuç üretti, otomatik yeniden gönderim yapılmadı.
- PC TX: 22/22; RX: 28/28; Python protokol: 9/9. Model; snapshot/karar yarışı, eşzamanlı done+error, HAL dönmeden callback, EN açık abort callback, geç done, tick sarımı ve sonuç kutusu sahipliğini kapsar. Bu zamanlama testleri donanım kanıtı diye sunulmuyor.
- Kart kaydı: `.build/board-tests/t4-accepted.log`; aynı firmware: `.build/board-tests/t4-accepted.elf`, SHA256 `c46405a7ec65a6903472506a5fd4680e68f81aa08e0d425f2fba070647a5e23f`.
- Üretim tam rebuild: `.build/production-build.log`, text 28252 / data 12 / bss 2412 bayt; `UART_COMM_TEST` derleme satırı 0, test kapısı/sembolü yok. Error/RX/TX/abort callback'leri tek tanım. DMA tamponları SRAM'dedir; `.ioc`, IRQ önceliği, SysTick ve pin atamaları değiştirilmedi.
- RTOS CPU/stack/uyku ve FIFO ölçümleri kullanıcı kapsamı dışında; bunlara başarı iddiası yok.

## Son bağımsız inceleme ve düzeltme

- Tüm değişiklikler fresh-context `tx_final_review` incelemesinden geçti; Critical/Minor yok, üç Important bulgu düzeltildi. Tek düzeltme turu; ikinci reviewer koşusu yapılmadı. Son bütün regresyonlar yeniden çalıştırıldı.
- Final: fixed aktif TX DMA CR read-modify-write yarışı — `ht_clear_completion_race` RED (EN eski değerle geri yazılıyor) → GREEN. HTIE artık F407 peripheral bit-band ile yalnız bit 3 yazılarak temizlenir; IRQ veya donanım EN değişimi geri yazılmaz. Model bu belirli yarış penceresini ayrıca uygular.
- Final: fixed ilk geçerli RX çerçevesinin recovery dönemini kapatmaması — `valid_frame_closes_recovery` RED → GREEN. Sağlıklı donanım ve bekleyen hata yokken kısa korumayla bütçe kapanır; handler koruma dışında çalışır. Kartta altı ardışık hata/restart/geçerli çerçeve döngüsü ayrıca PASS.
- Final: fixed soğuk başlangıç hatasında kalıcı durma — `cold_start_recovers` RED → GREEN. HAL_ERROR/FAULT dönüşü görünür kalır, owner aynı sınırlı toparlanmayı otomatik işler. `main.c` USER CODE içindeki RX başlangıç `Error_Handler` yolu kaldırıldı; başlangıç dönüşü `uart_rx_start_status` değişkeninde korunur. Kartta hatalı başlangıç sonrası otomatik restart ve sonraki DMA çerçevesi PASS.
- Son suite: RX 30/30, TX 23/23, protokol 9/9; kart birim 29/29, loopback 15/15, kabul 49/49; diff whitespace kontrolü temiz.
- Eski RX bekleme döngülerine de TX servis/tüketimi eklendi; yeni T4 helper'ı kendi sonucunu tükettiği için bu otomatik tüketim ona uygulanmadı. Son kart regresyonu tekrar 49/49 PASS.
- Son kart firmware/kayıt: `.build/board-tests/final-accepted.elf` ve `final-accepted.log`; SHA256 `e1313504bf6593cfb7c979103813bab67aecc0e5ecf6d8fab01f520f23506aaf`. Test build text 52100 / data 12 / bss 4292 bayt.
- Son üretim build: `.build/production-build.log`; text 28324 / data 12 / bss 2420 bayt, 0 uyarı/hata, 0 UART_COMM_TEST satırı. `.build/production-symbols.log`: beş ilgili callback tek tanım; test/force/loopback sembolü yok. Önceki T4 test ELF'inde TX SRAM `0x20000a38`, RX SRAM `0x200008f8`; CCM yalnız kasıtlı TE test kaynağıdır.
- İncelemenin kapsam dışında bıraktıkları değerlendirilip korundu: FreeRTOS/FIFO/uyku/CPU/stack/birleşme kullanıcının TX sınırı gereği; çoklu UART/farklı stream desteği mevcut USART2 hedefinin dışında; ACK/otomatik tekrar gönderim yok; birden fazla DMA turu boyunca IRQ engellenirse kesin tur hesabı yapılamaz; owner dışı HAL/register müdahalesinde güvenli duruş kanıtı olmadan yeniden kullanım yok; kullanıcının `.settings/language.settings.xml` değişikliği korunur. Bu sınırların yanlış yorumlanması ilgili yeni kullanımda ek tasarım/test gerektirir.
- Kod mevcut `uart-birlesik` çalışma ağacında bırakıldı; kullanıcı değişiklikleriyle karışan commit, merge veya push yapılmadı. F0–M2 bilinçli olarak açık bırakıldı.

/*
 * uart_tx.c
 *
 * Tek aktif gonderim. Sozlesme ve gerekceler uart_tx.h icinde.
 */
#include <stddef.h>
#include <string.h>

#include "uart_tx.h"

/* --- Modul ici durum --- */
static UART_HandleTypeDef *s_huart;                /* uart_tx_init baglar */
static uint8_t             s_buf[UART_TX_BUF_SIZE];/* DMA bunu okur */
static uint8_t             s_len;                  /* aktif cercevenin boyu */

/* Ana baglam yazar, kesme okur */
static volatile uart_tx_state_t s_state = UART_TX_IDLE;

/* Kesme yazar, ana baglam okur ve sifirlar (uart_rx'teki s_rx_pending gibi) */
static volatile uint8_t    s_tx_done;

uart_tx_stats_t uart_tx_stats;


HAL_StatusTypeDef uart_tx_init(UART_HandleTypeDef *huart)
{
    /* SOZLESME:
       - huart veya huart->hdmatx NULL ise HAL_ERROR doner ve hicbir sey
         baglanmaz.
       - Donusten sonra modul UART_TX_IDLE durumunda olmali; FAULT durumundan
         cikisin tek yolu bu fonksiyon.
       - Sayaclari SIFIRLAMA: gecmis hata bilgisi silinmemeli (uart_rx
         toparlanmasinda frame_parser_discard / _init ayrimiyla ayni gerekce).
       - Burada DMA BASLATILMAZ: gonderilecek bir sey yok. */
    
	if ((huart == NULL) || (huart->hdmatx == NULL)){
		return HAL_ERROR;
	}

	if(s_state==UART_TX_SENDING){
		return HAL_BUSY;
	}

	s_huart=huart;
	s_len=0U;
	s_tx_done=0U;
	s_state=UART_TX_IDLE;

	return HAL_OK;
}


uart_tx_status_t uart_tx_send_copy(const uint8_t *data, uint8_t len)
{
    /* SOZLESME:
       - uart_tx_init cagrilmamissa veya durum FAULT ise UART_TX_NOT_READY.
       - data NULL, len 0 veya len > UART_TX_BUF_SIZE ise UART_TX_INVALID ve
         rejected_invalid artar. Hicbir sey degismez.
       - Durum IDLE degilse UART_TX_BUSY ve rejected_busy artar.
         AKTIF GONDERIM BOZULMAZ: s_buf'a dokunulmaz.
       - Kabul edilirse: veri s_buf'a KOPYALANIR, boylece donusten sonra
         cagiranin tamponu serbesttir.
       - Durum HAL cagrisindan ONCE UART_TX_SENDING olmali (tamamlanma kesmesi
         cagri donmeden gelebilir); HAL dusserse durum IDLE'a GERI ALINMALI,
         start_fails artmali ve UART_TX_BUSY donmeli.

       Kullanacagin: memcpy(s_buf, data, len)
                     HAL_UART_Transmit_DMA(s_huart, s_buf, len) */


	if(s_huart==NULL || s_state==UART_TX_FAULT){
		return UART_TX_NOT_READY;
	}

	if(data==NULL || len == 0 || len>UART_TX_BUF_SIZE){
		uart_tx_stats.rejected_invalid++;
		return UART_TX_INVALID;
	}

	if(s_state != UART_TX_IDLE){
		uart_tx_stats.rejected_busy++;
		return UART_TX_BUSY;
	}

	memcpy(s_buf,data,len);
	s_len=len;
	s_state=UART_TX_SENDING;

	if(HAL_UART_Transmit_DMA(s_huart, s_buf, len)!=HAL_OK){
		s_state=UART_TX_IDLE;
		uart_tx_stats.start_fails++;
		return UART_TX_BUSY;
	}
    return UART_TX_OK;
}


void uart_tx_service(void)
{
    /* SOZLESME:
       - Bekleyen tamamlanma bildirimi yoksa hicbir sey yapmaz.
       - Bildirim varsa: bayrak sifirlanir, durum IDLE'a doner, frames_sent ve
         bytes_sent guncellenir.
       - Bayrak kesmede set edildigi icin once OKUNUP sifirlanmali, sonra is
         yapilmali; aksi halde is sirasinda gelen yeni bildirim kaybolur.
         (uart_rx_service'teki s_rx_pending ile ayni kalip.)
       - Siradaki cerceveyi BURADA baslatma: Asama 2'de kuyruk yok. */

	if(s_tx_done != 0U){
		s_tx_done=0U;
		s_state = UART_TX_IDLE;
		uart_tx_stats.frames_sent++;
		uart_tx_stats.bytes_sent += s_len;//byte sayisi kadar ekleme yapilabilir
	}

}


uart_tx_state_t uart_tx_get_state(void)
{
    return s_state;
}


/* --- HAL callback ---
   HAL'de __weak tanimli ve butun UART'lar icin ortaktir; bu yuzden hangi UART
   oldugu kontrol edilir (uart_rx.c'deki callback'lerle ayni kalip).

   DMA TC DEGIL bu callback kullanilir: DMA TC son bayti USART veri kaydina
   yazdiginda olusur, ama o bayt hala kaydirma kaydindadir ve hatta ~87 us
   daha surer. Tamponu o anda serbest biraksak son bayt hat uzerindeyken
   uzerine yazilabilirdi. Bu callback UART TC'den gelir: bayt gercekten
   hattan cikmistir. TX icin USART2 kesmesinin gerekli olmasinin sebebi de
   budur. */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    /* SOZLESME:
       - Baska bir UART'in callback'i ise hicbir sey yapma.
       - Yalnizca BILDIR: sayaci artir ve bayragi kaldir.
       - Burada durum degistirme, yeni gonderim baslatma, muhasebe yapma.
         Kesme baglaminda yapilacak is en kisa olmali; Asama 4'te siradaki
         cerceveyi kuyruktan almak ortak taskin isi olacak. */

    if ((s_huart != NULL) && (huart->Instance == s_huart->Instance)){
    	s_tx_done=1U;
    	uart_tx_stats.tx_complete_events++;
    }
}


/* --- ADIM 1.2 TASLAGI: TX bildirim kapilari ---

   EKLENECEK MODUL ICI DURUM (ucunu de YALNIZCA kesme yazar):
       static volatile uint8_t  s_tx_error;        bekleyen hata bildirimi
       static volatile uint32_t s_tx_error_code;   ham HAL ErrorCode
       static volatile uint8_t  s_abort_done;      iptal tamamlanma bildirimi

   NEDEN s_tx_error_code AYRI BIR DEGISKEN (dogrudan
   uart_tx_stats.last_hal_error'a yazmak yerine):
   last_hal_error struct'in "yalnizca ana baglam yazar" bolumunde duruyor.
   Kesmeden oraya yazmak kendi sahiplik kuralimizi bozardi; alan volatile
   olmadigi icin derleyici de onu onbellekleme hakkina sahip. Ham deger
   kesmeye ait bir volatile kutuda bekler, ana baglam aktarimin DUSTUGUNE
   karar verdiginde degeri oradan alip last_hal_error'a yazar. Bildirim ile
   MUHASEBE boylece ayri kalir.

   uart_tx_on_error(huart, error)
       - s_huart NULL ya da baska UART -> hicbir sey yapma
       - uart_tx_stats.tx_error_events++     (volatile; CAGRI sayisi)
       - s_tx_error_code |= error            (bkz. KARAR 1)
       - s_tx_error = 1U
       - DURUM DEGISTIRME, HAL CAGIRMA, MUHASEBE YAPMA

   KARAR 1: '=' mi '|=' mi?
   Servis calismadan ikinci bir hata gelirse '=' ilk hatanin bitlerini siler.
   HAL'in kendisi de ErrorCode'a '|=' ile yazar; ayni yaklasimi surdurmek
   "ne oldu" bilgisini korur. Secim: '|='.

   KARAR 2: kesme s_state'e bakip "TX aktif degil, bildirmeyeyim" demeli mi?
   HAYIR. Kesme BILDIRIR, karar VERMEZ (uart_rx'teki s_rx_error ile ayni
   kalip). Aktif TX yoksa bayrak bosa dusmus olur: uart_tx_service onu SENDING
   degilse yok sayar, send_copy de yeni gonderimden once bekleyen bildirimleri
   temizler (bkz. uart_tx.h send_copy sozlesmesi). Sonuc: RX akisindan gelen
   bir DMA hatasi tx_error_events'i artirir ama transfer_errors'i artirmaz -
   iki sayacin FARKLI olmasinin anlami tam olarak budur.

   uart_tx_on_abort_complete(huart)
       - s_huart NULL ya da baska UART -> hicbir sey yapma
       - uart_tx_stats.abort_complete_events++
       - s_abort_done = 1U
       - Durus DOGRULAMASI BURADA YAPILMAZ: bu bir bildirim, kanit degil
         (bkz. uart_tx.h). Dogrulama Adim 1.5'te, servis icinde.

   HAL_UART_AbortTransmitCpltCallback(huart)
       HAL'in __weak callback'i. YALNIZCA TX'e ait oldugu icin ortak degil:
       bu dosya onu dogrudan sahiplenebilir, uart_rx.c ile cakisma yok.
       Govdesi tek satir olacak:  uart_tx_on_abort_complete(huart);

   PEKI ARADAKI DOLAYLAMA NE ISE YARIYOR?
   Iki sey icin: (1) TEST KANCASI - gec gelen ya da hic gelmeyen bir iptal
   bildirimini donanimsiz uretmenin tek deterministik yolu bu kapiyi
   cagirmaktir (uart_rx_test_inject_error ile ayni gerekce, bkz. uart_rx.h);
   (2) Asama 4'te callback'ler ortak bir yere tasinirsa kapi hazir durur.

   HAL_UART_TxCpltCallback'e 1.2'de DOKUNULMAZ: hala yalnizca s_tx_done
   kaldiriyor. ABORTING/FAULT'ta gelen tamamlanmanin "gec bildirim" sayilmasi
   ana baglamin isidir ve Adim 1.7'ye aittir.
*/

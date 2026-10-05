/*
 * crc16.h
 *
 *  Created on: Sep 29, 2026
 *      Author: kg083
 */

#ifndef INC_CRC16_H_
#define INC_CRC16_H_

#include <stdint.h>

/**
  * @brief  CRC-16/IBM-3740 (CRC-16/CCITT-FALSE) hesaplar.
  *         Polinom 0x1021, baslangic 0xFFFF, yansitma yok, cikis XOR yok.
  * @param  data     Hesaba katilacak baytlar. Fonksiyon veriyi degistirmez.
  * @param  len      data icindeki bayt sayisi. 0 olabilir.
  * @retval Hesaplanan CRC. len 0 veya data NULL ise 0xFFFF doner.
  */
uint16_t crc16_ccitt(const uint8_t *data, uint16_t len);

#endif /* INC_CRC16_H_ */

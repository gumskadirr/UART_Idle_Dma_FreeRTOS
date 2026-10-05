/*
 * crc16.c
 *
 *  Created on: Sep 29, 2026
 *      Author: kg083
 */
#include <stddef.h>
#include "crc16.h"

#define CRC16_POLY   0x1021U
#define CRC16_INIT   0xFFFFU

uint16_t crc16_ccitt(const uint8_t *data, uint16_t len)
{
    uint16_t crc = CRC16_INIT;
    uint16_t i;
    uint8_t  bit;

    if (data == NULL)
    {
        return crc;
    }

    for (i = 0U; i < len; i++)
    {
        /* Bayti crc'nin ust yarisina XOR'la: reflect in = false oldugu icin
           bitler en anlamlidan isleniyor. */
        crc ^= (uint16_t)((uint16_t)data[i] << 8);

        for (bit = 0U; bit < 8U; bit++)
        {
            if ((crc & 0x8000U) != 0U)
            {
                /* Ust bit 1: kaydir ve polinomu uygula */
                crc = (uint16_t)((uint16_t)(crc << 1) ^ CRC16_POLY);
            }
            else
            {
                /* Ust bit 0: sadece kaydir */
                crc = (uint16_t)(crc << 1);
            }
        }
    }

    return crc;   /* XOR out = 0x0000, ek islem yok */
}

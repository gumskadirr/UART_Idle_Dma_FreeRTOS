/*
 * app_protocol.c
 *
 * Sozlesme app_protocol.h'de. Bu dosya HAL ve DMA tanimaz: yalnizca
 * cozulmus cerceve alanlarini yorumlar.
 */
#include <stddef.h>

#include "app_protocol.h"
#include "frame.h"

app_proto_state_t app_proto_state;


void app_protocol_init(void)
{
    app_proto_state.last_seq       = 0U;
    app_proto_state.next_seq       = 0U;
    app_proto_state.seq_gap_events = 0U;
    app_proto_state.seq_synced     = 0U;
    app_proto_state.joy_x          = 0;
    app_proto_state.joy_y          = 0;
    app_proto_state.frames_handled = 0U;
}


void app_protocol_on_frame(const frame_info_t *info, void *user_data)
{
    (void)user_data;

    if (info == NULL)
    {
        return;
    }

    app_proto_state.frames_handled++;

    if (app_proto_state.seq_synced == 0U)
    {
        /* Ilk cerceve: gonderenin hangi degerden basladigini bilemeyiz.
           Karsilastirma yapmadan referans aliyoruz (RTP alicisi da boyle
           yapar: ilk pakette sira numarasina senkronize olur). */
        app_proto_state.seq_synced = 1U;
    }
    else if (info->seq != app_proto_state.next_seq)
    {
        app_proto_state.seq_gap_events++;
    }
    else
    {
        /* Beklenen sira geldi */
    }

    app_proto_state.last_seq = info->seq;

    /* Beklentiyi GELEN degerden turetiyoruz. "next_seq++" yazsaydik tek bir
       kayiptan sonra kalici olarak bir geri kalir ve sonraki her cerceveyi
       kayip sayardik. (uint16_t) cast'i 65535 -> 0 sarimini halleder. */
    app_proto_state.next_seq = (uint16_t)(info->seq + 1U);

    if ((info->type == FRAME_TYPE_JOYSTICK) && (info->payload_len == 4U))
    {
        /* payload yalnizca bu cagri suresince gecerli: DEGER kopyalaniyor,
           isaretci SAKLANMIYOR. */
        app_proto_state.joy_x = (int16_t)((uint16_t)info->payload[0] |
                                          ((uint16_t)info->payload[1] << 8));
        app_proto_state.joy_y = (int16_t)((uint16_t)info->payload[2] |
                                          ((uint16_t)info->payload[3] << 8));
    }
}

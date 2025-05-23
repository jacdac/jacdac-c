// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "jd_services.h"
#include "jacdac/dist/c/bitmap.h"

struct srv_state {
    SRV_COMMON;
};


void bitmap_process(srv_t *state) {
    // likely a NOP
}

void bitmap_handle_packet(srv_t *state, jd_packet_t *pkt) {
    switch (pkt->service_command) {
    case JD_BITMAP_CMD_FILL:
        // TBD
        break;
    default:
        // TBD service_handle_register_final(state, pkt, buzzer_regs);
        break;
    }
}

// SRV_DEF(buzzer, JD_SERVICE_CLASS_BUZZER);
void bitmap_init(uint16_t width, uint16_t height) {
    // TBD
}
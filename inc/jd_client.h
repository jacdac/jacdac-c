// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "jd_protocol.h"

// An announce packet was first spotted and a device was created (jd_device_t, jd_packet_t)
#define JD_CLIENT_EV_DEVICE_CREATED 0x0001
// We have not seen an announce for 2s, so device was garbage collected (jd_device_t, NULL)
// (or the device was reset, which triggers destroy and create)
#define JD_CLIENT_EV_DEVICE_DESTROYED 0x0002
// An announce packet with lower reset count was spotted (jd_device_t, jd_packet_t)
// This event is always followed by DEVICE_DESTROYED and DEVICE_CREATED
#define JD_CLIENT_EV_DEVICE_RESET 0x0003
// A regular packet for named service was received (jd_device_service_t, jd_packet_t)
#define JD_CLIENT_EV_SERVICE_PACKET 0x0004
// A non-regular packet (CRC-ACK, pipe, etc) was received (jd_device_t?, jd_packet_t)
// This can also happen if packet is received before announce, in which case first argument is NULL.
#define JD_CLIENT_EV_NON_SERVICE_PACKET 0x0005
// A broadcast (JD_FRAME_FLAG_IDENTIFIER_IS_SERVICE_CLASS) packet was received (NULL, jd_packet_t)
#define JD_CLIENT_EV_BROADCAST_PACKET 0x0006
// A value of register was first received, or has changed (jd_device_service_t, jd_register_query_t)
#define JD_CLIENT_EV_SERVICE_REGISTER_CHANGED 0x0007
// Register was marked as not implemented (jd_device_service_t, jd_register_query_t)
#define JD_CLIENT_EV_SERVICE_REGISTER_NOT_IMPLEMENTED 0x0008

typedef struct jd_device_service {
    uint32_t service_class;
    uint8_t service_index;
    uint8_t flags;
    uint16_t userflags;
    void *userdata;
} jd_device_service_t;

#define JD_REGISTER_QUERY_MAX_INLINE 4
typedef struct jd_register_query {
    struct jd_register_query *next;
    uint16_t reg_code;
    uint8_t service_index;
    uint8_t resp_size;
    uint32_t last_query;
    union {
        uint32_t u32;
        uint8_t data[JD_REGISTER_QUERY_MAX_INLINE];
        uint8_t *buffer;
    } value;
} jd_register_query_t;

static inline bool jd_register_not_implemented(const jd_register_query_t *q) {
    return (q->service_index & 0x80) != 0;
}

static inline const void *jd_register_data(const jd_register_query_t *q) {
    return q->resp_size > JD_REGISTER_QUERY_MAX_INLINE ? q->value.buffer : q->value.data;
}

typedef struct jd_device {
    struct jd_device *next;
    jd_register_query_t *_queries;
    uint64_t device_identifier;
    uint8_t num_services;
    uint16_t announce_flags;
    char short_id[5];
    uint32_t _expires;
    void *userdata;
    jd_device_service_t services[0];
} jd_device_t;

extern jd_device_t *jd_devices;

void jd_client_process(void);
void jd_client_handle_packet(jd_packet_t *pkt);

void jd_client_log_event(int event_id, void *arg0, void *arg1);
void jd_client_emit_event(int event_id, void *arg0, void *arg1);

int jd_send_pkt(jd_packet_t *pkt);

// jd_device_t methods
jd_device_t *jd_device_lookup(uint64_t device_identifier);
void jd_device_short_id(char short_id[5], uint64_t long_id);

// jd_device_service_t  methods
static inline jd_device_t *jd_service_parent(jd_device_service_t *serv) {
    return (jd_device_t *)((uint8_t *)(serv - serv->service_index) - sizeof(jd_device_t));
}
int jd_service_send_cmd(jd_device_service_t *serv, uint16_t service_command, const void *data,
                        size_t datasize);
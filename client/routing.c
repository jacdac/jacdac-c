#include "jd_client.h"

/*
- sending packet to device
- query register on device
*/

static uint32_t next_gc;
static uint8_t verbose_log;

#define EXPIRES_USEC (2000 * 1000)
jd_device_t *jd_devices;

// https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
// this is not jd_hash_fnv1a()!
static uint32_t hash_fnv1(const void *data, unsigned len) {
    const uint8_t *d = (const uint8_t *)data;
    uint32_t h = 0x811c9dc5;
    while (len--) {
        h = (h * 0x1000193) ^ *d++;
    }
    return h;
}

static uint32_t mkcd_hash(const void *buf, size_t length, int bits) {
    if (bits < 1) {
        return 0;
    }

    uint32_t h = hash_fnv1(buf, length);

    if (bits >= 32) {
        return h;
    } else {
        return (h ^ (h >> bits)) & ((1 << bits) - 1);
    }
}

void jd_device_short_id(char short_id[5], uint64_t long_id) {
    uint32_t h = mkcd_hash(&long_id, sizeof(long_id), 30);
    short_id[0] = 'A' + (h % 26);
    short_id[1] = 'A' + ((h / 26) % 26);
    short_id[2] = '0' + ((h / (26 * 26)) % 10);
    short_id[3] = '0' + ((h / (26 * 26 * 10)) % 10);
    short_id[4] = 0;
}

void jd_client_log_event(int event_id, void *arg0, void *arg1) {
    jd_device_t *d = arg0;
    jd_device_service_t *serv = arg0;
    jd_packet_t *pkt = arg1;
    jd_register_query_t *reg = arg1;

    switch (event_id) {
    case JD_CLIENT_EV_DEVICE_CREATED:
        DMESG("device created: %s", d->short_id);
        break;
    case JD_CLIENT_EV_DEVICE_DESTROYED:
        DMESG("device destroyed: %s", d->short_id);
        break;
    case JD_CLIENT_EV_DEVICE_RESET:
        DMESG("device reset: %s", d->short_id);
        break;
    case JD_CLIENT_EV_SERVICE_PACKET:
        if (verbose_log) {
            DMESG("serv %s/%d[0x%x] - pkt cmd=%x", jd_service_parent(serv)->short_id,
                  serv->service_index, serv->service_class, pkt->service_command);
        }
        break;
    case JD_CLIENT_EV_NON_SERVICE_PACKET:
        DMESG("unbound pkt d=%s cmd=%x", d ? d->short_id : "n/a", pkt->service_command);
        break;
    case JD_CLIENT_EV_BROADCAST_PACKET:
        // arg0 == NULL here
        if (verbose_log)
            DMESG("brd cmd=%x", pkt->service_command);
        break;
    case JD_CLIENT_EV_SERVICE_REGISTER_CHANGED:
        DMESG("serv %s/%d reg chg %x [sz=%d]", jd_service_parent(serv)->short_id,
              serv->service_index, reg->reg_code, reg->resp_size);
        break;
    }
}

void jd_client_emit_event(int event_id, void *arg0, void *arg1) {
    jd_client_log_event(event_id, arg0, arg1);
    // TODO
}

static jd_device_t *jd_device_alloc(jd_packet_t *announce) {
    int num_services = announce->service_size >> 2;
    int sz = sizeof(jd_device_t) + (num_services * sizeof(jd_device_service_t));
    jd_device_t *d = jd_alloc(sz);
    memset(d, 0, sz);
    d->num_services = num_services;
    d->device_identifier = announce->device_identifier;
    d->_expires = now + EXPIRES_USEC;
    jd_device_short_id(d->short_id, d->device_identifier);

    uint32_t *services = (uint32_t *)announce->data;
    d->announce_flags = services[0] & 0xffff;
    for (int i = 0; i < num_services; ++i) {
        d->services[i].service_class = i == 0 ? 0 : services[i];
        d->services[i].service_index = i;
    }

    d->next = jd_devices;
    jd_devices = d;
    jd_client_emit_event(JD_CLIENT_EV_DEVICE_CREATED, d, announce);
    return d;
}

static void jd_device_free(jd_device_t *d) {
    jd_client_emit_event(JD_CLIENT_EV_DEVICE_DESTROYED, d, NULL);
    jd_register_query_t *tmp, *q = d->_queries;
    while (q) {
        tmp = q;
        q = q->next;
        if (tmp->resp_size > JD_REGISTER_QUERY_MAX_INLINE)
            jd_free(tmp->value.buffer);
        jd_free(tmp);
    }
    jd_free(d);
}

static void jd_device_unlink(jd_device_t *d) {
    if (d == jd_devices) {
        jd_devices = d->next;
    } else {
        for (jd_device_t *p = jd_devices; p; p = p->next) {
            if (p->next == d) {
                p->next = d->next;
                break;
            }
        }
    }
}

static void jd_device_gc(void) {
    jd_device_t *p, *q;
    while (jd_devices && in_past(jd_devices->_expires)) {
        p = jd_devices;
        jd_devices = p->next;
        jd_device_free(p);
    }
    for (p = jd_devices; p && p->next; p = p->next) {
        if (in_past(p->next->_expires)) {
            q = p->next;
            p->next = q->next;
            jd_device_free(q);
        }
    }
}

jd_device_t *jd_device_lookup(uint64_t device_identifier) {
    jd_device_t *p = jd_devices;
    while (p) {
        if (p->device_identifier == device_identifier)
            return p;
        p = p->next;
    }
    return NULL;
}

int jd_service_send_cmd(jd_device_service_t *serv, uint16_t service_command, const void *data,
                        size_t datasize) {
    jd_packet_t *pkt = jd_alloc(JD_SERIAL_FULL_HEADER_SIZE + datasize);
    pkt->flags = JD_FRAME_FLAG_COMMAND;
    pkt->device_identifier = jd_service_parent(serv)->device_identifier;
    pkt->service_index = serv->service_index;
    pkt->service_command = service_command;
    pkt->service_size = datasize;
    memcpy(pkt->data, data, datasize);
    int r = jd_send_pkt(pkt);
    jd_free(pkt);
    return r;
}

jd_register_query_t *jd_service_query_lookup(jd_device_service_t *serv, int reg_code) {
    jd_register_query_t *q = jd_service_parent(serv)->_queries;
    int idx = serv->service_index;
    while (q) {
        if (q->reg_code == reg_code && (q->service_index & JD_SERVICE_INDEX_MASK) == idx)
            return q;
    }
    return NULL;
}

const jd_register_query_t *jd_service_query(jd_device_service_t *serv, int reg_code,
                                            int refresh_ms) {
    jd_register_query_t *q = jd_service_query_lookup(serv, reg_code);
    if (!q) {
        q = jd_alloc(sizeof(jd_register_query_t));
        memset(q, 0, sizeof(*q));
        jd_device_t *dev = jd_service_parent(serv);
        q->reg_code = reg_code;
        q->service_index = serv->service_index;
        q->next = dev->_queries;
        dev->_queries = q;
    }
    if (!jd_register_not_implemented(q) &&
        (!q->last_query || (refresh_ms && in_past(q->last_query + refresh_ms * 1000)))) {
        jd_service_send_cmd(serv, JD_GET(reg_code), NULL, 0);
        q->last_query = now;
    }
    return q;
}

void jd_client_process(void) {
    if (jd_should_sample(&next_gc, 300 * 1000)) {
        jd_device_gc();
    }
}

static void handle_register(jd_device_service_t *serv, jd_packet_t *pkt) {
    if (pkt->service_size >= 4 && pkt->service_command == JD_CMD_COMMAND_NOT_IMPLEMENTED) {
        uint16_t cmd = *(uint16_t *)pkt->data;
        if (JD_IS_GET(cmd)) {
            jd_register_query_t *q = jd_service_query_lookup(serv, JD_REG_CODE(cmd));
            if (q && !jd_register_not_implemented(q)) {
                q->service_index |= 0x80;
                jd_client_emit_event(JD_CLIENT_EV_SERVICE_REGISTER_NOT_IMPLEMENTED, serv, q);
            }
        }
    } else if (JD_IS_GET(pkt->service_command)) {
        jd_register_query_t *q = jd_service_query_lookup(serv, JD_REG_CODE(pkt->service_command));
        if (q) {
            int chg = 0;
            if (q->resp_size != pkt->service_size ||
                memcmp(pkt->data, jd_register_data(q), q->resp_size))
                chg = 1;

            if (pkt->service_size > JD_REGISTER_QUERY_MAX_INLINE) {
                if (q->resp_size != pkt->service_size) {
                    if (q->resp_size > JD_REGISTER_QUERY_MAX_INLINE)
                        jd_free(q->value.buffer);
                    q->value.buffer = jd_alloc(pkt->service_size);
                }
            }
            q->resp_size = pkt->service_size;
            memcpy((void *)jd_register_data(q), pkt->data, q->resp_size);
            if (chg)
                jd_client_emit_event(JD_CLIENT_EV_SERVICE_REGISTER_CHANGED, serv, q);
        }
    }
}

void jd_client_handle_packet(jd_packet_t *pkt) {
    if (pkt->flags & JD_FRAME_FLAG_IDENTIFIER_IS_SERVICE_CLASS) {
        jd_client_emit_event(JD_CLIENT_EV_BROADCAST_PACKET, NULL, pkt);
        return;
    }

    jd_device_t *dev = jd_device_lookup(pkt->device_identifier);
    jd_device_service_t *serv = NULL;

    if (dev) {
        int idx = pkt->service_index & JD_SERVICE_INDEX_MASK;
        if (idx < dev->num_services) {
            serv = &dev->services[idx];
        }
    }

    if (pkt->flags & JD_FRAME_FLAG_COMMAND) {

    } else {
        // report
        if (pkt->service_index == 0 && pkt->service_command == 0) {
            if (!dev)
                dev = jd_device_alloc(pkt);
            uint16_t new_flags = *(uint16_t *)pkt->data;
            if (dev->announce_flags != new_flags) {
                if ((dev->announce_flags & JD_CONTROL_ANNOUNCE_FLAGS_RESTART_COUNTER_STEADY) >
                    (new_flags & JD_CONTROL_ANNOUNCE_FLAGS_RESTART_COUNTER_STEADY)) {
                    jd_client_emit_event(JD_CLIENT_EV_DEVICE_RESET, dev, pkt);
                    jd_device_unlink(dev);
                    jd_device_free(dev);
                    dev = jd_device_alloc(pkt);
                } else {
                    dev->announce_flags = new_flags;
                }
            }
            serv = &dev->services[0]; // re-assign in case we (re)allocated dev
            dev->_expires = now + EXPIRES_USEC;
        }

        if (serv)
            handle_register(serv, pkt);
    }

    if (serv)
        jd_client_emit_event(JD_CLIENT_EV_SERVICE_PACKET, serv, pkt);
    else
        jd_client_emit_event(JD_CLIENT_EV_NON_SERVICE_PACKET, dev, pkt);
}
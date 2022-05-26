#include "jacs_internal.h"
#include "jacdac/dist/c/timeseriesaggregator.h"

#include <math.h>

// all values in ms
#define MAX_WINDOW (12 * 3600 * 1000)
#define CONT_WINDOW_0 5000
#define CONT_WINDOW_FINAL 60000
#define CONT_WINDOW_STEP 2000 // every 5000ms
#define DISCRETE_WINDOW 1000

#define LOG(msg, ...) DMESG("tsagg: " msg, ##__VA_ARGS__)
#define MS(n) ((n) << 10)

typedef struct tsagg_series {
    struct tsagg_series *next;

    jd_device_service_t *service;
    uint8_t prev_streaming_samples;
    uint8_t streaming_samples;

    uint8_t mode;

    char *label;

    uint32_t window_ms;
    uint32_t previous_sample;

    double v_previous;

    jd_timeseries_aggregator_stored_report_t acc;
} tsagg_series_t;

struct srv_state {
    SRV_COMMON;

    // regs:
    uint32_t default_contiuous_window;
    uint32_t default_discrete_window;
    uint8_t fast_start;

    // not regs:
    uint32_t next_local_streaming_samples;
    uint32_t next_global_streaming_samples;

    const jacscloud_api_t *api;
    tsagg_series_t *series;

    uint32_t current_cont_window;
};

REG_DEFINITION(                                              //
    tsagg_regs,                                              //
    REG_SRV_COMMON,                                          //
    REG_U32(JD_TIMESERIES_AGGREGATOR_REG_CONTINUOUS_WINDOW), //
    REG_U32(JD_TIMESERIES_AGGREGATOR_REG_DISCRETE_WINDOW),   //
    REG_U8(JD_TIMESERIES_AGGREGATOR_REG_FAST_START),         //
)

const jacs_packed_service_desc_t *jacs_get_packed_service_desc(uint32_t service_class) {
    int l = 0;
    int r = jacs_num_packed_service_descs - 1;
    while (l <= r) {
        int m = (l + r) / 2;
        if (jacs_packed_service_descs[m].service_class < service_class)
            l = m + 1;
        else if (jacs_packed_service_descs[m].service_class > service_class)
            r = m - 1;
        else
            return &jacs_packed_service_descs[m];
    }
    return NULL;
}

static void log_series(tsagg_series_t *s, const char *msg) {
    if (s->service) {
        char shortid[5];
        jd_device_short_id(shortid, jd_service_parent(s->service)->device_identifier);
        if (s->label)
            LOG("%s (%s/%d): %s", s->label, shortid, s->service->service_index, msg);
        else
            LOG("%s/%d: %s", shortid, s->service->service_index, msg);
    } else {
        LOG("%s: %s", s->label, msg);
    }
}

static uint32_t get_window_ms(srv_t *state, tsagg_series_t *ts) {
    if (ts->window_ms != 0)
        return ts->window_ms;

    if (ts->mode == JD_TIMESERIES_AGGREGATOR_DATA_MODE_DISCRETE)
        return state->default_discrete_window;
    if (state->fast_start) {
        if (!state->current_cont_window)
            state->current_cont_window = CONT_WINDOW_0;
        return state->current_cont_window;
    } else {
        return state->default_contiuous_window;
    }
}

static tsagg_series_t *add_series(srv_t *state, jd_device_service_t *serv, uint8_t mode) {
    tsagg_series_t *ts = jd_alloc(sizeof(tsagg_series_t));

    ts->mode = mode;
    ts->service = serv;

    ts->next = state->series;
    state->series = ts;

    ts->v_previous = NAN;
    ts->acc.start_time = now_ms;
    ts->acc.end_time = now_ms + get_window_ms(state, ts);

    if (serv) {
        jd_role_t *r = jd_role_by_service(serv);
        if (r)
            ts->label = jd_strdup(r->name);
    }

    log_series(ts, "created");

    return ts;
}

static tsagg_series_t *lookup_series(srv_t *state, const char *label) {
    for (tsagg_series_t *ts = state->series; ts; ts = ts->next) {
        if (ts->label && strcmp(ts->label, label) == 0)
            return ts;
    }
    return NULL;
}

static tsagg_series_t *lookup_or_add_series(srv_t *state, const char *label) {
    tsagg_series_t *ts = lookup_series(state, label);
    if (ts)
        return ts;
    ts = add_series(state, NULL, JD_TIMESERIES_AGGREGATOR_DATA_MODE_CONTINUOUS);
    ts->label = jd_strdup(label);
    return ts;
}

static void dev_created(srv_t *state, jd_device_t *dev) {
    for (unsigned i = 1; i < dev->num_services; ++i) {
        const jacs_packed_service_desc_t *desc =
            jacs_get_packed_service_desc(dev->services[i].service_class);
        if (!desc)
            continue;
        add_series(state, &dev->services[i], desc->mode);
    }
}

static void free_series(tsagg_series_t *ts) {
    jd_free(ts->label);
    jd_free(ts);
}

static void dev_destroyed(srv_t *state, jd_device_t *dev) {
    tsagg_series_t *n, *prev = NULL;
    tsagg_series_t *ts = state->series;

    while (ts) {
        n = ts->next;
        if (ts->service && jd_service_parent(ts->service) == dev) {
            if (prev == NULL)
                state->series = n;
            else
                prev->next = n;
            free_series(ts);
            ts = n;
        } else {
            prev = ts;
            ts = n;
        }
    }
}

static void role_changed(srv_t *state, jd_device_service_t *serv, jd_role_t *role) {
    // we don't care about un-assignments
    if (!serv)
        return;

    for (tsagg_series_t *ts = state->series; ts; ts = ts->next) {
        int eq = ts->label == NULL ? 0 : strcmp(ts->label, role->name) == 0;
        if (ts->service == serv) {
            if (!eq) {
                jd_free(ts->label);
                ts->label = jd_strdup(role->name);
            }
        } else if (eq) {
            jd_free(ts->label);
            ts->label = NULL;
        }
    }
}

static void series_reset(tsagg_series_t *ts) {
    ts->acc.min = ts->acc.max = ts->v_previous;
    ts->acc.avg = 0;
    ts->acc.num_samples = 0;
    ts->previous_sample = ts->acc.start_time;
}

static void series_update(srv_t *state, tsagg_series_t *ts, double v) {
    if (isnan(v))
        return;
    if (isnan(ts->v_previous)) {
        // first sample
        ts->v_previous = v;
        series_reset(ts);
    }

    ts->acc.num_samples++;

    if (in_past_ms(ts->acc.end_time)) {
        uint32_t delta = ts->acc.end_time - ts->previous_sample;
        ts->acc.avg += ts->v_previous * delta;
        ts->previous_sample = ts->acc.end_time;
    } else {
        uint32_t delta = now_ms - ts->previous_sample;
        ts->acc.avg += ts->v_previous * delta;
        ts->previous_sample = now_ms;
    }

    if (v > ts->acc.max)
        ts->acc.max = v;
    if (v < ts->acc.min)
        ts->acc.min = v;
    ts->v_previous = v;
}

static void dev_packet(srv_t *state, jd_device_service_t *serv, jd_packet_t *pkt) {
    if (pkt->service_command != JD_GET(JD_REG_READING))
        return;

    const jacs_packed_service_desc_t *desc = jacs_get_packed_service_desc(serv->service_class);
    if (desc == NULL)
        return;

    for (tsagg_series_t *ts = state->series; ts; ts = ts->next) {
        if (ts->service == serv) {
            if (ts->streaming_samples)
                ts->streaming_samples--;
            double v = jacs_read_number(pkt->data, pkt->service_size, desc->numfmt);
            series_update(state, ts, v);
        }
    }
}

void tsagg_client_ev(void *_state, int event_id, void *arg0, void *arg1) {
    srv_t *state = _state;

    switch (event_id) {
    case JD_CLIENT_EV_DEVICE_CREATED:
        dev_created(state, arg0);
        break;

    case JD_CLIENT_EV_DEVICE_DESTROYED:
        dev_destroyed(state, arg0);
        break;

    case JD_CLIENT_EV_SERVICE_PACKET:
        dev_packet(state, arg0, arg1);
        break;

    case JD_CLIENT_EV_ROLE_CHANGED:
        role_changed(state, arg0, arg1);
        break;
    }
}

static bool should_send(tsagg_series_t *ts) {
    if (ts->acc.num_samples == 0)
        return 0;
    switch (ts->mode) {
    case JD_TIMESERIES_AGGREGATOR_DATA_MODE_CONTINUOUS:
        return 1;
    case JD_TIMESERIES_AGGREGATOR_DATA_MODE_DISCRETE:
        return ts->acc.min != ts->acc.max;
    default:
        oops();
    }
}

void tsagg_process(srv_t *state) {
    // every 5s set streaming_samples of everyone who didn't stream anything
    if (jd_should_sample(&state->next_global_streaming_samples, MS(5000))) {
        if (state->current_cont_window) {
            if (state->current_cont_window < state->default_contiuous_window)
                state->current_cont_window += CONT_WINDOW_STEP;
            if (state->current_cont_window > state->default_contiuous_window)
                state->current_cont_window = state->default_contiuous_window;
        }

        for (tsagg_series_t *ts = state->series; ts; ts = ts->next) {
            if (ts->streaming_samples == ts->prev_streaming_samples)
                ts->streaming_samples = 0;
            ts->prev_streaming_samples = ts->streaming_samples;
        }
    }

    if (jd_should_sample(&state->next_local_streaming_samples, MS(100))) {
        for (tsagg_series_t *ts = state->series; ts; ts = ts->next) {
            if (ts->service && ts->streaming_samples < 50) {
                uint8_t num = 250;
                if (jd_service_send_cmd(ts->service, JD_SET(JD_REG_STREAMING_SAMPLES), &num,
                                        sizeof(num)) != 0) {
                    // sending failed; try again soon
                    state->next_local_streaming_samples = now + MS(8);
                    break;
                }
                ts->streaming_samples = num;
                log_series(ts, "set streaming samples");
            }
        }
    }

    for (tsagg_series_t *ts = state->series; ts; ts = ts->next) {
        if (in_future_ms(ts->acc.end_time))
            continue;

        // log_series(ts, "window expire");
        if (should_send(ts)) {
            uint32_t delta = ts->acc.end_time - ts->previous_sample;
            double sum = ts->acc.avg + ts->v_previous * delta;
            jd_timeseries_aggregator_stored_report_t data = ts->acc;
            data.avg = sum / (ts->acc.end_time - ts->acc.start_time);
            if (jd_send(state->service_index, JD_TIMESERIES_AGGREGATOR_CMD_STORED, &data,
                        sizeof(data)) != 0)
                continue; // try again later

            if (state->api->agg_upload(ts->label, ts->service, ts->mode, &data) != 0)
                log_series(ts, "upload failed");
        } else if (ts->acc.num_samples == 0) {
            // no values
            ts->v_previous = NAN;
        }

        ts->acc.start_time = ts->acc.end_time;
        ts->acc.end_time += get_window_ms(state, ts);
        series_reset(ts);
    }
}

void tsagg_handle_packet(srv_t *state, jd_packet_t *pkt) {
    switch (pkt->service_command) {
    case JD_TIMESERIES_AGGREGATOR_CMD_CLEAR:
        return;

    default:
        switch (service_handle_register_final(state, pkt, tsagg_regs)) {
        case JD_TIMESERIES_AGGREGATOR_REG_CONTINUOUS_WINDOW:
            if (state->default_contiuous_window > MAX_WINDOW)
                state->default_contiuous_window = MAX_WINDOW;
            break;
        case JD_TIMESERIES_AGGREGATOR_REG_DISCRETE_WINDOW:
            if (state->default_discrete_window > MAX_WINDOW)
                state->default_discrete_window = MAX_WINDOW;
            break;
        }
        return;
    }
}

static srv_t *tsagg_state;

SRV_DEF(tsagg, JD_SERVICE_CLASS_TIMESERIES_AGGREGATOR);
void tsagg_init(const jacscloud_api_t *cloud_api) {
    SRV_ALLOC(tsagg);
    tsagg_state = state;
    state->api = cloud_api;
    state->fast_start = 1;
    state->default_contiuous_window = CONT_WINDOW_FINAL;
    state->default_discrete_window = DISCRETE_WINDOW;
    jd_client_subscribe(tsagg_client_ev, state);
}

void tsagg_update(const char *name, double v) {
    srv_t *state = tsagg_state;
    tsagg_series_t *ts = lookup_or_add_series(state, name);
    series_update(state, ts, v);
}
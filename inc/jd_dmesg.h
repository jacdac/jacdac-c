// Copyright (c) Jacdac Organization.
// Licensed under the MIT license.

#ifndef JD_DMESG_H
#define JD_DMESG_H

#include "jd_config.h"

#if JD_DMESG_BUFFER_SIZE > 0

struct JacdacLogStore {
    volatile uint32_t ptr;
    char buffer[JD_DMESG_BUFFER_SIZE];
};
extern struct JacdacLogStore jacdacLogStore;

__attribute__((format(printf, 1, 2))) void jd_dmesg(const char *format, ...);
void jd_vdmesg(const char *format, va_list ap);
void jd_dmesg_write(const char *data, unsigned len);

// read-out data from dmesg buffer
unsigned jd_dmesg_read(void *dst, unsigned space, uint32_t *state);
// similar, but at most until next '\n'
unsigned jd_dmesg_read_line(void *dst, unsigned space, uint32_t *state);
// get the oldest possible starting point (for *state above)
uint32_t jd_dmesg_startptr(void);
static inline uint32_t jd_dmesg_currptr(void) {
    return jacdacLogStore.ptr;
}

#ifndef JD_DMESG
#define JD_DMESG jd_dmesg
#endif

#else

#ifndef JD_DMESG
#define JD_DMESG(...) ((void)0)
#endif

#endif

#ifndef JD_LOG
#define JD_LOG JD_DMESG
#endif

#endif

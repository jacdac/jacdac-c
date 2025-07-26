#include "jd_protocol.h"
#include "jd_dmesg.h"

#if JD_DMESG_BUFFER_SIZE > 0

#if JD_DMESG_BUFFER_SIZE < 256
#error "Too small JD_DMESG buffer"
#endif

struct JacdacLogStore jacdacLogStore;

JD_FAST
void jd_dmesg_write(const char *msg, unsigned len) {
    target_disable_irq();
    unsigned space = sizeof(jacdacLogStore.buffer) - jacdacLogStore.ptr;
    if (space < len + 1) {
        memcpy(jacdacLogStore.buffer + jacdacLogStore.ptr, msg, space);
        len -= space;
        msg += space;
        if (len + 2 > sizeof(jacdacLogStore.buffer))
            len = 1; // shouldn't happen
        jacdacLogStore.ptr = 0;
    }
    memcpy(jacdacLogStore.buffer + jacdacLogStore.ptr, msg, len);
    jacdacLogStore.ptr += len;
    jacdacLogStore.buffer[jacdacLogStore.ptr] = 0;
    target_enable_irq();
}

void jd_vdmesg(const char *format, va_list ap) {
    char tmp[JD_DMESG_LINE_BUFFER];
    jd_vsprintf(tmp, sizeof(tmp) - 1, format, ap);
    int len = strlen(tmp);
#if JD_LORA || JD_ADVANCED_STRING
    while (len && (tmp[len - 1] == '\r' || tmp[len - 1] == '\n'))
        len--;
#endif
    tmp[len++] = '\n';
    jd_dmesg_write(tmp, len);
}

void jd_dmesg(const char *format, ...) {
    va_list arg;
    va_start(arg, format);
    jd_vdmesg(format, arg);
    va_end(arg);
}

JD_FAST
uint32_t jd_dmesg_startptr(void) {
    target_disable_irq();
    // if we wrapped around already, we start at ptr+1 (buf[ptr] is always '\0')
    uint32_t curr_ptr = jacdacLogStore.ptr + 1;
    // if we find '\0' at ptr+1, it means we didn't wrap around yet - start at 0
    if (curr_ptr >= sizeof(jacdacLogStore.buffer) || jacdacLogStore.buffer[curr_ptr] == 0)
        curr_ptr = 0;
    target_enable_irq();
    return curr_ptr;
}

JD_FAST
unsigned jd_dmesg_read(void *dst, unsigned space, uint32_t *state) {
    target_disable_irq();
    if (*state >= sizeof(jacdacLogStore.buffer))
        *state = 0;
    uint32_t curr_ptr = jacdacLogStore.ptr;
    if (curr_ptr < *state)
        curr_ptr = sizeof(jacdacLogStore.buffer);
    unsigned towrite = curr_ptr - *state;
    if (towrite > 0) {
        if (towrite > space)
            towrite = space;
        memcpy(dst, jacdacLogStore.buffer + *state, towrite);
        *state += towrite;
    }
    target_enable_irq();

    return towrite;
}

unsigned jd_dmesg_read_line(void *dst, unsigned space, uint32_t *state) {
    target_disable_irq();
    if (*state >= sizeof(jacdacLogStore.buffer))
        *state = 0;
    uint32_t curr_ptr = jacdacLogStore.ptr;
    uint32_t sp = *state;
    unsigned len = 0;
    if (sp != curr_ptr) {
        char *dp = dst;
        char *endp = dp + space - 1;
        while (dp < endp) {
            char c = jacdacLogStore.buffer[sp++];
            if (c == 0 || c == '\n')
                break;
            *dp++ = c;
            if (sp == sizeof(jacdacLogStore.buffer))
                sp = 0;
            if (sp == curr_ptr)
                break;
        }
        *state = sp;
        len = dp - (char *)dst;
        *dp = 0;
    }
    target_enable_irq();

    return len;
}

#endif

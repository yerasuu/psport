// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PS_RUNTIME_USBD_H
#define PS_RUNTIME_USBD_H
/* libSceUsbd (the SDK's libusb) as both runtime_devices.c, with no device plugged in, and the
 * emulated toy pad (runtime_toypad.c) answer it. */
#include <stdint.h>
#define USBD_INVALID_ARG ((int32_t)0x80240002)
#define USBD_NO_DEVICE ((int32_t)0x80240004)
#define USBD_NOT_FOUND ((int32_t)0x80240005)
#define USBD_NO_MEMORY ((int32_t)0x8024000B)
#define USBD_TRANSFER_TYPE_INTERRUPT 3
#define USBD_DEFAULT_EVENT_TIMEOUT_SECONDS 60
/* libusb_transfer: handle, flags, endpoint, type, timeout, status, length, actual length, callback,
 * user data, buffer, iso packet count, then 12 bytes per iso packet descriptor. */
typedef struct {
    void *dev_handle;
    uint8_t flags, endpoint, type, padding;
    uint32_t timeout;
    int32_t status, length, actual_length;
    void *callback, *user_data;
    uint8_t *buffer;
    int32_t num_iso_packets;
    uint8_t iso_packet_desc[];
} UsbdTransfer;
_Static_assert(__builtin_offsetof(UsbdTransfer,iso_packet_desc)==60,"libusb_transfer layout");
/* struct timeval */
typedef struct { int64_t tv_sec, tv_usec; } UsbdTimeval;
/* How long event handling may wait: libusb's 60 s without a timeout; 0 when the timeout is invalid. */
static inline int usbd_event_wait_ns(const UsbdTimeval *p_timeout, uint64_t *p_out_wait_ns) {
    *p_out_wait_ns=(uint64_t)USBD_DEFAULT_EVENT_TIMEOUT_SECONDS*1000000000u;
    if (!p_timeout) return 1;
    if (p_timeout->tv_sec<0 || p_timeout->tv_usec<0 || p_timeout->tv_usec>=1000000) return 0;
    *p_out_wait_ns=(uint64_t)p_timeout->tv_sec*1000000000u+(uint64_t)p_timeout->tv_usec*1000u;
    return 1;
}
#endif

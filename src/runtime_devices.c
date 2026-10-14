/* Peripherals a console without them answers for: no USB device is plugged in (libSceUsbd, the
 * SDK's libusb) and no PlayStation Camera is attached. The libraries run; device lists are empty
 * and anything that needs a device fails with the library's no-device error. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <time.h>

/* ---- libSceUsbd ---- */
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
static pthread_mutex_t s_usbd_events_lock=PTHREAD_MUTEX_INITIALIZER;
static int s_is_usbd_logged;
static ABI int32_t usbd_init(void) {
    if (!s_is_usbd_logged) { s_is_usbd_logged=1; puts("Runtime: Usbd initialized; no USB device is connected"); }
    return 0;
}
static ABI int32_t usbd_exit(void) { return 0; }
static ABI int64_t usbd_get_device_list(void ***p_out_list) {
    if (!p_out_list) return USBD_INVALID_ARG;
    *p_out_list=calloc(1,sizeof(void *));
    return *p_out_list ? 0 : USBD_NO_MEMORY;
}
static ABI void usbd_free_device_list(void **pp_list,int32_t should_unref) { (void)should_unref; free(pp_list); }
static ABI int32_t usbd_no_device(void) { return USBD_NO_DEVICE; }
static ABI uint8_t usbd_device_number(void *p_device) { (void)p_device; return 0; }
static ABI void usbd_close(void *p_handle) { (void)p_handle; }
static ABI void usbd_free_config_descriptor(void *p_config) { (void)p_config; }
static ABI UsbdTransfer *usbd_alloc_transfer(int32_t c_iso_packets) {
    if (c_iso_packets<0) return NULL;
    UsbdTransfer *p_transfer=calloc(1,sizeof(UsbdTransfer)+(size_t)c_iso_packets*12+4);
    if (p_transfer) p_transfer->num_iso_packets=c_iso_packets;
    return p_transfer;
}
static ABI void usbd_free_transfer(UsbdTransfer *p_transfer) { free(p_transfer); }
static ABI void usbd_fill_interrupt_transfer(UsbdTransfer *p_transfer,void *p_device_handle,uint8_t endpoint,uint8_t *p_buffer,
                                             int32_t cb_buffer,void *pfn_callback,void *p_user_data,uint32_t timeout_ms) {
    if (!p_transfer) return;
    p_transfer->dev_handle=p_device_handle; p_transfer->endpoint=endpoint; p_transfer->type=USBD_TRANSFER_TYPE_INTERRUPT;
    p_transfer->timeout=timeout_ms; p_transfer->buffer=p_buffer; p_transfer->length=cb_buffer;
    p_transfer->user_data=p_user_data; p_transfer->callback=pfn_callback;
}
/* A transfer is never in flight: none could be submitted. */
static ABI int32_t usbd_cancel_transfer(UsbdTransfer *p_transfer) { (void)p_transfer; return USBD_NOT_FOUND; }
/* struct timeval. With no device nothing completes, so event handling waits out its timeout as
 * libusb does. */
typedef struct { int64_t tv_sec, tv_usec; } UsbdTimeval;
static int32_t wait_for_usbd_events(const UsbdTimeval *p_timeout) {
    struct timespec wait={USBD_DEFAULT_EVENT_TIMEOUT_SECONDS,0};
    if (p_timeout) {
        if (p_timeout->tv_sec<0 || p_timeout->tv_usec<0 || p_timeout->tv_usec>=1000000) return USBD_INVALID_ARG;
        wait.tv_sec=(time_t)p_timeout->tv_sec; wait.tv_nsec=(long)p_timeout->tv_usec*1000;
    }
    const uint64_t wait_start_ns=runtime_wait_clock();
    while (nanosleep(&wait,&wait)) {}
    runtime_wait_note(3,runtime_wait_clock()-wait_start_ns);
    return 0;
}
static ABI int32_t usbd_handle_events(void) { return wait_for_usbd_events(NULL); }
static ABI int32_t usbd_handle_events_timeout(const UsbdTimeval *p_timeout) { return wait_for_usbd_events(p_timeout); }
static ABI int32_t usbd_handle_events_locked(const UsbdTimeval *p_timeout) { return wait_for_usbd_events(p_timeout); }
static ABI void usbd_lock_events(void) { pthread_mutex_lock(&s_usbd_events_lock); }
static ABI void usbd_unlock_events(void) { pthread_mutex_unlock(&s_usbd_events_lock); }
static ABI int32_t usbd_event_handling_ok(void) { return 1; }

/* ---- libSceCamera ---- */
#define CAMERA_NOT_OPEN ((int32_t)0x802E0004)
#define CAMERA_NOT_CONNECTED ((int32_t)0x802E000E)
static ABI int32_t camera_is_attached(int32_t i_device) { (void)i_device; return 0; }
static ABI int32_t camera_open(int32_t h_user,int32_t type,int32_t i_device,const void *p_param) {
    (void)h_user; (void)type; (void)i_device; (void)p_param;
    return CAMERA_NOT_CONNECTED;
}
static ABI int32_t camera_not_open(void) { return CAMERA_NOT_OPEN; }

static const RuntimeExport exports[]={
    {"sceUsbdInit",usbd_init}, {"sceUsbdExit",usbd_exit},
    {"sceUsbdGetDeviceList",usbd_get_device_list}, {"sceUsbdFreeDeviceList",usbd_free_device_list},
    {"sceUsbdOpen",usbd_no_device}, {"sceUsbdClose",usbd_close},
    {"sceUsbdGetDeviceDescriptor",usbd_no_device}, {"sceUsbdGetConfigDescriptor",usbd_no_device},
    {"sceUsbdFreeConfigDescriptor",usbd_free_config_descriptor}, {"sceUsbdGetConfiguration",usbd_no_device},
    {"sceUsbdGetStringDescriptorAscii",usbd_no_device}, {"sceUsbdGetMaxPacketSize",usbd_no_device},
    {"sceUsbdGetBusNumber",usbd_device_number}, {"sceUsbdGetDeviceAddress",usbd_device_number},
    {"sceUsbdCheckConnected",usbd_no_device}, {"sceUsbdKernelDriverActive",usbd_no_device},
    {"sceUsbdDetachKernelDriver",usbd_no_device}, {"sceUsbdAttachKernelDriver",usbd_no_device},
    {"sceUsbdClaimInterface",usbd_no_device}, {"sceUsbdReleaseInterface",usbd_no_device},
    {"sceUsbdControlTransfer",usbd_no_device}, {"sceUsbdSubmitTransfer",usbd_no_device},
    {"sceUsbdAllocTransfer",usbd_alloc_transfer}, {"sceUsbdFreeTransfer",usbd_free_transfer},
    {"sceUsbdFillInterruptTransfer",usbd_fill_interrupt_transfer}, {"sceUsbdCancelTransfer",usbd_cancel_transfer},
    {"sceUsbdHandleEvents",usbd_handle_events}, {"sceUsbdHandleEventsTimeout",usbd_handle_events_timeout},
    {"sceUsbdHandleEventsLocked",usbd_handle_events_locked},
    {"sceUsbdLockEvents",usbd_lock_events}, {"sceUsbdUnlockEvents",usbd_unlock_events},
    {"sceUsbdEventHandlingOk",usbd_event_handling_ok},
    {"sceCameraIsAttached",camera_is_attached}, {"sceCameraOpen",camera_open},
    {"sceCameraClose",camera_not_open}, {"sceCameraStart",camera_not_open}, {"sceCameraSetConfig",camera_not_open},
    {"sceCameraSetVideoSync",camera_not_open}, {"sceCameraGetFrameData",camera_not_open},
    {"sceCameraIsValidFrameData",camera_not_open},
};
uintptr_t runtime_devices_resolve(const char *sz_name) { return RUNTIME_LOOKUP(exports,sz_name); }
#else
uintptr_t runtime_devices_resolve(const char *sz_name) { (void)sz_name; return 0; }
#endif

// USB devices on the Xbox's OHCI controller (ohci.c).
#pragma once
#include <stdint.h>

enum { USB_SETUP = 0, USB_OUT = 1, USB_IN = 2 };
#define USB_NAK (-2)
#define USB_STALL (-1)

typedef struct usb_endpoint {
    uint8_t *pos;      // where the next data goes or comes from
    int remain;        // bytes left in this transfer
    uint8_t buf[4096];
} usb_endpoint;

typedef struct usb_device usb_device;
struct usb_device {
    const uint8_t *dev_desc, *conf_desc;   // the configuration with its interface and endpoints
    const uint8_t *const *strings;
    int nstrings;
    // a request that is not a standard one: set up ep[0] for the data stage; <0 stalls
    int (*vendor_request)(usb_device *d, const uint8_t *setup);
    // a bulk packet: bytes moved, or USB_NAK
    int (*bulk)(usb_device *d, int ep, int pid, uint8_t *buf, int size);
    void (*status_stage)(usb_device *d);   // after an OUT data stage
    usb_endpoint ep[16];
    int address, new_address, configured, ctl_in;
};

void ohci_plug(int port, usb_device *d);   // port 0..3
void ohci_frame(void);                      // every millisecond
uint32_t ohci_mmio_read(uint32_t o, int size);
void ohci_mmio_write(uint32_t o, uint32_t v, int size);
void usb_device_reset(usb_device *d);
int usb_transfer(usb_device *d, int ep, int pid, uint8_t *buf, int size);

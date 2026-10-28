#ifndef ZENITH_USB_H
#define ZENITH_USB_H

/*
 * What class drivers outside usb.c (USB networking) get from the xHCI
 * driver. Probing, control transfers and completions all run on the USB
 * thread; usb_submit() may be called from any thread.
 */
#include <kernel.h>

struct usb_dev;
struct usb_ep;

struct usb_ep_desc {
    uint8_t addr;           /* bEndpointAddress, bit 7 = IN */
    uint8_t attr;           /* bmAttributes: 2 bulk, 3 interrupt */
    uint16_t mps;
    uint8_t interval;
};

/* completion of one submitted transfer: cc 1 = success, 13 = short packet */
typedef void (*usb_done_fn)(void *ctx, struct usb_ep *ep, uintptr_t tag, int cc, uint32_t actual);

/* control transfer on endpoint 0 (USB thread only); returns 0 on success */
int  usb_ctrl(struct usb_dev *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *data, uint16_t len);
bool usb_open_endpoints(struct usb_dev *d, const struct usb_ep_desc *e, int n, struct usb_ep **out, usb_done_fn done,
                        void *ctx);
bool usb_submit(struct usb_ep *ep, uint64_t phys, uint32_t len, uintptr_t tag);
int  usb_speed(struct usb_dev *d);                      /* 1 full, 2 low, 3 high, 4 super */
void usb_set_name(struct usb_dev *d, const char *name);
void usb_set_detach(struct usb_dev *d, void (*detach)(void *ctx), void *ctx);

/* class drivers */
bool usbnet_probe(struct usb_dev *d, const uint8_t *dev_desc);   /* true if it took the device */
void usbnet_service(void);                                        /* called by the USB thread */

#endif

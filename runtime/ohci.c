// The Xbox's USB host controller (OHCI, 0xFED00000, bus interrupt 1) and a
// minimal USB device layer for what is plugged into it: on a Chihiro, the
// baseboard's two AN2131 microcontrollers (baseboard.c).
//
// Written from the OpenHCI 1.0a specification. Every millisecond the
// interrupt thread calls ohci_frame(): the frame number goes to the HCCA,
// the periodic list for this frame, then the control and bulk lists are
// run (every pending transfer descriptor of every endpoint, not one a
// frame), retired descriptors go on the done queue, and the done queue is
// written back once WritebackDoneHead is clear.
#include "xbox.h"
#include "usb.h"
#include <pthread.h>
#include <string.h>

#define OHCI_BASE 0xFED00000u
#define OHCI_LEVEL 1
#define NPORTS 4

enum { HcRevision, HcControl, HcCommandStatus, HcInterruptStatus, HcInterruptEnable, HcInterruptDisable, HcHCCA,
       HcPeriodCurrentED, HcControlHeadED, HcControlCurrentED, HcBulkHeadED, HcBulkCurrentED, HcDoneHead,
       HcFmInterval, HcFmRemaining, HcFmNumber, HcPeriodicStart, HcLSThreshold, HcRhDescriptorA,
       HcRhDescriptorB, HcRhStatus, HcRhPortStatus1, NREGS = HcRhPortStatus1 + NPORTS };

// HcControl
#define PLE (1u << 2)
#define CLE (1u << 4)
#define BLE (1u << 5)
// HcCommandStatus
#define HCR 1u
#define CLF 2u
#define BLF 4u
// interrupts
#define WDH (1u << 1)
#define SF (1u << 2)
#define RHSC (1u << 6)
#define MIE (1u << 31)
// port status
#define CCS 1u
#define PES 2u
#define PSS 4u
#define POCI 8u
#define PRS 0x10u
#define PPS 0x100u
#define LSDA 0x200u
#define CSC 0x10000u
#define PESC 0x20000u
#define PSSC 0x40000u
#define POCIC 0x80000u
#define PRSC 0x100000u

static uint32_t regs[NREGS];
static usb_device *port_dev[NPORTS];
static int reset_delay[NPORTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int started;
// the done queue's interrupt delay (OHCI 6.4.4): the smallest DelayInterrupt
// of the descriptors retired since the last write-back, counted down a frame
// at a time; 7 is "no interrupt pending". The done queue is written back (and
// WDH raised) only when it reaches 0, so descriptors marked DI 7 (a
// transfer's setup and data stages) wait for the one that asks to interrupt.
static int done_delay = 7;

static uint32_t ram32(uint32_t pa) { uint32_t v; memcpy(&v, M + dma_addr(pa), 4); return v; }
static void ram32w(uint32_t pa, uint32_t v) { memcpy(M + dma_addr(pa), &v, 4); }

// back to power-on state: nothing plugged in
void ohci_reset(void)
{
    pthread_mutex_lock(&lock);
    memset(regs, 0, sizeof regs);
    memset(port_dev, 0, sizeof port_dev);
    memset(reset_delay, 0, sizeof reset_delay);
    started = 0;
    done_delay = 7;
    pthread_mutex_unlock(&lock);
}

void ohci_plug(int port, usb_device *d)
{
    if (port < 0 || port >= NPORTS) return;
    port_dev[port] = d;
    d->address = 0;
}

static void ohci_start(void)
{
    started = 1;
    regs[HcRevision] = 0x10;
    regs[HcFmInterval] = 0x2edf;
    regs[HcLSThreshold] = 0x628;
    regs[HcRhDescriptorA] = NPORTS;
    regs[HcControl] = 0;   // UsbReset
    for (int p = 0; p < NPORTS; p++)
        regs[HcRhPortStatus1 + p] = port_dev[p] ? (CCS | CSC) : 0;
}

static void update_irq(void)
{
    if ((regs[HcInterruptEnable] & MIE) && (regs[HcInterruptStatus] & regs[HcInterruptEnable] & 0x7fffffff))
        ke_raise_interrupt(OHCI_LEVEL);
}

static usb_device *dev_at(int addr)
{
    for (int p = 0; p < NPORTS; p++)
        if (port_dev[p] && (regs[HcRhPortStatus1 + p] & PES) && port_dev[p]->address == addr) return port_dev[p];
    return NULL;
}

uint32_t ohci_mmio_read(uint32_t o, int size)
{
    pthread_mutex_lock(&lock);
    if (!started) ohci_start();
    uint32_t v = (o / 4 < NREGS) ? regs[o / 4] : 0;
    // CH_VERBOSE=4: every register read but the frame counters (the interrupt status, its enables)
    if (hle_verbose > 3 && o / 4 != HcFmNumber && o / 4 != HcFmRemaining)
        hle_log("ohci: r %02x = %08x (status %08x enable %08x)\n", o, v, regs[HcInterruptStatus], regs[HcInterruptEnable]);
    pthread_mutex_unlock(&lock);
    (void)size;
    return v;
}

void ohci_mmio_write(uint32_t o, uint32_t v, int size)
{
    (void)size;
    pthread_mutex_lock(&lock);
    if (!started) ohci_start();
    uint32_t r = o / 4;
    if (hle_verbose > 2 && (r != HcInterruptStatus || hle_verbose > 3))
        hle_log("ohci: w %02x = %08x (status %08x enable %08x)\n", o, v, regs[HcInterruptStatus], regs[HcInterruptEnable]);
    if (r >= NREGS) goto out;
    switch (r) {
    case HcRevision: case HcFmRemaining: case HcFmNumber: case HcDoneHead:
        break;
    case HcControl: {
        uint32_t old = regs[r];
        regs[r] = v;
        if ((old >> 6 & 3) == 0 && (v >> 6 & 3) != 0) {   // out of reset: the root hub has news
            regs[HcInterruptStatus] |= RHSC;
        }
        break;
    }
    case HcCommandStatus:
        if (v & HCR) {
            uint32_t keep_ports[NPORTS];
            memcpy(keep_ports, &regs[HcRhPortStatus1], sizeof keep_ports);
            ohci_start();
            regs[HcControl] = 3u << 6;   // UsbSuspend after a software reset
            memcpy(&regs[HcRhPortStatus1], keep_ports, sizeof keep_ports);
            v &= ~HCR;
        }
        regs[r] |= v;
        break;
    case HcInterruptStatus:
        regs[r] &= ~v;
        break;
    case HcInterruptEnable:
        regs[r] |= v;
        break;
    case HcInterruptDisable:
        regs[HcInterruptEnable] &= ~v;
        break;
    case HcRhStatus:
        break;   // power switching: always on
    default:
        if (r >= HcRhPortStatus1) {
            int p = r - HcRhPortStatus1;
            uint32_t s = regs[r], old = s;
            if (!port_dev[p]) break;
            if (v & CCS) s &= ~PES;                       // ClearPortEnable
            if (v & PES) s |= PES;                        // SetPortEnable
            if (v & PSS) s |= PSS;
            if (v & POCI) s &= ~PSS;
            if (v & PRS) {                                // SetPortReset: done 10 ms later
                s |= PRS;
                port_dev[p]->address = 0;
                usb_device_reset(port_dev[p]);
                reset_delay[p] = 10;
            }
            if (v & PPS) s |= PPS;
            if (v & LSDA) s &= ~PPS;
            s &= ~(v & (CSC | PESC | PSSC | POCIC | PRSC));   // change bits: write 1 to clear
            regs[r] = s;
            if (s != old && (s & (CSC | PESC | PSSC | POCIC | PRSC))) regs[HcInterruptStatus] |= RHSC;
            break;
        }
        regs[r] = v;
        break;
    }
    update_irq();
out:
    pthread_mutex_unlock(&lock);
}

// ---- transfers ------------------------------------------------------------------------------------
// the byte at offset i of a TD's buffer: CBP to the end of its page, then BE's page
static uint32_t td_byte_addr(uint32_t cbp, uint32_t be, uint32_t i)
{
    uint32_t first = 0x1000 - (cbp & 0xfff);
    return i < first ? cbp + i : (be & ~0xfffu) + (i - first);
}

static uint32_t td_len(uint32_t cbp, uint32_t be)
{
    if (!cbp) return 0;
    if ((cbp ^ be) & ~0xfffu) return (0x1000 - (cbp & 0xfff)) + (be & 0xfff) + 1;
    return be - cbp + 1;
}

// run an endpoint's descriptors; returns 1 if any was processed
static int service_ed(uint32_t ed)
{
    uint32_t d0 = ram32(ed), tailp = ram32(ed + 4) & ~0xfu, headp = ram32(ed + 8);
    if ((d0 >> 14 & 1) || (headp & 1)) return 0;   // skip, or halted
    if (d0 >> 15 & 1) return 0;                     // isochronous: not used here
    int fa = d0 & 0x7f, en = d0 >> 7 & 0xf, dir = d0 >> 11 & 3, mps = d0 >> 16 & 0x7ff;
    if (!mps) mps = 8;
    int any = 0;
    while ((headp & ~0xfu) != tailp) {
        uint32_t td = headp & ~0xfu;
        uint32_t t0 = ram32(td), cbp = ram32(td + 4), next = ram32(td + 8) & ~0xfu, be = ram32(td + 12);
        int pid = dir == 1 ? USB_OUT : dir == 2 ? USB_IN : (int)(t0 >> 19 & 3);   // 0 setup, 1 out, 2 in
        uint32_t len = td_len(cbp, be), done = 0;
        int toggle = (t0 >> 25 & 1) ? (t0 >> 24 & 1) : (headp >> 1 & 1);
        int cc = 0, nak = 0;
        usb_device *dev = dev_at(fa);
        if (!dev) {
            cc = 5;   // DeviceNotResponding
        } else {
            uint8_t buf[1024];
            do {
                int chunk = (int)(len - done) < mps ? (int)(len - done) : mps;
                if (pid == USB_SETUP) chunk = len < 8 ? (int)len : 8;
                if (pid != USB_IN)
                    for (int i = 0; i < chunk; i++) buf[i] = M[dma_addr(td_byte_addr(cbp, be, done + i))];
                int n = usb_transfer(dev, en, pid, buf, chunk);
                if (n == USB_NAK) {
                    nak = 1;
                    break;
                }
                if (n == USB_STALL) {
                    cc = 4;
                    break;
                }
                if (pid == USB_IN)
                    for (int i = 0; i < n; i++) M[dma_addr(td_byte_addr(cbp, be, done + i))] = buf[i];
                done += n;
                toggle ^= 1;
                if (n < chunk) {   // short packet
                    if (!(t0 >> 18 & 1) && done < len) cc = 9;   // DataUnderrun unless rounding is allowed
                    break;
                }
            } while (done < len);
        }
        if (hle_verbose > 2)
            hle_log("ohci: ed 0x%x fa %d ep %d pid %d len %u done %u cc %d%s\n", ed, fa, en, pid, len, done, cc, nak ? " nak" : "");
        if (nak) {
            if (done) ram32w(td + 4, td_byte_addr(cbp, be, done));
            break;   // try again next frame
        }
        any = 1;
        // retire: completion code, toggle, buffer pointer
        t0 = (t0 & 0x00ffffffu & ~(3u << 24)) | (uint32_t)cc << 28 | (2u | toggle) << 24;
        ram32w(td, t0);
        ram32w(td + 4, done >= len ? 0 : td_byte_addr(cbp, be, done));
        ram32w(td + 8, regs[HcDoneHead]);
        regs[HcDoneHead] = td;
        int di = cc ? 0 : (int)(t0 >> 21 & 7);   // an error interrupts at once
        if (di < done_delay) done_delay = di;
        headp = next | (uint32_t)toggle << 1 | (cc ? 1u : 0u);
        ram32w(ed + 8, headp);
        if (cc) break;   // halted
    }
    return any;
}

static void run_list(uint32_t head, uint32_t filled_bit)
{
    if (!(regs[HcCommandStatus] & filled_bit)) return;
    regs[HcCommandStatus] &= ~filled_bit;
    int guard = 0;
    for (uint32_t ed = head; ed && guard < 256; ed = ram32(ed + 12) & ~0xfu, guard++) {
        if (hle_verbose > 3) hle_log("ohci: list ed 0x%x: %08x %08x %08x %08x\n", ed, ram32(ed), ram32(ed + 4), ram32(ed + 8), ram32(ed + 12));
        service_ed(ed);
    }
}

void ohci_frame(void)
{
    pthread_mutex_lock(&lock);
    if (!started) goto out;
    for (int p = 0; p < NPORTS; p++)
        if (reset_delay[p] && !--reset_delay[p]) {
            regs[HcRhPortStatus1 + p] = (regs[HcRhPortStatus1 + p] & ~PRS) | PRSC | PES;
            regs[HcInterruptStatus] |= RHSC;
        }
    if ((regs[HcControl] >> 6 & 3) == 2) {   // UsbOperational
        uint32_t hcca = regs[HcHCCA] & ~0xffu;
        if (done_delay > 0 && done_delay < 7) done_delay--;   // a new frame
        regs[HcFmNumber] = (regs[HcFmNumber] + 1) & 0xffff;
        if (hcca) {
            ram32w(hcca + 0x80, regs[HcFmNumber]);
            if (regs[HcControl] & PLE) {
                int guard = 0;
                for (uint32_t ed = ram32(hcca + 4 * (regs[HcFmNumber] & 31)) & ~0xfu; ed && guard < 256;
                     ed = ram32(ed + 12) & ~0xfu, guard++)
                    service_ed(ed);
            }
        }
        if (regs[HcControl] & CLE) run_list(regs[HcControlHeadED] & ~0xfu, CLF);
        if (regs[HcControl] & BLE) run_list(regs[HcBulkHeadED] & ~0xfu, BLF);
        regs[HcInterruptStatus] |= SF;
        if (regs[HcDoneHead] && done_delay == 0 && !(regs[HcInterruptStatus] & WDH) && hcca) {
            uint32_t other = regs[HcInterruptStatus] & regs[HcInterruptEnable] & ~WDH;
            ram32w(hcca + 0x84, regs[HcDoneHead] | (other ? 1 : 0));
            regs[HcDoneHead] = 0;
            regs[HcInterruptStatus] |= WDH;
            done_delay = 7;
        }
    }
    update_irq();
out:
    pthread_mutex_unlock(&lock);
}

// ---- the device layer: standard requests on endpoint 0 ---------------------------------------------
void usb_device_reset(usb_device *d)
{
    d->address = 0;
    d->new_address = -1;
    d->configured = 0;
    for (int i = 0; i < 16; i++) d->ep[i].pos = NULL, d->ep[i].remain = 0;
}

static int standard_request(usb_device *d, const uint8_t *s)
{
    uint8_t type = s[0], req = s[1];
    uint16_t value = s[2] | s[3] << 8, length = s[6] | s[7] << 8;
    usb_endpoint *e = &d->ep[0];
    if ((type & 0x60) != 0) return 0;   // class or vendor
    e->pos = e->buf;
    e->remain = 0;
    switch (req) {
    case 0:   // GET_STATUS
        e->buf[0] = e->buf[1] = 0;
        e->remain = 2;
        break;
    case 1: case 3:   // CLEAR_FEATURE, SET_FEATURE
        break;
    case 5:   // SET_ADDRESS: takes effect after the status stage
        d->new_address = value & 0x7f;
        break;
    case 6: {   // GET_DESCRIPTOR
        int t = value >> 8, i = value & 0xff;
        const uint8_t *p = NULL;
        int n = 0;
        if (t == 1) p = d->dev_desc, n = d->dev_desc[0];
        else if (t == 2) p = d->conf_desc, n = d->conf_desc[2] | d->conf_desc[3] << 8;
        else if (t == 3 && i < d->nstrings) p = d->strings[i], n = d->strings[i][0];
        if (!p) return -1;
        memcpy(e->buf, p, n);
        e->remain = n;
        break;
    }
    case 8:   // GET_CONFIGURATION
        e->buf[0] = (uint8_t)d->configured;
        e->remain = 1;
        break;
    case 9:   // SET_CONFIGURATION
        d->configured = value & 0xff;
        break;
    case 10:  // GET_INTERFACE
        e->buf[0] = 0;
        e->remain = 1;
        break;
    case 11:  // SET_INTERFACE
        break;
    default:
        return 0;
    }
    if (e->remain > length) e->remain = length;
    return 1;
}

int usb_transfer(usb_device *d, int ep, int pid, uint8_t *buf, int size)
{
    usb_endpoint *e = &d->ep[ep & 15];
    if (pid == USB_SETUP) {
        if (ep != 0 || size < 8) return USB_STALL;
        d->ctl_in = buf[0] >> 7;
        int r = standard_request(d, buf);
        if (r < 0) return USB_STALL;
        if (r == 0 && d->vendor_request(d, buf) < 0) return USB_STALL;
        return 8;
    }
    if (ep == 0) {
        // the status stage: a zero-length packet against the data direction
        if (size == 0 || (pid == USB_IN) != d->ctl_in) {
            if (d->new_address >= 0) {
                d->address = d->new_address;
                d->new_address = -1;
            }
            if (d->ctl_in == 0 && d->status_stage) d->status_stage(d);
            return 0;
        }
        int n = size < e->remain ? size : e->remain;
        if (n < 0) n = 0;
        if (pid == USB_IN) {
            if (n && e->pos) memcpy(buf, e->pos, n);
        } else if (n && e->pos) {
            memcpy(e->pos, buf, n);
        }
        if (e->pos) e->pos += n;
        e->remain -= n;
        return pid == USB_IN ? n : size;
    }
    return d->bulk(d, ep, pid, buf, size);
}

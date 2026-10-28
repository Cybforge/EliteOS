/*
 * Intel 8255x (EtherExpress PRO/100, 82557/8/9, 82550/1, 82562 in the
 * ICH chipsets). The chip runs two engines: the command unit walks a list
 * of command blocks (configure, set address, transmit), the receive unit
 * fills a ring of receive frame descriptors. Both in "simplified" mode,
 * where the data sits right behind each descriptor.
 */
#include <kernel.h>
#include <dev.h>
#include <mm.h>
#include <spinlock.h>
#include <x86.h>
#include "../netdev.h"

#define SCB_STATUS 0x00
#define SCB_ACK    0x01
#define SCB_CMD    0x02
#define SCB_MASK   0x03
#define SCB_PTR    0x04
#define SCB_PORT   0x08
#define SCB_EEPROM 0x0E
#define SCB_MDI    0x10

#define CU_START   0x10
#define CU_RESUME  0x20
#define CU_BASE    0x60
#define RU_START   0x01
#define RU_BASE    0x06

#define CB_C   0x8000       /* status: complete */
#define CB_OK  0x2000
#define CB_EL  0x8000       /* command: end of list */
#define CB_S   0x4000       /* command: suspend afterwards */
#define CMD_IA     1
#define CMD_CONFIG 2
#define CMD_TX     4

#define NRX 128
#define NTX 32
#define FRAME 1520

struct cb {
    uint16_t status, command;
    uint32_t link;
    union {
        struct { uint32_t tbd; uint16_t count; uint8_t threshold, tbd_num; uint8_t data[FRAME]; } PACKED tx;
        uint8_t mac[6];
        uint8_t config[22];
    };
} PACKED;

struct rfd {
    uint16_t status, command;
    uint32_t link, rbd;
    uint16_t count, size;
    uint8_t data[FRAME];
} PACKED;

/* the classic 82557 configuration, with "discard short frames" off */
static const uint8_t config_bytes[22] = {
    22, 0x08, 0, 0, 0, 0, 0x32, 0x02, 1, 0, 0x2E, 0, 0x60, 0, 0xF2, 0x48, 0, 0x40, 0xF2, 0x80, 0x3F, 0x05,
};

struct e100 {
    struct netdev nd;
    volatile uint8_t *csr;
    struct cb *tx;
    struct rfd *rx;
    unsigned tx_cur, rx_cur;
    bool tx_started;
    spinlock_t lock;
};

static bool scb_idle(struct e100 *c)
{
    for (int i = 0; i < 100000; i++) {
        if (!mmio_r8(c->csr, SCB_CMD)) return true;
        cpu_relax();
    }
    return false;
}

static void scb_cmd(struct e100 *c, uint8_t cmd, uint64_t ptr, bool with_ptr)
{
    scb_idle(c);
    if (with_ptr) mmio_w32(c->csr, SCB_PTR, (uint32_t)ptr);
    mmio_w8(c->csr, SCB_CMD, cmd);
}

static bool e100_send(struct netdev *nd, const void *data, size_t len)
{
    struct e100 *c = nd->priv;
    if (len > FRAME) return false;
    spin_lock(&c->lock);
    struct cb *cb = &c->tx[c->tx_cur];
    if (cb->command) {
        for (int i = 0; i < 100000 && !(((volatile struct cb *)cb)->status & CB_C); i++) cpu_relax();
        if (!(cb->status & CB_C)) { spin_unlock(&c->lock); return false; }
    }
    cb->status = 0;
    cb->tx.tbd = 0xFFFFFFFF;
    cb->tx.count = (uint16_t)(len | 0x8000);        /* EOF */
    cb->tx.threshold = 0xE0;
    cb->tx.tbd_num = 0;
    memcpy(cb->tx.data, data, len);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cb->command = CMD_TX | CB_S;
    /* let the command unit run on from the previous block */
    struct cb *prev = &c->tx[(c->tx_cur + NTX - 1) % NTX];
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    prev->command &= (uint16_t)~CB_S;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint8_t cus = mmio_r8(c->csr, SCB_STATUS) >> 6;
    if (!c->tx_started || cus == 0) {
        scb_cmd(c, CU_START, V2P(cb), true);
        c->tx_started = true;
    } else {
        scb_cmd(c, CU_RESUME, 0, false);
    }
    c->tx_cur = (c->tx_cur + 1) % NTX;
    spin_unlock(&c->lock);
    return true;
}

static int e100_poll(struct netdev *nd, void *buf, size_t cap)
{
    struct e100 *c = nd->priv;
    int got = 0;
    spin_lock(&c->lock);
    for (;;) {
        struct rfd *r = &c->rx[c->rx_cur];
        uint16_t st = ((volatile struct rfd *)r)->status;
        if (!(st & CB_C)) break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        /* (QEMU leaves the EOF/F bits of the count clear) */
        if ((st & CB_OK) && (r->count & 0x3FFF)) {
            got = MIN((int)(r->count & 0x3FFF), (int)cap);
            memcpy(buf, r->data, (size_t)got);
        }
        /* recycle: this one becomes the end of the list */
        r->status = 0;
        r->count = 0;
        r->command = CB_EL;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        c->rx[(c->rx_cur + NRX - 1) % NRX].command = 0;
        c->rx_cur = (c->rx_cur + 1) % NRX;
        if (got) break;
    }
    /* the receive unit stops when it reaches the end of the list: restart it */
    uint8_t rus = (mmio_r8(c->csr, SCB_STATUS) >> 2) & 0xF;
    if (rus != 4 && !(c->rx[c->rx_cur].status & CB_C)) scb_cmd(c, RU_START, V2P(&c->rx[c->rx_cur]), true);
    mmio_w8(c->csr, SCB_ACK, 0xFF);
    spin_unlock(&c->lock);
    return got;
}

static int mdi_read(struct e100 *c, int reg)
{
    mmio_w32(c->csr, SCB_MDI, ((uint32_t)reg << 16) | (1u << 21) | (2u << 26));
    for (int i = 0; i < 2000; i++) {
        uint32_t v = mmio_r32(c->csr, SCB_MDI);
        if (v & (1u << 28)) return (int)(v & 0xFFFF);
        udelay(5);
    }
    return -1;
}

static bool e100_link(struct netdev *nd)
{
    struct e100 *c = nd->priv;
    spin_lock(&c->lock);
    int bmsr = mdi_read(c, MII_BMSR);
    spin_unlock(&c->lock);
    return bmsr < 0 || (bmsr & BMSR_LINK);
}

/* EEPROM lines in the SCB: SK bit 0, CS bit 1, DI bit 2, DO bit 3 */
static void ee_out(void *ctx, bool cs, bool sk, bool di)
{
    struct e100 *c = ctx;
    mmio_w8(c->csr, SCB_EEPROM, (uint8_t)((sk ? 1 : 0) | (cs ? 2 : 0) | (di ? 4 : 0)));
    mmio_r8(c->csr, SCB_STATUS);
}

static bool ee_in(void *ctx)
{
    struct e100 *c = ctx;
    return mmio_r8(c->csr, SCB_EEPROM) & 8;
}

/* run one action command synchronously */
static bool action(struct e100 *c, struct cb *cb)
{
    cb->status = 0;
    cb->command |= CB_EL;
    cb->link = (uint32_t)V2P(cb);
    scb_cmd(c, CU_START, V2P(cb), true);
    for (int i = 0; i < 100000; i++) {
        if (((volatile struct cb *)cb)->status & CB_C) return true;
        udelay(1);
    }
    return false;
}

static struct netdev *e100_attach(struct pci_dev *p, const struct nic_id *id)
{
    uint64_t bar = pci_bar_addr(p, 0);
    if (!bar || pci_bar_is_io(p, 0)) return NULL;
    struct e100 *c = kzalloc(sizeof(*c));
    c->lock = (spinlock_t)SPINLOCK_INIT("eepro100");
    c->csr = vmm_map_mmio(bar, 4096);
    pci_enable_busmaster(p);

    mmio_w32(c->csr, SCB_PORT, 0);                  /* software reset */
    udelay(20);
    mmio_w8(c->csr, SCB_MASK, 1);                   /* mask interrupts */
    scb_cmd(c, CU_BASE, 0, true);                   /* linear addressing */
    scb_cmd(c, RU_BASE, 0, true);

    struct microwire ee = { c, ee_out, ee_in, 0 };
    for (int i = 0; i < 3; i++) {
        uint16_t w = microwire_read(&ee, i);
        c->nd.mac[i * 2] = (uint8_t)w;
        c->nd.mac[i * 2 + 1] = (uint8_t)(w >> 8);
    }

    c->tx = dma_alloc(NTX * sizeof(struct cb), NULL);
    c->rx = dma_alloc(NRX * sizeof(struct rfd), NULL);
    struct cb *cmd = dma_alloc(sizeof(struct cb), NULL);
    if (!c->tx || !c->rx || !cmd) return NULL;

    cmd->command = CMD_CONFIG;
    memcpy(cmd->config, config_bytes, sizeof(config_bytes));
    if (!action(c, cmd)) klog("eepro100: configure command did not complete");
    memset(cmd, 0, sizeof(*cmd));
    cmd->command = CMD_IA;
    memcpy(cmd->mac, c->nd.mac, 6);
    if (!action(c, cmd)) klog("eepro100: address setup did not complete");

    for (int i = 0; i < NTX; i++) c->tx[i].link = (uint32_t)V2P(&c->tx[(i + 1) % NTX]);
    for (int i = 0; i < NRX; i++) {
        c->rx[i].link = (uint32_t)V2P(&c->rx[(i + 1) % NRX]);
        c->rx[i].rbd = 0xFFFFFFFF;
        c->rx[i].size = FRAME;
        c->rx[i].command = i == NRX - 1 ? CB_EL : 0;
    }
    scb_cmd(c, RU_START, V2P(&c->rx[0]), true);

    /* the generic IDs cover several chips; the revision tells them apart */
    const char *chip = NULL;
    if (id->device == 0x1229 || id->device == 0x1209) {
        uint8_t rev = p->rev;
        chip = rev <= 3 ? "82557" : rev <= 5 ? "82558" : rev <= 8 ? "82559" : rev == 9 ? "82559ER"
             : rev <= 0x0E ? "82550" : "82551";
    }
    if (chip) snprintf(c->nd.name, sizeof(c->nd.name), "Intel %s PRO/100", chip);
    else snprintf(c->nd.name, sizeof(c->nd.name), "Intel %s", id->model);
    c->nd.send = e100_send;
    c->nd.poll = e100_poll;
    c->nd.link = e100_link;
    c->nd.priv = c;
    return &c->nd;
}

static const struct nic_id ids[] = {
    { 0x8086, 0x1229, "82557/8/9/0/1 PRO/100" },
    { 0x8086, 0x1209, "82559ER/82551IT PRO/100" },
    { 0x8086, 0x1029, "82559 PRO/100" },
    { 0x8086, 0x1030, "82559 InBusiness 10/100" },
    { 0x8086, 0x1059, "82551QM PRO/100 M (mobile)" },
    { 0x8086, 0x2449, "82562EM PRO/100 VE (ICH2)" },
    { 0x8086, 0x2459, "82562 PRO/100 VE (ICH2, mobile)" },
    { 0x8086, 0x245D, "82562 PRO/100 VM (ICH2)" },
    { 0x8086, 0x1031, "82562ET PRO/100 VE (ICH3)" },
    { 0x8086, 0x1032, "82562ET PRO/100 VE (ICH3)" },
    { 0x8086, 0x1033, "82562EM PRO/100 VM (ICH3)" },
    { 0x8086, 0x1034, "82562EM PRO/100 VM (ICH3)" },
    { 0x8086, 0x1038, "82562 PRO/100 VM (ICH3)" },
    { 0x8086, 0x1039, "82562ET PRO/100 VE (ICH4)" },
    { 0x8086, 0x103A, "82562 PRO/100 VE (ICH4)" },
    { 0x8086, 0x103D, "82562 PRO/100 VE (ICH4, mobile)" },
    { 0x8086, 0x103E, "82562 PRO/100 VM (ICH4, mobile)" },
    { 0x8086, 0x1050, "82562EZ PRO/100 VE (ICH5)" },
    { 0x8086, 0x1051, "82562 PRO/100 VE (ICH5)" },
    { 0x8086, 0x1064, "82562ET/EZ/GT/GZ PRO/100 VE (ICH6)" },
    { 0x8086, 0x1065, "82562 PRO/100 VE (ICH6)" },
    { 0x8086, 0x1068, "82562ET PRO/100 VE (ICH6)" },
    { 0x8086, 0x1069, "82562EM/EX/GX PRO/100 VM (ICH6)" },
    { 0x8086, 0x1091, "82562GX PRO/100 VE (ICH7)" },
    { 0x8086, 0x1092, "82562G PRO/100 VE (ICH7)" },
    { 0x8086, 0x1094, "82562GT PRO/100 VE (ICH7)" },
    { 0x8086, 0x27DC, "82801G PRO/100 VE (ICH7)" },
    { 0 },
};

const struct nic_driver drv_eepro100 = { "eepro100", "Intel PRO/100 (8255x, 82562 in ICH2-ICH7)", ids, e100_attach };

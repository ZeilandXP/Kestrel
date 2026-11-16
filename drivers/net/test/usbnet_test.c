/* drivers/net/test/usbnet_test.c -- the USB network framing (drivers/net/usbnet.c) on
 * the build machine: CDC NCM blocks, which no emulator offers a device for,
 * and RNDIS and ECM transfers, built and taken apart.
 *
 *   make test-usbnet
 *
 * The driver is compiled in with the kernel's calls stubbed out. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#define main usbnet_unused_main
#include "../usbnet.c"
#undef main

/* ---- stubs ---------------------------------------------------------------------------------- */
static int nframes;
static uint8_t frames[16][ETH_FRAME_MAX + 4];
static size_t flen[16];
void net_rx_frame(struct netif *nif, const void *f, size_t len)
{
    (void)nif;
    if (nframes < 16) { memcpy(frames[nframes], f, len); flen[nframes] = len; }
    nframes++;
}
uint64_t net_lock(void) { return 0; }
void net_unlock(uint64_t f) { (void)f; }
void netif_register(struct netif *n) { (void)n; }
void netif_carrier(struct netif *n, bool on) { (void)n; (void)on; }
void net_hotplug(struct netif *n) { (void)n; }
void arp_flush_if(struct netif *n) { (void)n; }
void kprintf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void *kzalloc(size_t n) { return calloc(1, n); }
void kfree(void *p) { free(p); }
void task_sleep_ms(uint64_t ms) { (void)ms; }
int xhci_control_transfer(uint8_t a, uint8_t b, uint8_t c, uint16_t d, uint16_t e, uint16_t f, void *g)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; return XHCI_ERR_NODEV; }
int xhci_control_transfer_len(uint8_t a, uint8_t b, uint8_t c, uint16_t d, uint16_t e, uint16_t f, void *g, uint16_t *h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; return XHCI_ERR_NODEV; }
int xhci_net_get(int i, struct xhci_net_info *o) { (void)i; (void)o; return XHCI_ERR_NODEV; }
bool xhci_net_alive(uint8_t s, uint32_t g) { (void)s; (void)g; return false; }
int xhci_net_start(uint8_t s) { (void)s; return XHCI_ERR_NODEV; }
int xhci_net_rx(uint8_t s, const uint8_t **d, uint32_t *l) { (void)s; (void)d; (void)l; return 0; }
void xhci_net_rx_release(uint8_t s) { (void)s; }
int xhci_net_tx(uint8_t s, const void *b, uint32_t l) { (void)s; (void)b; (void)l; return XHCI_OK; }

/* ---- checks -------------------------------------------------------------------------------- */
static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("usbnet_test:  %s  %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

static void frame(uint8_t *f, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++) f[i] = (uint8_t)(seed + i * 7);
}

static bool same(int i, const uint8_t *f, size_t len) { return i < nframes && flen[i] == len && !memcmp(frames[i], f, len); }

int main(void)
{
    static struct usbnet u;
    uint8_t f1[1514], f2[60], f3[600];
    frame(f1, sizeof f1, 1); frame(f2, sizeof f2, 2); frame(f3, sizeof f3, 3);

    /* NCM: one frame per block, as sent; the device's alignment kept */
    u.proto = XHCI_NET_NCM; u.ndp_div = 4; u.ndp_rem = 0; u.ntb_out_max = 16384;
    uint32_t n = wrap(&u, f1, sizeof f1);
    CHECK(n == 28 + sizeof f1 && rd32(u.tx) == NTH16_SIG && rd16(u.tx + 8) == n && rd16(u.tx + 10) == 12 &&
          rd32(u.tx + 12) == NDP16_SIG && rd16(u.tx + 20) == 28 && rd16(u.tx + 22) == sizeof f1, "NCM: an NTB16 around a 1514-byte frame");
    static uint8_t blk[XHCI_NET_TX_SIZE];
    memcpy(blk, u.tx, n);
    nframes = 0;
    CHECK(unwrap(&u, blk, n) && nframes == 1 && same(0, f1, sizeof f1), "NCM: the block taken apart again");
    u.ndp_div = 16; u.ndp_rem = 2;
    n = wrap(&u, f2, sizeof f2);
    CHECK(rd16(u.tx + 20) % 16 == 2 && n == rd16(u.tx + 20) + sizeof f2, "NCM: datagram placed at divisor 16, remainder 2");
    u.ntb_out_max = 1000;
    CHECK(wrap(&u, f1, sizeof f1) == 0, "NCM: a frame over the device's block size is refused");
    CHECK(u.seq == 2, "NCM: sequence numbers count the blocks sent");

    /* NCM: a block as a device sends it: three datagrams over two NDPs */
    static uint8_t b[4096];
    memset(b, 0, sizeof b);
    wr32(b, NTH16_SIG); wr16(b + 4, 12); wr16(b + 6, 7); wr16(b + 10, 16);
    wr32(b + 16, NDP16_SIG); wr16(b + 20, 20); wr16(b + 22, 40);         /* NDP 1: two datagrams, then the next NDP */
    wr16(b + 24, 64); wr16(b + 26, sizeof f2);
    wr16(b + 28, 128); wr16(b + 30, sizeof f3);
    wr32(b + 40, 0x314D434E); wr16(b + 44, 16); wr16(b + 46, 0);         /* NDP 2: "NCM1" (CRC kind), one datagram */
    wr16(b + 48, 800); wr16(b + 50, sizeof f2);
    memcpy(b + 64, f2, sizeof f2); memcpy(b + 128, f3, sizeof f3); memcpy(b + 800, f2, sizeof f2);
    wr16(b + 8, 800 + sizeof f2);
    nframes = 0;
    CHECK(unwrap(&u, b, 1024) && nframes == 3 && same(0, f2, sizeof f2) && same(1, f3, sizeof f3) && same(2, f2, sizeof f2),
          "NCM: three datagrams in two chained NDPs");
    /* malformed blocks */
    wr16(b + 28, 4000);                                                  /* past the block: skipped */
    nframes = 0;
    CHECK(unwrap(&u, b, 1024) && nframes == 2, "NCM: a datagram pointing past the block is skipped");
    wr16(b + 22, 16);                                                    /* an NDP chained to itself */
    nframes = 0;
    unwrap(&u, b, 1024);
    CHECK(nframes <= 8, "NCM: an NDP chain that loops ends");
    b[0] ^= 0xff;
    nframes = 0;
    CHECK(!unwrap(&u, b, 1024) && nframes == 0, "NCM: a block without the NCMH signature is refused");
    nframes = 0;
    CHECK(!unwrap(&u, b, 8) && nframes == 0, "NCM: a truncated block is refused");

    /* RNDIS */
    u.proto = XHCI_NET_RNDIS;
    n = wrap(&u, f1, sizeof f1);
    CHECK(n == RNDIS_HDR + sizeof f1 && rd32(u.tx) == 1 && rd32(u.tx + 4) == n && rd32(u.tx + 8) == 36 && rd32(u.tx + 12) == sizeof f1,
          "RNDIS: a packet message around a frame");
    uint8_t two[2 * RNDIS_HDR + 600 + 60 + 8];
    memset(two, 0, sizeof two);
    n = wrap(&u, f3, sizeof f3); memcpy(two, u.tx, n);
    uint32_t n2 = wrap(&u, f2, sizeof f2); memcpy(two + n, u.tx, n2);
    nframes = 0;
    CHECK(unwrap(&u, two, sizeof two) && nframes == 2 && same(0, f3, sizeof f3) && same(1, f2, sizeof f2),
          "RNDIS: two messages in one transfer, zero padding after");
    wr32(two + 4, 5000);
    nframes = 0;
    CHECK(!unwrap(&u, two, sizeof two) && nframes == 0, "RNDIS: a message longer than the transfer is refused");

    /* ECM */
    u.proto = XHCI_NET_ECM;
    n = wrap(&u, f2, sizeof f2);
    nframes = 0;
    CHECK(n == sizeof f2 && unwrap(&u, u.tx, n) && same(0, f2, sizeof f2), "ECM: frames as they are");
    CHECK(!unwrap(&u, f2, 10), "ECM: a runt is refused");

    printf(failures ? "usbnet_test: FAILED (%d)\n" : "usbnet_test: all checks passed\n", failures);
    return failures != 0;
}

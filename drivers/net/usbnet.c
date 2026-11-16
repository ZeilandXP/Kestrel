/* drivers/net/usbnet.c -- USB network functions: phone tethering and USB
 * Ethernet adapters and gadgets, over the xHCI driver's bulk endpoints
 *
 * Three protocols, as the control interface announces (drivers/usb/xhci.c
 * finds them while it reads a device's configurations):
 *   RNDIS    Microsoft's Remote NDIS, what Android phones speak when USB
 *            tethering is switched on. Every frame goes in a 44-byte
 *            RNDIS_PACKET_MSG; the device is set up with control messages
 *            (INITIALIZE, QUERY of the MAC address and frame size, SET of
 *            the packet filter) sent with SEND_ENCAPSULATED_COMMAND and
 *            answered through GET_ENCAPSULATED_RESPONSE.
 *   CDC ECM  plain Ethernet frames, one per bulk transfer (many USB
 *            Ethernet adapters, Linux gadgets, some phones and modems).
 *   CDC NCM  frames gathered in NTB16 blocks (NTH16 + NDP16), what newer
 *            phones and adapters offer; several frames may arrive in one
 *            transfer. Frames are sent one per block.
 * A device that offers more than one (in separate configurations, as Linux
 * gadgets do) gets NCM, then ECM, then RNDIS; kestrel.usbnet=rndis, ecm or
 * ncm prefers one (drivers/usb/xhci.c: plat.net_prefer).
 *
 * Interfaces are named usb0, usb1, ... and come up with DHCP as cards found
 * at boot do (kestrel.net=off leaves them down). The usbd thread
 * (kernel/usbhost.c) calls usbnet_scan() every 50 ms: a device that went
 * away takes its interface's carrier (IFF_RUNNING) with it, and the next one
 * plugged in gets the first idle interface again, with its own MAC address
 * and a new DHCP lease, so a phone that is unplugged and plugged back in
 * comes back as the same interface.
 *
 * Receiving: the xHCI driver keeps 16 transfers of 16 KiB queued; netd
 * drains the completed ones (poll) and gives each buffer back. The tick
 * looks for completed transfers (pending), and the xHCI completion kicks
 * netd too (kernel/usbhost.c: net_rx). Sending copies each frame into one
 * of 32 transmit buffers; with all of them in flight the frame is dropped,
 * as a full ring drops it on a card. */
#include <kernel/net.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/cpu.h>
#include "xhci.h"
#include "usb.h"

#define MAX_USBNET      4

/* class requests (CDC 1.2, NCM 1.0, RNDIS 1.0) */
#define REQ_SEND_ENCAPSULATED       0x00
#define REQ_GET_ENCAPSULATED        0x01
#define REQ_SET_ETH_PACKET_FILTER   0x43
#define REQ_GET_NTB_PARAMETERS      0x80
#define REQ_SET_NTB_INPUT_SIZE      0x86
#define CDC_FILTER_DEFAULT          0x000E      /* directed, broadcast, all multicast */

#define RNDIS_PACKET_MSG            0x00000001
#define RNDIS_INITIALIZE_MSG        0x00000002
#define RNDIS_QUERY_MSG             0x00000004
#define RNDIS_SET_MSG               0x00000005
#define RNDIS_KEEPALIVE_MSG         0x00000008
#define RNDIS_CMPLT                 0x80000000
#define OID_GEN_MAXIMUM_FRAME_SIZE  0x00010106
#define OID_GEN_CURRENT_PACKET_FILTER 0x0001010E
#define OID_802_3_PERMANENT_ADDRESS 0x01010101
#define RNDIS_FILTER_DEFAULT        0x0000000D  /* directed, all multicast, broadcast */
#define RNDIS_HDR                   44

#define NTH16_SIG                   0x484D434Eu /* "NCMH" */
#define NDP16_SIG                   0x304D434Eu /* "NCM0": no CRC */

struct usbnet {
    struct netif nif;
    bool     attached;
    uint8_t  slot, proto, ctrl;
    uint32_t gen;
    uint32_t rndis_id;
    /* NCM: what the device asked for in its transmit blocks */
    uint16_t ndp_div, ndp_rem, ndp_align;
    uint32_t ntb_out_max;
    uint16_t seq;
    uint8_t  tx[XHCI_NET_TX_SIZE];              /* a frame being wrapped (net_lock held) */
    /* counters for /proc/net/nic */
    uint64_t rx_transfers, rx_bad, tx_busy;
    uint32_t attaches;
    char     product[48];
};

static struct usbnet *devs[MAX_USBNET];
static int ndevs;
static struct { uint8_t slot; uint32_t gen; } failed[8];
static unsigned nfailed;

static const char *const proto_name[] = { "?", "RNDIS", "CDC ECM", "CDC NCM" };

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* ---- frames: wrapping and unwrapping ----------------------------------------------------- */
/* Build what goes out for one frame into u->tx; its length, 0 if it cannot. */
static uint32_t wrap(struct usbnet *u, const uint8_t *frame, size_t len)
{
    uint8_t *t = u->tx;
    if (u->proto == XHCI_NET_ECM) {
        if (len >= XHCI_NET_TX_SIZE) return 0;
        memcpy(t, frame, len);
        return (uint32_t)len;
    }
    if (u->proto == XHCI_NET_RNDIS) {
        if (len + RNDIS_HDR >= XHCI_NET_TX_SIZE) return 0;
        memset(t, 0, RNDIS_HDR);
        wr32(t + 0, RNDIS_PACKET_MSG);
        wr32(t + 4, (uint32_t)(RNDIS_HDR + len));
        wr32(t + 8, RNDIS_HDR - 8);             /* DataOffset, from this field */
        wr32(t + 12, (uint32_t)len);
        memcpy(t + RNDIS_HDR, frame, len);
        return (uint32_t)(RNDIS_HDR + len);
    }
    /* NCM: NTH16 (12 bytes), NDP16 at 12 (one datagram and the terminator,
     * 16 bytes), the datagram where the device wants it: offset % divisor
     * == remainder */
    uint32_t div = u->ndp_div ? u->ndp_div : 4, rem = u->ndp_rem % div;
    uint32_t off = 28;
    if (off % div != rem) off += (rem + div - off % div) % div;
    if (off + len >= XHCI_NET_TX_SIZE || (u->ntb_out_max && off + len > u->ntb_out_max)) return 0;
    memset(t, 0, off);
    wr32(t + 0, NTH16_SIG);
    wr16(t + 4, 12);                            /* wHeaderLength */
    wr16(t + 6, u->seq++);
    wr16(t + 8, (uint16_t)(off + len));         /* wBlockLength */
    wr16(t + 10, 12);                           /* wNdpIndex */
    wr32(t + 12, NDP16_SIG);
    wr16(t + 16, 16);                           /* wLength: 8 + two entries */
    wr16(t + 18, 0);                            /* wNextNdpIndex */
    wr16(t + 20, (uint16_t)off);
    wr16(t + 22, (uint16_t)len);
    memcpy(t + off, frame, len);
    return (uint32_t)(off + len);
}

/* Hand every frame in one received transfer to the stack; false if it was not understood. */
static bool unwrap(struct usbnet *u, const uint8_t *p, uint32_t n)
{
    if (u->proto == XHCI_NET_ECM) {
        if (n < ETH_HLEN || n > ETH_FRAME_MAX + 4) return false;
        net_rx_frame(&u->nif, p, n);
        return true;
    }
    if (u->proto == XHCI_NET_RNDIS) {
        bool any = false;
        for (uint32_t off = 0; off + 8 <= n;) {
            uint32_t type = rd32(p + off), mlen = rd32(p + off + 4);
            if (!type && !mlen) break;                      /* padding at the end */
            if (mlen < 8 || mlen > n - off) return any;
            if (type == RNDIS_PACKET_MSG && mlen >= RNDIS_HDR) {
                uint32_t doff = rd32(p + off + 8), dlen = rd32(p + off + 12);
                if (doff + 8 <= mlen && dlen <= mlen - doff - 8 && dlen >= ETH_HLEN && dlen <= ETH_FRAME_MAX) {
                    net_rx_frame(&u->nif, p + off + 8 + doff, dlen);
                    any = true;
                }
            }
            off += mlen;
        }
        return any;
    }
    /* NCM: NTH16, then a chain of NDP16s, each a list of (index, length) */
    if (n < 12 || rd32(p) != NTH16_SIG || rd16(p + 4) != 12) return false;
    uint32_t blen = rd16(p + 8);
    if (blen > n || blen < 12) blen = n;
    bool any = false;
    uint32_t ndp = rd16(p + 10);
    for (int chain = 0; ndp && chain < 8; chain++) {
        if (ndp + 16 > blen || (rd32(p + ndp) & 0x00FFFFFFu) != (NDP16_SIG & 0x00FFFFFFu)) break;   /* NCM0 or NCM1 */
        uint32_t nlen = rd16(p + ndp + 4);
        if (nlen < 16 || ndp + nlen > blen) break;
        for (uint32_t e = ndp + 8; e + 4 <= ndp + nlen; e += 4) {
            uint32_t di = rd16(p + e), dl = rd16(p + e + 2);
            if (!di || !dl) break;
            if (di + dl > blen || dl < ETH_HLEN || dl > ETH_FRAME_MAX + 4) continue;
            net_rx_frame(&u->nif, p + di, dl);
            any = true;
        }
        ndp = rd16(p + ndp + 6);
    }
    return any;
}

/* ---- netif operations (net_lock held) ---------------------------------------------------- */
static int unet_xmit(struct netif *nif, const void *frame, size_t len)
{
    struct usbnet *u = nif->priv;
    if (!u->attached) return -1;
    uint32_t n = wrap(u, frame, len);
    if (!n) return -1;
    int rc = xhci_net_tx(u->slot, u->tx, n);
    if (rc == XHCI_ERR_BUSY) u->tx_busy++;
    return rc == XHCI_OK ? 0 : -1;
}

static void unet_poll(struct netif *nif)
{
    struct usbnet *u = nif->priv;
    if (!u->attached) return;
    const uint8_t *data;
    uint32_t len;
    for (int i = 0; i < XHCI_NET_RX_BUFS * 2 && xhci_net_rx(u->slot, &data, &len) == 1; i++) {
        u->rx_transfers++;
        if (len && !unwrap(u, data, len)) { u->rx_bad++; nif->rx_errors++; }
        xhci_net_rx_release(u->slot);
    }
}

static bool unet_pending(struct netif *nif)
{
    struct usbnet *u = nif->priv;
    const uint8_t *data;
    uint32_t len;
    return u->attached && xhci_net_rx(u->slot, &data, &len) == 1;
}

static bool unet_link(struct netif *nif) { return ((struct usbnet *)nif->priv)->attached; }

static size_t unet_info(struct netif *nif, char *buf, size_t cap)
{
    struct usbnet *u = nif->priv;
    size_t n = (size_t)snprintf(buf, cap, "%s: USB %s \"%s\", %s", nif->name, proto_name[u->proto <= 3 ? u->proto : 0],
                                u->product, u->attached ? "attached" : "unplugged");
    if (u->attached) n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0, " (slot %u)", u->slot);
    n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0, ", plugged in %u time(s)\n"
                          "  %lu receive transfers, %lu not understood, %lu frames dropped with every transmit buffer busy\n",
                          u->attaches, u->rx_transfers, u->rx_bad, u->tx_busy);
    if (u->proto == XHCI_NET_NCM)
        n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0, "  NCM: transmit blocks up to %u bytes, datagrams at %u mod %u\n",
                              u->ntb_out_max, u->ndp_rem, u->ndp_div);
    return n < cap ? n : cap;
}

/* ---- setting a device up (usbd thread: control transfers sleep) --------------------------- */
static int ctl(uint8_t slot, uint8_t reqtype, uint8_t req, uint16_t value, uint16_t index, uint16_t len, void *data)
{
    return xhci_control_transfer(slot, reqtype, req, value, index, len, data);
}
#define CLASS_OUT   (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE)
#define CLASS_IN    (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE)

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* ECM/NCM: the MAC address string ("0A1B2C3D4E5F", UTF-16) */
static bool read_mac_string(uint8_t slot, uint8_t index, uint8_t mac[6])
{
    if (!index) return false;
    uint8_t buf[64];
    memset(buf, 0, sizeof buf);
    if (ctl(slot, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
            (uint16_t)(USB_DT_STRING << 8 | index), 0x0409, sizeof buf, buf) != XHCI_OK) return false;
    if (buf[0] < 2 + 24 || buf[1] != USB_DT_STRING) return false;
    for (int i = 0; i < 6; i++) {
        int h = hexval((char)buf[2 + 4 * i]), l = hexval((char)buf[2 + 4 * i + 2]);
        if (h < 0 || l < 0) return false;
        mac[i] = (uint8_t)(h << 4 | l);
    }
    return true;
}

/* One RNDIS control message and its completion (same request id) in resp. */
static int rndis_command(struct usbnet *u, uint8_t *msg, uint32_t len, uint8_t *resp, uint16_t cap)
{
    uint32_t type = rd32(msg), id = ++u->rndis_id;
    wr32(msg + 8, id);
    if (ctl(u->slot, CLASS_OUT, REQ_SEND_ENCAPSULATED, 0, u->ctrl, (uint16_t)len, msg) != XHCI_OK) return -1;
    /* the device says on its interrupt endpoint when an answer is ready;
     * asking until one comes does as well (Linux does the same) */
    for (int tries = 0; tries < 100; tries++) {
        uint16_t got = 0;
        memset(resp, 0, cap);
        if (xhci_control_transfer_len(u->slot, CLASS_IN, REQ_GET_ENCAPSULATED, 0, u->ctrl, cap, resp, &got) != XHCI_OK)
            return -1;
        if (got >= 16 && rd32(resp) == (type | RNDIS_CMPLT) && rd32(resp + 8) == id)
            return rd32(resp + 12) == 0 ? 0 : -1;               /* status: RNDIS_STATUS_SUCCESS */
        if (got >= 12 && rd32(resp) == RNDIS_KEEPALIVE_MSG) {   /* answer it, then look again */
            uint8_t k[16];
            wr32(k, RNDIS_KEEPALIVE_MSG | RNDIS_CMPLT); wr32(k + 4, 16); memcpy(k + 8, resp + 8, 4); wr32(k + 12, 0);
            ctl(u->slot, CLASS_OUT, REQ_SEND_ENCAPSULATED, 0, u->ctrl, 16, k);
            continue;
        }
        if (got < 8) task_sleep_ms(tries < 10 ? 2 : 20);         /* nothing yet */
    }
    return -1;
}

static int rndis_query(struct usbnet *u, uint32_t oid, void *out, uint32_t want)
{
    /* with a zeroed buffer of the answer's size, as Linux sends: some
     * devices (QEMU's) refuse a query that has none */
    uint8_t m[28 + 48], r[256];
    memset(m, 0, sizeof m);
    wr32(m, RNDIS_QUERY_MSG); wr32(m + 4, sizeof m); wr32(m + 12, oid);
    wr32(m + 16, 48);                           /* InformationBufferLength */
    wr32(m + 20, 20);                           /* InformationBufferOffset, from the request id */
    if (rndis_command(u, m, sizeof m, r, sizeof r)) return -1;
    uint32_t blen = rd32(r + 16), boff = rd32(r + 20);
    if (blen < want || 8 + boff + want > sizeof r) return -1;
    memcpy(out, r + 8 + boff, want);
    return 0;
}

static int rndis_setup(struct usbnet *u, uint8_t mac[6], uint16_t *mtu)
{
    uint8_t m[32], r[128];
    memset(m, 0, sizeof m);
    wr32(m, RNDIS_INITIALIZE_MSG); wr32(m + 4, 24);
    wr32(m + 12, 1); wr32(m + 16, 0);           /* version 1.0 */
    wr32(m + 20, XHCI_NET_RX_SIZE);             /* the largest transfer we take */
    if (rndis_command(u, m, 24, r, sizeof r)) { kprintf("usbnet: RNDIS: no answer to INITIALIZE\n"); return -1; }
    if (rndis_query(u, OID_802_3_PERMANENT_ADDRESS, mac, 6)) { kprintf("usbnet: RNDIS: no MAC address\n"); return -1; }
    uint32_t fs = 0;
    if (!rndis_query(u, OID_GEN_MAXIMUM_FRAME_SIZE, &fs, 4) && fs >= 576 && fs < ETH_MTU) *mtu = (uint16_t)fs;
    memset(m, 0, sizeof m);
    wr32(m, RNDIS_SET_MSG); wr32(m + 4, 32); wr32(m + 12, OID_GEN_CURRENT_PACKET_FILTER);
    wr32(m + 16, 4); wr32(m + 20, 20); wr32(m + 28, RNDIS_FILTER_DEFAULT);
    if (rndis_command(u, m, 32, r, sizeof r)) { kprintf("usbnet: RNDIS: the packet filter was refused\n"); return -1; }
    return 0;
}

static int ncm_setup(struct usbnet *u, const struct xhci_net_info *in)
{
    uint8_t p[28];
    memset(p, 0, sizeof p);
    if (ctl(u->slot, CLASS_IN, REQ_GET_NTB_PARAMETERS, 0, u->ctrl, sizeof p, p) != XHCI_OK) {
        kprintf("usbnet: NCM: no NTB parameters\n");
        return -1;
    }
    if (!(rd16(p + 2) & 1)) { kprintf("usbnet: NCM: the device has no 16-bit blocks\n"); return -1; }
    uint32_t in_max = rd32(p + 4);
    u->ntb_out_max = rd32(p + 16);
    u->ndp_div = rd16(p + 20);
    u->ndp_rem = rd16(p + 22);
    u->ndp_align = rd16(p + 24);
    if (u->ndp_div == 0 || u->ndp_div > 256 || (u->ndp_div & (u->ndp_div - 1))) u->ndp_div = 4;
    if (in_max > XHCI_NET_RX_SIZE) {                    /* blocks must fit our receive transfers */
        uint8_t sz[4];
        wr32(sz, XHCI_NET_RX_SIZE);
        if (ctl(u->slot, CLASS_OUT, REQ_SET_NTB_INPUT_SIZE, 0, u->ctrl, 4, sz) != XHCI_OK) {
            kprintf("usbnet: NCM: the device sends blocks of %u bytes and will not make them smaller\n", in_max);
            return -1;
        }
    }
    /* the data interface: alternate setting 0 resets the function, the
     * one with the endpoints starts it */
    ctl(u->slot, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE, USB_REQ_SET_INTERFACE, 0, in->data_iface, 0, NULL);
    if (ctl(u->slot, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE, USB_REQ_SET_INTERFACE,
            in->data_alt, in->data_iface, 0, NULL) != XHCI_OK) return -1;
    ctl(u->slot, CLASS_OUT, REQ_SET_ETH_PACKET_FILTER, CDC_FILTER_DEFAULT, u->ctrl, 0, NULL);   /* optional */
    return 0;
}

static int ecm_setup(struct usbnet *u, const struct xhci_net_info *in)
{
    if (in->data_alt &&
        ctl(u->slot, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE, USB_REQ_SET_INTERFACE,
            in->data_alt, in->data_iface, 0, NULL) != XHCI_OK) return -1;
    ctl(u->slot, CLASS_OUT, REQ_SET_ETH_PACKET_FILTER, CDC_FILTER_DEFAULT, u->ctrl, 0, NULL);   /* optional */
    return 0;
}

static bool is_known(uint8_t slot, uint32_t gen)
{
    for (int i = 0; i < ndevs; i++) if (devs[i]->attached && devs[i]->slot == slot && devs[i]->gen == gen) return true;
    for (unsigned i = 0; i < nfailed && i < 8; i++) if (failed[i].slot == slot && failed[i].gen == gen) return true;
    return false;
}

static void attach(const struct xhci_net_info *in)
{
    /* an interface whose device went away is used again; else a new one */
    struct usbnet *u = NULL;
    bool fresh = false;
    for (int i = 0; i < ndevs && !u; i++) if (!devs[i]->attached) u = devs[i];
    if (!u) {
        if (ndevs >= MAX_USBNET) { kprintf("usbnet: more than %d USB network devices: slot %u ignored\n", MAX_USBNET, in->slot); goto fail; }
        u = kzalloc(sizeof *u);
        if (!u) goto fail;
        fresh = true;
    }
    struct usbnet tmp;                                  /* set up apart: u may be in the stack's list */
    memset(&tmp, 0, sizeof tmp);
    tmp.slot = in->slot; tmp.gen = in->generation; tmp.proto = in->proto; tmp.ctrl = in->ctrl_iface;
    tmp.rndis_id = u->rndis_id;
    uint8_t mac[6] = { 0 };
    uint16_t mtu = ETH_MTU;
    int rc;
    if (in->proto == XHCI_NET_RNDIS) rc = rndis_setup(&tmp, mac, &mtu);
    else {
        rc = in->proto == XHCI_NET_NCM ? ncm_setup(&tmp, in) : ecm_setup(&tmp, in);
        if (!rc && !read_mac_string(in->slot, in->mac_string, mac)) {
            uint64_t r = rdtsc();                       /* none given: a local one, as Linux makes */
            for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(r >> (8 * i) ^ r >> 40);
            mac[0] = (uint8_t)((mac[0] & 0xFE) | 0x02);
            kprintf("usbnet: slot %u gave no MAC address: using a random one\n", in->slot);
        }
        if (in->max_segment >= 590 + ETH_HLEN && in->max_segment < ETH_FRAME_MAX) mtu = (uint16_t)(in->max_segment - ETH_HLEN);
    }
    if (rc || xhci_net_start(in->slot) != XHCI_OK) {
        kprintf("usbnet: slot %u (%s): setup failed\n", in->slot, proto_name[in->proto <= 3 ? in->proto : 0]);
        if (fresh) kfree(u);
        goto fail;
    }
    uint64_t f = net_lock();
    u->slot = tmp.slot; u->gen = tmp.gen; u->proto = tmp.proto; u->ctrl = tmp.ctrl; u->rndis_id = tmp.rndis_id;
    u->ntb_out_max = tmp.ntb_out_max; u->ndp_div = tmp.ndp_div; u->ndp_rem = tmp.ndp_rem; u->ndp_align = tmp.ndp_align;
    u->seq = 0;
    u->attaches++;
    strlcpy(u->product, in->dev && in->dev->product_name[0] ? in->dev->product_name : "?", sizeof u->product);
    struct netif *n = &u->nif;
    if (!fresh) arp_flush_if(n);                        /* another device: other neighbours */
    memcpy(n->mac, mac, 6);
    n->mtu = mtu;
    u->attached = true;
    net_unlock(f);
    if (fresh) {
        snprintf(n->name, sizeof n->name, "usb%d", ndevs);
        n->driver = in->proto == XHCI_NET_RNDIS ? "rndis" : in->proto == XHCI_NET_NCM ? "cdc_ncm" : "cdc_ether";
        n->xmit = unet_xmit;
        n->poll = unet_poll;
        n->pending = unet_pending;
        n->link_up = unet_link;
        n->info = unet_info;
        n->priv = u;
        devs[ndevs++] = u;
        netif_register(n);
    } else {
        n->driver = in->proto == XHCI_NET_RNDIS ? "rndis" : in->proto == XHCI_NET_NCM ? "cdc_ncm" : "cdc_ether";
        kprintf("net: %s: %s again, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", n->name, n->driver,
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    kprintf("usbnet: %s: %s on slot %u (\"%s\"), MTU %u\n", n->name, proto_name[u->proto], u->slot, u->product, n->mtu);
    net_hotplug(n);
    return;
fail:
    failed[nfailed++ % 8] = (typeof(failed[0])){ in->slot, in->generation };
}

void usbnet_scan(void)
{
    for (int i = 0; i < ndevs; i++) {
        struct usbnet *u = devs[i];
        if (!u->attached || xhci_net_alive(u->slot, u->gen)) continue;
        uint64_t f = net_lock();
        u->attached = false;
        net_unlock(f);
        netif_carrier(&u->nif, false);
        kprintf("usbnet: %s: unplugged\n", u->nif.name);
    }
    struct xhci_net_info in;
    for (int i = 0; xhci_net_get(i, &in) == XHCI_OK; i++)
        if (!is_known(in.slot, in.generation)) attach(&in);
}

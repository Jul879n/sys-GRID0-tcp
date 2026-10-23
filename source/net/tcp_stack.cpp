#include "tcp_stack.hpp"

extern "C" {
#include "lwip/init.h"
#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/timeouts.h"
}

namespace ztnx::net {

namespace {

TcpStack g_tcp;
netif g_netif;
TcpStack::NowFn g_now = nullptr;
u32 g_rng = 0x2545F491u;
bool g_lwipInitDone = false;

u16_t pcb_queue_free(const tcp_pcb *pcb)
{
    return pcb->snd_queuelen >= TCP_SND_QUEUELEN ? 0 : static_cast<u16_t>(TCP_SND_QUEUELEN - pcb->snd_queuelen);
}

ip4_addr_t ToLwip(u32 hostIp)
{
    ip4_addr_t a;
    a.addr = hton32(hostIp);
    return a;
}

u32 FromLwip(const ip4_addr_t &a) { return ntoh32(a.addr); }

}  // namespace

extern "C" uint32_t ztnx_lwip_now(void) { return g_now != nullptr ? g_now() : 0; }

extern "C" uint32_t ztnx_lwip_rand(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng + ztnx_lwip_now();
}

extern "C" u32_t sys_now(void) { return ztnx_lwip_now(); }

TcpStack *TcpStack::instance() { return &g_tcp; }

void TcpStack::init(SendIpFn send, void *ctx, NowFn now)
{
    g_now = now;
    if (!g_lwipInitDone) {
        lwip_init();
        g_lwipInitDone = true;
        g_rng ^= now != nullptr ? now() : 0;
    }
    // Reinicio: libera pcbs de una sesión anterior (la red cambió o el nodo se reinició).
    for (int c = 0; c < MaxConns; ++c) { if (m_conns[c].state != State::Free) { release(c); } }
    for (int l = 0; l < MaxListeners; ++l) { closeListener(l); }
    abortLingering();
    m_send = send;
    m_sendCtx = ctx;
    if (!m_ready) {
        ip4_addr_t any = {};
        netif_add(&g_netif, &any, &any, &any, this, &TcpStack::netifInit, &ip_input);
        netif_set_default(&g_netif);
        netif_set_up(&g_netif);
        netif_set_link_up(&g_netif);
        m_ready = true;
    }
    m_ip = 0;
}

signed char TcpStack::netifInit(netif *n)
{
    n->name[0] = 'z';
    n->name[1] = 't';
    n->mtu = 1400;                    // cabe en TCP_MSS (1360) + 40
    n->output = &TcpStack::netifOutput;
    n->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

signed char TcpStack::netifOutput(netif *n, pbuf *p, const ip4_addr *)
{
    auto *self = static_cast<TcpStack *>(n->state);
    if (self == nullptr || self->m_send == nullptr || p->tot_len < ip4::MinHeader) { return ERR_IF; }
    u8 buf[1536];
    if (p->tot_len > sizeof(buf)) { return ERR_BUF; }
    pbuf_copy_partial(p, buf, p->tot_len, 0);
    const unsigned int ihl = static_cast<unsigned int>(buf[ip4::VerIhl] & 0x0F) * 4u;
    if (ihl < ip4::MinHeader || ihl > p->tot_len) { return ERR_VAL; }
    const u32 dst = rd32(buf + ip4::Dst);
    return self->m_send(self->m_sendCtx, dst, buf[ip4::Proto], buf + ihl, p->tot_len - ihl)
               ? ERR_OK : ERR_IF;
}

// Cierra sin ceremonia lo que quedó en las listas de lwIP (FIN_WAIT, TIME_WAIT,
// conexiones huérfanas): la red virtual cambió y esos pcbs ya no tienen interlocutor.
void TcpStack::abortLingering()
{
    auto clear = [](tcp_pcb *pcb) {
        tcp_arg(pcb, nullptr);
        tcp_recv(pcb, nullptr);
        tcp_err(pcb, nullptr);
    };
    while (tcp_active_pcbs != nullptr) { tcp_pcb *p = tcp_active_pcbs; clear(p); tcp_abort(p); }
    while (tcp_tw_pcbs != nullptr) { tcp_pcb *p = tcp_tw_pcbs; clear(p); tcp_abort(p); }
}

void TcpStack::setAddress(u32 ip, u32 netmask)
{
    if (!m_ready) { return; }
    if (ip == m_ip) { return; }
    m_ip = ip;
    const ip4_addr_t a = ToLwip(ip), m = ToLwip(netmask), g = {};
    netif_set_addr(&g_netif, &a, &m, &g);
}

void TcpStack::input(u32 srcIp, u32 dstIp, const u8 *payload, unsigned int len)
{
    if (!ready() || len < 20 || len > 1480) { return; }
    // lwIP espera el paquete IP completo: se reconstruye una cabecera mínima.
    pbuf *p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(len + ip4::MinHeader), PBUF_POOL);
    if (p == nullptr) { return; }
    u8 hdr[ip4::MinHeader] = {};
    hdr[ip4::VerIhl] = 0x45;
    wr16(hdr + ip4::TotalLen, static_cast<u16>(len + ip4::MinHeader));
    hdr[ip4::Ttl] = 64;
    hdr[ip4::Proto] = IpProto_TCP;
    wr32(hdr + ip4::Src, srcIp);
    wr32(hdr + ip4::Dst, dstIp);
    if (pbuf_take(p, hdr, sizeof(hdr)) != ERR_OK ||
        pbuf_take_at(p, payload, static_cast<u16_t>(len), sizeof(hdr)) != ERR_OK ||
        g_netif.input(p, &g_netif) != ERR_OK) {
        pbuf_free(p);
    }
}

void TcpStack::poll()
{
    if (m_ready) { sys_check_timeouts(); }
}

/* ---- conexiones --------------------------------------------------------- */

int TcpStack::allocConn()
{
    for (int c = 0; c < MaxConns; ++c) {
        if (m_conns[c].state == State::Free) { m_conns[c] = Conn{}; return c; }
    }
    return -1;
}

void TcpStack::attach(int c, tcp_pcb *pcb)
{
    m_conns[c].pcb = pcb;
    m_conns[c].peerIp = FromLwip(pcb->remote_ip);
    m_conns[c].peerPort = pcb->remote_port;
    m_conns[c].localPort = pcb->local_port;
    tcp_arg(pcb, reinterpret_cast<void *>(static_cast<intptr_t>(c)));
    tcp_recv(pcb, reinterpret_cast<tcp_recv_fn>(&TcpStack::cbRecv));
    tcp_err(pcb, reinterpret_cast<tcp_err_fn>(&TcpStack::cbError));
    pcb->so_options |= SOF_KEEPALIVE;
    tcp_nagle_disable(pcb);          // las respuestas FTP son escrituras chicas: sin espera de ACK
}

void TcpStack::release(int c)
{
    Conn &cn = m_conns[c];
    if (cn.pcb != nullptr) {
        tcp_arg(cn.pcb, nullptr);
        tcp_recv(cn.pcb, nullptr);
        tcp_err(cn.pcb, nullptr);
        if (tcp_close(cn.pcb) != ERR_OK) { tcp_abort(cn.pcb); }
    }
    if (cn.rx != nullptr) { pbuf_free(cn.rx); }
    cn = Conn{};
}

signed char TcpStack::cbRecv(void *arg, tcp_pcb *pcb, pbuf *p, signed char)
{
    const int c = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    Conn &cn = g_tcp.m_conns[c];
    if (p == nullptr) { cn.peerClosed = true; return ERR_OK; }
    if (cn.rx == nullptr) { cn.rx = p; } else { pbuf_cat(cn.rx, p); }
    (void)pcb;
    return ERR_OK;
}

signed char TcpStack::cbConnected(void *arg, tcp_pcb *, signed char err)
{
    const int c = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    g_tcp.m_conns[c].state = err == ERR_OK ? State::Established : State::Failed;
    return ERR_OK;
}

void TcpStack::cbError(void *arg, signed char)
{
    // lwIP ya liberó el pcb.
    const int c = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    g_tcp.m_conns[c].pcb = nullptr;
    g_tcp.m_conns[c].state = State::Failed;
}

signed char TcpStack::cbAccept(void *arg, tcp_pcb *newpcb, signed char err)
{
    Listener &l = g_tcp.m_listeners[static_cast<int>(reinterpret_cast<intptr_t>(arg))];
    if (err != ERR_OK || newpcb == nullptr) { return ERR_VAL; }
    const int c = g_tcp.allocConn();
    if (c < 0 || l.count >= AcceptQueue) {
        if (c >= 0) { g_tcp.m_conns[c] = Conn{}; }
        return ERR_MEM;               // lwIP aborta el pcb
    }
    g_tcp.m_conns[c].state = State::Established;
    g_tcp.attach(c, newpcb);
    l.queue[l.count++] = c;
    tcp_backlog_delayed(newpcb);
    return ERR_OK;
}

/* ---- listeners ---------------------------------------------------------- */

int TcpStack::listen(u16 port)
{
    if (!ready()) { return -1; }
    for (int l = 0; l < MaxListeners; ++l) {
        if (m_listeners[l].used && m_listeners[l].pcb != nullptr && m_listeners[l].pcb->local_port == port) {
            return l;                  // ya escuchando en ese puerto
        }
    }
    for (int l = 0; l < MaxListeners; ++l) {
        if (m_listeners[l].used) { continue; }
        tcp_pcb *pcb = tcp_new();
        if (pcb == nullptr) { return -1; }
        if (tcp_bind(pcb, IP_ANY_TYPE, port) != ERR_OK) { tcp_abort(pcb); return -1; }
        tcp_pcb *lp = tcp_listen_with_backlog(pcb, AcceptQueue);
        if (lp == nullptr) { tcp_abort(pcb); return -1; }
        m_listeners[l] = Listener{};
        m_listeners[l].used = true;
        m_listeners[l].pcb = lp;
        tcp_arg(lp, reinterpret_cast<void *>(static_cast<intptr_t>(l)));
        tcp_accept(lp, reinterpret_cast<tcp_accept_fn>(&TcpStack::cbAccept));
        return l;
    }
    return -1;
}

void TcpStack::closeListener(int l)
{
    if (l < 0 || l >= MaxListeners || !m_listeners[l].used) { return; }
    Listener &ls = m_listeners[l];
    for (int i = 0; i < ls.count; ++i) { release(ls.queue[i]); }
    if (ls.pcb != nullptr) { tcp_close(ls.pcb); }
    ls = Listener{};
}

int TcpStack::pending(int l) const
{
    return l >= 0 && l < MaxListeners && m_listeners[l].used ? m_listeners[l].count : 0;
}

int TcpStack::accept(int l, u32 *peerIp, u16 *peerPort)
{
    if (l < 0 || l >= MaxListeners || m_listeners[l].count == 0) { return -1; }
    Listener &ls = m_listeners[l];
    const int c = ls.queue[0];
    for (int i = 1; i < ls.count; ++i) { ls.queue[i - 1] = ls.queue[i]; }
    --ls.count;
    if (peerIp != nullptr) { *peerIp = m_conns[c].peerIp; }
    if (peerPort != nullptr) { *peerPort = m_conns[c].peerPort; }
    return c;
}

/* ---- datos -------------------------------------------------------------- */

int TcpStack::connect(u32 ip, u16 port)
{
    if (!ready()) { return -1; }
    const int c = allocConn();
    if (c < 0) { return -1; }
    tcp_pcb *pcb = tcp_new();
    if (pcb == nullptr) { m_conns[c] = Conn{}; return -1; }
    m_conns[c].state = State::Connecting;
    pcb->so_options |= SOF_KEEPALIVE;
    tcp_nagle_disable(pcb);
    tcp_arg(pcb, reinterpret_cast<void *>(static_cast<intptr_t>(c)));
    tcp_recv(pcb, reinterpret_cast<tcp_recv_fn>(&TcpStack::cbRecv));
    tcp_err(pcb, reinterpret_cast<tcp_err_fn>(&TcpStack::cbError));
    m_conns[c].pcb = pcb;
    const ip4_addr_t dst = ToLwip(ip);
    if (tcp_connect(pcb, &dst, port, reinterpret_cast<tcp_connected_fn>(&TcpStack::cbConnected)) != ERR_OK) {
        tcp_abort(pcb);
        m_conns[c] = Conn{};
        return -1;
    }
    m_conns[c].peerIp = ip;
    m_conns[c].peerPort = port;
    m_conns[c].localPort = pcb->local_port;
    return c;
}

bool TcpStack::established(int c) const
{
    return c >= 0 && c < MaxConns && m_conns[c].state == State::Established;
}

bool TcpStack::failed(int c) const
{
    return c < 0 || c >= MaxConns || m_conns[c].state == State::Failed || m_conns[c].state == State::Free;
}

int TcpStack::recv(int c, void *buf, unsigned int max, bool peek)
{
    if (c < 0 || c >= MaxConns || m_conns[c].state == State::Free) { return Failed; }
    Conn &cn = m_conns[c];
    if (cn.rx != nullptr) {
        const unsigned int n = cn.rx->tot_len < max ? cn.rx->tot_len : max;
        pbuf_copy_partial(cn.rx, buf, static_cast<u16_t>(n), 0);
        if (!peek) {
            cn.rx = pbuf_free_header(cn.rx, static_cast<u16_t>(n));
            if (cn.pcb != nullptr) { tcp_recved(cn.pcb, static_cast<u16_t>(n)); }
        }
        return static_cast<int>(n);
    }
    if (cn.state == State::Failed) { return Failed; }
    if (cn.peerClosed) { return 0; }
    return WouldBlock;
}

int TcpStack::send(int c, const void *buf, unsigned int len)
{
    if (c < 0 || c >= MaxConns) { return Failed; }
    Conn &cn = m_conns[c];
    if (cn.state == State::Failed || cn.pcb == nullptr) { return Failed; }
    if (cn.state != State::Established) { return WouldBlock; }
    u16_t room = tcp_sndbuf(cn.pcb);
    if (room == 0) { return WouldBlock; }
    // Un par sin opción MSS (536 B) necesitaría más segmentos que TCP_SND_QUEUELEN para llenar
    // el buffer: tcp_write fallaría para siempre con ERR_MEM. Se escribe solo lo que cabe en la cola.
    const u16_t qFree = pcb_queue_free(cn.pcb);
    if (qFree == 0) { return WouldBlock; }
    const unsigned int qBytes = static_cast<unsigned int>(qFree) * cn.pcb->mss;
    const unsigned int cap = qBytes < room ? qBytes : room;
    const u16_t n = len < cap ? static_cast<u16_t>(len) : static_cast<u16_t>(cap);
    if (tcp_write(cn.pcb, buf, n, TCP_WRITE_FLAG_COPY) != ERR_OK) { return WouldBlock; }
    tcp_output(cn.pcb);
    return n;
}

bool TcpStack::readable(int c) const
{
    if (c < 0 || c >= MaxConns) { return true; }
    const Conn &cn = m_conns[c];
    return cn.state == State::Failed || cn.state == State::Free || cn.rx != nullptr || cn.peerClosed;
}

bool TcpStack::writable(int c) const
{
    if (c < 0 || c >= MaxConns) { return true; }
    const Conn &cn = m_conns[c];
    if (cn.state == State::Failed || cn.state == State::Free) { return true; }
    return cn.state == State::Established && cn.pcb != nullptr && tcp_sndbuf(cn.pcb) > 0;
}

void TcpStack::shutdownWrite(int c)
{
    if (c >= 0 && c < MaxConns && m_conns[c].pcb != nullptr) { tcp_shutdown(m_conns[c].pcb, 0, 1); }
}

void TcpStack::close(int c)
{
    if (c >= 0 && c < MaxConns && m_conns[c].state != State::Free) { release(c); }
}

bool TcpStack::peer(int c, u32 *ip, u16 *port) const
{
    if (c < 0 || c >= MaxConns || m_conns[c].state == State::Free) { return false; }
    *ip = m_conns[c].peerIp;
    *port = m_conns[c].peerPort;
    return true;
}

u16 TcpStack::localPort(int c) const
{
    return c >= 0 && c < MaxConns ? m_conns[c].localPort : 0;
}

unsigned int TcpStack::connsInUse() const
{
    unsigned int n = 0;
    for (const Conn &c : m_conns) { if (c.state != State::Free) { ++n; } }
    return n;
}

}  // namespace ztnx::net

/*
 * Tests de host de TCP sobre la red virtual: lwIP real (TcpStack) y el lado
 * remoto simulado a mano (segmentos con checksum calculado en este archivo,
 * independiente del código bajo prueba).
 *
 *   make -C tests tcp
 */
#include "../source/net/vnet.hpp"
#include "../source/net/tcp_stack.hpp"
#include "../source/mitm/tcp_sockets.hpp"

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

using namespace ztnx::net;
using ztnx::mitm::TcpSockets;

static int g_checks = 0, g_failures = 0;
static const char *g_case = "";

static void expect(bool ok, const char *what)
{
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("  FAIL  %s :: %s\n", g_case, what); }
}
static void expectEq(long long got, long long want, const char *what)
{
    ++g_checks;
    if (got != want) { ++g_failures; std::printf("  FAIL  %s :: %s (got %lld, want %lld)\n", g_case, what, got, want); }
}

constexpr u32 LocalIp = 0xC0A8C4C2;   // 192.168.196.194
constexpr u32 PeerIp  = 0xC0A8C47A;   // 192.168.196.122
static u16 PeerPort = 40000;   // distinto por prueba: evita colisión con pcbs en cierre
constexpr u16 SvcPort = 5000;

enum : u8 { FIN = 0x01, SYN = 0x02, RST = 0x04, PSH = 0x08, ACK = 0x10 };

struct Seg { u16 sport, dport; u32 seq, ack; u8 flags; std::vector<u8> data; bool csumOk; };

static u16 csum(const u8 *p, size_t n, u32 sum)
{
    for (size_t i = 0; i + 1 < n; i += 2) { sum += static_cast<u32>((p[i] << 8) | p[i + 1]); }
    if (n & 1) { sum += static_cast<u32>(p[n - 1] << 8); }
    while (sum >> 16) { sum = (sum & 0xFFFF) + (sum >> 16); }
    return static_cast<u16>(~sum);
}

static u16 tcpChecksum(u32 src, u32 dst, const u8 *seg, size_t len)
{
    u32 sum = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + 6 + static_cast<u32>(len);
    return csum(seg, len, sum);
}

static std::vector<u8> build(u32 src, u32 dst, const Seg &s)
{
    std::vector<u8> b(20 + s.data.size(), 0);
    wr16(&b[0], s.sport); wr16(&b[2], s.dport);
    wr32(&b[4], s.seq);   wr32(&b[8], s.ack);
    b[12] = 5 << 4; b[13] = s.flags;
    wr16(&b[14], 4096);
    if (!s.data.empty()) { memcpy(&b[20], s.data.data(), s.data.size()); }
    wr16(&b[16], tcpChecksum(src, dst, b.data(), b.size()));
    return b;
}

static Seg parse(u32 src, u32 dst, const u8 *p, size_t len)
{
    Seg s{};
    s.sport = rd16(p); s.dport = rd16(p + 2); s.seq = rd32(p + 4); s.ack = rd32(p + 8);
    s.flags = p[13];
    const size_t off = static_cast<size_t>(p[12] >> 4) * 4;
    s.data.assign(p + off, p + len);
    s.csumOk = tcpChecksum(src, dst, p, len) == 0;
    return s;
}

/* Lo que lwIP emite (por sendIp), capturado. */
static std::vector<Seg> g_out;
static u32 g_now = 1000;

static bool captureSend(void *, u32 dst, u8 proto, const u8 *payload, unsigned int len)
{
    if (proto != 6 || dst != PeerIp) { return false; }
    g_out.push_back(parse(LocalIp, dst, payload, len));
    return true;
}
static u32 nowFn() { return g_now; }

static void inject(TcpStack &t, const Seg &s)
{
    const std::vector<u8> b = build(PeerIp, LocalIp, s);
    t.input(PeerIp, LocalIp, b.data(), static_cast<unsigned int>(b.size()));
}

static Seg takeLast()
{
    if (g_out.empty()) { return Seg{}; }
    Seg s = g_out.back();
    g_out.clear();
    return s;
}

static void testHandshakeAndData()
{
    g_case = "handshake + datos";
    TcpStack &t = *TcpStack::instance();
    t.init(&captureSend, nullptr, &nowFn);
    t.setAddress(LocalIp, 0xFFFFFF00);
    expect(t.ready(), "ready tras setAddress");

    const int l = t.listen(SvcPort);
    expect(l >= 0, "listen abre un listener");
    expectEq(t.listen(SvcPort), l, "listen repetido en el mismo puerto reutiliza el listener");

    // SYN remoto con ISN 1000
    inject(t, Seg{PeerPort, SvcPort, 1000, 0, SYN, {}, true});
    Seg synack = takeLast();
    expect((synack.flags & (SYN | ACK)) == (SYN | ACK), "responde SYN+ACK");
    expectEq(synack.ack, 1001, "SYN+ACK confirma ISN+1");
    expect(synack.csumOk, "checksum TCP del SYN+ACK válido");
    expectEq(synack.sport, SvcPort, "puerto origen del SYN+ACK");
    expectEq(synack.dport, PeerPort, "puerto destino del SYN+ACK");

    expectEq(t.pending(l), 0, "sin ACK final aún no hay conexión pendiente");
    inject(t, Seg{PeerPort, SvcPort, 1001, synack.seq + 1, ACK, {}, true});
    expectEq(t.pending(l), 1, "tras el ACK hay una conexión pendiente");

    u32 pip = 0; u16 pport = 0;
    const int c = t.accept(l, &pip, &pport);
    expect(c >= 0, "accept entrega la conexión");
    expectEq(pip, PeerIp, "IP del par");
    expectEq(pport, PeerPort, "puerto del par");
    expect(t.established(c), "conexión establecida");
    expectEq(t.pending(l), 0, "cola vacía tras accept");

    // El cliente envía "hello"
    g_out.clear();
    const std::string hello = "hello";
    inject(t, Seg{PeerPort, SvcPort, 1001, synack.seq + 1, static_cast<u8>(PSH | ACK),
                  std::vector<u8>(hello.begin(), hello.end()), true});
    expect(t.readable(c), "legible con datos");
    char buf[32] = {};
    expectEq(t.recv(c, buf, sizeof(buf), true), 5, "peek devuelve 5 bytes");
    expectEq(t.recv(c, buf, sizeof(buf), false), 5, "recv consume 5 bytes");
    expect(std::string(buf, 5) == hello, "contenido recibido");
    expectEq(t.recv(c, buf, sizeof(buf), false), TcpStack::WouldBlock, "sin más datos: WouldBlock");
    g_now += 300;               // el ACK de lwIP es diferido (timer de 250 ms)
    t.poll();
    expect(!g_out.empty(), "lwIP emite un ACK");
    if (!g_out.empty()) { expectEq(g_out.back().ack, 1006, "ACK cubre los 5 bytes"); }

    // La consola responde "world"
    g_out.clear();
    expectEq(t.send(c, "world", 5), 5, "send acepta 5 bytes");
    bool found = false;
    for (const Seg &s : g_out) {
        if (s.data.size() == 5 && memcmp(s.data.data(), "world", 5) == 0) {
            found = true;
            expect(s.csumOk, "checksum TCP del segmento de datos válido");
            expectEq(s.seq, synack.seq + 1, "número de secuencia del primer byte");
        }
    }
    expect(found, "segmento con 'world' emitido");

    // FIN del cliente -> EOF
    inject(t, Seg{PeerPort, SvcPort, 1006, synack.seq + 6, static_cast<u8>(FIN | ACK), {}, true});
    expectEq(t.recv(c, buf, sizeof(buf), false), 0, "FIN remoto -> EOF (0)");
    expect(t.readable(c), "EOF cuenta como legible");

    g_out.clear();
    t.close(c);
    bool fin = false;
    for (const Seg &s : g_out) { if (s.flags & FIN) { fin = true; } }
    expect(fin, "close emite FIN");
    expectEq(static_cast<long long>(t.connsInUse()), 0, "conexión liberada");
    t.closeListener(l);
}

static void testReset()
{
    g_case = "RST remoto";
    PeerPort = 40001;
    TcpStack &t = *TcpStack::instance();
    t.init(&captureSend, nullptr, &nowFn);
    t.setAddress(LocalIp, 0xFFFFFF00);
    const int l = t.listen(SvcPort);
    inject(t, Seg{PeerPort, SvcPort, 500, 0, SYN, {}, true});
    Seg sa = takeLast();
    inject(t, Seg{PeerPort, SvcPort, 501, sa.seq + 1, ACK, {}, true});
    const int c = t.accept(l, nullptr, nullptr);
    expect(c >= 0, "conexión aceptada");
    inject(t, Seg{PeerPort, SvcPort, 501, sa.seq + 1, static_cast<u8>(RST | ACK), {}, true});
    expect(t.failed(c), "RST marca la conexión como fallida");
    char b[4];
    expectEq(t.recv(c, b, sizeof(b), false), TcpStack::Failed, "recv tras RST -> Failed");
    expectEq(t.send(c, "x", 1), TcpStack::Failed, "send tras RST -> Failed");
    t.close(c);
    t.closeListener(l);
}

static void testClosedPort()
{
    g_case = "puerto cerrado";
    PeerPort = 40002;
    TcpStack &t = *TcpStack::instance();
    t.init(&captureSend, nullptr, &nowFn);
    t.setAddress(LocalIp, 0xFFFFFF00);
    inject(t, Seg{PeerPort, 9999, 700, 0, SYN, {}, true});
    const Seg r = takeLast();
    expect((r.flags & RST) != 0, "SYN a un puerto sin listener -> RST");
}

static void testBadChecksum()
{
    g_case = "checksum inválido";
    PeerPort = 40003;
    TcpStack &t = *TcpStack::instance();
    t.init(&captureSend, nullptr, &nowFn);
    t.setAddress(LocalIp, 0xFFFFFF00);
    const int l = t.listen(SvcPort);
    std::vector<u8> b = build(PeerIp, LocalIp, Seg{PeerPort, SvcPort, 1, 0, SYN, {}, true});
    b[16] ^= 0xFF;
    g_out.clear();
    t.input(PeerIp, LocalIp, b.data(), static_cast<unsigned int>(b.size()));
    expect(g_out.empty(), "SYN con checksum malo se descarta sin respuesta");
    t.closeListener(l);
}

static void testOutboundConnect()
{
    g_case = "connect saliente";
    PeerPort = 40004;
    TcpStack &t = *TcpStack::instance();
    t.init(&captureSend, nullptr, &nowFn);
    t.setAddress(LocalIp, 0xFFFFFF00);
    g_out.clear();
    const int c = t.connect(PeerIp, 8080);
    expect(c >= 0, "connect devuelve conexión en curso");
    expect(!t.established(c), "aún no establecida");
    expect(!g_out.empty() && (g_out.back().flags & SYN), "emite SYN");
    const Seg syn = g_out.back();
    expectEq(syn.dport, 8080, "puerto destino del SYN");
    inject(t, Seg{8080, syn.sport, 9000, syn.seq + 1, static_cast<u8>(SYN | ACK), {}, true});
    expect(t.established(c), "SYN+ACK remoto -> establecida");
    t.close(c);
}

static void testSockets()
{
    g_case = "TcpSockets";
    PeerPort = 40005;
    TcpStack &t = *TcpStack::instance();
    t.init(&captureSend, nullptr, &nowFn);
    t.setAddress(LocalIp, 0xFFFFFF00);
    TcpSockets ts;
    const u64 pid = 77;
    expect(ts.track(pid, 10), "track fd 10");
    ts.onBind(pid, 10, 0, SvcPort);
    expect(ts.onListen(t, pid, 10, LocalIp), "listen en 0.0.0.0:5000 abre listener virtual");
    expect(ts.virtualFd(pid, 10), "el fd 10 es virtual");

    expect(ts.track(pid, 11), "track fd 11");
    ts.onBind(pid, 11, 0xC0A80164, 6000);   // IP de la LAN física, no la de ZeroTier
    expect(!ts.onListen(t, pid, 11, LocalIp), "listen en IP física no abre listener virtual");

    inject(t, Seg{PeerPort, SvcPort, 1000, 0, SYN, {}, true});
    Seg sa = takeLast();
    inject(t, Seg{PeerPort, SvcPort, 1001, sa.seq + 1, ACK, {}, true});
    expectEq(ts.pending(t, pid, 10), 1, "una conexión pendiente");
    expectEq(ts.pollEvents(t, pid, 10, ztnx::mitm::PollIn), ztnx::mitm::PollIn, "poll(listener): POLLIN");
    expect(ts.acceptInto(t, pid, 10, 20), "acceptInto liga el fd marcador 20");
    expect(ts.virtualFd(pid, 20), "fd 20 virtual");
    expectEq(ts.pollEvents(t, pid, 20, ztnx::mitm::PollIn), 0, "sin datos: sin POLLIN");
    expect((ts.pollEvents(t, pid, 20, ztnx::mitm::PollOut) & ztnx::mitm::PollOut) != 0, "escribible: POLLOUT");

    inject(t, Seg{PeerPort, SvcPort, 1001, sa.seq + 1, static_cast<u8>(PSH | ACK),
                  std::vector<u8>{'h', 'i'}, true});
    char buf[8];
    expectEq(ts.pollEvents(t, pid, 20, ztnx::mitm::PollIn), ztnx::mitm::PollIn, "con datos: POLLIN");
    expectEq(ts.recv(t, pid, 20, buf, sizeof(buf), false), 2, "recv por fd");
    expectEq(ts.recv(t, pid, 20, buf, sizeof(buf), false), -ztnx::mitm::ErrAgain, "sin datos: -EAGAIN");
    expectEq(ts.send(t, pid, 20, "ok", 2), 2, "send por fd");
    u32 ip = 0; u16 port = 0;
    expect(ts.localPeer(t, pid, 20, &ip, &port) && ip == PeerIp && port == PeerPort, "peer del fd");
    expectEq(ts.recv(t, pid, 99, buf, sizeof(buf), false), -ztnx::mitm::ErrBadF, "fd desconocido -> EBADF");

    ts.forgetPid(t, pid);
    expectEq(static_cast<long long>(ts.used()), 0, "forgetPid limpia la tabla");
    expectEq(static_cast<long long>(t.connsInUse()), 0, "y libera las conexiones de lwIP");
}

/* VNet completo: una trama Ethernet con un SYN entra por onFrame y el SYN+ACK sale por emit. */
struct Frame { u64 dstMac; u16 etherType; std::vector<u8> payload; };
static std::vector<Frame> g_frames;
static void emitFn(void *, u64 dstMac, u16 etherType, const void *payload, unsigned int len)
{
    g_frames.push_back({dstMac, etherType, std::vector<u8>(static_cast<const u8 *>(payload), static_cast<const u8 *>(payload) + len)});
}
static void tcpInputThunk(void *ctx, u32 s, u32 d, const u8 *seg, unsigned int len)
{
    static_cast<TcpStack *>(ctx)->input(s, d, seg, len);
}
static bool sendIpThunk(void *ctx, u32 dst, u8 proto, const u8 *p, unsigned int len)
{
    static_cast<VNet *>(ctx)->sendIp(dst, proto, p, len);
    return true;
}

static void testThroughVNet()
{
    g_case = "VNet + lwIP";
    PeerPort = 40006;
    VNet vnet;
    vnet.reset();
    vnet.setEmit(&emitFn, nullptr);
    TcpStack &t = *TcpStack::instance();
    t.init(&sendIpThunk, &vnet, &nowFn);
    vnet.setTcpInput(&tcpInputThunk, &t);
    const u64 localMac = 0x2AE1E07998F6ull, peerMac = 0x2A0000000122ull;
    vnet.configure(localMac, LocalIp, 0xFFFFFF00, 2800);
    t.setAddress(LocalIp, 0xFFFFFF00);
    const int l = t.listen(SvcPort);

    const std::vector<u8> seg = build(PeerIp, LocalIp, Seg{PeerPort, SvcPort, 4242, 0, SYN, {}, true});
    std::vector<u8> ip(20 + seg.size(), 0);
    ip[0] = 0x45; wr16(&ip[2], static_cast<u16>(ip.size())); wr16(&ip[4], 1); ip[8] = 64; ip[9] = 6;
    wr32(&ip[12], PeerIp); wr32(&ip[16], LocalIp);
    wr16(&ip[10], csum(ip.data(), 20, 0));
    memcpy(&ip[20], seg.data(), seg.size());
    g_frames.clear();
    // La ARP del par no se conoce: se aprende de la trama entrante (como en producción).
    vnet.onFrame(peerMac, localMac, 0x0800, ip.data(), static_cast<unsigned int>(ip.size()));

    expectEq(static_cast<long long>(vnet.stats().rxTcp), 1, "VNet cuenta el segmento TCP");
    bool ok = false;
    for (const Frame &f : g_frames) {
        if (f.etherType == 0x0800 && f.dstMac == peerMac && f.payload.size() >= 40 && f.payload[9] == 6) {
            const Seg s = parse(LocalIp, PeerIp, f.payload.data() + 20, f.payload.size() - 20);
            ok = (s.flags & (SYN | ACK)) == (SYN | ACK) && s.ack == 4243 && s.csumOk;
            expectEq(rd32(&f.payload[12]), LocalIp, "IP origen del SYN+ACK");
            expectEq(rd32(&f.payload[16]), PeerIp, "IP destino del SYN+ACK");
            expectEq(static_cast<long long>(csum(f.payload.data(), 20, 0)), 0, "checksum IP válido");
        }
    }
    expect(ok, "SYN+ACK sale como trama IPv4 hacia la MAC del par");
    t.closeListener(l);
}

/* Handshake completo de un cliente remoto; devuelve el id de conexión aceptada o -1. */
static int connectPeer(TcpStack &t, int l, u16 port, u32 isn)
{
    g_out.clear();
    inject(t, Seg{port, SvcPort, isn, 0, SYN, {}, true});
    const Seg sa = takeLast();
    if ((sa.flags & (SYN | ACK)) != (SYN | ACK)) { return -2; }   // rechazada (RST) o sin respuesta
    inject(t, Seg{port, SvcPort, isn + 1, sa.seq + 1, ACK, {}, true});
    return t.accept(l, nullptr, nullptr);
}

static void testLimits()
{
    g_case = "límites";
    TcpStack &t = *TcpStack::instance();
    t.init(&captureSend, nullptr, &nowFn);
    t.setAddress(LocalIp, 0xFFFFFF00);

    const int l = t.listen(SvcPort);
    const int l2 = t.listen(5001);
    expect(l >= 0 && l2 >= 0 && l != l2, "dos listeners");
    expect(t.listen(5002) >= 0 && t.listen(5003) >= 0, "listeners 3 y 4 (control, otro servicio y 2 PASV)");
    expectEq(t.listen(5004), -1, "quinto listener -> -1 (sin caída)");

    int ids[TcpStack::MaxConns];
    for (int i = 0; i < TcpStack::MaxConns; ++i) {
        ids[i] = connectPeer(t, l, static_cast<u16>(41000 + i), static_cast<u32>(2000 + i));
        expect(ids[i] >= 0, "conexión dentro del límite aceptada");
    }
    expectEq(static_cast<long long>(t.connsInUse()), TcpStack::MaxConns, "tabla llena");

    // La quinta: se rechaza limpiamente (no SYN+ACK aceptado) y no deja basura.
    expect(connectPeer(t, l, 41100, 3000) < 0, "conexión sobre el límite no se acepta (RST o sin SYN+ACK)");
    expectEq(static_cast<long long>(t.connsInUse()), TcpStack::MaxConns, "la rechazada no ocupa ranura");

    // connect saliente con la tabla llena falla limpio.
    expectEq(t.connect(PeerIp, 80), -1, "connect saliente sin ranura -> -1");

    // Al cerrar se recuperan las ranuras y se puede aceptar de nuevo.
    for (int i = 0; i < TcpStack::MaxConns; ++i) { t.close(ids[i]); }
    expectEq(static_cast<long long>(t.connsInUse()), 0, "cerrar libera todas las ranuras");
    const int again = connectPeer(t, l, 41200, 4000);
    expect(again >= 0, "tras liberar se acepta otra conexión");
    t.close(again);

    // Cola de accept llena (4) sin que la app acepte: la siguiente no se encola.
    TcpStack &t2 = t;
    int queued = 0;
    for (int i = 0; i < 6; ++i) {
        g_out.clear();
        inject(t2, Seg{static_cast<u16>(42000 + i), SvcPort, 5000u + static_cast<u32>(i), 0, SYN, {}, true});
        const Seg sa = takeLast();
        if ((sa.flags & (SYN | ACK)) == (SYN | ACK)) {
            inject(t2, Seg{static_cast<u16>(42000 + i), SvcPort, 5001u + static_cast<u32>(i), sa.seq + 1, ACK, {}, true});
        }
    }
    queued = t2.pending(l);
    expect(queued <= TcpStack::AcceptQueue, "la cola de accept no supera su límite");
    t.closeListener(l);
    t.closeListener(l2);
    expectEq(static_cast<long long>(t.connsInUse()), 0, "closeListener libera lo encolado");
}

// Par sin opción MSS (536 B): la cola de envío (TCP_SND_QUEUELEN segmentos) se llena antes que
// TCP_SND_BUF; send debe escribir lo que cabe y devolver WouldBlock después, nunca fallar.
static void testSmallMssSend()
{
    g_case = "MSS pequeño";
    PeerPort = 40200;
    TcpStack &t = *TcpStack::instance();
    t.init(&captureSend, nullptr, &nowFn);
    t.setAddress(LocalIp, 0xFFFFFF00);
    const int l = t.listen(SvcPort);
    const int c = connectPeer(t, l, 43500, 9000);
    expect(c >= 0, "conexión establecida");
    std::vector<u8> big(20000, 0x42);
    const int n1 = t.send(c, big.data(), static_cast<unsigned>(big.size()));
    expect(n1 > 0, "el primer send escribe algo aunque el MSS sea de 536");
    long long total = n1 > 0 ? n1 : 0;
    for (int i = 0; i < 4; ++i) {
        const int n = t.send(c, big.data(), static_cast<unsigned>(big.size()));
        if (n > 0) { total += n; }
    }
    expect(t.send(c, big.data(), static_cast<unsigned>(big.size())) == TcpStack::WouldBlock, "cola llena -> WouldBlock (no Failed)");
    expect(!t.failed(c), "la conexión sigue viva");
    expect(total <= 20 * 536, "no se encola más de TCP_SND_QUEUELEN segmentos");
    t.close(c);
    t.closeListener(l);
}

static void testKeepalive()
{
    g_case = "keepalive";
    TcpStack &t = *TcpStack::instance();
    g_now = 1000;
    t.init(&captureSend, nullptr, &nowFn);
    t.setAddress(LocalIp, 0xFFFFFF00);
    const int l = t.listen(SvcPort);
    const int c = connectPeer(t, l, 43000, 6000);
    expect(c >= 0, "conexión establecida");
    expect(!t.failed(c), "conexión viva");

    // El par desaparece: sin respuesta a los sondeos, lwIP aborta la conexión.
    // Reloj en pasos de 100 ms (TCP_TMR_INTERVAL de lwipopts.h), como el poll real del nodo.
    unsigned secs = 0;
    for (unsigned step = 0; step < 10 * 600 && !t.failed(c); ++step) {
        g_now += 100; t.poll(); g_out.clear();
        if (step % 10 == 9) { ++secs; }
    }
    expect(secs >= 150 && secs <= 300, "keepalive aborta entre 150 y 300 s (idle 120 s + 4x15 s)");
    expect(t.failed(c), "par mudo: keepalive marca la conexión como fallida");
    t.close(c);
    expectEq(static_cast<long long>(t.connsInUse()), 0, "la ranura se recupera");
    t.closeListener(l);
}

// FTP pasivo por la VPN (ftpsrv): control en 5000, otro listener propio de Sphaira y un
// socket PASV por transferencia, enlazado a la IP ZeroTier (getsockname del control).
static void testPasvDataChannel()
{
    g_case = "PASV";
    PeerPort = 40100;
    TcpStack &t = *TcpStack::instance();
    t.init(&captureSend, nullptr, &nowFn);
    t.setAddress(LocalIp, 0xFFFFFF00);
    TcpSockets ts;
    const u64 pid = 88;
    expect(ts.track(pid, 10), "fd 10: control");
    ts.onBind(pid, 10, 0, SvcPort);
    expect(ts.onListen(t, pid, 10, LocalIp), "control 0.0.0.0:5000 -> listener");
    expect(ts.track(pid, 11), "fd 11: otro servicio");
    ts.onBind(pid, 11, 0, 28280);
    expect(ts.onListen(t, pid, 11, LocalIp), "segundo listener propio de la app");

    for (int round = 0; round < 3; ++round) {
        const int pasvFd = 30 + round;
        const u16 pasvPort = static_cast<u16>(49152 + round);
        expect(ts.track(pid, pasvFd), "socket PASV rastreado");
        ts.onBind(pid, pasvFd, LocalIp, pasvPort);          // bind a la IP ZeroTier
        expect(ts.onListen(t, pid, pasvFd, LocalIp), "PASV en IP ZeroTier abre listener lwIP");

        const u16 pport = static_cast<u16>(41000 + round);
        inject(t, Seg{pport, pasvPort, 7000, 0, SYN, {}, true});
        const Seg sa = takeLast();
        inject(t, Seg{pport, pasvPort, 7001, sa.seq + 1, ACK, {}, true});
        expectEq(ts.pending(t, pid, pasvFd), 1, "el cliente conecta al puerto de datos");
        const int dataFd = 50 + round;
        expect(ts.acceptInto(t, pid, pasvFd, dataFd), "accept del canal de datos");
        expectEq(ts.send(t, pid, dataFd, "listing", 7), 7, "se envía el listado");
        ts.shutdown(t, pid, dataFd, 2);
        ts.forget(t, pid, dataFd);
        ts.forget(t, pid, pasvFd);      // close() del socket PASV libera su listener
    }
    expectEq(static_cast<long long>(ts.used()), 2, "solo quedan control y el otro servicio");
    ts.forgetPid(t, pid);
    expectEq(static_cast<long long>(ts.used()), 0, "forgetPid limpia la tabla");
}

int main()
{
    testLimits();
    testKeepalive();
    testSmallMssSend();
    testHandshakeAndData();
    testReset();
    testClosedPort();
    testBadChecksum();
    testOutboundConnect();
    testSockets();
    testPasvDataChannel();
    testThroughVNet();
    std::printf("tcp tests\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

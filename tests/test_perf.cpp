/*
 * Rendimiento TCP simulado: lwIP real (TcpStack) y un receptor remoto simulado, con
 * retardo de ida fijo y el reloj discreto de milisegundos. Modela lo que el módulo
 * hace en la consola:
 *   - el hilo del nodo despierta cada `loopMs` (poll de 10 ms) o antes si llega un paquete
 *     (rx se atiende al instante); lo que genera el hilo de la app sale en el siguiente tick;
 *   - el hilo de la app reintenta `send` cada `appSleepMs` si no cabe (TcpSleep).
 *
 *   make -C tests perf     compara el baseline (ZTNX_PERF_BASELINE) con la configuración actual
 */
#include "../source/net/vnet.hpp"
#include "../source/net/tcp_stack.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

using namespace ztnx::net;

constexpr u32 LocalIp = 0xC0A8C4C2;
constexpr u32 PeerIp  = 0xC0A8C47A;
constexpr u16 SvcPort = 5000, PeerPort = 41000;
enum : u8 { FIN = 0x01, SYN = 0x02, RST = 0x04, PSH = 0x08, ACK = 0x10 };

struct Seg { u16 sport, dport; u32 seq, ack; u8 flags; u16 wnd; std::vector<u8> data; bool csumOk; };

static u16 csum(const u8 *p, size_t n, u32 sum)
{
    for (size_t i = 0; i + 1 < n; i += 2) { sum += static_cast<u32>((p[i] << 8) | p[i + 1]); }
    if (n & 1) { sum += static_cast<u32>(p[n - 1] << 8); }
    while (sum >> 16) { sum = (sum & 0xFFFF) + (sum >> 16); }
    return static_cast<u16>(~sum);
}
static u16 tcpChecksum(u32 src, u32 dst, const u8 *seg, size_t len)
{
    return csum(seg, len, (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + 6 + static_cast<u32>(len));
}
static std::vector<u8> build(u32 src, u32 dst, const Seg &s)
{
    const size_t hl = (s.flags & SYN) ? 24 : 20;      // SYN con opción MSS 1460, como un Mac real
    std::vector<u8> b(hl + s.data.size(), 0);
    wr16(&b[0], s.sport); wr16(&b[2], s.dport);
    wr32(&b[4], s.seq);   wr32(&b[8], s.ack);
    b[12] = static_cast<u8>((hl / 4) << 4); b[13] = s.flags;
    wr16(&b[14], s.wnd);
    if (hl == 24) { b[20] = 2; b[21] = 4; wr16(&b[22], 1460); }
    if (!s.data.empty()) { memcpy(&b[hl], s.data.data(), s.data.size()); }
    wr16(&b[16], tcpChecksum(src, dst, b.data(), b.size()));
    return b;
}
static Seg parse(u32 src, u32 dst, const u8 *p, size_t len)
{
    Seg s{};
    s.sport = rd16(p); s.dport = rd16(p + 2); s.seq = rd32(p + 4); s.ack = rd32(p + 8);
    s.flags = p[13]; s.wnd = rd16(p + 14);
    const size_t off = static_cast<size_t>(p[12] >> 4) * 4;
    s.data.assign(p + off, p + len);
    s.csumOk = tcpChecksum(src, dst, p, len) == 0;
    return s;
}

enum class Dir { ToRemote, ToConsole };
struct Event { u32 at; Dir dir; std::vector<u8> bytes; };

struct Sim {
    u32 now = 1000, delay = 20, loopMs = 10, appSleepMs = 5;
    bool inInput = false;
    std::vector<Event> q;                 // en vuelo
    std::deque<std::vector<u8>> held;     // lo que genera el hilo de la app: sale en el próximo tick del nodo
    u32 rNext = 0, rIsn = 5000, rSeq = 5001;   // receptor remoto
    unsigned unacked = 0; u32 delAckAt = 0;
    size_t received = 0;
    u32 lIsnApprox = 0;
    std::map<u32, u32> ooo;
    unsigned lost = 0, dropEvery = 0, dataCount = 0;   // pérdida determinista (0 = ninguna)
} g;

static u32 nowFn() { return g.now; }

static bool sendFn(void *, u32 dst, u8 proto, const u8 *payload, unsigned int len)
{
    if (proto != 6 || dst != PeerIp) { return false; }
    std::vector<u8> b(payload, payload + len);
    if (g.inInput) { g.q.push_back({g.now + g.delay, Dir::ToRemote, b}); }
    else { g.held.push_back(b); }
    return true;
}

static void remoteSend(u8 flags)
{
    Seg s{PeerPort, SvcPort, g.rSeq, g.rNext, flags, 65535, {}, true};
    g.q.push_back({g.now + g.delay, Dir::ToConsole, build(PeerIp, LocalIp, s)});
}

/* Receptor tipo macOS: ACK cada 2 segmentos o a los 40 ms, ventana 65535, solo en orden. */
static void remoteReceive(const std::vector<u8> &raw)
{
    const Seg s = parse(LocalIp, PeerIp, raw.data(), raw.size());
    if (!s.csumOk) { std::printf("checksum inválido desde lwIP\n"); std::exit(2); }
    if (s.flags & SYN) { g.rNext = s.seq + 1; g.lIsnApprox = s.seq + 1; remoteSend(ACK); return; }
    if (s.data.empty()) { return; }
    if (g.dropEvery && ++g.dataCount % g.dropEvery == 0) {
        ++g.lost;
        if (std::getenv("PERF_TRACE")) { std::printf("  t=%u DROP seq+%u (rNext+%u)\n", g.now, s.seq - g.lIsnApprox, g.rNext - g.lIsnApprox); }
        return;
    }
    if (std::getenv("PERF_TRACE") && s.seq != g.rNext) { std::printf("  t=%u OOO seq+%u want+%u\n", g.now, s.seq - g.lIsnApprox, g.rNext - g.lIsnApprox); }
    if (s.seq == g.rNext) {
        g.rNext += static_cast<u32>(s.data.size());
        g.received += s.data.size();
        bool filled = false;              // el receptor real conserva los segmentos fuera de orden
        for (auto it = g.ooo.find(g.rNext); it != g.ooo.end(); it = g.ooo.find(g.rNext)) {
            g.rNext += it->second; g.received += it->second; g.ooo.erase(it); filled = true;
        }
        if (filled || ++g.unacked >= 2) { g.unacked = 0; g.delAckAt = 0; remoteSend(ACK); }
        else if (g.delAckAt == 0) { g.delAckAt = g.now + 40; }
    } else {
        if (s.seq - g.rNext < 0x80000000u) { g.ooo[s.seq] = static_cast<u32>(s.data.size()); }
        remoteSend(ACK);                  // dup ACK
    }
}

/* La consola envía `total` bytes (FTP RETR). Devuelve los ms virtuales o -1 si no terminó. */
static double download(size_t total, u32 delay, u32 loopMs, u32 appSleepMs, unsigned dropEvery, unsigned *lostOut)
{
    TcpStack &t = *TcpStack::instance();
    Sim fresh;
    fresh.delay = delay; fresh.loopMs = loopMs; fresh.appSleepMs = appSleepMs; fresh.dropEvery = dropEvery;
    g = fresh;
    t.init(&sendFn, nullptr, &nowFn);
    t.setAddress(LocalIp, 0xFFFFFF00);
    const int l = t.listen(SvcPort);
    if (l < 0) { std::printf("listen falló\n"); std::exit(2); }

    const Seg syn{PeerPort, SvcPort, g.rIsn, 0, SYN, 65535, {}, true};
    g.q.push_back({g.now, Dir::ToConsole, build(PeerIp, LocalIp, syn)});

    std::vector<u8> payload(16384, 0x5A);
    size_t sent = 0;
    int conn = -1;
    const u32 start = g.now;
    u32 nextTick = g.now + g.loopMs, nextApp = g.now;
    for (u32 guard = 0; guard < 900000 && g.received < total; ++guard, ++g.now) {
        for (size_t i = 0; i < g.q.size();) {
            if (g.q[i].at > g.now) { ++i; continue; }
            const Event e = g.q[i];
            g.q.erase(g.q.begin() + static_cast<long>(i));
            if (e.dir == Dir::ToRemote) { remoteReceive(e.bytes); continue; }
            g.inInput = true;             // la respuesta de lwIP a un paquete entrante sale al instante
            t.input(PeerIp, LocalIp, e.bytes.data(), static_cast<unsigned>(e.bytes.size()));
            g.inInput = false;
        }
        if (g.delAckAt != 0 && g.now >= g.delAckAt) { g.delAckAt = 0; g.unacked = 0; remoteSend(ACK); }
        if (g.now >= nextTick) {
            nextTick = g.now + g.loopMs;
            g.inInput = true;
            t.poll();
            g.inInput = false;
            while (!g.held.empty()) {
                g.q.push_back({g.now + g.delay, Dir::ToRemote, g.held.front()});
                g.held.pop_front();
            }
        }
        if (g.now >= nextApp) {
            if (conn < 0 && t.pending(l) > 0) { conn = t.accept(l, nullptr, nullptr); }
            nextApp = g.now + g.appSleepMs;
            while (conn >= 0 && sent < total) {
                size_t want = total - sent < payload.size() ? total - sent : payload.size(); if (getenv("PERF_CHUNK")) { const size_t c = static_cast<size_t>(atoi(getenv("PERF_CHUNK"))); if (want > c) want = c; }
                const int n = t.send(conn, payload.data(), static_cast<unsigned>(want));
                if (n <= 0) { if (getenv("PERF_DEBUG") && g.now - start < 3000) std::printf("  send=%d est=%d failed=%d t=%u\n", n, t.established(conn), t.failed(conn), g.now - start); break; }
                sent += static_cast<size_t>(n);
                nextApp = g.now;
            }
        }
    }
    if (getenv("PERF_DEBUG")) { std::printf("  [dbg] sent=%zu received=%zu conn=%d now=%u q=%zu held=%zu\n", sent, g.received, conn, g.now - start, g.q.size(), g.held.size()); }
    if (lostOut != nullptr) { *lostOut = g.lost; }
    const double ms = static_cast<double>(g.now - start);
    if (conn >= 0) { t.close(conn); }
    t.closeListener(l);
    return g.received >= total ? ms : -1.0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const size_t total = 1u << 20;
    struct Case { const char *name; u32 delay, loopMs, sleepMs; unsigned drop; };
    // Modelo "actual": poll de 10 ms y TcpSleep de 5 ms. Modelo "rápido": poll de 2 ms con TCP activo y sleep de 1 ms.
    const bool fast = argc > 1 && std::strcmp(argv[1], "fast") == 0;
    const u32 loopMs = fast ? 2 : 10, sleepMs = fast ? 1 : 5;
    const Case cases[] = {
        {"RTT  10 ms, sin pérdida", 5, loopMs, sleepMs, 0},
        {"RTT  40 ms, sin pérdida", 20, loopMs, sleepMs, 0},
        {"RTT 100 ms, sin pérdida", 50, loopMs, sleepMs, 0},
        {"RTT  40 ms, 1 de 100 perdido", 20, loopMs, sleepMs, 100},
    };
    int rc = 0;
    for (const Case &c : cases) {
        unsigned lost = 0;
        const double ms = download(total, c.delay, c.loopMs, c.sleepMs, c.drop, &lost);
        if (ms < 0) { std::printf("%-30s NO TERMINÓ\n", c.name); rc = 1; continue; }
        const double kbs = static_cast<double>(total) / 1024.0 / (ms / 1000.0);
        std::printf("%-30s 1 MiB en %7.0f ms = %6.0f KB/s (perdidos %u)\n", c.name, ms, kbs, lost);
#ifndef ZTNX_PERF_BASELINE
        // Regresión: el techo teórico es ventana/RTT (10880 B / 40 ms = 265 KB/s); exigir >= 70 % con el modelo rápido.
        if (fast && c.delay == 20 && c.drop == 0 && kbs < 185.0) { std::printf("  FAIL: throughput bajo el mínimo (185 KB/s)\n"); rc = 1; }
        if (fast && c.drop != 0 && kbs < 150.0) { std::printf("  FAIL: con pérdida bajo el mínimo (150 KB/s)\n"); rc = 1; }
#endif
    }
    return rc;
}

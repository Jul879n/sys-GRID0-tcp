/*
 * sys-zerotier -- TCP sobre la red virtual (lwIP, API raw, NO_SYS).
 *
 * VNet entrega aquí los segmentos TCP; lwIP responde por `sendIp`. Sin Horizon,
 * testeable en host. No es thread-safe: quien llama serializa (Port usa
 * m_vnetLock, el mismo que protege VNet).
 *
 * Los handles son índices de tablas fijas (4 conexiones, 2 listeners). lwIP tiene
 * 6 PCB: 2 de holgura para conexiones en cierre (FIN_WAIT/LAST_ACK) mientras se
 * acepta la siguiente; sin ella, cerrar y reabrir seguido se quedaría sin PCB.
 */
#pragma once

#include "net.hpp"

struct tcp_pcb;
struct pbuf;
struct netif;
struct ip4_addr;

namespace ztnx::net {

    class TcpStack {
      public:
        static constexpr int MaxConns     = 6;
        static constexpr int MaxListeners = 4;
        static constexpr int AcceptQueue  = 4;

        /* Resultados de recv/send. */
        static constexpr int WouldBlock = -1;
        static constexpr int Failed     = -2;   /* reset, abortada o inexistente */

        using SendIpFn = bool (*)(void *ctx, u32 dstIp, u8 proto, const u8 *payload, unsigned int len);
        using NowFn    = u32 (*)();

        static TcpStack *instance();

        /* Idempotente: reinicia las tablas. Debe llamarse antes de cualquier otra función. */
        void init(SendIpFn send, void *ctx, NowFn now);
        void setAddress(u32 ip, u32 netmask);
        bool ready() const { return m_ready && m_ip != 0; }

        /* VNet -> TCP: `payload` es el segmento TCP sin cabecera IP. */
        void input(u32 srcIp, u32 dstIp, const u8 *payload, unsigned int len);
        /* Timers de lwIP (retransmisión, TIME_WAIT...). */
        void poll();

        int  listen(u16 port);                      /* listener id o -1 */
        void closeListener(int l);
        int  pending(int l) const;
        int  accept(int l, u32 *peerIp, u16 *peerPort);   /* conn id o -1 */

        int  connect(u32 ip, u16 port);             /* conn id (en curso) o -1 */
        bool established(int c) const;
        bool failed(int c) const;                   /* error o RST */
        int  recv(int c, void *buf, unsigned int max, bool peek);   /* >0, 0 = EOF, WouldBlock, Failed */
        int  send(int c, const void *buf, unsigned int len);        /* >0, WouldBlock, Failed */
        bool readable(int c) const;                 /* datos, EOF o error */
        bool writable(int c) const;                 /* establecida con espacio, o error */
        void shutdownWrite(int c);
        void close(int c);
        bool peer(int c, u32 *ip, u16 *port) const;
        u16  localPort(int c) const;
        unsigned int connsInUse() const;

      private:
        enum class State : u8 { Free, Connecting, Established, Failed };
        struct Conn {
            State state;
            bool  peerClosed;
            tcp_pcb *pcb;
            pbuf *rx;
            u32 peerIp;
            u16 peerPort, localPort;
        };
        struct Listener {
            bool used;
            tcp_pcb *pcb;
            int queue[AcceptQueue];
            int count;
        };

        static signed char cbAccept(void *arg, tcp_pcb *newpcb, signed char err);
        static signed char cbRecv(void *arg, tcp_pcb *pcb, pbuf *p, signed char err);
        static signed char cbConnected(void *arg, tcp_pcb *pcb, signed char err);
        static void cbError(void *arg, signed char err);
        static signed char netifInit(netif *n);
        static signed char netifOutput(netif *n, pbuf *p, const ip4_addr *ipaddr);

        void abortLingering();
        int  allocConn();
        void attach(int c, tcp_pcb *pcb);
        void release(int c);

        bool      m_ready = false;
        u32       m_ip = 0;
        SendIpFn  m_send = nullptr;
        void     *m_sendCtx = nullptr;
        Conn      m_conns[MaxConns] = {};
        Listener  m_listeners[MaxListeners] = {};
    };

}  // namespace ztnx::net

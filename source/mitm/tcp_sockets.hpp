/*
 * Tabla de sockets TCP virtuales del MITM de bsd:u: asocia (pid, fd) de un
 * proceso interceptado con listeners/conexiones de TcpStack (lwIP).
 *
 * El fd que ve la aplicación siempre es un fd real de bsd (un socket
 * "marcador" que el shim abre por reenvío); aquí solo se guarda qué fds están
 * atendidos por lwIP. Sin Horizon: testeable en host.
 *
 * NO es thread-safe: toda llamada se hace con el lock de Port tomado
 * (Port::WithTcp), el mismo que protege VNet y lwIP.
 */
#pragma once

#include "../net/tcp_stack.hpp"

namespace ztnx::mitm {

    using net::u16; using net::u32; using net::u64; using net::u8;
    using s32 = int32_t;

    /* errno estilo Linux, la misma convención que ya usa bsd_shim.cpp. Sin
     * verificar en hardware contra los errno reales de bsd:u. */
    constexpr int ErrAgain = 11, ErrBadF = 9, ErrInval = 22, ErrNoMem = 12, ErrPipe = 32,
                  ErrConnAborted = 103, ErrConnReset = 104, ErrIsConn = 106,
                  ErrNotConn = 107, ErrTimedOut = 110, ErrConnRefused = 111,
                  ErrInProgress = 115;

    constexpr short PollIn = 0x0001, PollOut = 0x0004, PollErr = 0x0008, PollHup = 0x0010;

    constexpr int MaxTcpSocks = 12;

    class TcpSockets {
      public:
        enum class Kind : u8 { Free, Plain, Listener, Conn };

        struct Info {
            Kind kind = Kind::Free;
            s32  listener = -1, conn = -1;
            bool nonblock = false;
            bool connectPending = false;
            u32  bindIp = 0;
            u16  bindPort = 0;
            u32  peerIp = 0;
            u16  peerPort = 0;
        };

        bool track(u64 pid, s32 fd);                       // socket(AF_INET, SOCK_STREAM)
        Info get(u64 pid, s32 fd) const;
        bool tracked(u64 pid, s32 fd) const { return get(pid, fd).kind != Kind::Free; }
        /* Atendido por lwIP: listener o conexión virtual. */
        bool virtualFd(u64 pid, s32 fd) const;
        void onBind(u64 pid, s32 fd, u32 ip, u16 port);
        void setNonblock(u64 pid, s32 fd, bool on);
        void forget(net::TcpStack &t, u64 pid, s32 fd);
        void forgetPid(net::TcpStack &t, u64 pid);

        /* listen() real ya tuvo éxito: abre el listener en la IP ZeroTier si el
         * socket está en 0.0.0.0 o en esa IP. */
        bool onListen(net::TcpStack &t, u64 pid, s32 fd, u32 ztIp);
        int  pending(net::TcpStack &t, u64 pid, s32 listenFd) const;
        /* Saca una conexión pendiente y la liga a `newFd`. false si no hay. */
        bool acceptInto(net::TcpStack &t, u64 pid, s32 listenFd, s32 newFd);

        /* connect() hacia la subred ZeroTier. 0 en curso/ok, o -errno. */
        int  connectVirtual(net::TcpStack &t, u64 pid, s32 fd, u32 ip, u16 port);
        /* Estado de una conexión virtual: 0 conectada, ErrInProgress, o errno. */
        int  connectStatus(net::TcpStack &t, u64 pid, s32 fd) const;

        /* >0 bytes, 0 EOF, o -errno (-ErrAgain si no hay datos). */
        int  recv(net::TcpStack &t, u64 pid, s32 fd, void *buf, unsigned int max, bool peek);
        int  send(net::TcpStack &t, u64 pid, s32 fd, const void *buf, unsigned int len);
        void shutdown(net::TcpStack &t, u64 pid, s32 fd, int how);
        /* revents virtuales para un pollfd (solo fds virtuales). */
        short pollEvents(net::TcpStack &t, u64 pid, s32 fd, short events) const;

        u16  localPort(net::TcpStack &t, u64 pid, s32 fd) const;
        bool localPeer(net::TcpStack &t, u64 pid, s32 fd, u32 *ip, u16 *port) const;
        unsigned int used() const;

      private:
        struct Entry {
            u64 pid;
            s32 fd;
            Info info;
        };
        Entry *find(u64 pid, s32 fd);
        const Entry *find(u64 pid, s32 fd) const;
        void release(net::TcpStack &t, Entry &e);

        Entry m_e[MaxTcpSocks] = {};
    };

}  // namespace ztnx::mitm

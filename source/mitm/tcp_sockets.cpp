#include "tcp_sockets.hpp"

namespace ztnx::mitm {

TcpSockets::Entry *TcpSockets::find(u64 pid, s32 fd)
{
    for (Entry &e : m_e) { if (e.info.kind != Kind::Free && e.pid == pid && e.fd == fd) { return &e; } }
    return nullptr;
}

const TcpSockets::Entry *TcpSockets::find(u64 pid, s32 fd) const
{
    for (const Entry &e : m_e) { if (e.info.kind != Kind::Free && e.pid == pid && e.fd == fd) { return &e; } }
    return nullptr;
}

bool TcpSockets::track(u64 pid, s32 fd)
{
    if (find(pid, fd) != nullptr) { return true; }
    for (Entry &e : m_e) {
        if (e.info.kind != Kind::Free) { continue; }
        e = Entry{};
        e.pid = pid;
        e.fd = fd;
        e.info.kind = Kind::Plain;
        return true;
    }
    return false;
}

TcpSockets::Info TcpSockets::get(u64 pid, s32 fd) const
{
    const Entry *e = find(pid, fd);
    return e != nullptr ? e->info : Info{};
}

bool TcpSockets::virtualFd(u64 pid, s32 fd) const
{
    const Entry *e = find(pid, fd);
    return e != nullptr && (e->info.kind == Kind::Listener || e->info.kind == Kind::Conn);
}

void TcpSockets::onBind(u64 pid, s32 fd, u32 ip, u16 port)
{
    if (Entry *e = find(pid, fd); e != nullptr) { e->info.bindIp = ip; e->info.bindPort = port; }
}

void TcpSockets::setNonblock(u64 pid, s32 fd, bool on)
{
    if (Entry *e = find(pid, fd); e != nullptr) { e->info.nonblock = on; }
}

void TcpSockets::release(net::TcpStack &t, Entry &e)
{
    if (e.info.kind == Kind::Listener) { t.closeListener(e.info.listener); }
    else if (e.info.kind == Kind::Conn) { t.close(e.info.conn); }
    e = Entry{};
}

void TcpSockets::forget(net::TcpStack &t, u64 pid, s32 fd)
{
    if (Entry *e = find(pid, fd); e != nullptr) { release(t, *e); }
}

void TcpSockets::forgetPid(net::TcpStack &t, u64 pid)
{
    for (Entry &e : m_e) { if (e.info.kind != Kind::Free && e.pid == pid) { release(t, e); } }
}

bool TcpSockets::onListen(net::TcpStack &t, u64 pid, s32 fd, u32 ztIp)
{
    Entry *e = find(pid, fd);
    if (e == nullptr || e->info.kind != Kind::Plain || e->info.bindPort == 0) { return false; }
    if (e->info.bindIp != 0 && e->info.bindIp != ztIp) { return false; }
    const int l = t.listen(e->info.bindPort);
    if (l < 0) { return false; }
    e->info.kind = Kind::Listener;
    e->info.listener = l;
    return true;
}

int TcpSockets::pending(net::TcpStack &t, u64 pid, s32 listenFd) const
{
    const Entry *e = find(pid, listenFd);
    return e != nullptr && e->info.kind == Kind::Listener ? t.pending(e->info.listener) : 0;
}

bool TcpSockets::acceptInto(net::TcpStack &t, u64 pid, s32 listenFd, s32 newFd)
{
    const Entry *l = find(pid, listenFd);
    if (l == nullptr || l->info.kind != Kind::Listener) { return false; }
    u32 ip = 0;
    u16 port = 0;
    const int c = t.accept(l->info.listener, &ip, &port);
    if (c < 0) { return false; }
    if (!track(pid, newFd)) { t.close(c); return false; }
    Entry *n = find(pid, newFd);
    n->info.kind = Kind::Conn;
    n->info.conn = c;
    n->info.peerIp = ip;
    n->info.peerPort = port;
    return true;
}

int TcpSockets::connectVirtual(net::TcpStack &t, u64 pid, s32 fd, u32 ip, u16 port)
{
    Entry *e = find(pid, fd);
    if (e == nullptr || e->info.kind == Kind::Listener) { return -ErrInval; }
    if (e->info.kind == Kind::Conn) { return -ErrIsConn; }
    const int c = t.connect(ip, port);
    if (c < 0) { return -ErrNoMem; }
    e->info.kind = Kind::Conn;
    e->info.conn = c;
    e->info.peerIp = ip;
    e->info.peerPort = port;
    e->info.connectPending = true;
    return 0;
}

int TcpSockets::connectStatus(net::TcpStack &t, u64 pid, s32 fd) const
{
    const Entry *e = find(pid, fd);
    if (e == nullptr || e->info.kind != Kind::Conn) { return ErrBadF; }
    if (t.established(e->info.conn)) { return 0; }
    return t.failed(e->info.conn) ? ErrConnRefused : ErrInProgress;
}

int TcpSockets::recv(net::TcpStack &t, u64 pid, s32 fd, void *buf, unsigned int max, bool peek)
{
    const Entry *e = find(pid, fd);
    if (e == nullptr || e->info.kind != Kind::Conn) { return -ErrBadF; }
    const int n = t.recv(e->info.conn, buf, max, peek);
    if (n >= 0) { return n; }
    return n == net::TcpStack::WouldBlock ? -ErrAgain : -ErrConnReset;
}

int TcpSockets::send(net::TcpStack &t, u64 pid, s32 fd, const void *buf, unsigned int len)
{
    const Entry *e = find(pid, fd);
    if (e == nullptr || e->info.kind != Kind::Conn) { return -ErrBadF; }
    const int n = t.send(e->info.conn, buf, len);
    if (n >= 0) { return n; }
    return n == net::TcpStack::WouldBlock ? -ErrAgain : -ErrPipe;
}

void TcpSockets::shutdown(net::TcpStack &t, u64 pid, s32 fd, int how)
{
    const Entry *e = find(pid, fd);
    if (e != nullptr && e->info.kind == Kind::Conn && (how == 1 || how == 2)) {
        t.shutdownWrite(e->info.conn);
    }
}

short TcpSockets::pollEvents(net::TcpStack &t, u64 pid, s32 fd, short events) const
{
    const Entry *e = find(pid, fd);
    if (e == nullptr) { return 0; }
    short r = 0;
    if (e->info.kind == Kind::Listener) {
        if ((events & PollIn) && t.pending(e->info.listener) > 0) { r |= PollIn; }
        return r;
    }
    if (e->info.kind != Kind::Conn) { return 0; }
    const int c = e->info.conn;
    if (t.failed(c)) { return static_cast<short>(PollErr | PollHup | (events & (PollIn | PollOut))); }
    if ((events & PollIn) && t.readable(c)) { r |= PollIn; }
    if ((events & PollOut) && t.writable(c)) { r |= PollOut; }
    return r;
}

u16 TcpSockets::localPort(net::TcpStack &t, u64 pid, s32 fd) const
{
    const Entry *e = find(pid, fd);
    return e != nullptr && e->info.kind == Kind::Conn ? t.localPort(e->info.conn) : 0;
}

bool TcpSockets::localPeer(net::TcpStack &t, u64 pid, s32 fd, u32 *ip, u16 *port) const
{
    const Entry *e = find(pid, fd);
    if (e == nullptr || e->info.kind != Kind::Conn) { return false; }
    return t.peer(e->info.conn, ip, port);
}

unsigned int TcpSockets::used() const
{
    unsigned int n = 0;
    for (const Entry &e : m_e) { if (e.info.kind != Kind::Free) { ++n; } }
    return n;
}

}  // namespace ztnx::mitm

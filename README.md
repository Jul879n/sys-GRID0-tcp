# sys-GRID0

A port of ZeroTier with the GRID0 network to the nintendo switch as a sysmodule, very AI assisted in the making, yet extremely functional with better results than i had hoped. Get the latest release from the releases page, or read the build instructions in build.md

## where sys-GRID0 is at right now

The whole point of this was to make native LAN play work over the internet
without needing a PC relay or another console sitting on the same network. We
have reached that point, and it has worked in actual matches.

Here is what is working at the moment:

- ZeroTier runs as a Horizon sysmodule, joins a network, keeps its identity and
  managed IP, and survives normal sleep and wake.
- The virtual IPv4 side handles ARP, IPv4, UDP and ICMP. The host tests are at
  88/88, with fuzz testing of the packet path as well.
- The `bsd:u` and `nifm:u` MITMs are doing their job. LAN discovery and game
  sockets can be sent through ZeroTier while ordinary Switch networking keeps
  working too.
- Mario Kart 8 Deluxe, Splatoon 2 and Splatoon 3 have all managed LAN play over
  ZeroTier with Switch and Ryujinx players. The other games in the whitelist
  are ready for people to try and report back on.
- There is now a proper Ultrahand overlay. It shows the current status, lets a
  user pick a saved network or type the entire 16-digit network ID, applies a
  network change without rebooting, and has switches for the sysmodule, BSD,
  NIFM and detailed logging.
- New installs turn on the sysmodule and both MITMs automatically. Saved
  networks live in `/config/sys-GRID0/networks.ini`, so an update does not
  wipe them out.
- A lot of memory work and logging cleanup has gone into keeping the module
  alive on real hardware. The larger BSD/NIFM logs are optional; the normal
  build keeps the small boot, status and uplink logs running.

## current limitations

The latest test build adds game-exit/reinitialization cleanup and automatic
UPnP/NAT-PMP router mapping. Those changes still need repeated-session testing
on hardware; see [the test notes](LIFECYCLE_NAT_TESTING.md). Router mapping can
help restrictive connections, but cannot promise a direct path through CGNAT
or networks that block UDP.

There are still things left to do. Games that only support local wireless mode
need an `ldn:u` MITM before they can use ZeroTier. The LAN whitelist covers the
games tested so far, but more compatibility testing is welcome. If something
breaks, include `status.txt`, `uplink.log`, and (when enabled) `bsd.log` and
`nifm.log` with the report so it can actually be investigated.

## the important folders

```text
source/                 sysmodule, ZeroTier port and LAN MITMs
source/net/             virtual IPv4 network and packet handling
overlay/                Ultrahand/Tesla overlay
compat/                 Horizon and dependency compatibility shims
patches/                small Horizon-specific ZeroTier patch
tests/                  host-side VNet tests and packet reference data
Atmosphere-libs/        pinned Atmosphère dependency submodule
ZeroTierOne/            pinned ZeroTier dependency submodule
```

For prerequisites, submodules, build outputs, installation, and tests, see
[build.md](build.md).

## licensing

The ZeroTier `node/` sources are MPL-2.0. Atmosphère/libstratosphere and the
other bundled dependencies retain their upstream licenses. The port, shim and
overlay code should be distributed with the license terms of the project as it
is released; do not add ZeroTier's separately licensed `libzt` component.

## TCP por la VPN (fork `sys-GRID0-tcp`)

Este fork agrega una pila TCP (lwIP 2.2.0, API raw) sobre la red virtual, para que
un servicio TCP de la consola (p. ej. un servidor FTP homebrew) sea alcanzable por la IP
ZeroTier, y para que un homebrew/juego pueda conectarse por TCP a otros miembros.

- `source/net/tcp_stack.*`: TCP sobre `VNet` (entrada por `deliverIpv4`, salida por `sendIp`).
  `lib/lwip-port/lwipopts.h`: 4 conexiones, 2 listeners, ventana de 4 x MSS; ~48 KB de `.bss`.
- `source/mitm/tcp_sockets.*` y `bsd_shim.cpp`: los sockets TCP de un proceso interceptado se
  siguen creando en el `bsd:u` real. Si el socket escucha en `0.0.0.0` o en la IP ZeroTier, se abre
  además un listener de lwIP en ese puerto. `accept`/`recv`/`send`/`poll`/`select`/`close` sobre
  conexiones que llegan por la VPN se atienden con lwIP; el fd que ve la app es un socket real
  "marcador". Un `connect` TCP hacia la subred ZeroTier también va por lwIP.
- `source/mitm/mitm_policy.hpp`: qué procesos se interceptan. Nunca módulos de sistema.

### Claves nuevas de `config.ini` (requieren reiniciar)
| Clave | Efecto |
|---|---|
| `extra_titles = 0100..., 0100...` | program ids extra a interceptar (aplicaciones o applets) |
| `mitm_homebrew = 0/1` | `1` intercepta toda aplicación (title takeover); por defecto `0` |
| `bsd_s_mitm = 0/1` | `1` registra también el MITM de `bsd:s`. libnx con `BsdServiceType_Auto` (Sphaira y gran parte del homebrew) prueba `bsd:s` antes que `bsd:u`, y sin esto no pasan por el MITM. Por defecto `0`: falta medir en hardware el pointer buffer de reenvío de `bsd:s` y el costo de que sm consulte `ShouldMitm` por cada proceso de sistema. sys-GRID0 no se intercepta a sí mismo (`SelfProgramId`). |

El `config.ini` existente en la consola no se reescribe: agregue las claves a mano.

### Cómo obtener el program id del FTP/DBI
1. Con `debug_logging = 1` en `config.ini` (reinicie), abra el homebrew.
2. Lea `sdmc:/config/sys-zerotier/bsd.log`: aparece `ShouldMitm program <id> -> no (fuera de la política)`
   una vez por program id (máx. 64).
3. Si el homebrew corre como **applet** (se abre con hbmenu desde el Álbum), el id es el del Álbum
   (`010000000000100D`) y debe ir en `extra_titles`. Si corre por **title takeover** (hbmenu mantenido
   pulsando R al abrir un juego), el id es el del juego: use `extra_titles` con ese id o `mitm_homebrew = 1`.

### Estado
- Verificado en host (`make -C tests run SANITIZE=`): handshake, datos, FIN, RST, puerto cerrado,
  checksum inválido, `connect` saliente, tabla de sockets, VNet + lwIP y la política del MITM.
- Compila el `.nsp` completo con devkitA64.
- **Sin verificar en hardware:** que el fd marcador se comporte como se espera en el `bsd:u` real,
  los errno (se usan valores estilo Linux, como el resto del shim), el valor de `O_NONBLOCK`/`FIONBIO`,
  el efecto del `.bss` adicional en el proceso, y que el FTP elegido pase por el MITM.

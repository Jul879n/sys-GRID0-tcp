# TCP por VPN en sys-GRID0 — evaluación de viabilidad

Fecha: 2026-10-23. Copia de trabajo de `~/Documents/sys-GRID0` (el original no se tocó). **Veredicto (actualizado): lwIP sí es viable en memoria y toolchain; el TCP hacia homebrew queda bloqueado por el alcance del MITM. No se integró en el netif ni en `bsd_shim.cpp`.**

## Objetivo
Que un servicio TCP de la consola (p. ej. FTP en el puerto 5000) sea accesible por la IP ZeroTier (192.168.196.194).

## Evidencia verificada
| Punto | Hallazgo | Fuente |
|---|---|---|
| lwIP | No existe en `/opt/devkitpro/portlibs/switch/lib`, ni en el `dkp` de `sys-GRID0-fw23`, ni en el árbol de sys-GRID0 (`find -ipath "*lwip*"` sin resultados). `pacman`/`dkp-pacman` no están en el PATH, por lo que no se pudo consultar un paquete | búsqueda local |
| Pila de red actual | VNet implementa ARP, IPv4, UDP e ICMP; "There is no lwIP here and no TCP" | `source/net/vnet.hpp` |
| Entrega IP | `deliverIpv4` solo atiende UDP e ICMP; el resto se descarta | `source/net/vnet.cpp:238-243` |
| Sockets de VNet | `MaxSockets = 8` (UDP) | `source/net/vnet.hpp:29` |
| Memoria | Arena fija `MallocBufferSize = 1152 KB`; con el nodo corriendo quedan ~500–544 KB libres con bloque contiguo mayor de ~500 KB. 1024 KB es "known-bad". El MITM ya cuesta ~320 KB | `source/main.cpp:44-72`, `boot.log` (arena 586 KB libres, 540 KB bloque mayor) |
| Alcance del MITM | Solo intercepta la aplicación en ejecución; los procesos de sistema usan el `bsd:u` real | `source/main.cpp:735-738` |
| Virtualización actual | Solo sockets UDP LAN de juegos (puertos 30000, 35000, 40000, 49152–49155) | `source/mitm/bsd_shim.cpp:553` |

## Por qué se detiene
1. **Sin lwIP disponible.** Habría que incorporar su código fuente (vendoring), configurarlo para el ensamblado de Horizon y mantenerlo. No se pudo comprobar que compile con esta toolchain.
2. **Memoria al límite.** El arena ya ha tenido fallos reales por falta de bloque contiguo (crash `report_0000000066005c2f.bin`). lwIP con TCP añadiría PCBs, pbufs y ventanas por conexión. Subir el arena le quita memoria al pool de sistema que necesita el bloque de 2048 KB de validación de identidad; ese equilibrio es frágil y no se puede probar sin la consola.
3. **Un homebrew de servidor no pasa por el MITM actual.** El FTP de la consola corre como aplicación o applet distinto del juego; no se verificó si `ShouldMitm` lo interceptaría ni qué sockets abre. Para que el puerto 5000 responda por la IP ZeroTier habría que virtualizar `bind`/`listen`/`accept` TCP de ese proceso o reenviar desde el `bsd:u` real, lo que cambia la semántica de la red de la consola.
4. **Riesgo.** sys-GRID0 arranca como sysmodule; un fallo deja crash reports o sin red, y solo se prueba en hardware.

## Diseño si se retomara
- Archivos a tocar: `source/net/vnet.cpp` (despachar TCP a lwIP; hoy `default: break`), `source/net/vnet.hpp` (netif de lwIP sobre `sendIpv4`), `source/mitm/bsd_shim.cpp` (`Listen`, `Accept`, `Connect`, `Bind` para TCP en la IP ZeroTier), `source/main.cpp` (arena), `Makefile` (fuentes de lwIP), `tests/` (VNet de host).
- Requisitos previos: (a) obtener lwIP y confirmar que compila con `DEVKITPRO=$HOME/Documents/sys-GRID0-fw23/dkp`; (b) medir con `boot.log` el costo real de memoria con 1–2 conexiones; (c) decidir qué proceso se virtualiza (el homebrew de FTP) y cómo se reconoce.

## Estado
- Implementación: **no iniciada** (viabilidad no demostrada).
- Pendiente de verificar: compilación de lwIP, costo de memoria en hardware, comportamiento de `ShouldMitm` con homebrew.
- Alternativa sin cambios en sys-GRID0: copiar archivos por FTP desde la red local de la consola, como hasta ahora.


## Actualización: evidencia nueva (lwIP y alcance del MITM)

### lwIP: viable (verificado)
- Obtenido STABLE-2_2_0_RELEASE desde GitHub en `~/Documents/lwip-src` (datos no confiables; no se ejecutó nada) y copiado a `lib/lwip/` (solo `src/` y `COPYING`).
- `lib/lwip-port/lwipopts.h` y `arch/cc.h`: NO_SYS=1, sin sockets/netconn, IPv4 + TCP, sin ARP/UDP/DNS/DHCP, heap propio de 24 KB, 4 PCB TCP, 2 de escucha, 16 pbuf de 1536 B, ventana de 4 x MSS (1360).
- Compilación real con `aarch64-none-elf-gcc` 16.1.0 (devkitA64), `-Os`, 18 archivos del núcleo (`core/` y `core/ipv4/`): sin errores. `size`: text 38 140 B, data 6 B, bss 52 456 B.
- Presupuesto: ~90 KB entre código y estáticos. Frente a ~540 KB contiguos libres en el arena el margen es real, pero los estáticos de lwIP irían en el .bss del NSO y no en el arena: **no medido**; se debe verificar el tamaño del segmento .bss del NSO y el `boot.log` en hardware.

### MITM: bloqueo para homebrew (verificado en código)
- `BsdShim::ShouldMitm` (`source/mitm/bsd_shim.cpp:1306-1337`) devuelve `true` solo si el `program_id` es un application id (`0x0100000000010000`–`0x01FFFFFFFFFFFFFF`, `ncm_content_meta_id.hpp:38-42`) **y** está en la lista blanca `LanTitles` (`source/mitm/lan_titles.hpp`, 21 títulos de juegos). Además descarta la primera sesión de cada proceso ("dummy session", línea 1359).
- Consecuencia: un servidor FTP homebrew (DBI, ftpd, etc.) **nunca** es interceptado. Si corre como applet (hbmenu desde el álbum), su program id no es un application id; si corre por "title takeover", su id es el del juego secuestrado y solo se interceptaría si ese juego estuviera en la lista.
- Para virtualizar `listen`/`accept` TCP de un homebrew habría que (a) ampliar `ShouldMitm` a aplicaciones fuera de la lista (el propio código advierte que interceptar procesos equivocados rompe la red de la consola), y (b) no hay forma de interceptar applets porque `ShouldMitm` filtra por application id. No verificado en hardware qué id tiene el FTP del usuario.

### Estado
- Hecho: lwIP vendored y compilado con la toolchain; configuración mínima y medición de código/estáticos.
- No hecho: netif de lwIP sobre VNet, despacho TCP en `vnet.cpp:238`, virtualización TCP en `bsd_shim.cpp`, tests de host, `Makefile`. Motivo: sin un camino para que el MITM alcance al homebrew, implementar TCP en el shim no cumpliría el criterio de FTP por VPN, y el cambio no puede probarse sin la consola.
- Pendiente de verificar en hardware: program id del FTP del usuario, medición del .bss del NSO, comportamiento al ampliar `ShouldMitm`.


## Actualización final (implementación)

**Estado: implementado en el fork y verificado en host; sin probar en la consola.** Ver la sección "TCP por la VPN" del `README.md`.

- Hecho: netif de lwIP sobre VNet (`source/net/tcp_stack.*`, hook `setTcpInput`/`sendIp` en `vnet.*`), virtualización TCP en `bsd_shim.cpp` (socket, bind, listen, accept, connect, send, recv, close, shutdown, poll, select, fcntl/ioctl no bloqueante, getpeername/getsockname, SO_ERROR), política del MITM (`mitm_policy.hpp`, claves `extra_titles` y `mitm_homebrew`), tests de host (`tests/test_tcp.cpp`, `tests/test_policy.cpp`) y entradas en `tests/Makefile`.
- Medido (devkitA64, `size` del ELF, base = `sys-GRID0.elf` del original): text +44 156 B, data +864 B, **bss +49 152 B (48 KB)**. El `.nsp` pasa de 708 448 B a 745 592 B. El arena de 1152 KB no cambia: lwIP usa memoria estática (heap 24 KB + pools), no el arena.
- Cambio de política: antes solo se interceptaban los 21 juegos de la lista blanca; ahora además `extra_titles`, y con `mitm_homebrew = 1` toda aplicación. Por defecto no cambia nada.
- Pendiente en hardware: ver README.

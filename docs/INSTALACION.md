# Instalación: ZeroTier en la Switch (sys-GRID0-tcp + overlay zerotier-nx)

Estado verificado el 2026-10-23 en una consola real con Atmosphère. Los pasos de abajo son los que se usaron; lo que no se probó está marcado.

## Qué se instala
| Pieza | Qué hace | Ruta en la SD |
|---|---|---|
| Módulo `sys-GRID0-tcp` (`exefs.nsp`) | Pone la consola en la red ZeroTier. Fork de sys-GRID0 con TCP (lwIP) y MITM ampliado | `atmosphere/contents/4200000000005A54/exefs.nsp` |
| Overlay `zerotier-nx` (`zerotier-nx.ovl`) | Pantalla en Ultrahand: estado, login por QR o manual, redes guardadas | `switch/.overlays/zerotier-nx.ovl` |
| `config.ini` | Claves del módulo | `config/sys-zerotier/config.ini` |

Requisitos: Atmosphère, Ultrahand (para el overlay) y un servidor FTP en la consola (por ejemplo el de Sphaira, puerto 5000) o un lector de tarjeta SD. Una red ZeroTier en ZeroTier Central (límite de 10 dispositivos en el plan gratuito).

## 1. Respaldo (obligatorio antes de cambiar nada)
Descargue y guarde, por FTP o desde la SD:
- `atmosphere/contents/4200000000005A54/exefs.nsp` (módulo actual).
- `config/sys-zerotier/config.ini` y `networks.ini`.

Hay además `exefs.nsp.v0.1.2` en esa carpeta (módulo original v0.1.2 de sys-GRID0).

## 2. Instalar el módulo
1. Compile o consiga `sys-GRID0-tcp.nsp`. Archivo vigente: `sys-GRID0-tcp.fast.nsp` (ventana TCP mayor y menos esperas), sha256 `99de17a63c08defbe15d043a6b1a3225ae1d392fbad925b8bc0c62bef9430e22`, instalado el 2026-10-23 y aún sin probar en hardware. Última versión probada en hardware: `sys-GRID0-tcp.pasv.nsp`, sha256 `bc3feb0b825cec14c7ed7b442464b5a9e7ed8937c5ed183260164f911b8c6169`.
2. Súbalo a la SD como `atmosphere/contents/4200000000005A54/exefs.nsp`. Ejemplo por FTP (ajuste IP y puerto):
   `curl -T sys-GRID0-tcp.pasv.nsp ftp://<IP-consola>:5000/sdmc:/atmosphere/contents/4200000000005A54/exefs.nsp`
3. Compruebe que el sha256 del archivo remoto coincide con el local:
   `curl -s ftp://<IP-consola>:5000/sdmc:/atmosphere/contents/4200000000005A54/exefs.nsp | shasum -a 256`
4. Verifique que existe `atmosphere/contents/4200000000005A54/flags/boot2.flag` (el módulo arranca con la consola). El overlay lo gestiona con el interruptor "Conexión".
5. Reinicie la consola. Las claves de `config.ini` se leen solo al arrancar.

Compilar desde el código (en el Mac): `cd sys-GRID0-tcp && DEVKITPRO=$HOME/Documents/sys-GRID0-fw23/dkp PORTLIBS=/opt/devkitpro/portlibs/switch make`. Tests de host: `make -C tests run SANITIZE=` (ASan y UBSan abortan al arrancar en este Mac, por eso `SANITIZE=` vacío).

## 3. Configurar `config/sys-zerotier/config.ini`
Sección `[sys-zerotier]`. Claves usadas:

```ini
[sys-zerotier]
network =
nwid =
bsd_mitm = 1
nifm_mitm = 1
debug_logging = 1
port_mapping = 1
mitm_homebrew = 1
bsd_s_mitm = 1
extra_titles = 010000000000100D
```

- `nwid`: el ID de red de 16 dígitos hex (también se elige desde el overlay).
- `bsd_mitm` y `nifm_mitm`: interceptan los sockets de juegos y homebrew.
- `bsd_s_mitm`: intercepta también `bsd:s`. Los homebrew (Sphaira) abren primero `bsd:s`; sin esto no pasan por el módulo.
- `mitm_homebrew`: intercepta cualquier aplicación (title takeover). `extra_titles`: ids de applets o homebrew que se interceptan siempre (`010000000000100D` es el applet Álbum, desde donde se lanzan hbmenu y Sphaira). Con `debug_logging = 1`, `bsd.log` registra el id de cada proceso fuera de la política.
- `debug_logging`: deje en 0 una vez terminadas las pruebas; los logs consumen memoria y escrituras en la SD.

Las claves nuevas del fork van a mano; la consola no reescribe el archivo.

## 4. Autorizar la consola en ZeroTier
Al primer arranque aparece un miembro nuevo en ZeroTier Central. Autorícelo (casilla "Auth"). Su Node ID se ve en el overlay ("Estado") y en `atmosphere/contents/4200000000005A54/zt/identity.public`.

## 5. Instalar el overlay
1. Compile con `cd zerotier-nx && DEVKITPRO=$HOME/Documents/sys-GRID0-fw23/dkp PORTLIBS=/opt/devkitpro/portlibs/switch make` (genera `zerotier-nx.ovl`).
2. Súbalo a `switch/.overlays/zerotier-nx.ovl` y compare sha256.
3. Ábralo desde Ultrahand. Pantallas: Estado, Iniciar sesión con QR (puerto 47613 del teléfono), Iniciar sesión manual, Redes guardadas, Acerca de.

## 6. Verificación
Con el Mac en la misma red ZeroTier (192.168.196.x en esta instalación):
1. `status.txt` (por FTP en `config/sys-zerotier/`): `online yes`, `netstatus ok`, `address 192.168.196.x/24`.
2. `ping <IP ZeroTier de la consola>`.
3. Abra el FTP de Sphaira y pruebe por la IP ZeroTier: `curl -l ftp://<IP ZeroTier>:5000/`. `bsd.log` debe mostrar `ShouldMitm … -> yes` y `ZT tcp accept … from <IP ZeroTier del Mac>`.
4. Verificado: listado, descarga de 746 535 B con hash idéntico y subida de 3 MB con hash idéntico, con el Mac en la misma LAN que la consola (camino DIRECT).

## 7. Revertir
1. Restaure el `exefs.nsp` respaldado (o `exefs.nsp.v0.1.2`) en `atmosphere/contents/4200000000005A54/` y el `config.ini` respaldado.
2. Reinicie la consola.
3. Si la consola no arranca, haga lo mismo desde la SD con un lector, o borre `atmosphere/contents/4200000000005A54/flags/boot2.flag` para que el módulo no arranque con la consola.

## Riesgos y límites conocidos
- El módulo corre al arranque de la consola. Un fallo puede dejarla sin red o generar crash reports en `atmosphere/crash_reports/`.
- No verificado: uso prolongado (memoria libre del arena y conexiones), FTP desde la red móvil por relay con medición, y juegos LAN por UDP.
- Rendimiento: una descarga de 746 KB por VPN tardó 38,9 s con camino directo; `sys-GRID0-tcp.fast.nsp` busca mejorarlo (en simulación, de 105 a 248 KB/s con RTT de 40 ms); falta medirlo en la consola.
- Por relay (CGNAT, red móvil) la latencia medida fue de 230–440 ms.
- El FTP no es accesible por la VPN con el módulo original de sys-GRID0; solo con este fork (TCP) y con `bsd_s_mitm` activo.
- Los juegos con modo inalámbrico local (`ldn:u`) no tienen soporte.
- El fork está en un repo privado (`Jul879n/sys-GRID0-tcp`) sin commit todavía.

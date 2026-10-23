#!/bin/sh
# Arma el paquete para la SD de la consola (sin logs de depuración) y crea el release privado en GitHub.
# Uso: tools/release.sh <tag> [ruta-ovl]   Requiere: make ya ejecutado, gh autenticado.
set -eu
TAG="${1:?uso: tools/release.sh <tag> [ruta-ovl]}"
OVL="${2:-$HOME/Documents/zerotier-nx/zerotier-nx.ovl}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NSP="$ROOT/sys-GRID0-tcp.nsp"
[ -f "$NSP" ] || { echo "falta $NSP (compile con make)"; exit 1; }
[ -f "$OVL" ] || { echo "falta $OVL"; exit 1; }
OUT="$ROOT/dist/zerotier-switch-$TAG"
rm -rf "$OUT"
mkdir -p "$OUT/atmosphere/contents/4200000000005A54/flags" "$OUT/switch/.overlays" "$OUT/config/sys-zerotier"
cp "$NSP" "$OUT/atmosphere/contents/4200000000005A54/exefs.nsp"
: > "$OUT/atmosphere/contents/4200000000005A54/flags/boot2.flag"
cp "$OVL" "$OUT/switch/.overlays/zerotier-nx.ovl"
cat > "$OUT/config/sys-zerotier/config.ini" <<'EOC'
[sys-zerotier]
network =
nwid =
bsd_mitm = 1
nifm_mitm = 1
debug_logging = 0
port_mapping = 1
mitm_homebrew = 1
bsd_s_mitm = 1
extra_titles = 010000000000100D
EOC
cp "$ROOT/docs/INSTALACION.md" "$OUT/LEEME.md" 2>/dev/null || true
( cd "$OUT" && shasum -a 256 atmosphere/contents/4200000000005A54/exefs.nsp switch/.overlays/zerotier-nx.ovl > SHA256SUMS.txt )
ZIP="$ROOT/dist/zerotier-switch-$TAG.zip"
rm -f "$ZIP"
( cd "$OUT" && zip -qr "$ZIP" . )
shasum -a 256 "$ZIP"
gh release create "$TAG" "$ZIP" --repo "$(git -C "$ROOT" remote get-url origin | sed 's#.*github.com[/:]##; s#\.git$##')" \
    --title "ZeroTier para Switch $TAG" --notes "Paquete para copiar a la raíz de la SD (Atmosphère + Ultrahand). Sin logs de depuración (debug_logging = 0). Pasos y reversión en LEEME.md."

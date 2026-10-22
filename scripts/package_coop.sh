#!/usr/bin/env bash
# Linux/cross-build twin of package_coop.ps1: builds dist/cdmp-coop-<date>.zip with the ASI loader
# (as winmm.dll), the mod, the host's and the guest's configs, the relay and the instructions.
# Quest sharing is switched on in both configs ([coop] stage_sync=1, quest_share=1).
#   scripts/package_coop.sh [bin dir, default build-x/bin]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
bin=${1:-$root/build-x/bin}
for f in cdmp.asi cdmp_peer.exe; do [ -f "$bin/$f" ] || { echo "Build first: $f not found in $bin" >&2; exit 1; }; done
loader=$root/third_party/ultimate-asi-loader/v9.7.4/dinput8.dll
echo "031a3e5576d91dce1e438d36b9a3d462c7334ab4791990a8ff1e3ddc0e132daf  $loader" | sha256sum -c --quiet
stamp=$(date +%Y%m%d)
staging=$root/dist/cdmp-coop-$stamp
rm -rf "$staging" "$staging.zip"; mkdir -p "$staging"
cp "$loader" "$staging/winmm.dll"
cp "$bin/cdmp.asi" "$bin/cdmp_peer.exe" "$staging/"
quests() {  # add the quest keys right after [coop]
  sed 's/^\[coop\]\r\{0,1\}$/[coop]\
; Mision compartida (experimental): las ramas de escena y los encargos aceptados del anfitrion\
; tambien en el invitado. Cada uno entrega su propio encargo.\
; Shared quests (experimental): the host'"'"'s scene branches and accepted requests also on the guest.\
; Each player hands in their own request.\
stage_sync=1\
quest_share=1/' "$1" | sed 's/^\[sniffer\]\r\{0,1\}$/[sniffer]\
; Grabadora de vuelo: flight\\*.cofr para comparar las dos partidas si algo sale mal (Ctrl+F12 guarda una)\
; Flight recorder: flight\\*.cofr to compare both games when something goes wrong (Ctrl+F12 saves one)\
flight=1/'
}
quests "$root/kit/coop/cdmp.ini.anfitrion" > "$staging/cdmp.ini.anfitrion"
quests "$root/kit/coop/cdmp.ini.invitado" > "$staging/cdmp.ini.invitado"
cp "$staging/cdmp.ini.invitado" "$staging/cdmp.ini"
cp "$root/kit/coop/LEEME.txt" "$staging/"
cp "$root/kit/coop/TEST-QUESTS.md" "$staging/"
grep -c '^quest_share=1' "$staging/cdmp.ini.anfitrion" "$staging/cdmp.ini.invitado" >/dev/null
grep -c '^flight=1' "$staging/cdmp.ini.anfitrion" "$staging/cdmp.ini.invitado" >/dev/null
(cd "$staging" && sha256sum $(ls | grep -v SHA256.txt) > SHA256.txt)
(cd "$(dirname "$staging")" && python3 -I -c "
import sys, zipfile, os
d = sys.argv[1]
with zipfile.ZipFile(d + '.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for f in sorted(os.listdir(d)): z.write(os.path.join(d, f), f)
" "$(basename "$staging")")
echo "Co-op kit: $staging.zip ($(( $(stat -c %s "$staging.zip") / 1024 )) KB)"

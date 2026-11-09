#!/usr/bin/env bash
# Est-ce que les colonnes IN et OUT bougent vraiment ?
#
#   test/meter-motion-test.bash [chemin.du.VoLum.vst3|le.AppImage]
#
# VoLum ne mesure ses niveaux qu'apres avoir recu le signal que son editeur est
# ouvert (OnUIOpen) : un port qui oublie cette poignee montre une fenetre qui se
# charge, se dessine, prend la souris, fait tourner l'ampli - avec deux colonnes
# eteintes et rien dans le journal. Le seul moyen de separer « les metres sont
# morts » de « l'ampli ne regoit rien », ce sont les pixels. Le backend Linux
# relit son propre back-buffer (VOLUM_LINUX_CAPTURE) et ecrit trois images de la
# fenetre en cours de fonctionnement ; ce test mesure les deux colonnes la-dedans.
#
# Rien ne sort dans les enceintes : le son entre dans le plug-in par --in-file,
# donc aucune carte d'acquisition n'est ouverte, et la sortie part dans un sink
# PipeWire jete qui est demonte - le sink par defaut du bureau remis - avant que
# le script ne se termine. Le son par paliers (fort, doux, fort...) est ce qui
# donne a une barre une raison de changer de hauteur entre deux images : une
# colonne qui clignote au hasard ne passe pas le test.
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
target="${1:-}"
outdir="${OUTDIR:-/tmp/volum-meter-motion}"
run_seconds="${RUN_SECONDS:-9}"
tone=/tmp/volum-meter-motion-step-tone.wav
mod=""
old_sink=""
xvfb_pid=""

mkdir -p "$outdir"
rm -f "$outdir"/meter.*.ppm

cleanup()
{
  [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null
  if [ -n "$mod" ]; then
    pactl set-default-sink "$old_sink" 2>/dev/null
    pactl unload-module "$mod" 2>/dev/null
  fi
  rm -f "$tone"
}
trap cleanup EXIT

# --- ce qu'on fait tourner : le paquet, ou le fichier installe ---------------
if [ -z "$target" ]; then
  target="$here/../../../build/linux-appimage"
fi

if [ -d "$target" ]; then
  appimage=$(find "$target" -maxdepth 1 -name "VoLum-linux-x86_64-*.AppImage" | head -1)
  [ -n "$appimage" ] || appimage=""
  if [ -n "$appimage" ]; then
    # `gh release download` leaves the executable bit off, and the point of this
    # test is the file someone downloads - so put the bit back instead of
    # testing a copy of it somewhere else.
    [ -x "$appimage" ] || chmod +x "$appimage"
    run=("$appimage" --appimage-extract-and-run)
  else
    bundle="$target/VoLum.vst3"
    [ -d "$bundle" ] || bundle="$here/../../../build/linux/VoLum.vst3"
    [ -x "$target/volum-app" ] || target="$here/../../../build/linux"
    run=("$target/volum-app" --module "$bundle")
  fi
elif [ -f "$target" ]; then
  case "$target" in
    *.AppImage)
      [ -x "$target" ] || chmod +x "$target"
      run=("$target" --appimage-extract-and-run)
      ;;
    *) run=("$here/../../../build/linux/volum-app" --module "$target") ;;
  esac
else
  echo "rien a tester sur « $target » (ni un .AppImage, ni un paquet VST3)"
  exit 1
fi

command -v pactl >/dev/null 2>&1 || { echo "test ignore : pactl est absent, pas de sink jete possible"; exit 0; }

# --- le peripherique de sortie qu'on a le droit de faire sonner --------------
old_sink=$(pactl get-default-sink 2>/dev/null || true)
mod=$(pactl load-module module-null-sink sink_name=volum-meter-test \
        sink_properties=device.description=VoLum-metre-test 2>/dev/null || true)
[ -n "$mod" ] || { echo "test ignore : pas de null-sink PipeWire, aucun peripherique muet disponible"; exit 0; }
pactl set-default-sink volum-meter-test

# --- un son qui monte et qui redescend --------------------------------------
python3 - "$tone" <<'PY'
import math, struct, sys, wave
rate, f = 48000, 440.0
frames = bytearray()
for i in range(int(rate * 12.0)):
    amp = 0.2 if int(i / (0.4 * rate)) % 2 == 0 else 0.0032
    s = int(32767 * amp * math.sin(2 * math.pi * f * i / rate))
    frames += struct.pack("<hh", s, s)
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
    w.writeframes(bytes(frames))
PY
[ -f "$tone" ] || { echo "le son de test n'a pas pu etre ecrit"; exit 1; }

# --- l'ecran que personne ne regarde ----------------------------------------
if ! xdpyinfo -display :99 >/dev/null 2>&1; then
  (Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1) &
  xvfb_pid=$!
  sleep 2
fi

# --- la fenetre, mesuree -----------------------------------------------------
DISPLAY=:99 VOLUM_LINUX_CAPTURE="$outdir/meter" \
  "${run[@]}" --out default --in-file "$tone" --period 256 \
  > "$outdir/meter.log" 2>&1 &
app=$!
sleep "$run_seconds"
kill -TERM "$app" 2>/dev/null
wait "$app" 2>/dev/null

ls "$outdir"/meter.*.ppm >/dev/null 2>&1 || {
  echo "aucune image capturee : le backend n'a pas dessine (voir $outdir/meter.log)"
  exit 1
}

python3 - "$outdir" <<'PY'
import sys
import numpy as np
from PIL import Image

out = sys.argv[1]
fr = [np.asarray(Image.open(f"{out}/meter.{i}.ppm")).astype(np.int32) for i in (1, 2, 3)]
h, w = fr[0].shape[:2]
bands = {"IN": (int(w * 0.010), int(w * 0.040)), "OUT": (int(w * 0.960), int(w * 0.990))}
y0, y1 = int(h * 0.75), int(h * 0.97)

def column(img, x0, x1):
    lum = img[y0:y1, x0:x1].sum(axis=2) / 3.0
    lit = (lum > 150).any(axis=1)
    idx = np.nonzero(lit)[0]
    return (len(idx), int(y0 + idx.min())) if len(idx) else (0, 0)

print(f"fenetre {w} x {h}, bandes {bands}, lignes {y0}-{y1}")
worst = 10**9
for name, (x0, x1) in bands.items():
    heights = [column(img, x0, x1) for img in fr]
    for i, (n, top) in enumerate(heights, start=1):
        print(f"  {name:3s} image {i} : {n:3d} segments allumes, sommet y={top}")
    spread = max(n for n, _ in heights) - min(n for n, _ in heights)
    worst = min(worst, spread, max(n for n, _ in heights))
    print(f"  {name:3s} : ecart entre la plus haute et la plus basse = {spread} segments")

changed = int((np.abs(fr[1] - fr[2]).sum(axis=2) > 18).sum())
print(f"pixels qui changent entre l'image 2 et l'image 3 : {changed}")

if worst >= 10 and changed > 500:
    print("verdict : les colonnes IN et OUT suivent le son")
    sys.exit(0)
print("verdict : une des colonnes ne bouge pas - le plug-in n'a pas ete averti que son editeur est ouvert")
sys.exit(1)
PY

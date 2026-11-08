#!/usr/bin/env bash
# Check the portal client of the Linux port without asking anybody to click.
#
#   test/portal-test.bash [build-dir]
#
# Everything happens on a throwaway session bus whose only portal is
# test/fake-portal.py, so nothing appears on the desktop of whoever is sitting at
# the machine. The fake also refuses a request it cannot read: a filter list, a
# folder hint or a token that left this process in the wrong shape is a test
# failure rather than a dialog nobody looks at.
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="${1:-"$here/../../../build/linux"}"
client="$build/volum-portal-check"
log=/tmp/volum-portal-fake.log

if [ ! -x "$client" ]; then
  echo "volum-portal-check absent : construis build/linux (le configure a besoin de gio-2.0)"
  exit 1
fi

if ! command -v dbus-run-session >/dev/null 2>&1 || ! python3 -c 'import gi' >/dev/null 2>&1; then
  echo "test ignore : il faut dbus-run-session et python3-gi (glib2-devel suffit ailleurs)"
  exit 0
fi

# Re-run ourselves inside a private bus, which is what keeps this off the session's
# real portal - and off the screen.
if [ -z "${VOLUM_PORTAL_TEST_BUS:-}" ]; then
  export VOLUM_PORTAL_TEST_BUS=1
  exec dbus-run-session -- "$0" "$build"
fi

fake=""

stop_fake()
{
  if [ -n "$fake" ]; then
    kill "$fake" 2>/dev/null
    wait "$fake" 2>/dev/null
    fake=""
  fi
}
trap stop_fake EXIT

start_fake()
{
  stop_fake
  python3 "$here/fake-portal.py" "$@" >"$log" 2>&1 &
  fake=$!

  # The bus name has to be owned before the first request goes out, or the client
  # would time out and look broken when it is the fixture that is not ready.
  for _ in $(seq 1 50); do
    if python3 -c '
import gi, sys
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib
conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
reply = conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                       "ListNames", None, GLib.VariantType("(as)"), Gio.DBusCallFlags.NONE, 2000, None)
sys.exit(0 if reply and "org.freedesktop.portal.Desktop" in reply.unpack()[0] else 1)
' 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done

  echo "ECHEC : le faux portail n'a pas pris le nom org.freedesktop.portal.Desktop"
  cat "$log"
  exit 1
}

check()
{
  local name="$1" want="$2" pattern="$3"
  shift 3
  local out code
  # timeout, because the client would otherwise sit in front of a dialog that never
  # arrived for its full fifteen minutes - and CI would wait with it.
  out="$(timeout 30 "$client" "$@" 2>&1)"
  code=$?

  if [ "$code" -ne "$want" ] || ! printf '%s\n' "$out" | grep -q -- "$pattern"; then
    echo "ECHEC $name : code $code (attendu $want), sortie : $(printf '%s' "$out" | tr '\n' ' ')"
    fail=1
  else
    checks=$((checks + 1))
    echo "ok      $name"
  fi
}

checks=0
fail=0

check_log()
{
  local name="$1" pattern="$2"

  if grep -q -- "$pattern" "$log"; then
    checks=$((checks + 1))
    echo "ok      $name"
  else
    echo "ECHEC $name : introuvable dans le journal du faux portail"
    sed -n '1,20p' "$log"
    fail=1
  fi
}

start_fake
check "le paquet repond sans clic" 0 'file chooser version 4' --probe
check "ouvrir des captures" 0 'picked: /tmp/volum-fake/capture.nam' --open --path /tmp
check "un nom avec un espace, decoding %25" 0 'picked: /tmp/volum-fake/room ir.wav' --open
check "choisir un dossier" 0 'picked: /tmp/volum-fake$' --dir
check "enregistrer un pack" 0 'picked: /tmp/volum-fake/pack de VoLum.volumpack' --save --path /tmp/volum-fake --name "pack de VoLum.volumpack"
check "couleur" 0 'color: 0.500 0.250 0.125' --color
# iPlug spells the types as extensions; the desktop wants patterns, and a "*" means
# the caller meant every file, so no type menu at all.
check "une extension joker ne propose aucun filtre" 0 'picked: /tmp/volum-fake/capture.nam' --open --ext "*"
check_log "les filtres sont arrives" 'filters=VoLum files:\*\.nam,\*\.wav'
check_log "le dossier et le nom a enregistrer sont arrives" 'folder=/tmp/volum-fake name=pack de VoLum.volumpack'
check_log "un dossier est demande comme dossier" 'directory=True'
check_log "le joker n'a transmis aucun filtre" 'filters=none'
stop_fake

if grep -q 'refused' "$log"; then
  echo "ECHEC : le faux portail a refuse au moins une requete"
  cat "$log"
  fail=1
fi

start_fake --cancel
check "annulation" 3 'cancelled' --open
stop_fake

start_fake --legacy-rgb
check "couleur a l'ancienne (ii)" 0 'color: 0.502 0.251 0.125' --color
stop_fake

if grep -q 'refused' "$log"; then
  echo "ECHEC : le faux portail a refuse au moins une requete"
  cat "$log"
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  echo "--- portal-check : $(cat "$log")"
  exit 1
fi

echo "portail : $checks controles, aucun clic"
exit 0

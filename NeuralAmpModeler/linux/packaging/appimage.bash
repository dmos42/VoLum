#!/usr/bin/env bash
# Build an AppImage of the VoLum standalone.
#
#   NeuralAmpModeler/linux/packaging/appimage.bash [build-dir] [out-dir]
#
# An AppImage is a squashfs image of a mini filesystem tree (the "AppDir") with a
# small runtime program glued in front of it: the runtime mounts the image, exports
# APPDIR and a few paths, and execs AppRun. So most of this script is assembling that
# tree, which is the same install rules `cmake --install` already provides:
#
#   usr/bin/volum-app                     the program, exactly as installed
#   usr/share/applications/volum.desktop   the menu entry, also copied to the root
#   usr/share/icons/hicolor/.../volum.png  the icon, found through XDG_DATA_DIRS
#   usr/VoLum.vst3                         the plug-in the program loads
#   AppRun                                 what the runtime executes
#
# The bundle goes one level above bin/ on purpose: volum-app searches its own
# directory, then its parent, for VoLum.vst3 - so the application carries its plug-in
# and still loads it the way it loads an installed one, with no search path invented
# for the package.
#
# Nothing is pulled out of the system to be shipped inside: no libasound (it reads the
# user's ALSA configuration and its plugin modules live beside the system library), no
# libGL or libX11 (they belong to the machine's graphics stack), no glib (the portal is
# the desktop's own, and a private copy could not talk to its bus). What an AppImage of
# VoLum contains is VoLum.
set -uo pipefail

# Pinned, because an artifact that is supposed to be reproducible should not be built
# by whatever the release channel handed out that day.
tool="${VOLUM_APPIMAGETOOL:-appimagetool-x86_64.AppImage}"
tool_url="${VOLUM_APPIMAGETOOL_URL:-https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage}"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
build="${1:-"$repo/build/linux"}"
out="${2:-"$repo/build/linux-appimage"}"

if [ ! -x "$build/volum-app" ]; then
  echo "volum-app absent de $build : construis d'abord (cmake --build build/linux)"
  exit 1
fi
if [ ! -d "$build/VoLum.vst3" ]; then
  echo "paquet VoLum.vst3 absent de $build"
  exit 1
fi

# The version the release scripts read (NeuralAmpModeler/config.h), so the file name
# and the "VoLum <version>" line the plug-in logs on startup say the same thing.
version="$(sed -n 's/.*PLUG_VERSION_STR[[:space:]]*"\([^"]*\)".*/\1/p' "$repo/NeuralAmpModeler/config.h" 2>/dev/null | head -1)"
[ -n "$version" ] || version="dev"

arch="$(uname -m)"
[ "$arch" = "x86_64" ] || { echo "AppImage pour x86_64 seulement ici (arch=$arch)"; exit 1; }

appdir="$out/AppDir"
echo "AppDir : $appdir"
rm -rf "$appdir"
mkdir -p "$appdir"

# The install rules, staged. --prefix /usr is what the AppImage layout wants: the
# runtime puts the tree at $APPDIR, so "system" paths inside it are /usr/... relative.
DESTDIR="$appdir" cmake --install "$build" --prefix /usr > /dev/null
test -x "$appdir/usr/bin/volum-app" || { echo "l'installation stagee n'a pas de volum-app"; exit 1; }

# Where the install rules put the VST3 bundle is a path, not a prefix: the build tree's
# cache says "$HOME/.vst3", and DESTDIR turns that into $appdir/home/<user>/.vst3 - a
# second full copy of the plug-in and its 249 rigs, 83 MB of it, under a username that
# means nothing on the machine that will run this. The bundle the program actually loads
# is the one put next to bin/ below, so the staged home tree goes.
if [ -d "$appdir/home" ]; then
  rm -rf "$appdir/home"
  echo "retire : $appdir/home (copie de trop du paquet VST3, 83 Mo)"
fi

# The plug-in, next to the prefix's bin so the program finds it the way it finds an
# installed bundle (its own directory, then its parent).
cp -a "$build/VoLum.vst3" "$appdir/usr/VoLum.vst3"

cp "$appdir/usr/share/applications/volum.desktop" "$appdir/volum.desktop"
sed -i 's|^Exec=.*|Exec=AppRun|' "$appdir/volum.desktop"
grep -q '^Icon=volum' "$appdir/volum.desktop" || sed -i '$a Icon=volum' "$appdir/volum.desktop"
cp "$appdir/usr/share/icons/hicolor/256x256/apps/volum.png" "$appdir/volum.png"

cat > "$appdir/AppRun" <<'APPRUN'
#!/usr/bin/env bash
# The entry point an AppImage runs. Kept to what VoLum needs: the bundle beside this
# tree, the icons for whoever shows them, and nothing else - in particular no
# LD_LIBRARY_PATH pointing at a private copy of ALSA or GLib, whose job is to talk to
# this machine's sound server and session bus.
here="$(dirname "$(readlink -f "${0}")")"

export XDG_DATA_DIRS="$here/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export VOLUM_APPIMAGE=1

exec "$here/usr/bin/volum-app" "$@"
APPRUN
chmod +x "$appdir/AppRun"

# appimagetool itself: fetched on demand rather than vendored (it is ~10 MB of
# prebuilt binary, and it is a build tool, not part of what is shipped).
toolpath="${VOLUM_APPIMAGETOOL_PATH:-/tmp/volum-tools/$tool}"
if [ ! -x "$toolpath" ]; then
  mkdir -p "$(dirname "$toolpath")"
  echo "telechargement de $(basename "$tool") (pinne par URL ; utilise VOLUM_APPIMAGETOOL_PATH pour le fournir)"
  curl -fsSL -o "$toolpath" "$tool_url" || { echo "impossible de telecharger $tool_url"; exit 1; }
  chmod +x "$toolpath"
fi

name="VoLum-linux-${arch}-${version}.AppImage"
rm -f "$out/$name"

# appimagetool refuses an AppDir whose files are not world-readable, and a working
# tree's umask does not guarantee that: the staged tree is made readable here instead
# of hoping the checkout came out that way.
chmod -R a+rX "$appdir"
chmod +x "$appdir/AppRun" "$appdir/usr/bin/volum-app"

# zstd is what this machine's squashfs-tools was built with (xz is not compiled in on
# Fedora 44), and it is appimagetool's own default; a kernel older than 5.9 cannot read
# the image, which is a wider floor than the glibc one VoLum already carries.
# No update information is embedded: there is no AppImage release to point at yet, and
# an update string that resolves to nothing is worse than none. When releases exist, the
# line to add is -u with a zsync URL. -n skips the AppStream metadata check, which
# wants a .metainfo.xml VoLum does not have yet.
ARCH="$arch" "$toolpath" --comp zstd -n "$appdir" "$out/$name" || {
  echo "appimagetool a echoue (FUSE necessaire pour le lancer ; sans lui, monter l'image a la main)"
  exit 1
}

ls -l "$out/$name"
echo "verifie : $(basename "$out")/$name"

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
#   usr/share/applications/com.github.guitarlum.VoLum.desktop
#                                        the menu entry, also copied to the root
#   usr/share/icons/hicolor/.../com.github.guitarlum.VoLum.png
#                                        the icon, found through XDG_DATA_DIRS
#   usr/share/metainfo/com.github.guitarlum.VoLum.metainfo.xml
#                                        what a software centre shows about this program
#   usr/VoLum.vst3                        the plug-in the program loads
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

# The AppStream application id, which is also the name of the .desktop file, of the hicolor
# icon and of the metainfo file - three names that have to agree or the desktop's software
# centre sees a program with no icon and no description. It follows the upstream project
# (guitarlum/VoLum) so an upstream Linux build reuses the same id; the fork is only the one
# that ships a Linux build today, which is why the metainfo's bug tracker is the fork's.
appid="com.github.guitarlum.VoLum"

# Update information, written into the runtime in front of the squashfs so a user is offered
# an update without anyone having to invent a channel: gh-releases-zsync resolves to the
# newest release's asset of this name, and the .zsync beside it is what turns the update into
# a partial download rather than 34 MB again. The string only resolves once such a release
# exists, and it is read back out of the finished image below rather than trusted.
# VOLUM_UPDATE_INFO='' builds an image without it, which is what a throwaway test wants.
update_info="${VOLUM_UPDATE_INFO-gh-releases-zsync|dmos42|VoLum|latest|VoLum-linux-x86_64-*.AppImage}"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
build="${1:-"$repo/build/linux"}"
out="${2:-"$repo/build/linux-appimage"}"

# Made absolute on purpose. appimagetool names its side artifacts - the .zsync an updater
# reads - after the destination it is given, and a relative destination resolves against the
# working directory rather than the image: building from the repository root with
# `build/linux-appimage/VoLum…` as the target left the .zsync lying at the root, beside the
# checkout, while the build directory had no idea it existed.
mkdir -p "$out"
out="$(cd "$out" && pwd)"

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

cp "$appdir/usr/share/applications/$appid.desktop" "$appdir/$appid.desktop"
sed -i 's|^Exec=.*|Exec=AppRun|' "$appdir/$appid.desktop"
# appimagetool reads the desktop entry at the AppDir root and takes the icon from the name
# in `Icon=`: an icon that does not exist under that name gives a package with no picture in
# any menu, and no complaint at build time. So the name is checked, not guessed.
grep -q "^Icon=$appid\$" "$appdir/$appid.desktop" \
  || { echo "le .desktop installe n'a pas Icon=$appid : l'icone installee ne serait pas trouvee"; exit 1; }
cp "$appdir/usr/share/icons/hicolor/256x256/apps/$appid.png" "$appdir/$appid.png"
test -s "$appdir/usr/share/metainfo/$appid.metainfo.xml" \
  || { echo "metainfo $appid.metainfo.xml absent de l'installation stagee"; exit 1; }

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
img="$out/$name"
rm -f "$img" "$img.zsync"

# appimagetool is itself an AppImage, so it wants FUSE to start - and a GitHub runner, or a
# build container, has none. `--appimage-extract-and-run` is the same tool with a temporary
# extraction instead of a mount, which is also what the release notes tell a user without
# FUSE to do when running VoLum's own image. The mount device is checked rather than assumed:
# a machine that has /dev/fuse gets the plain path, and the extraction cost stays off it.
toolrun=("$toolpath")
if [ ! -e /dev/fuse ]; then
  echo "pas de /dev/fuse : appimagetool tourne par extraction (--appimage-extract-and-run)"
  toolrun=("$toolpath" --appimage-extract-and-run)
fi

# appimagetool refuses an AppDir whose files are not world-readable, and a working
# tree's umask does not guarantee that: the staged tree is made readable here instead
# of hoping the checkout came out that way.
chmod -R a+rX "$appdir"
chmod +x "$appdir/AppRun" "$appdir/usr/bin/volum-app"

# zstd is what this machine's squashfs-tools was built with (xz is not compiled in on
# Fedora 44), and it is appimagetool's own default; a kernel older than 5.9 cannot read
# the image, which is a wider floor than the glibc one VoLum already carries.
#
# No -n: the AppDir carries AppStream metadata now, so appimagetool validates it, and part of
# what it validates is that the metainfo's id, the .desktop at the root and the icon it names
# are the same program. A tree where those three disagree used to fail in a software centre
# six months later, silently.
toolargs=(--comp zstd)
if [ -n "$update_info" ]; then
  toolargs+=(-u "$update_info")
  # appimagetool's own help says the .zsync is produced "if zsyncmake is installed", and what
  # "installed" means here is inside appimagetool's own image rather than on this machine's
  # PATH - which is why the file is not looked for before the build but insisted on after it,
  # next to the image it describes. VOLUM_REQUIRE_ZSYNC=1 is what CI sets: an image that
  # announces an update channel with nothing behind it is worse than one that announces
  # nothing, and the release notes' fingerprints are worth less than a real updater.
  :
fi

# Run the tool from inside the output directory, because that is where it writes what it
# produces beside the image: measured here with an absolute destination given, the `.zsync`
# still appeared in the directory the build was started from (twice: the repository root,
# which is where nobody looks for it and where `git status` finds it immediately). One `cd`,
# and both files land where the release step will look for them.
(
  cd "$out" || exit 1
  ARCH="$arch" "${toolrun[@]}" "${toolargs[@]}" "$appdir" "$name"
) || {
  echo "appimagetool a echoue ($toolpath)"
  exit 1
}

# Belt and braces for the same behaviour from a different direction: if a .zsync did turn up
# at the caller's working directory, it belongs beside the image, not there.
if [ -s "$name.zsync" ] && [ "$out" != "$(pwd)" ]; then
  mv -f "$name.zsync" "$img.zsync"
  echo "deplace : $(basename "$img").zsync (appimagetool l'avait ecrit dans $(pwd))"
fi

ls -l "$img"

if [ -n "$update_info" ]; then
  # The update string is not a side file: it is an ELF section of the runtime glued in front
  # of the squashfs, so the finished image is what gets read. An image that says it can be
  # updated and carries nothing to update from is worse than one that says nothing.
  embedded="$(readelf -p .upd_info "$img" 2>/dev/null \
              | sed -n 's/^ *\[[ 0-9a-f]*\] \(.*\)$/\1/p' | head -1)"
  case "$embedded" in
    *zsync*) echo "verifie : update information dans l'image -> $embedded" ;;
    *) echo "l'image ne contient pas l'update information demandee ($update_info)"; exit 1 ;;
  esac
  if [ -s "$img.zsync" ]; then
    # And the .zsync is the half an updater can actually read: the image announces a channel,
    # this is what is in it. Checked by name and by content, since a file left at the working
    # directory by a relative destination would satisfy the name test and serve nobody.
    grep -q "^URL: ${name}$" "$img.zsync" \
      || echo "attention : le .zsync n'annonce pas $(basename "$img") comme URL ($(sed -n 's/^URL: //p' "$img.zsync" | head -1))"
    ls -l "$img.zsync"
  elif [ "${VOLUM_REQUIRE_ZSYNC:-0}" = "1" ]; then
    echo "pas de .zsync a cote de l'image : l'updater saurait quoi chercher mais n'aurait rien a lire"
    exit 1
  else
    echo "pas de .zsync : cette image se teste, elle ne se publie pas (VOLUM_REQUIRE_ZSYNC=1 en decide autrement)"
  fi
fi

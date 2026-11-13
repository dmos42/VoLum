#!/usr/bin/env bash
# Build the distribution packages the AppImage does not cover (issue #4):
#
#   NeuralAmpModeler/linux/packaging/dist.bash [build-dir] [out-dir]
#
#   VoLum-linux-x86_64-<version>.tar.gz   the stand-alone, the VST3, LV2 and CLAP
#                                         bundles, the menu entry, the icon and the
#                                         metainfo, in a /usr
#                                         layout: `sudo tar -C / -xzf <file>` installs
#                                         it, or unpack it anywhere and copy usr/ into
#                                         the prefix of your choice. The rig collection
#                                         is deliberately NOT in it - a 4 MB package
#                                         and a 77 MB data set have different update
#                                         rhythms, so the rigs travel separately.
#   VoLum-rigs-<version>.tar.gz           the 249 bundled captures, as VoLum/VoLumRigs/:
#                                         `sudo tar -C /usr/share -xzf <file>` for a
#                                         system install, or
#                                         `tar -C ~/.local/share -xzf <file>` per user -
#                                         both are folders the plug-in searches
#                                         (VoLumPaths.h, LinuxRigsCandidates).
#   x86_64/volum-<version>-1.*.rpm        the same two as RPMs for Fedora/openSUSE,
#                                         built by rpmbuild from those tarballs
#                                         (volum + noarch volum-rigs).
#
# The AppImage stays the release's headline artifact; this script exists for the
# machines that do not run AppImages or that want a package manager to own the files.
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
build="${1:-"$repo/build/linux"}"
out="${2:-"$repo/build/linux-dist"}"

version="$(sed -n 's/.*PLUG_VERSION_STR[[:space:]]*"\([^"]*\)".*/\1/p' "$repo/NeuralAmpModeler/config.h" 2>/dev/null | head -1)"
[ -n "$version" ] || { echo "version introuvable dans NeuralAmpModeler/config.h"; exit 1; }
arch="$(uname -m)"

mkdir -p "$out"
out="$(cd "$out" && pwd)"

if [ ! -d "$build/VoLum.vst3" ] || [ ! -x "$build/volum-app" ]; then
  echo "build incomplet dans $build : il faut VoLum.vst3 et volum-app (cmake --build build/linux)"
  exit 1
fi

# ---------------------------------------------------------------------------
# Stage the install rules into a /usr tree. The plug-in bundles land under the
# user's home (the build tree's cache says $HOME/.vst3, ~/.lv2, ~/.clap, and
# DESTDIR prepends that home), so they are moved to where a system package
# keeps them: /usr/lib/vst3, /usr/lib/lv2, /usr/lib/clap.
# ---------------------------------------------------------------------------
stage="$out/stage"
rm -rf "$stage"
mkdir -p "$stage"
DESTDIR="$stage" cmake --install "$build" --prefix /usr > /dev/null || exit 1
move_bundle() {
  local name="$1" dest="$2" found
  found="$(find "$stage" -path "$stage/usr" -prune -o -name "$name" -print 2>/dev/null | head -1)"
  if [ -n "$found" ]; then
    mkdir -p "$stage/usr/lib/$dest"
    mv "$found" "$stage/usr/lib/$dest/$name"
  fi
}
move_bundle VoLum.vst3 vst3
move_bundle VoLum.lv2 lv2
move_bundle VoLum.clap clap
find "$stage" -mindepth 1 -maxdepth 1 -type d ! -name usr -exec rm -rf {} +
test -d "$stage/usr/lib/vst3/VoLum.vst3" || { echo "l'installation stagee n'a pas de VoLum.vst3"; exit 1; }
test -d "$stage/usr/lib/lv2/VoLum.lv2" || { echo "l'installation stagee n'a pas de VoLum.lv2"; exit 1; }
test -f "$stage/usr/lib/clap/VoLum.clap" || { echo "l'installation stagee n'a pas de VoLum.clap"; exit 1; }
test -x "$stage/usr/bin/volum-app" || { echo "l'installation stagee n'a pas de volum-app"; exit 1; }

# ---------------------------------------------------------------------------
# Main tarball: the staged tree, minus the rigs. The bundles in the build tree
# carry them (VOLUM_COPY_RIGS=ON, the AppImage wants them); a distribution
# package splits them the way the issue asks.
# ---------------------------------------------------------------------------
rm -rf "$stage/usr/lib/vst3/VoLum.vst3/Contents/Resources/VoLumRigs"
main_tar="$out/VoLum-linux-${arch}-${version}.tar.gz"
tar -C "$stage" -czf "$main_tar" usr
echo "$main_tar ($(du -h "$main_tar" | cut -f1))"

# ---------------------------------------------------------------------------
# Rigs tarball: VoLum/VoLumRigs/, so one file serves the system prefix
# (/usr/share) and the per-user one (XDG_DATA_HOME, ~/.local/share) - both are
# folders the plug-in already searches.
# ---------------------------------------------------------------------------
rigs_tar="$out/VoLum-rigs-${version}.tar.gz"
rm -rf "$out/VoLum"
mkdir -p "$out/VoLum"
cp -a "$repo/rigs" "$out/VoLum/VoLumRigs"
tar -C "$out" -czf "$rigs_tar" VoLum
rm -rf "$out/VoLum"
rigs_count="$(tar -tzf "$rigs_tar" | grep -c '\.nam$')"
echo "$rigs_tar ($(du -h "$rigs_tar" | cut -f1), $rigs_count captures)"
[ "$rigs_count" -ge 200 ] || { echo "le jeu de captures est incomplet ($rigs_count)"; exit 1; }

# ---------------------------------------------------------------------------
# RPMs, from those tarballs, when rpmbuild is there. The spec is checked in next
# to this script; rpmbuild only needs a topdir with the sources in it.
# ---------------------------------------------------------------------------
if ! command -v rpmbuild >/dev/null 2>&1; then
  echo "rpmbuild absent : les RPM ne sont pas construits (Fedora : rpm-build, Debian/Ubuntu : rpm)"
  exit 0
fi

topdir="$out/rpmbuild"
rm -rf "$topdir"
mkdir -p "$topdir"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
cp "$main_tar" "$topdir/SOURCES/"
cp "$rigs_tar" "$topdir/SOURCES/"
rpmbuild -ba \
  --define "_topdir $topdir" \
  --define "_target_cpu $arch" \
  --define "volum_version $version" \
  "$here/volum.spec" || { echo "rpmbuild a echoue"; exit 1; }

find "$topdir/RPMS" -name '*.rpm' -exec mv {} "$out/" \; -exec ls -l {} \;
rm -rf "$topdir" "$stage"
echo "paquets prets dans $out"

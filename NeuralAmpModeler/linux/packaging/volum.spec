# RPM spec for the VoLum Linux port (issue #4), built by packaging/dist.bash from the
# tarballs that script makes. Fedora is the target spelling of the dependencies
# (openSUSE: alsa-lib -> alsa, mesa-libGL -> Mesa-libGL1; the file list is identical).
#
# Two packages, because the data and the code do not move together: `volum` is the
# stand-alone, the VST3 bundle and the desktop metadata (~4 MB); `volum-rigs` is the
# 249 bundled captures (~77 MB, noarch), installed where VoLumPaths.h looks for a
# system-wide rig collection: /usr/share/VoLum/VoLumRigs.
#
# The Steinberg VST3 licence has to be read before redistributing a system-wide build
# of the VST3 SDK surface; the AppImage, which this fork publishes today, already
# carries the same module, and this spec exists for the distributions that want the
# files owned by their package manager instead.

Name:           volum
Version:        %{volum_version}
Release:        1%{?dist}
Summary:        Neural amp capture player: stand-alone and VST3 plug-in

License:        MIT
URL:            https://github.com/dmos42/VoLum
Source0:        VoLum-linux-x86_64-%{version}.tar.gz
Source1:        VoLum-rigs-%{version}.tar.gz

ExclusiveArch:  x86_64
Requires:       alsa-lib
Requires:       glib2%{?_isa}
Requires:       libX11%{?_isa}
Requires:       mesa-libGL%{?_isa}
Recommends:     volum-rigs = %{version}-%{release}

%description
VoLum loads neural amp captures in the .nam format and puts them where a guitar
signal goes: a clean boost, an overdriven pre, a cabinet, and the effects around
them. This package carries the stand-alone program and the VST3 bundle a DAW
loads; the bundled capture collection is the volum-rigs package.

%prep
# The two tarballs share no top-level folder (usr/, VoLum/), so both extract into
# one build directory and %install copies each into the buildroot it belongs to.
%setup -q -c -a 1

%install
mkdir -p %{buildroot}/usr/share/VoLum
cp -a usr %{buildroot}/usr
cp -a VoLum/VoLumRigs %{buildroot}/usr/share/VoLum/

%files
%dir /usr/lib/vst3/VoLum.vst3
/usr/lib/vst3/VoLum.vst3/*
/usr/bin/volum-app
/usr/share/applications/com.github.guitarlum.VoLum.desktop
/usr/share/icons/hicolor/*/apps/com.github.guitarlum.VoLum.png
/usr/share/metainfo/com.github.guitarlum.VoLum.metainfo.xml

# ---------------------------------------------------------------------------
# The captures, noarch: the same .nam files every other platform ships inside its
# package, at the path /usr/share/VoLum/VoLumRigs the plug-in searches.
# ---------------------------------------------------------------------------
%package        rigs
Summary:        Bundled neural amp captures for VoLum
BuildArch:      noarch
Requires:       volum = %{version}-%{release}

%description rigs
The two hundred and forty-nine .nam captures VoLum ships, from clean Fenders to
high-gain Marshalls, installed as a system-wide rig collection.

%files rigs
/usr/share/VoLum/VoLumRigs/

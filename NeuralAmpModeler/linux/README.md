# VoLum on Linux

Upstream VoLum ships `windows-setup.exe`, `windows-portable.zip` and macOS packages only
([issue #17](https://github.com/guitarlum/VoLum/issues/17) tracks the request, and
`backlog/O1-linux-support-spike.md` is the spike ticket). This folder is the Linux
equivalent of `NeuralAmpModeler/projects/NeuralAmpModeler-vst3.vcxproj`: a CMake project
that builds the **VST3** plug-in natively, with the iPlug 2 graphics backend implemented for
X11 + GLX.

Status of the port: the audio path, the VST3 wrapper, the editor window, mouse/keyboard
input, in-app menus and text entry, clipboard, cursors and fonts all work. The file / folder
/ colour dialogs, the native message box and the standalone application are still missing —
see [Limitations](#limitations).

## Requirements

Fedora 44 (what the port was developed on). Everything is in the repositories:

```bash
sudo dnf install @development-tools cmake ninja-build clang \
  alsa-lib-devel libX11-devel libglvnd-devel mesa-libGL-devel freetype-devel \
  xcb-util-devel xcb-util-image-devel xcb-util-wm-devel xcb-util-cursor-devel libXcursor-devel \
  xorg-x11-server-Xvfb
```

| package | why |
| --- | --- |
| `@development-tools` | gcc, g++, make, patch, git helpers (GCC 16 is what the code compiles clean on) |
| `cmake`, `ninja-build` | configure and build |
| `clang` | kept as an alternative compiler (`-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`) |
| `libX11-devel` | the window, events, clipboard, cursors |
| `xcb-util-{image,wm,cursor}-devel`, `libXcursor-devel` | cursor loading and the Xfixes/Xrender headers those pull in |
| `libglvnd-devel`, `mesa-libGL-devel` | GLX + OpenGL 2 for NanoVG (`IGRAPHICS_GL2`) |
| `freetype-devel` | font loading through NanoVG |
| `alsa-lib-devel` | pulled in by the audio dependencies; the plug-in itself does not open a device |
| `xorg-x11-server-Xvfb` | headless X server, only for the probe harness and CI |

Debian/Ubuntu names, for anyone who ports the CI later — not verified on a Debian box:
`build-essential cmake ninja-build libx11-dev libgl1-mesa-dev libfreetype-dev
libxcursor-dev libxcb-util-dev libxcb-image0-dev libxcb-icccm4-dev libxcb-cursor-dev xvfb`.

The VST3 SDK is a vendored dependency of iPlug 2, not a submodule commit you can check out.
If `iPlug2/Dependencies/IPlug/VST3_SDK` is empty:

```bash
bash iPlug2/Dependencies/IPlug/download-vst3-sdk.sh
```

## Build

```bash
cmake -S NeuralAmpModeler/linux -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux
```

The result is a Steinberg-layout bundle:

```
build/linux/VoLum.vst3/
├── Contents/x86_64-linux/VoLum.vst3.so     # the module
└── Contents/Resources/{fonts,img,VoLumRigs}
```

`-DVOLUM_COPY_RIGS=OFF` skips the 249 bundled `.nam` rigs (the bundle drops from ~83 MB to a
few MB); the plug-in then looks for rigs in the paths listed under [Files VoLum
writes](#files-volum-writes-on-linux).

A debug build keeps `DBGMSG` output (the SDK is always told it is release code, so `NDEBUG`
is what turns iPlug's logging off — see the comment in `CMakeLists.txt`):

```bash
cmake -S NeuralAmpModeler/linux -B build/linux-dbg -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

## Install into a DAW

```bash
cmake --install build/linux          # -> ~/.vst3/VoLum.vst3
```

That path is `$HOME/.vst3` on purpose: it is the per-user VST3 folder every Linux host
scans (`VOLUM_INSTALL_DIR` overrides it, e.g. `-DVOLUM_INSTALL_DIR=/usr/local/lib/vst3`).

* **Bitwig Studio** — Options → Audio and MIDI → VST3: make sure `~/.vst3` is in the plug-in
  path list, then rescan.
* **Reaper** — Options → Preferences → Plug-ins → VST: add `~/.vst3` to the VST3 path list.
* **Carla / Ardour / Qtractor / any other VST3 host** — point the plug-in path at `~/.vst3`.

**Flatpak hosts cannot see `~/.vst3`** — the sandbox hides your home directory. Grant the
folder to the app (per user, reversible):

```bash
flatpak override --user --filesystem=~/.vst3 studio.kx.carla
flatpak override --user --reset studio.kx.carla     # undo
```

## The probe harness

`volum-vst3-probe` is a 740-line host: `dlopen` the module, run the factory, instantiate,
feed a test tone through the audio path, and optionally open the editor in a bare X11
window. It answers "does the port load and make sound?" without a DAW, which is what CI
needs.

```bash
# audio only, 2 s of 48 kHz test signal, prints RMS of the output
./build/linux/volum-vst3-probe build/linux/VoLum.vst3/Contents/x86_64-linux/VoLum.vst3.so

# open the editor for 6 s on a real display
./build/linux/volum-vst3-probe <module> --ui --uiseconds 6

# headless: no display of your own, no window on your desktop
Xvfb :99 -screen 0 1280x800x24 &
DISPLAY=:99 ./build/linux/volum-vst3-probe <module> --ui

# dump three frames of the editor as PPM (frame 20, 60, 120) for a visual check
VOLUM_LINUX_CAPTURE=/tmp/volum Xvfb ... ; convert /tmp/volum.2.ppm /tmp/volum.2.png
```

Other flags: `--seconds N`, `--rate R`, `--param IDX:VAL` (normalized), `--ui-only`. Exit
code 0 means the module loaded, processed without a crash and, with `--ui`, that the editor
opened and closed cleanly.

## Files VoLum writes on Linux

| path | contents |
| --- | --- |
| `~/.config/VoLum/volum-settings.json` | UI/window settings (or `$XDG_CONFIG_HOME/VoLum/...`) |
| `~/.config/VoLum/volum-update-state.json` | update-check state |
| `~/.vst3/VoLum.vst3/Contents/Resources/VoLumRigs/` | the bundled rig collection, copied by the build |

`RigsRootCandidates()` in `VoLumPaths.h` resolves the rig collection in this order: the
bundle's own `Resources/VoLumRigs`, the tree above the module (a dev or portable build, found
with `dladdr`), `~/.local/share/VoLum`, `~/.vst3/VoLum.vst3/Contents/Resources`, then
`/usr/local/share`, `/usr/share`, `/usr/lib{,64}`.

## Limitations

Not implemented yet, each with a `TODO(linux)` at the site:

* `PromptForFile` / `PromptForFiles` / `PromptForDirectory` / `PromptForColor` (around
  `IGraphicsLinux.cpp:1223`). Importing your own `.nam` through the plug-in's file browser is
  therefore unavailable; drop the file into the rig collection folder instead. The planned
  fix is `org.freedesktop.portal.FileChooser` over D-Bus, which works on X11 and Wayland.
* `ShowMessageBox` returns without showing anything (`IGraphicsLinux.cpp:1211`).
* The macOS/Windows **standalone application** does not build: its host layer is SWELL, and
  SWELL has no Linux backend here. VST3 only — no AU, CLAP, LV2 or JIT for now.
* The window is X11 (GLX). On a Wayland desktop it runs through XWayland, so a Wayland-only
  compositor works but the window is an X11 one: no client-side decorations, and window
  tiling/PICT scaling follows XWayland.
* Menus and text entry are drawn in-app by iPlug 2 (`IPopupMenuControl`,
  `ITextEntryControl`), not by the toolkit.
* Not covered: MIDI 2, multiple simultaneous instances tested under a real host, HiDPI
  fractional scaling, ARM (`aarch64-linux` is wired up in `CMakeLists.txt` but never built).

## Layout of the port

```
NeuralAmpModeler/linux/CMakeLists.txt   # the build itself
NeuralAmpModeler/linux/tools/vst3-probe.cpp
NeuralAmpModeler/VoLumPaths.h           # __linux__ branches: rigs, settings, update state
NeuralAmpModeler/NeuralAmpModeler.cpp   # null-checks for tag lookups during createView()/attached()
iPlug2 (branch volum/linux-port)
  IGraphics/Platforms/IGraphicsLinux.{h,cpp}   # 11 lines of \todo -> ~1600 lines of X11/GLX backend
  IGraphics/Drawing/IGraphicsNanoVG.{h,cpp}    # GL2 context plumbing the backend needs
  IGraphics/IGraphics_select.h, IGraphics_include_in_plug_{hdr,src}.h, IGraphicsPrivate.h
  IPlug/IPlugPaths.cpp                         # Linux app-support / bundle resource paths
  IPlug/IPlugTimer.{h,cpp}                     # a timer that runs on the X11 event loop
  IPlug/VST3/IPlugVST3_View.h                  # reparenting the window into a host
  Dependencies/IGraphics/MetalNanoVG           # drop the per-frame sync blit; drain on teardown
```

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
libxcursor-dev libxcb-util-dev libxcb-image0-dev libxcb-icccm4-dev libxcb-cursor-dev
libatomic1 xvfb`.

`libatomic1` matters: two 64-bit atomics compile to `__atomic_store`/`__atomic_load` libcalls,
and GCC below 14 does not add `-latomic` on its own. A shared library links fine with an
undefined symbol, so the build stays green and the failure appears when a DAW loads the
bundle — `dlopen ... undefined symbol: __atomic_store`. The CMake project tests that
`-latomic` links and adds it when it does, so Fedora (where GCC passes it for you) and Ubuntu
(where it does not) both work. It cannot use `find_library`: on Fedora the `.so` the linker
needs is under `/usr/lib/gcc`, not in the loader path, so the search returns NOTFOUND.

The VST3 SDK is a vendored dependency of iPlug 2, not a submodule commit you can check out.
If `iPlug2/Dependencies/IPlug/VST3_SDK` is empty, the script clones it into the **current**
directory, so run it from its own folder:

```bash
cd iPlug2/Dependencies/IPlug && bash download-vst3-sdk.sh
```

It deletes the SDK's own `.git` at the end, so its last line (`git checkout README.md`, which
restores the one file iPlug 2 does track in that directory) is resolved against the
*enclosing* repository. That only works inside `iPlug2/Dependencies/IPlug`; run it from the
repo root and the SDK lands at `./VST3_SDK` and the checkout fails.

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

Ubuntu turns `-D_FORTIFY_SOURCE=3` on for every optimised build; Fedora does not. Bugs only
the fortifier catches — an `snprintf` whose size argument is larger than the room left in the
buffer, which is what happened in iPlug 2's `CurrentTime()` — pass on a Fedora box and abort
the process in CI (`*** buffer overflow detected ***`, exit 134, before a host can even load
the bundle). Compile the way CI does when touching the platform layer:

```bash
cmake -S NeuralAmpModeler/linux -B build/linux-fort -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DVOLUM_FORTIFY=ON
```

## Install into a DAW

```bash
cmake --install build/linux          # -> ~/.vst3/VoLum.vst3
```

That path is `$HOME/.vst3` on purpose: it is the per-user VST3 folder every Linux host
scans (`VOLUM_INSTALL_DIR` overrides it, e.g. `-DVOLUM_INSTALL_DIR=/usr/local/lib/vst3`).

### Two names for the module

The install puts one real file and one relative link next to it:

    VoLum.vst3/Contents/x86_64-linux/VoLum.vst3.so   Steinberg rule: module named after the bundle
    VoLum.vst3/Contents/x86_64-linux/VoLum.so        -> VoLum.vst3.so, the name JUCE hosts rebuild

[Steinberg's format description](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Locations+Format/Plugin+Format.html)
says the shared object and the bundle must have the same name, which is what upstream does on
Windows and macOS. JUCE rebuilds the path itself, as
`Contents/$(uname -m)-linux/<bundle name without .vst3>.so` (`getDLLFileFromBundle` in
`juce_VST3PluginFormat.cpp`), so a bundle carrying only the Steinberg name never reaches
`dlopen` in that family of hosts: Carla 2.5.10 printed `No plugins found` and
`JUCE Assertion failure in juce_VST3PluginFormat.cpp:999` for exactly this reason, and listed
`VoLum`, 2 in / 2 out, 1 MIDI in and 233 parameters as soon as the link existed. Hosts that
instead glob `*.so` in the architecture folder can show VoLum twice; delete `VoLum.so` if that
bothers you.

* **Bitwig Studio** — Options → Audio and MIDI → VST3: make sure `~/.vst3` is in the plug-in
  path list, then rescan.
* **Reaper** — Options → Preferences → Plug-ins → VST: add `~/.vst3` to the VST3 path list.
* **Carla / Ardour / Qtractor / any other VST3 host** — point the plug-in path at `~/.vst3`.

### The host's audio engine on a PipeWire desktop

Finding the module is only half of it; the host also has to reach the audio interface, and on a
PipeWire desktop two of Carla's engines cannot:

* **ALSA** — PipeWire owns the card, so opening it fails. `arecord -D hw:3,0` gives the same
  answer, `Périphérique ou ressource occupé` / `Device or resource busy`.
* **PulseAudio** — the engine Carla picks by default. It only offers playback devices, so an
  instrument attached to the interface never appears as an input at all.
* **JACK** — the one that works: `pipewire-jack` answers that name, Carla joins the PipeWire
  graph, and the interface's own capture ports become Carla's inputs.

Keep Carla's buffer size equal to the quantum PipeWire hands out. With Carla asking for 512 on a
PipeWire running at 1024 the log fills with `Carla assertion failure: "nframes ==
pData->bufferSize" ... v1 512, v2 1024` (`CarlaEngineJack.cpp:2857`); setting 1024 silences it.

**Flatpak hosts cannot see `~/.vst3`** — the sandbox hides your home directory. Grant the
folder to the app (per user, reversible):

```bash
flatpak override --user --filesystem=~/.vst3 studio.kx.carla
flatpak override --user --reset studio.kx.carla     # undo
```

Granting the folder is necessary but not sufficient for a sandboxed host: the Flatpak runtime
carries an older glibc than a Fedora 44 build demands, so that Carla still refuses the module.
See [issue #7](https://github.com/dmos42/VoLum/issues/7) and the portability note in
[Limitations](#limitations); the packaged (native) Carla loads it normally.

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

CI runs exactly those two probe invocations on `ubuntu-latest`:
`.github/workflows/linux-vst3.yml` installs the X11/GL headers, configures this folder with
`-DVOLUM_COPY_RIGS=OFF`, builds, probes audio then the editor under `xvfb-run`, and uploads
the bundle as an artifact.

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
* Binary portability is not handled yet. The module inherits the C library of the machine that
  built it, and Fedora 44 (glibc 2.43) stamps even plain `sqrtf`/`atan2f`/`log10f` calls with
  `GLIBC_2.43`. A host running on an older sandboxed runtime then refuses the file at `dlopen`
  without saying why: Carla 2.5.10 Flatpak (runtime glibc 2.42) reports only
  `No plugins found`. `libstdc++` is not the constraint (the module needs at most
  `GLIBCXX_3.4.32`). Build anything you hand out on an older base — a Fedora 42 or Debian 12
  container is enough — and check the ceiling with
  `objdump -T .../VoLum.vst3.so | grep -o 'GLIBC_2\.[0-9]*' | sort -Vu | tail -1`. Issue #7.

## Relationship with upstream's own Linux work

Checked on 2026-11-08 against `guitarlum/VoLum` and `guitarlum/iPlug2`: `main` has no Linux
target, but the owner is porting Linux on side branches that are **not merged anywhere** —
`feature/linux-01-box` … `feature/linux-18-callback-budget` in VoLum, with matching
`volum/linux`, `volum/linux-07a-xlib`, `volum/linux-07b-pugl`, … `volum/linux-18` in the
iPlug2 fork (last commit 2026-10-26). Their backend is split into
`IGraphicsLinux{GL,Keys,Font,Clipboard,Dialogs}.cpp` (~880 lines in the main file, glad for
OpenGL loading, plus `IPlugAPP_linux.cpp` for the standalone app, which this port does not
have). Ours is a single `IGraphicsLinux.cpp` (1608 lines, plain GL2) built by
`NeuralAmpModeler/linux/CMakeLists.txt` for VST3 only.

Before continuing here, compare with `feature/linux-18-callback-budget`: whoever picks this
up should pick one backend and move the missing pieces across, not grow a third fork. Their
stage list (dialogs, standalone, packaging, e2e, MIDI, xrun, callback budget) is a better
map of the remaining work than the list above.

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

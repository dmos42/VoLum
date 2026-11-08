# VoLum on Linux

Upstream VoLum ships `windows-setup.exe`, `windows-portable.zip` and macOS packages only
([issue #17](https://github.com/guitarlum/VoLum/issues/17) tracks the request, and
`backlog/O1-linux-support-spike.md` is the spike ticket). This folder is the Linux
equivalent of `NeuralAmpModeler/projects/NeuralAmpModeler-vst3.vcxproj`: a CMake project
that builds the **VST3** plug-in natively, with the iPlug 2 graphics backend implemented for
X11 + GLX.

[![Linux VST3](https://github.com/dmos42/VoLum/actions/workflows/linux-vst3.yml/badge.svg?branch=volum%2Flinux-port)](https://github.com/dmos42/VoLum/actions/workflows/linux-vst3.yml?query=branch%3Avolum%2Flinux-port)

Status of the port: the audio path, the VST3 wrapper, the editor window, mouse/keyboard
input, in-app menus and text entry, clipboard, cursors and fonts all work. The file / folder
/ colour dialogs, the native message box and the standalone application are still missing —
see [Limitations](#limitations). Two command-line hosts are built alongside the plug-in: a
probe that answers "does the module load and make sound?", and
[`volum-live`](#running-volum-without-a-daw), which plays the plug-in straight against ALSA
with its own window, no DAW in the way.

The work happens on `volum/linux-port`, which is that repository's default branch:
[`dmos42/VoLum`](https://github.com/dmos42/VoLum). [Issue #8](https://github.com/dmos42/VoLum/issues/8)
is the map — what is already covered by a test, the seven pieces that are missing (one issue
each), and the order to pick them up in. The iPlug 2 half of the port lives on
[`dmos42/iPlug2@volum/linux-port`](https://github.com/dmos42/iPlug2/tree/volum/linux-port),
which is what `.gitmodules` resolves.

![The editor as the Linux build draws it](volum-editor-x11.png)

Captured headless by the probe harness (`VOLUM_LINUX_CAPTURE` under Xvfb) at 900 x 600: amp
view, rig list, the Pre and Post pedalboards, the header buttons. Fonts, icons, gradients and
the pedal artwork all come through NanoVG, so this is
[`IGraphicsLinux.cpp`](../../iPlug2/IGraphics/Platforms/IGraphicsLinux.cpp) doing on X11 + GLX
what the macOS and Windows backends do. What a still cannot show is the file picker and the
message box that do nothing yet — see [Limitations](#limitations).

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
| `alsa-lib-devel` | needed by `volum-live`, the host that opens the capture and playback streams itself; the plug-in never touches a device |
| `xorg-x11-server-Xvfb` | headless X server, only for the probe harness and CI |

Debian/Ubuntu names, for anyone who ports the CI later — not verified on a Debian box:
`build-essential cmake ninja-build libx11-dev libgl1-mesa-dev libfreetype-dev
libxcursor-dev libxcb-util-dev libxcb-image0-dev libxcb-icccm4-dev libxcb-cursor-dev
libasound2-dev libatomic1 xvfb`.

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

Two command-line hosts are built next to it, both directly in `build/linux/`:
`volum-vst3-probe`, described under [the probe harness](#the-probe-harness), and
`volum-live`, described under [running VoLum without a DAW](#running-volum-without-a-daw).
The live host is the only target that needs ALSA; when its headers are missing the build
prints a warning and simply leaves it out.

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

## Running VoLum without a DAW

`volum-live` is a host of its own: it opens the ALSA streams itself, loads the same
`VoLum.vst3` module, and puts the plug-in's own window on the screen. Device and period
size are options you hand to the program instead of settings you look for in a host's
dialogs, and no audio server sits between the guitar and the amp. Its messages are in
French; the options are not.

```bash
./build/linux/volum-live --list     # what ALSA can see, then stop
./build/linux/volum-live --check    # negotiate everything, print it, play nothing
./build/linux/volum-live            # editor, in and out on the default device
```

Options worth knowing:

| option | meaning |
| --- | --- |
| `--out DEV`, `--in DEV` | playback and capture devices. `--in` defaults to the device named by `--out`; `--no-in` opens no capture at all |
| `--period N` | frames per ALSA period, which is also the block size handed to the plug-in — 64 frames is 1.33 ms at 48 kHz |
| `--periods N` | periods kept in flight: 2 is the tightest, more is safety against dropouts |
| `--gain X` | input trim before the plug-in, for an interface that records quietly |
| `--in-file F.wav`, `--out-file F.wav` | feed the plug-in from a file, and record its output. Without an `--out`, a WAV in and a WAV out is a plain offline conversion that never opens a card |
| `--param IDX:VAL` | set a plug-in parameter before the first block; `--list-params` prints the indices |
| `--meter N`, `--seconds N`, `--no-ui` | level report every N seconds, run length, skip the window |

The same keys may be written in `~/.config/VoLum/live.conf`, one `key=value` per line, and
the command line wins over the file. Ctrl-C ends a run; so does closing the plug-in's window.

Levels are reported as crest and RMS in dBFS in both directions, with a warning when the
input clips, and the run finishes with a verdict — sound coming out, or an empty output
explained as either a missing signal or an input nobody fed. A run that underruns its
playback buffer says so and suggests `--periods`.

### Straight from the card, or through the audio server

`default` works in both directions and cannot break anything, but PipeWire re-frames the
stream at its own quantum, so the period you asked for is not the period you hear; the
tool says so while opening the stream. The lowest round trip needs the card to yourself,
which means asking the desktop to let go of it for a while:

```bash
systemctl --user stop wireplumber pipewire pipewire-pulse   # frees the card
./build/linux/volum-live --out front:CARD=THR10X,DEV=0 --in front:CARD=THR10X,DEV=0 --period 64
systemctl --user start wireplumber pipewire pipewire-pulse  # the desktop takes it back
```

The numbers say which way to go. Through `default`, playback alone takes a small period
without complaint — three seconds at `--period 128` ended with no underruns at all. Add a
capture stream and the server starts feeding blocks the size of its own quantum, 1024 frames
by default, and the same period then emptied the playback buffer 273 times in three seconds.
So through the server keep the period near `clock.quantum` — read it with
`pw-metadata -n settings 0` — or raise `--periods`; small periods are worth asking for only
when the card is yours.

Stopping the server also stops the desktop's sound: no video, no browser tab, until the last
line puts it back. Nothing in this port needs that to be permanent.

A card another program holds answers `Device or resource busy`, and the error names both
ways out of it. Nothing else in this port asks you to touch the desktop audio server: that
one step is the price of a few milliseconds.

### What the window costs the audio thread

The plug-in's view runs on its own thread with its own X11 connection — that is
[iPlug 2's Linux platform layer](#layout-of-the-port) — so the audio loop keeps the main
thread and only pumps window events between blocks: redraws do not stall the stream, and
the run stops when the window closes. `--no-ui` leaves the window unopened, for a rack
started from a script.

## The probe harness

`volum-vst3-probe` is a headless host: `dlopen` the module, run the factory, instantiate,
feed a test tone through the audio path, and optionally open the editor in a bare X11
window. It answers "does the port load and make sound?" without a DAW, which is what CI
needs. Its VST3 plumbing — loading the module, the component handler, the X11 window that
takes the embedded editor — sits in `tools/host-support.h` and is shared with
[`volum-live`](#running-volum-without-a-daw), so both hosts see the same behaviour.

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

# resize the embedded window the way a DAW does, then check the editor is still alive
./build/linux/volum-vst3-probe <module> --ui-only --ui --uiseconds 6 \
  --resize 1:900x700 --resize 2:640x480 --resize 3:1x1
```

Other flags: `--seconds N`, `--rate R`, `--param IDX:VAL` (normalized), `--ui-only`,
`--resize SEC:WxH` (repeatable). Exit code 0 means the module loaded, processed without a
crash and, with `--ui`, that the editor opened and closed cleanly.

`--resize` exists because a crash was invisible without it. A DAW sizes the editor inside its
own container, so the plug-in only ever sees a ConfigureNotify for a window whose size it does
not own; Carla took a SIGSEGV on the first such size, in `IGraphicsNanoVG::EndFrame()` reading
a framebuffer that was never created because the GLX context had been released between frames.
Leaving the window alone proved nothing. Sizes worth asking for: the one the user drags to, a
tiling compositor's 1x1, and something bigger than the screen.

CI runs those probes and one live-host run on `ubuntu-latest`:
`.github/workflows/linux-vst3.yml` installs the X11/GL and `libasound2-dev` headers,
configures this folder with `-DVOLUM_COPY_RIGS=OFF`, builds, probes audio then the editor
under `xvfb-run`, walks the editor through five host-side sizes, plays a generated tone WAV
through `volum-live` in its card-free file mode, and uploads the bundle as an artifact.

## Files VoLum writes on Linux

| path | contents |
| --- | --- |
| `~/.config/VoLum/volum-settings.json` | UI/window settings (or `$XDG_CONFIG_HOME/VoLum/...`) |
| `~/.config/VoLum/volum-update-state.json` | update-check state |
| `~/.vst3/VoLum.vst3/Contents/Resources/VoLumRigs/` | the bundled rig collection, copied by the build |
| `~/.config/VoLum/volum.log` | iPlug 2's log, written by the plug-in whatever host loads it |

`~/.config/VoLum/live.conf` belongs to the same folder but is only read, never written: it
is where you keep the [live host's](#running-volum-without-a-daw) device and period size
between runs.

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

`volum-live` has limits of its own, and they are deliberate: no MIDI, one stereo pair of
buses, no resampling inside the host. Capture and playback have to agree on one rate, which
the tool checks and reports instead of hiding. A card that only does 44 100 Hz — many small
USB amps, the Yamaha THR10X among them — therefore runs the plug-in at 44 100 Hz, and it is
VoLum that has to cope: it carries `AudioDSPTools/dsp/ResamplingContainer` and
`VoLumResamplingNam.h` for the 48 kHz `.nam` models. That path has now been measured, offline
with `volum-live --in-file` (44 100 Hz in, 44 100 Hz out): a 220 Hz tone came back as the
tone plus its harmonics — 440, 660, 880, 1760 Hz — with no measurable energy above 16 kHz,
so nothing is folded back into the top end.

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
NeuralAmpModeler/linux/tools/host-support.h    # load the module, component handler, X11 window: shared by both hosts
NeuralAmpModeler/linux/tools/vst3-probe.cpp    # headless probe: audio path, then the editor
NeuralAmpModeler/linux/tools/alsa-live.cpp     # ALSA in/out host that shows the plug-in's window
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

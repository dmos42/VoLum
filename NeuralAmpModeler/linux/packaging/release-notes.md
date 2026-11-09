<!-- Le texte de la page https://github.com/dmos42/VoLum/releases/tag/v1.3.1-linux.3 .
     Ce qui compte est la page : la corriger ici puis la publier,
       gh release edit v1.3.1-linux.3 --repo dmos42/VoLum --notes-file \
           NeuralAmpModeler/linux/packaging/release-notes.md
     Les deux fichiers de cette page viennent de 2f82f8d, construits par
     packaging/appimage.bash ; les nombres qu'elle cite viennent de
     test/meter-motion-test.bash et du probe. -->

VoLum for Linux, x86_64. Same amplifier, same 249 bundled captures, built from the Linux
port branch rather than `main`: the VST3 a DAW loads, and a standalone that needs no DAW.

The first full release of this port: the two tags before it were previews. It lives on
`dmos42/VoLum`, the fork carrying the Linux work: upstream
`guitarlum/VoLum` has no Linux build, and nothing on this page touches the Windows or macOS
artifacts. It replaces [v1.3.1-linux.2](https://github.com/dmos42/VoLum/releases/tag/v1.3.1-linux.2) — whose
audio is fine, but whose IN and OUT meter columns never moved — and
[v1.3.1-linux.1](https://github.com/dmos42/VoLum/releases/tag/v1.3.1-linux.1), whose assets were
removed because it asked a sound server for 128 frames of buffer and had its output cut into
pieces for it (see "If the sound comes out in pieces"). Use this one.

## Assets

| file | what it is |
| --- | --- |
| `VoLum-linux-x86_64-1.3.1.AppImage` | the standalone. `chmod +x` it, run it: the window opens, a guitar goes in, the amp comes out. Nothing to install — the plug-in rides inside the package and is loaded from there |
| `VoLum-linux-x86_64-1.3.1.vst3.zip` | the plug-in for a DAW. Unpack it so the folder lands at `~/.vst3/VoLum.vst3` (`unzip VoLum-linux-x86_64-1.3.1.vst3.zip -d ~/.vst3`) |

Built from `2f82f8d`, the commit this tag points at. The `Linux VST3` job is green on it, and
green on the two commits after it that added the meter check quoted below (`66cd307`,
`928fd72`) — that script is on the branch rather than in this tag. Point it at the AppImage as
downloaded, or at the bundle unpacked from the zip, and it measures those same two columns.

Needs: x86_64 Linux, glibc 2.38 or newer, X11 with GLX (an X11 session, or Wayland through
XWayland), ALSA. The AppImage additionally needs FUSE to mount; without it, run it as
`./VoLum-linux-x86_64-1.3.1.AppImage --appimage-extract-and-run`.

## What was checked, by machine rather than by ear

- The VST3 loads, enumerates and processes: 2 in / 2 out, MIDI in, 233 parameters; a 440 Hz
  test tone in comes out at crest −17.0 dBFS, RMS −27.8 dBFS.
- It loads and sounds in a real sequencer: Carla 2.5.10 lists it and runs it.
- The window draws and answers mouse, keyboard, menus, text entry and clipboard — the X11/GLX
  graphics backend for iPlug 2 that this port had to write, ~1600 lines where upstream left 11
  lines of `\todo`.
- Resize from the host side, including a 1×1 window, leaves the editor alive: `1:900x700` then
  `2:1x1`, exit code 0.
- File, folder and colour pickers go to the desktop's own portal over D-Bus (XDG file
  chooser, file open, file save, colour), so no toolkit is dragged into the DAW's process.
  Exercised by a scripted client: 13 checks, nobody clicking.
- The standalone runs end to end with the window open: five minutes of full duplex through
  PipeWire, 45 970 blocks, no processing error and two underruns.
- **The IN and OUT meters move.** VoLum only measures its own levels once a host tells it the
  editor is open — a call iPlug 2 made on Windows and nowhere else, so on Linux the window
  loaded, drew, took the mouse and ran the amp with both columns dark and nothing in the log.
  Measured on the two files below rather than on a build tree: the package is started headless,
  its input comes from a WAV through `--in-file` and its output goes to a throwaway sink, and
  the back buffer of its window is read three times. The tone steps loud/quiet every 0.4 s, so
  the bars have a reason to change height. On the file downloaded from this page, the IN column
  holds 44 lit segments while the tone sits at −14 dBFS and falls to 16 when it drops to
  −50 dBFS, and OUT follows it at 40 and 16; between those two frames 14 065 pixels of that
  window change. On a build without that call: 0 segments in both columns, 0 pixels changing,
  and the script fails. Reproduce it with `NeuralAmpModeler/linux/test/meter-motion-test.bash`,
  on the branch from this commit on — it takes the package or a bundle as its argument, and
  restores the desktop's own output sink on the way out.
- The USB interface this port was developed on (a Yamaha THR10X) is resampled and measured:
  a 220 Hz input produces 220/440/660/880/1760 Hz and nothing above 16 kHz.
- **The amp does not feed on itself.** The probe now records a run while it plays, and its test
  tone drops 12 dB halfway through, so a chain that only colours the signal follows it down.
  Six configurations, 44 seconds each — plain, delay, reverb, reverb + shimmer,
  delay + reverb + shimmer, and the delay with feedback and mix at maximum: the first five came
  out 10.4 to 10.5 dB down, inside the 1.6 dB the amp's own compression accounts for, with no
  drift across the quiet half. The sixth held 3.6 dB, meaning 8.4 dB of the input change
  recirculated through its delay line — a rack repeating what it is given because it was set up
  to. That is how to tell a repeating sound caused by a delay from a fault in the port.

## If the sound comes out in pieces

It is the buffer, not the cable and not the model. Through a sound server the number that
decides is the whole stream buffer — `period x periods` — against the server's own quantum,
1024 frames on PipeWire (`pw-metadata -n settings 0`). Measured on the USB interface this port
was developed on, full duplex, editor window open, 8 seconds per run:

| period x periods | buffer | underruns in 8 s |
| --- | --- | --- |
| 64 x 2 | 128 | 375 |
| 256 x 2 | 512 | 376 |
| 64 x 16 | 1024 | 0 |
| 128 x 8 | 1024 | 0 |
| 256 x 4 | 1024 | 0 |
| 512 x 2 | 1024 | 0 |

The two that failed were being served at half the rate they asked for — 751 blocks in 8 seconds
where 1500 were due — so everything they played came out in pieces. This build holds that floor
itself: it opens 256-frame periods four of them at a time, prints that it did so what is on
screen matches what the hardware agreed to, and says out loud when a server answers with less
buffer than its own quantum. Rechecked on the packaged AppImage here: 1876 blocks in 10 seconds,
no processing error, no underrun.

On a machine that still crackles, without rebuilding anything — the AppImage reads this file
like every other host:

```bash
echo 'period=512' >> ~/.config/VoLum/app.conf
./VoLum-linux-x86_64-1.3.1.AppImage --list     # device names
```

The plug-in logs the same negotiation its own way (`~/.config/VoLum/volum.log`, one
`[audio] reset: 48000 Hz, block 256, ...` line per start), so the number can be read from a log
instead of hunted by ear. For the lowest round trip, take the card out of the server's hands
(`front:` / `plughw:`) and a 64-frame period is worth having: `NeuralAmpModeler/linux/README.md`
in [the port branch](https://github.com/dmos42/VoLum/tree/volum/linux-port/NeuralAmpModeler/linux)
gives the two commands and what they cost.

## If the meters stay dark on a machine that already had VoLum

The AppImage carries its own copy of the plug-in and loads that one, so it cannot be
affected by what else is installed. A DAW is a different story: Carla, REAPER and the rest
look in `~/.vst3`, and a `VoLum.vst3` left there by an earlier build of this port — anything
from before this release — still has the missing
`OnUIOpen()` call above. The window
draws, the amp runs, the two columns stay dark. Unpack the zip over it, or:

```bash
rm -rf ~/.vst3/VoLum.vst3 && unzip VoLum-linux-x86_64-1.3.1.vst3.zip -d ~/.vst3
```

The same file is what `volum-app` finds when it is not running from inside a package, so the
installed standalone needs that refresh too.

## What is not here yet

- AU, LV2 and CLAP: VST3 only on Linux.
- The standalone has no settings window and no MIDI: device, period and gain are `app.conf`
  keys for now, and the guitar has to be a line in. Both are open issues on this repository.
- Native Wayland: the editor is X11 (it runs under XWayland, including on KDE).
- No code signing and no AppStream metadata; `appimagetool` was told to skip the latter.
- The AppImage is built here, not yet by CI, and carries no update information, so nothing
  offers to self-update.

## Fingerprints

```
c242b74ecb50fbd33b0a17a36dbb646729869f63b501d84055034fe4cbffc3da  VoLum-linux-x86_64-1.3.1.AppImage
0d76c2b4461f5c460aaa1b46b3d3da2b1aa3724723ca600fb3bb8a8d05c70e0e  VoLum-linux-x86_64-1.3.1.vst3.zip
df79f77decda6c325ebe839eb699aa72cd7f5ec730b33398d714d1e28f8403a7  VoLum.vst3/Contents/x86_64-linux/VoLum.vst3.so
```

`SHA256SUMS.txt` next to the two files checks both. The third line is the plug-in itself, and
it is the same bytes inside the AppImage and inside the zip: `unzip -p
VoLum-linux-x86_64-1.3.1.vst3.zip VoLum.vst3/Contents/x86_64-linux/VoLum.vst3.so | sha256sum`,
or extract the AppImage and check it there. The meter reading above was taken from that file
twice — once as it sits in the downloaded AppImage, once installed at `~/.vst3` for a DAW to
find — and both read the fingerprint.

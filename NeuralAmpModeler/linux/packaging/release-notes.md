<!-- Le texte de cette page (https://github.com/dmos42/VoLum/releases/tag/v1.3.1-linux.4) .
     Le job `Linux AppImage` (.github/workflows/linux-appimage.yml) cree la page a partir de
     ce fichier quand un tag `v*-linux*` n'a pas encore de release, puis y depose les fichiers
     qu'il vient de construire : pousser un tag suffit, et texte et artifacts viennent du meme
     commit. Pour corriger la page sans pousser un nouveau tag :
       gh release edit v1.3.1-linux.4 --repo dmos42/VoLum --notes-file \
           NeuralAmpModeler/linux/packaging/release-notes.md
     Les nombres cites viennent de test/meter-motion-test.bash, du probe, et du job lui-meme. -->

VoLum 1.3.1 for Linux, x86_64: the VST3 a DAW loads, and a stand-alone that needs no DAW.
Same plug-in as [v1.3.1-linux.3](https://github.com/dmos42/VoLum/releases/tag/v1.3.1-linux.3)
— same amp, same 249 bundled captures, no audio code changed since — and what differs in this
one is who builds it and what the package tells the machine it runs on.

## What is on this page, and who made it

The `Linux AppImage` job runs the same recipe a person used to run by hand,
`NeuralAmpModeler/linux/packaging/appimage.bash`, on a runner, with the rigs included. Its
output *is* this release: the tag push creates the page from the file you are reading, and
uploads the four artifacts into it.

| file | what it is |
| --- | --- |
| `VoLum-linux-x86_64-1.3.1.AppImage` | the stand-alone, one file: `chmod +x` and run. It carries update information and AppStream metadata inside it now |
| `VoLum-linux-x86_64-1.3.1.AppImage.zsync` | what makes an update a partial download; with no such asset, the update string inside the image is decorative |
| `VoLum-linux-x86_64-1.3.1.vst3.zip` | the plug-in for a DAW — unpack it so the folder lands at `~/.vst3/VoLum.vst3` |
| `SHA256SUMS.txt` | made in the same job, over the two files above |

Numbers from the run that produced them: the image is 34 302 456 bytes, its `.zsync`
117 487, the image holds 249 `.nam` captures, and the packaged program played a 48 kHz 220 Hz
tone into a WAV that came out **byte for byte the same** as the same tone through the
unpackaged build of the same commit (`15e88ca0…` both). That comparison is the one worth
having: an AppImage does not usually fail by refusing to start, it fails by loading some other
copy of the plug-in — and neither a size nor a hash can see that.

## What the package now tells your desktop

**AppStream metadata.** The component id is `com.github.guitarlum.VoLum`, and it is the stem
of three file names AppStream requires to agree: the `.desktop` menu entry, the icon its
`Icon=` line resolves to, and `usr/share/metainfo/com.github.guitarlum.VoLum.metainfo.xml`.
The metainfo carries name, summary, description, categories, keywords, the project license,
the developer, a screenshot and one `<release>` entry. `desktop-file-validate` and
`appstreamcli validate-tree` run in CI against a staged `/usr` root and against the extracted
image: a package that installs an entry whose icon is filed under another name installs
happily and shows a generic picture forever, so here that is a build failure rather than a
screenshot nobody takes.

The id follows the upstream project (`guitarlum/VoLum`) so that a Linux build accepted
upstream later keeps the same identity in people's software centres; this fork is only the one
shipping a Linux build today, which is why the metainfo's bug-tracker URL is the fork's.

One consequence of the rename, for a machine that installed an earlier build with
`cmake --install`: the menu entry is now `com.github.guitarlum.VoLum.desktop`, and a leftover
`~/.local/share/applications/volum.desktop` still points at `volum-app` — the same program
under the same file name, so it works, and it is a duplicate in the menu:

```bash
rm -f ~/.local/share/applications/volum.desktop ~/.local/share/icons/hicolor/*/apps/volum.png
```

**Update information.** The image's runtime embeds
`gh-releases-zsync|dmos42|VoLum|latest|VoLum-linux-x86_64-*.AppImage`, so
[AppImageUpdate](https://github.com/AppImage/AppImageUpdate) resolves this file to the newest
release carrying that name, reads its `.zsync`, and offers the delta rather than 34 MB again.
The string is read back out of the finished image (`readelf -p .upd_info`) in the job rather
than trusted from the command line, and the job fails when a release would have an image with
no `.zsync` beside it. Nothing updates by itself: it is offered, and you accept.

**Needs:** x86_64 Linux, X11 with GLX (an X11 session, or Wayland through XWayland), ALSA. The
AppImage additionally needs FUSE to mount, or it must be run as
`./VoLum-linux-x86_64-1.3.1.AppImage --appimage-extract-and-run`. The C library floor belongs
to the machine that built the file, and this one was built in the CI job's Ubuntu 22.04
container, so the floor is glibc 2.35 — read it back before blaming the package for an older
install:

```bash
objdump -T VoLum.vst3/Contents/x86_64-linux/VoLum.vst3.so | grep -o 'GLIBC_2\.[0-9]*' | sort -Vu | tail -1
```

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
itself: it opens 256-frame periods four of them at a time, prints what it did so that what is
on screen matches what the hardware agreed to, and says out loud when a server answers with
less buffer than its own quantum. Rechecked on the packaged AppImage here: 1876 blocks in
10 seconds, no processing error, no underrun.

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

VoLum decides whether to measure levels at all from the handshake that says its editor is
open: `OnUIOpen()` raises a flag, `OnUIClose()` lowers it, and `ProcessBlock` steps over the
meter work while it is down. Only `IGraphicsWin` ever made that call, so on Linux the window
loaded, drew, took the mouse, opened the desktop's dialogs and ran the amp with two meter
columns dead and nothing logged about it. Fixed in the iPlug 2 fork at `ee74e0f48`, and in
this repository at `2f82f8d`.

The AppImage carries its own copy of the plug-in and loads that one, so it cannot be affected
by what else is installed. A DAW is a different story: Carla, REAPER and the rest look in
`~/.vst3`, and a `VoLum.vst3` left there by a build from before `v1.3.1-linux.3` still has the
missing call. The window draws, the amp runs, the two columns stay dark. Unpack the zip over
it, or:

```bash
rm -rf ~/.vst3/VoLum.vst3 && unzip VoLum-linux-x86_64-1.3.1.vst3.zip -d ~/.vst3
```

The same file is what `volum-app` finds when it is not running from inside a package, so the
installed stand-alone needs that refresh too.

## What was checked, by machine rather than by ear

- The VST3 loads, enumerates and processes: 2 in / 2 out, MIDI in, 233 parameters; a 440 Hz
  test tone in comes out at crest −17.0 dBFS, RMS −27.8 dBFS.
- It loads and sounds in a real sequencer: Carla 2.5.10 lists it and runs it.
- The window draws and answers mouse, keyboard, menus, text entry and clipboard — the X11/GLX
  graphics backend for iPlug 2 that this port had to write, ~1600 lines where upstream left 11
  lines of `\todo`.
- Resize from the host side, including a 1×1 window, leaves the editor alive: `1:900x700` then
  `2:1x1`, exit code 0.
- File, folder and colour pickers go to the desktop's own portal over D-Bus (XDG file chooser,
  file open, file save, colour), so no toolkit is dragged into the DAW's process. Exercised by
  a scripted client: 13 checks, nobody clicking.
- The stand-alone runs end to end with the window open: five minutes of full duplex through
  PipeWire, 45 970 blocks, no processing error and two underruns.
- **The package and the build make the same sound.** The `Linux AppImage` job runs the image
  with `--appimage-extract-and-run` over a tone and compares its WAV with the unpackaged host's
  byte for byte; and before that comparison means anything, the same image opens its window
  from inside itself on Xvfb (`XEmbed confirme` in the log, and its own WAV out).
- **The IN and OUT meters move.** Measured with `NeuralAmpModeler/linux/test/meter-motion-test.bash`,
  which starts the package headless into a throwaway PipeWire sink — the tone enters through
  `--in-file`, so no capture card opens and nothing reaches the speakers — reads the back buffer
  of its window three times, and counts lit segments. With the tone stepping −14 dBFS /
  −50 dBFS every 0.4 s: IN 44 segments then 16, OUT 40 then 16, and 14 065 pixels of that
  window changing between two frames. Run the same script against a bundle from before the fix
  and it reads 0, 0 and 0 pixels, and fails.
- **The amp does not feed on itself.** The probe records a run while it plays, and its test
  tone drops 12 dB halfway through, so a chain that only colours the signal follows it down.
  Six configurations, 44 seconds each — plain, delay, reverb, reverb + shimmer,
  delay + reverb + shimmer, and the delay with feedback and mix at maximum: the first five came
  out 10.4 to 10.5 dB down, inside the 1.6 dB the amp's own compression accounts for, with no
  drift across the quiet half. The sixth held 3.6 dB, meaning 8.4 dB of the input change
  recirculated through its delay line — a rack repeating what it is given because it was set up
  to. That is how to tell a repeating sound caused by a delay from a fault in the port.

## What is not here yet

Each of these is an issue on this repository, and nothing above claims them:

- LV2 and CLAP wrappers — VST3 is the only host format on Linux. (AU is Apple-only, so there
  is nothing to port there; the older bullet read "AU, LV2 and CLAP" because that is what
  upstream's format list looks like from Linux.) [#9](https://github.com/dmos42/VoLum/issues/9)
- The stand-alone has no settings window: device, period and gain are `app.conf` keys, and the
  guitar has to be a line in. [#10](https://github.com/dmos42/VoLum/issues/10) It has no MIDI
  source either, which a DAW supplies for free. [#11](https://github.com/dmos42/VoLum/issues/11)
- Native Wayland: the editor is X11, and runs through XWayland on a Wayland desktop.
  [#12](https://github.com/dmos42/VoLum/issues/12)
- Nothing is signed. The AppStream half of that older bullet is done above; whether to sign at
  all, with what key and where that key lives, is the owner's call.
  [#14](https://github.com/dmos42/VoLum/issues/14) Until then the sums file on this page is the
  check, and the job that made the files made it.
- The C library floor is still whatever the build machine has; moving it into CI changed the
  number rather than solving it, and lowering it deliberately — an older container — is
  [#7](https://github.com/dmos42/VoLum/issues/7).

## Fingerprints

```bash
sha256sum -c SHA256SUMS.txt     # the asset on this page, produced by the job that produced the files
```

`SHA256SUMS.txt` covers the AppImage and the zip. It stopped being a transcription of a laptop
in this release, which is the only reason a fingerprint is worth printing.

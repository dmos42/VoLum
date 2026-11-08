# VoLum Agent Notes

Keep this file as a small routing index. Detailed guidance lives in scoped Cursor rules and skills so agents load only what each task needs.

## Scope

- Main product code: `NeuralAmpModeler/`.
- Vendored/submodule code: `iPlug2/`, `NeuralAmpModelerCore/`, `eigen/`. Avoid edits unless the task explicitly targets them.
- `AudioDSPTools/` is a submodule, but VoLum actively depends on DSP there.
- Top-level `rigs/` is the dev source of bundled amp profiles. Shipped artifacts rename it to `VoLumRigs/`.
- Linux port: `NeuralAmpModeler/linux/` (CMake, VST3 only) plus the graphics backend on the `iPlug2` fork branch `volum/linux-port`. Windows/macOS build through Visual Studio/Xcode as before.

## Start Here

Rules below auto-attach on the files they own; you do not need to open them by hand.

- UI/layout work: `.cursor/rules/volum-ui.mdc`.
- Custom NAM import transactions: `NeuralAmpModeler/VoLumCustomNamImport.h`.
- Params, presets, state migration: `.cursor/rules/volum-state-params.mdc`.
- C++/DSP/audio-chain work: `.cursor/rules/neural-amp-modeler-native.mdc`.
- CI, installers, releases, artifacts: `.cursor/rules/volum-release-packaging.mdc` and skill `release-manager`.
- Build/CI failures: skill `native-build-debugger`.
- Linux work: `NeuralAmpModeler/linux/README.md` (build, hosts, host quirks, limitations). The open items are issues on `dmos42/VoLum`, whose default branch is `volum/linux-port`; #8 is the index. Never bump the `iPlug2` pointer before pushing the commit on that fork.
- Submodule or vendored code questions: `.cursor/rules/volum-submodules.mdc`.
- Upstream sync with NAMCore or NeuralAmpModelerPlugin: skill `upstream-sync`.
- Retraining bundled NAM captures to A2: skill `a2-training` (ops in `training/cloud/`).
- Writing or trimming rules/skills/`AGENTS.md`: `.cursor/rules/ai-artifact-authoring.mdc`.
- Wayfinder maps / scratch tickets: `.cursor/rules/volum-scratch-planning.mdc`.
- Goal+verify loops: `/conductor` for AFK, overnight, spec overnight, or dispatching a clear map. Ordinary one-slice features and bugs: Plan mode or implement (`vo-lum-workflow`). Foggy headlines: `/wayfinder`. Skip all three for one-step edits. Do not install pstack.

## Agent skills

### Issue tracker

Planning lives in `.scratch/`. Foggy multi-session: `/wayfinder`. Sitting-with-you
implementation: Plan / implement. AFK / overnight: `/conductor`. See
`docs/agents/issue-tracker.md`.

### Domain docs

Single-context. `CONTEXT.md` is a lazy glossary, not a spec. See
`docs/agents/domain.md`.

## Fast Commands

- Windows tests: `pwsh NeuralAmpModeler/scripts/run-tests-win.ps1` (`-Asan` = AddressSanitizer build with the macOS sanitizer job's exclusions; run it before pushing DSP or buffer code)
- macOS tests: `bash NeuralAmpModeler/scripts/run-tests-mac.sh`
- macOS sanitizer tests: `bash NeuralAmpModeler/scripts/run-tests-mac.sh --sanitize`
- Windows app smoke check: `pwsh NeuralAmpModeler/scripts/run-app-win.ps1` (sandbox library; `-RealLibrary` for the owner's)
- Worktrees: `pwsh NeuralAmpModeler/scripts/worktree-win.ps1 -Add <branch>` / `-Remove <path> [-DeleteBranch]`
- Merge a CI-green branch into dev: `pwsh NeuralAmpModeler/scripts/merge-verified-branch.ps1 -Branch <branch> [-DryRun]`
- Windows screenshot / click harness: `pwsh NeuralAmpModeler/scripts/win-screenshot.ps1`, `win-click.ps1`, `win-key.ps1` (recipes in `docs/screenshot-recipes.md`; debug-only `VOLUM_SEED_CUSTOM_AMPS=N` for sidebar overflow)
- Windows portable package: `cd NeuralAmpModeler\scripts; cmd /c makedist-win.bat full zip` (the script resolves its helpers relative to the working directory, so it must run from `scripts`)
- Windows installer package: `cd NeuralAmpModeler\scripts; cmd /c makedist-win.bat full installer`
- Windows standalone end-to-end scenarios: `pwsh NeuralAmpModeler/scripts/e2e-standalone-win.ps1`
- Windows standalone MIDI recall through the real WinMM stack: `pwsh NeuralAmpModeler/scripts/e2e-standalone-win.ps1 -Scenario midi -RequireMidi` (needs loopMIDI with a port named `VoLum Loop`; without `-RequireMidi` a missing port is a SKIP)
- macOS standalone end-to-end (fresh, upgrade, CoreMIDI, screenshots; CI job `macOS standalone e2e`): `bash NeuralAmpModeler/scripts/e2e-standalone-mac.sh --artifact-dir <VoLum-mac artifact>` (refuses outside GitHub Actions unless `--throwaway-vm`; needs `pip install python-rtmidi pyobjc-framework-Quartz pillow`)
- macOS REAPER + standalone crash smoke of the installed AU/VST3/app (empty library per launch, MallocScribble; moves the VoLum library aside and restores it): `bash NeuralAmpModeler/scripts/smoke-reaper-mac.sh --format all --runs 3` (needs REAPER in `/Applications` and a logged-in desktop; CI runs it on macOS 15)
- Windows standalone rate/buffer switching stress: `pwsh NeuralAmpModeler/scripts/stress-standalone-rate-switch-win.ps1` (needs a real ASIO device)
- macOS release-equivalent package: `bash NeuralAmpModeler/scripts/makedist-mac.sh full all`
- Watch/dispatch CI: `pwsh NeuralAmpModeler/scripts/ci-watch.ps1 -Ref <branch> [-Dispatch] [-NoWait]`
- Conductor loop status: `pwsh NeuralAmpModeler/scripts/conductor-status.ps1 -Effort <slug>`
- Format: `bash format.bash`
- Linux VST3 build: `cd iPlug2/Dependencies/IPlug && bash download-vst3-sdk.sh`, then `cmake -S NeuralAmpModeler/linux -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/linux` (`-DVOLUM_FORTIFY=ON` compiles the way Ubuntu CI does; `-DVOLUM_COPY_RIGS=OFF` skips the 249 rigs)
- Linux smoke: `./build/linux/volum-vst3-probe build/linux/VoLum.vst3/Contents/x86_64-linux/VoLum.vst3.so --ui --resize 1:900x700 --resize 2:1x1` (headless: prefix `xvfb-run -a`), and `./build/linux/volum-live` for ALSA in/out with the window; install with `cmake --install build/linux` (-> `~/.vst3`)
- Linux CI: `.github/workflows/linux-vst3.yml`, on the `Linux VST3` job — `gh run list --repo dmos42/VoLum --branch volum/linux-port`
- Linux AppImage: `NeuralAmpModeler/linux/packaging/appimage.bash` (needs FUSE for `appimagetool`) → `build/linux-appimage/VoLum-linux-x86_64-<version>.AppImage`. Choppy output from a package is the ALSA period, not the code: `period=256` in `~/.config/VoLum/app.conf`; 64 frames over PipeWire underruns, 256 does not.
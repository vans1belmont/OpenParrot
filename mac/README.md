# Running OpenParrot on macOS

These scripts run OpenParrot games on Apple Silicon Macs through CrossOver. This is experimental, and it has been tested with two games on CrossOver 26.3 and macOS 27:

| Game | Build | Graphics | Status |
|---|---|---|---|
| Dark Awake | 32-bit | Direct3D 8 | Plays, with music |
| Dead or Alive 6 | 64-bit, APM3 | Direct3D 11 and 9, D3DMetal | Plays, with movies |

Gamepad input has been tested with one USB controller, which CrossOver presented through XInput. Not tested: other games, other controllers, Direct3D 12, Player 2 controls, and peripherals such as force feedback and card readers.

## What you need

- [CrossOver](https://www.codeweavers.com/crossover)
- A game backup of your own. None are included.
- `OpenParrot-Win32.zip` and, for a 64-bit game, `OpenParrot-x64.zip`, from this branch's "Build Windows binaries" workflow
- For a game with MIDI music, a General MIDI sound bank (`.sf2` or `.dls`) with drum kits in bank 128
- For a game with WMV movies, [`gstreamer-1.0-1.24.13-universal.pkg`](https://gstreamer.freedesktop.org/data/pkg/osx/1.24.13/gstreamer-1.0-1.24.13-universal.pkg) (192 MB). Don't install it. The script takes what it needs and checks it against a pinned SHA-256. The package is not signed.

## Setup

Dark Awake:

```bash
./mac/setup-bottle.sh --game <game.zip> --openparrot <OpenParrot-Win32.zip> --soundbank <bank.sf2> --profile darkawake
```

Dead or Alive 6:

```bash
./mac/setup-bottle.sh --bottle OpenParrotDOA6 --game <game.zip> --openparrot <OpenParrot-x64.zip> --input-helper <OpenParrot-Win32.zip> --exe muscle_apm3.exe --profile doa6 --gstreamer <gstreamer.pkg> --skip-apm-startup --shared-copy
```

The script creates a bottle called `OpenParrot`, which you can change with `--bottle`, and:

- copies in the game and OpenParrot
- installs a key layout, if you pass `--profile`
- with `--soundbank`, replaces CrossOver's DirectMusic DLLs in that bottle with ones from Wine 11.21, keeping the originals as `.cxbackup`, and installs a repaired copy of your sound bank as `gm.dls`
- with `--gstreamer`, copies the `libav` plugin and its libraries into the bottle's `gst-libav` folder
- with `--skip-apm-startup`, patches the bottle's copy of `apm.dll`, keeping the original as `apm.dll.original`
- writes the game's launch settings to `C:\game\openparrot-mac.conf`, including the frame copy from `--shared-copy`

The Wine DLLs come from [Gcenx/macOS_Wine_builds](https://github.com/Gcenx/macOS_Wine_builds), an unsigned third-party build. The script downloads about 225 MB and checks it against a pinned SHA-256. Use `--wine-archive` if you already have the file.

## Playing

```bash
./mac/play.sh --bottle <name>
```

Leave out `--bottle` for the bottle called `OpenParrot`. Click the game window so it has focus. Close it with its red button. Only run one game at a time.

| Dark Awake control | Key | Gamepad (Xbox layout) |
|---|---|---|
| Move | Arrow keys | D-pad or left stick |
| Buttons 1-4 | Z, X, C, V | A, B, X, Y |
| Start | Return | Menu/Start |
| Test mode | T | |
| Test or service | Y, U | |

| Dead or Alive 6 control | Key | Gamepad (Xbox layout) |
|---|---|---|
| Move | Arrow keys | D-pad or left stick |
| Start | Return | Menu/Start |
| A, B, X, Y | Z, X, C, V | A, B, X, Y |
| Left and right shoulder | A, S | Left and right shoulder |
| Probably the triggers | D, F | |

Dead or Alive 6's inputs 0 and 15 close the game, so they have no key. Which move each button performs comes from the game's code and wasn't checked one by one.

Connect a gamepad before or during play. The helper uses the first one it finds and doesn't read analog triggers. Controllers that don't appear through XInput may number their buttons differently.

## Why each piece is needed

| Piece | Reason |
|---|---|
| `TP_REMOTETHREAD=1` | The default injection mode opens a crash dialog under Wine on macOS. |
| `TP_PUMP_MESSAGES=1` | Wine's Mac driver only delivers key and window events to a thread that pumps messages, and this game stops doing that after startup. |
| `OpenParrotInput` | Serves `\\.\pipe\TeknoParrotPipe`, which TeknoParrotUI otherwise provides. It must start before the game. |
| Sound bank | MIDI music needs one, and a CrossOver bottle has none. |
| Wine 11.21 DirectMusic DLLs | CrossOver 26.3's synth has an older underrun check that made the music skip. The whole family is replaced because the new synth alone left the drums missing. |
| `clear-builtin-marker.py` | Otherwise Wine may substitute its own builtin DLL of the same name. |
| `fix-soundfont.py` | Wine ignores SoundFont scale tuning, so fixed-pitch drums play at the wrong pitch. Partial values other than 0 and 100 are not corrected. |
| `TP_ENTRYPOINT_REMOTETHREAD_MS`, `TP_LOADER_MANAGED_INIT` | The 64-bit loader can't inspect a suspended game under Wine, so it parks the game at its entry point instead. |
| `skip-dll-startup.py` | `apm.dll`'s startup appears to deadlock in Wine's XInput. OpenParrot replaces every `apm.dll` function Dead or Alive 6 imports. |
| GStreamer `libav` plugin | CrossOver has no WMV decoder. Without it, Dead or Alive 6 stays on "Now Loading". |
| `TP_D3D9_SHARED_COPY` | Wine's d3d9 doesn't implement resource sharing, which Dead or Alive 6 uses to pass movie frames to Direct3D 11. Frames are copied through memory instead, in `OpenParrot/src/Functions/D3D9SharedCopy.cpp`, which logs to `OpenParrotVideo.log`. |

## Adding a key layout

Each line of a file in `profiles/` is `<virtual key> <byte> <mask>`, or `pad <input> <byte> <mask>` for a gamepad, where `<input>` is `up`, `down`, `left`, `right` or a button number from 1 to 28. To find a game's inputs, run `OpenParrotInput.exe --probe` in the bottle while the game runs, then use the game's own input test screen.

To find a controller's button numbers, press them during play and read the `pad:` lines in `drive_c/game/OpenParrotInput.log` in the bottle. The same log says which controller is in use, or nothing if none was found.

## Limits

- The bottle is not a sandbox. Wine programs can read and write your files, and CrossOver fails if its `y:` and `z:` drive links are removed.
- Mixing Wine DLLs or other GStreamer libraries into CrossOver is unsupported, so a CrossOver update could break it. The plugin is from GStreamer 1.24.13, built against slightly newer libraries than CrossOver's 1.24.
- Dead or Alive 6 crashed once after Start before the WMV decoder was added, and the cause wasn't proven. DXVK made it far worse, so leave it on CrossOver's default D3DMetal.
- Apple plans to reduce Rosetta 2 from macOS 28, and these scripts depend on it.
- Check the license of any sound bank before sharing it. Do not commit game files or sound banks to this repository.

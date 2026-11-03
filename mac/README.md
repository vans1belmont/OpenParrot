# Running OpenParrot on macOS

These scripts run OpenParrot games on Apple Silicon Macs through CrossOver. This is experimental, and it has only been tested with one game, Dark Awake (32-bit, D3D8), on CrossOver 26.3 and macOS 27.

Gamepad input has been tested with one USB controller, which CrossOver presented through XInput. Not tested: other controllers, 64-bit games, newer Direct3D versions, Player 2 controls, and peripherals such as force feedback and card readers.

## What you need

- [CrossOver](https://www.codeweavers.com/crossover)
- A game backup of your own, with `game.exe` at the top level. None are included.
- `OpenParrot-Win32.zip` from this branch's "Build Windows binaries" workflow
- A General MIDI sound bank (`.sf2` or `.dls`), if the game's music is MIDI. It needs drum kits in bank 128.

## Setup

```bash
./mac/setup-bottle.sh --game <game.zip> --openparrot <OpenParrot-Win32.zip> --soundbank <bank.sf2> --profile darkawake
```

The script creates a bottle called `OpenParrot`, which you can change with `--bottle`, and:

- copies in the game and OpenParrot
- installs a key layout, if you pass `--profile`
- replaces CrossOver's DirectMusic DLLs in that bottle with ones from Wine 11.21, keeping the originals as `.cxbackup`
- installs a repaired copy of your sound bank as `gm.dls`

The Wine DLLs come from [Gcenx/macOS_Wine_builds](https://github.com/Gcenx/macOS_Wine_builds), an unsigned third-party build. The script downloads about 225 MB and checks it against a pinned SHA-256. Use `--wine-archive` if you already have the file.

## Playing

```bash
./mac/play.sh
```

Click the game window so it has focus. Close it with its red button. Only run one game at a time.

| Dark Awake control | Key | Gamepad (Xbox layout) |
|---|---|---|
| Move | Arrow keys | D-pad or left stick |
| Buttons 1-4 | Z, X, C, V | A, B, X, Y |
| Start | Return | Menu/Start |
| Test mode | T | |
| Test or service | Y, U | |

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

## Adding a key layout

Each line of a file in `profiles/` is `<virtual key> <byte> <mask>`, or `pad <input> <byte> <mask>` for a gamepad, where `<input>` is `up`, `down`, `left`, `right` or a button number from 1 to 28. To find a game's inputs, run `OpenParrotInput.exe --probe` in the bottle while the game runs, then use the game's own input test screen.

To find a controller's button numbers, press them during play and read the `pad:` lines in `drive_c/game/OpenParrotInput.log` in the bottle. The same log says which controller is in use, or nothing if none was found.

## Limits

- The bottle is not a sandbox. Wine programs can read and write your files, and CrossOver fails if its `y:` and `z:` drive links are removed.
- Mixing Wine DLLs into CrossOver is unsupported, so a CrossOver update could break it.
- Apple plans to reduce Rosetta 2 from macOS 28, and these scripts depend on it.
- Check the license of any sound bank before sharing it. Do not commit game files or sound banks to this repository.

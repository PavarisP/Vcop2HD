# VC2HD

A single-file HD loader for the 1997 Windows PC release of **Virtua Cop 2**.

`VC2HD.exe` sits next to `PPJ2DD.EXE`, launches the original game, and enables a
configurable 32-bit display resolution. By default it auto-detects the current
Windows desktop resolution, so a non-1080p monitor works without editing config.
It does not contain or redistribute any Sega game code or assets. The game files
remain yours and are used in place.

## Requirements

- A legally owned copy of the 1997 Virtua Cop 2 PC release.
- The game folder must contain `PPJ2DD.EXE`, `HGL_D3D.DLL`, and `VCOP2.INI`.
- The game folder must be writable; the loader temporarily patches two files and restores them.
- Windows 10 or 11, 64-bit.
- A display mode matching the effective resolution at 32-bit.

## Usage

1. Download `VC2HD.exe` from the Releases page.
2. Put `VC2HD.exe` in the same folder as `PPJ2DD.EXE`.
3. Run `VC2HD.exe`.

The first run creates `VC2HD.ini`. For example, on a 1080p desktop:

```ini
[Display]
AutoDetect=1
Width=1920
Height=1080
```

With `AutoDetect=1`, the loader uses the current Windows desktop resolution on
every launch, so it follows the monitor. The written `Width` and `Height` are
only the detected reference values.

To force a specific resolution, set `AutoDetect=0` and edit `Width`/`Height` to
any even 32-bit mode reported by the driver (`320x240` through `7680x4320`).
An existing `VC2HD.ini` without an `AutoDetect` key is treated as
`AutoDetect=0`, so older manual settings are preserved.

If the selected mode is not reported by the driver, the loader falls back to
the current desktop mode (or the largest supported 32-bit mode) and records the
change in `VC2HD.log`. The configured values in `VC2HD.ini` are left unchanged,
so the same file can be reused on a different monitor.

The loader writes a temporary backup, patches the game for the effective
resolution, starts `PPJ2DD.EXE`, waits for it to exit, then restores `VCOP2.INI`
and `HGL_D3D.DLL` byte-for-byte.

If the loader or the game is killed before it can restore, run:

```text
VC2HD.exe --restore
```

The next normal launch also repairs leftover backups automatically. Diagnostics
are written to `VC2HD.log`.

## Supported build

This release targets the English 1997 PC build:

| File | SHA-256 |
| --- | --- |
| `PPJ2DD.EXE` | `C7A7389C18A1236250EBF85C8488E1FD70CC0C5113BED081515860DBC0352CE8` |
| `HGL_D3D.DLL` | `CF70ECA20B1554065958328E6D639C6C34BA2114293CAA514755811408F4803D` |

The loader verifies the patch locations before changing anything and refuses
unknown builds instead of guessing.

## What it changes

- Temporarily sets `[GraphicDevice] Width` and `Height` to the effective
  resolution, with `Color=32`, `Mode=1`, `D3D=7`, and `DD=0` in `VCOP2.INI`.
- Temporarily replaces the first entry in `HGL_D3D.DLL`'s fallback mode table
  with the effective resolution at 32-bit, so the engine selects that mode
  instead of snapping back to 640x480.
- Patches the two 640x480/480-line limits in `PPJ2DD.EXE` in the suspended
  process, in memory only, to the effective width and height. The executable on
  disk is never modified.
- Does not bypass the CD check, copy protection, or any other game logic.

## Build

Publish the `VC2HD` folder itself as the repository root so the included GitHub
Actions workflow is detected.

### MinGW-w64

Install a 64-bit MinGW-w64 toolchain, then run:

```bat
build-mingw.bat
```

The output is `dist\VC2HD.exe`.

### CMake

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

The output is `build\Release\VC2HD.exe` with MSVC, or `build\VC2HD.exe` with a
single-config generator.

## Notes and limitations

- The game still creates a normal window with a title bar and menu; the render
  target is the configured/effective resolution. Borderless/fullscreen
  presentation is a future improvement.
- Some 2D artwork is upscaled from the original low-resolution assets. The 3D
  scene and HUD render at the new resolution.
- 16:9 output can slightly stretch 4:3-era artwork. The engine has no native
  widescreen mode.
- On a non-1080p monitor, `AutoDetect=1` uses that monitor's current desktop
  resolution. On a high-DPI or 4K desktop, auto-detect will run the game at that
  resolution; set `AutoDetect=0` with `Width=1920` and `Height=1080` if you
  prefer lower GPU load.
- Other regional releases, repacks, or modified executables may use different
  offsets and are not supported by this build.
- An unsigned loader that writes to game files may trigger Windows SmartScreen
  or antivirus warnings. Build from source if you prefer.

## Legal

Virtua Cop 2 and all related assets are copyright SEGA. This project is not
affiliated with or endorsed by SEGA. It ships no game code or game assets and is
intended only for personal use with a legally owned copy. Do not redistribute
the original game files.

The Ghidra pseudocode used while developing the patches is not part of this
repository. Only the original loader source is published here.

## License

The loader source is licensed under the Mozilla Public License 2.0. See
[LICENSE](LICENSE).

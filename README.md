# GTAVI Unlock FPS Runtime Patcher (PS5)

A lightweight, one-shot `.elf` runtime payload for jailbroken **PlayStation 5** consoles that unlocks the 30 FPS cap in *GTAVI* directly in memory—without modifying any game files on disk.

## Requirements

* Jailbroken PlayStation 5
* **Grand Theft Auto VI (`PPSA38500`)**

## Download

Grab the latest prebuilt `.elf` directly from [**Releases**](https://github.com/HSgod/gta6-unlockfps-patcher/releases/latest).

## Usage

1. Load `GTAVI UnlockFPS_v1.0.elf` through your Payload Manager (PLDMGR / `elfldr`).
2. A PS5 notification will appear confirming the payload is ready and **waiting up to 10 minutes** for GTAVI to launch.
3. Start GTAVI (or load the payload while the game is already running).
4. Once detected, the payload verifies the target code in memory, applies the FPS unlock patch, confirms the write via readback, and exits automatically.

## How It Works

* Scans all running `eboot.bin` processes while ignoring PS5 system apps (`NPXS*`).
* Resolves the game's ASLR base address and checks **64 bytes of surrounding context** at both target FPS writer sites (`0x3f73e51` and `0x40d2be1`) before touching memory—unrelated games or unsupported updates are safely skipped.
* Patches both sites in RAM (using kernel DMAP page-table writes on FW >= 8.40, or `mdbg_copyin` on older firmwares) and updates the live `g_TargetFrameTimeUs` (`0xe3be220`) and `g_RequestedFrameLock` (`0xe7bbb08`) globals so the change takes effect immediately.

## Customizing Target FPS

On first run, the payload creates `/data/gtavi-unlockfps/fps.txt`:

```ini
fps=60
```

You can edit this file over FTP to set any target between `30` and `120` (for example, `fps=40` for smoother frame pacing on 120 Hz / VRR displays) without rebuilding the payload.

Logs are saved to `/data/gtavi-unlockfps/gtavi-unlockfps.log`.

## Building from Source

### Docker (macOS / Linux / Windows)
```sh
chmod +x build.sh
./build.sh
```
The compiled `.elf` is written to `src/GTAVI UnlockFPS_v1.0.elf`.

### Local `ps5-payload-sdk`
```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cd src
make
```

### GitHub Actions
Every push automatically builds the payload via [`.github/workflows/build.yml`](.github/workflows/build.yml) and publishes the `.elf` to **Releases**.

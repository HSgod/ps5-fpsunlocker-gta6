# GTAVI Unlock FPS Runtime Patcher (PS5)

A lightweight, one-shot `.elf` runtime payload for jailbroken **PlayStation 5** consoles that unlocks the 30 FPS cap and optionally lowers the internal render resolution scale in *Grand Theft Auto VI* directly in memory—without modifying any game files on disk.

## Requirements

* Jailbroken PlayStation 5
* **Grand Theft Auto VI (`PPSA38500`)**

## Download

Grab the latest prebuilt `.elf` directly from [**Releases**](https://github.com/HSgod/gta6-unlockfps-patcher/releases/latest).

## Usage

1. Load `GTAVI UnlockFPS_v1.1.elf` through your Payload Manager (PLDMGR / `elfldr`).
2. A PS5 notification will appear confirming the payload is ready and **waiting up to 10 minutes** for `PPSA38500` to launch.
3. Start GTAVI *(for Resolution Scale to take effect, load the payload **before** starting the game; FPS unlock alone also works if the game is already running)*.
4. Once detected, the payload verifies the target code in memory, applies the patches, confirms readback, and exits automatically.

## How It Works

* Watches running `eboot.bin` processes strictly for `PPSA38500` without touching memory of system apps or other titles.
* Resolves the game's ASLR base address and checks **64 bytes of surrounding context** at every patch site before writing.
* **FPS Unlock:** Patches both target FPS writer sites (`0x3f73e51`, `0x40d2be1`) and updates the live `g_TargetFrameTimeUs` (`0xe3be220`) and `g_RequestedFrameLock` (`0xe7bbb08`) globals.
* **Resolution Scale:** When `scale > 0`, patches both render-scale index sites (`0x3a2baa0`, `0x3f73e2c`) to use the engine's built-in RAGE ratio table before `TAA2U` upscaling—freeing GPU load and SmartShift power budget for higher framerates (ideal for hitting the 48–60 Hz VRR window).
* Uses kernel DMAP page-table writes on FW >= 8.40, or `mdbg_copyin` on older firmwares.

## Configuration

On first run, the payload creates `/data/gtavi-unlockfps/fps.txt`:

```ini
fps=60
scale=2
```

Edit over FTP without rebuilding:
* **`fps`**: target FPS cap (`30`–`120`, default `60`).
* **`scale`**: internal render resolution scale (`0`–`10`, default `2`):
  * `0` = stock (unchanged)
  * `1` = `1/2` (50% — e.g. 1080p internal on 4K)
  * `2` = `2/3` (67% — e.g. 1440p internal on 4K)
  * `3` = `3/4` (75% — e.g. 1620p internal on 4K)
  * `4` = `4/5` (80%), `5` = `5/6` (83%), `6` = `7/8`, `7` = `9/10`, `8` = `11/12`, `9` = `14/15`, `10` = `29/30`

Logs are saved to `/data/gtavi-unlockfps/gtavi-unlockfps.log`.

## Building from Source

### Docker (macOS / Linux / Windows)
```sh
chmod +x build.sh
./build.sh
```
The compiled `.elf` is written to `src/GTAVI UnlockFPS_v1.1.elf`.

### Local `ps5-payload-sdk`
```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cd src
make
```

### GitHub Actions
Every push automatically builds the payload via [`.github/workflows/build.yml`](.github/workflows/build.yml) and publishes the `.elf` to **Releases**.

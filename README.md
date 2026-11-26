# GTAVI Unlock FPS Runtime Patcher (PS5)

A lightweight, one-shot `.elf` runtime payload for jailbroken **PlayStation 5** consoles that unlocks the 30 FPS cap, fixes delta-time game speed above 30 FPS, and lets you customize the internal `TAA2U` render resolution scale in *Grand Theft Auto VI* directly in memory—without modifying any game files on disk or degrading graphics quality presets.

## Requirements

* Jailbroken PlayStation 5
* **Grand Theft Auto VI (`PPSA38500`)**

## Download

Grab the latest prebuilt `.elf` directly from [**Releases**](https://github.com/HSgod/ps5-fpsunlocker-gta6/releases/latest).

## Usage

1. Load `GTAVI UnlockFPS_v1.7.elf` through your Payload Manager (PLDMGR / `elfldr`).
2. A PS5 notification will appear confirming the payload is ready and **waiting up to 10 minutes** for `PPSA38500` to launch.
3. Start GTAVI *(load the payload **before** starting the game so the `TAA2U` resolution scale applies before render targets are allocated)*.
4. Once detected, the payload verifies the target code in memory, applies the patches, confirms readback, and exits automatically.

## How It Works

* Watches running `eboot.bin` processes strictly for `PPSA38500` without touching memory of system apps or other titles.
* Resolves the game's ASLR base address and checks **64 bytes of surrounding context** at every patch site before writing.
* **FPS Unlock & Delta-Time Speed Fix:** Patches both target FPS writer sites (`0x3f73e51`, `0x40d2be1`) and lowers `fwTimer`'s minimum delta-time floor (`0x3f73ecd`, `0xabab49c`, `0xabab4a0`) from `1/30s` to `1ms` so gameplay runs at exact 1.0× speed at any framerate above 30 FPS.
* **TAA2U Render Resolution Scale:** On base PS5, GTAVI hardcodes its `TAA2U` internal render scale to `0.6667` (`2560×1440` upscaled to 4K) inside `TAA2U::Update()` (`0x62eadc0`). Patching this function lowers the internal resolution (default `50%` = `1920×1080` → upscaled to 4K via `TAA2U`) while keeping all other graphics settings (shadows, lighting, RT, textures, LOD, foliage) 100% stock.
* **RT Reflections Toggle (`rt_reflections=0`):** Uses Rockstar's built-in Stochastic Reflections (`sr_tracing` + `sr_denoising`) guard (`0x63254df` / `0xb3f3dd4..0xb3f3dd6`) to cleanly disable ray-traced reflections while keeping RTGI (`indirect_diffuse_lighting`) and all other graphics settings 100% stock.
* Uses kernel DMAP page-table writes on FW >= 8.40, or `mdbg_copyin` on older firmwares.

## Configuration

On first run, the payload creates `/data/gtavi-unlockfps/fps.txt`:

```ini
fps=60
res_pct=50
rt_reflections=0
```

Edit over FTP without rebuilding:
* **`fps`**: target FPS cap (`30`–`120`, default `60`).
* **`res_pct`**: internal `TAA2U` render resolution as a percentage of 4K output (`0` = stock `67%` / `1440p`, or `25`–`100`):
  * `67` = stock (`2560×1440`)
  * `50` = **`1920×1080`** *(default — ~44% fewer pixels than stock, keeps stock graphics quality)*
  * `45` = `1728×972`
  * `40` = `1536×864`
  * `33` = `1280×720`
* **`rt_reflections`**: `0` = disable Ray-Traced Stochastic Reflections *(default — big FPS boost while keeping RTGI on)*, `1` = stock RT Reflections.

Logs are saved to `/data/gtavi-unlockfps/gtavi-unlockfps.log`.

## Building from Source

### Docker (macOS / Linux / Windows)
```sh
chmod +x build.sh
./build.sh
```
The compiled `.elf` is written to `src/GTAVI UnlockFPS_v1.7.elf`.

### Local `ps5-payload-sdk`
```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cd src
make
```

### GitHub Actions
Every push automatically builds the payload via [`.github/workflows/build.yml`](.github/workflows/build.yml) and publishes the `.elf` to **Releases**.

# GTAVI Unlock FPS Runtime Patcher (PS5)

A lightweight, one-shot `.elf` runtime payload for jailbroken **PlayStation 5** consoles that unlocks the 30 FPS cap, fixes delta-time game speed above 30 FPS, and lets you customize the internal `TAA2U` render resolution scale in *Grand Theft Auto VI* directly in memory—without modifying any game files on disk or degrading graphics quality presets.

## Requirements

* Jailbroken PlayStation 5
* **Grand Theft Auto VI (`PPSA01547`, `PPSA29660`, `PPSA38500` — v01.002.001)**

## Download

Grab the latest prebuilt `.elf` directly from [**Releases**](https://github.com/HSgod/ps5-fpsunlocker-gta6/releases/latest).

## Usage

1. Load `GTAVI UnlockFPS_v1.13.elf` through your Payload Manager (PLDMGR / `elfldr`).
2. A PS5 notification will appear confirming the payload is ready and **waiting up to 10 minutes** for GTAVI (`PPSA01547` / `PPSA29660` / `PPSA38500`) to launch.
3. Start GTAVI *(load the payload **before** starting the game so the `TAA2U` resolution scale applies before render targets are allocated)*.
4. Once detected, the payload verifies the target code in memory, applies the patches, confirms readback, and exits automatically.

## How It Works

* Watches running `eboot.bin` processes strictly for `PPSA01547`, `PPSA29660`, and `PPSA38500` without touching memory of system apps or other titles.
* Resolves the game's ASLR base address and checks **64 bytes of surrounding context** at every patch site before writing.
* **FPS Unlock & Delta-Time Speed Fix:** Patches both target FPS writer sites (`0x3f73e51`, `0x40d2be1`) and lowers `fwTimer`'s minimum delta-time floor (`0x3f73ecd`, `0x41ec831`, `0xabab49c`, `0xabab4a0`) from `1/30s` to `1/3000s` (`0x39aec33e`) so gameplay runs at exact 1.0× speed at any framerate above 30 FPS.
* **TAA2U Render Resolution Scale:** On base PS5, GTAVI hardcodes its `TAA2U` internal render scale to `0.6667` (`2560×1440` upscaled to 4K) inside `TAA2U::Update()` (`0x62eadc0`). Patching this function lowers the internal resolution (default `50%` = `1920×1080` → upscaled to 4K via `TAA2U`) while keeping all other graphics settings (shadows, lighting, RT, textures, LOD, foliage) 100% stock.
* **Optional Runtime RT Reflections Toggle:** Setting `rt_reflections=0` zeroes the per-frame RT Reflections runtime flag (`0xb3f3e90` via `0x149dd26`) while keeping render target buffers allocated (`0xb3f3dd4 = 1`), cleanly skipping `sr_tracing` / `sr_denoising` dispatches for extra GPU headroom.
* Uses kernel DMAP page-table writes on FW >= 8.40, or `mdbg_copyin` on older firmwares.

## Configuration

On first run, the payload creates `/data/gtavi-unlockfps/fps.txt`:

```ini
fps=60
res_pct=50
rt_reflections=1
daemon=0
```

Edit over FTP without rebuilding (out-of-range values are logged and fall back to their defaults):
* **`fps`**: target FPS cap (`30`–`120`, default `60`).
* **`res_pct`**: internal `TAA2U` render resolution as a percentage of 4K output (`0` = stock `67%` / `1440p`, or any integer `25`–`100`):
  * `67` = stock (`2560×1440`)
  * `60` = `2304×1296` *(sharper middle ground)*
  * `55` = `2112×1188` *(balanced middle ground between 1080p and 1440p)*
  * `50` = **`1920×1080`** *(default — ~44% fewer pixels than stock, keeps stock graphics quality)*
  * `25`–`49` = *experimental* (e.g. `33` ≈ `720p`; softer image / more `TAA2U` shimmering)
* **`rt_reflections`**: runtime Ray-Traced Reflections toggle (`1` = stock ON *(default)*, `0` = OFF *(experimental — +3–4 FPS, may cause ocean/water visual glitches)*).
* **`daemon`**: background mode (`0` = one-shot, waits up to 10 min and exits *(default)*; `1` = stays running in the background after jailbreak, reloads `fps.txt` and automatically re-patches GTAVI across game restarts).

Logs are saved to `/data/gtavi-unlockfps/gtavi-unlockfps.log`.

## Offsets (`v01.002.001`)

RVAs relative to `eboot.bin` map base (with `+0x400000` static base addresses in parentheses):

* **`0x03f73e51`** (`0x04373e51`) — Target FPS cap writer #1 (`b8 <fps_u32> 90`)
* **`0x040d2be1`** (`0x044d2be1`) — Target FPS cap writer #2 (`b8 <fps_u32> 66 90`)
* **`0x03f73ecd`** (`0x04373ecd`) — `fwTimer` delta-time floor divisor writer #1 (`b8 b8 0b 00 00 90` → `3000`)
* **`0x041ec831`** (`0x045ec831`) — `fwTimer` delta-time floor writer #2 (`9090909090909090`)
* **`0x062eadc0`** (`0x066eadc0`) — `TAA2U::Update()` internal render scale function (18 bytes)
* **`0x0149dd26`** (`0x0189dd26`) — RT Reflections per-frame runtime flag writer (`31c090909090`, only when `rt_reflections=0`)
* **`0x0abab49c`** (`0x0afab49c`) — RAM `g_MinFrameSec` (`3ec3ae39` = `1/3000s`)
* **`0x0abab4a0`** (`0x0afab4a0`) — RAM `g_TargetFrameSec` (`3ec3ae39` = `1/3000s`)
* **`0x0e3be220`** (`0x0e7be220`) — RAM `g_TargetFrameTimeUs` (`1000000 / fps`)
* **`0x0e7bbb08`** (`0x0ebbbb08`) — RAM `g_RequestedFrameLock` (`ff`)

## Building from Source

### Docker (macOS / Linux / Windows)
```sh
chmod +x build.sh
./build.sh
```
The compiled `.elf` is written to `src/GTAVI UnlockFPS_v1.13.elf`.

### Local `ps5-payload-sdk`
```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cd src
make
```

### GitHub Actions
Every push automatically builds the payload via [`.github/workflows/build.yml`](.github/workflows/build.yml) and publishes the `.elf` to **Releases**.

## Credits

* **[armanate](https://github.com/armanate/rdr2-60fps-runtime-patcher)** — runtime payload architecture, process/context verification, and DMAP page-table write logic.
* **a_Criminai** — additional `fwTimer` / region offsets referenced from the XML patch.

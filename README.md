# GTA VI FPS Unlocker (PS5)

A lightweight `.elf` runtime payload for jailbroken **PlayStation 5** & **PS5 Pro** consoles that unlocks the 30 FPS cap, fixes delta-time game speed above 30 FPS, and lets you customize the internal render resolution scale (`TAA2U` on base PS5, **PSSR** on PS5 Pro) in *Grand Theft Auto VI* directly in memory—without modifying any game files on disk or degrading graphics quality presets.

## Requirements

* Jailbroken PlayStation 5 or PS5 Pro
* **Grand Theft Auto VI (`PPSA01547`, `PPSA29660`, `PPSA38500` — `v01.002.001`)**

## Download

* **Direct download:** Grab the latest prebuilt `.elf` from [**Releases**](https://github.com/HSgod/ps5-fpsunlocker-gta6/releases/latest).
* **PS5 Payload Manager (auto-updates):**
  1. Open **PS5 Payload Manager** → **Settings** → enable **Multiple Payload Sources**.
  2. Go to **Manage Sources** → **+ Add Source**.
  3. Paste:
     ```
     https://raw.githubusercontent.com/HSgod/ps5-fpsunlocker-gta6/main/repo.json
     ```

## Usage

1. Load `GTAVI UnlockFPS_v1.15.elf` through your Payload Manager (PLDMGR / `elfldr`).
2. A PS5 notification will appear confirming the payload is ready and waiting for GTAVI (`PPSA01547` / `PPSA29660` / `PPSA38500`) to launch *(up to 10 minutes in one-shot mode, or indefinitely when `daemon=1`)*.
3. Start GTAVI *(load the payload **before** starting the game so `TAA2U` / `PSSR` scaling and optional buffer/RT settings apply before render targets are allocated)*.
4. Once detected, the payload verifies the target code in memory, applies the patches, confirms readback, and either exits automatically (`daemon=0`) or stays running in the background to re-patch future launches (`daemon=1`).

## Configuration

On first run, the payload creates `/data/gtavi-unlockfps/fps.txt`:

```ini
fps=60
res_pct=50
refresh_hz=60
output_res=0
pro_rt_half=0
daemon=0
```

Edit over FTP without rebuilding (out-of-range values are logged and fall back to their defaults):

| Option | Values | Default | Description |
| --- | --- | --- | --- |
| **`fps`** | `30`–`120` | `60` | Target FPS cap. On standard **60 Hz** output, GTAVI uses hardware V-Sync dividers (`30` = locked 30 FPS, `31`–`120` = unlocked up to 60 FPS). With forced **120 Hz** + `refresh_hz=120`, supports hardware locks at `30`, `40`, `60`, or `120` FPS. |
| **`res_pct`** | `0`, or `25`–`100` | `50` | Internal `TAA2U` (PS5) / `PSSR` (PS5 Pro) render resolution as a % of output (`0` = stock). |
| **`refresh_hz`** | `60` / `120` | `60` | Refresh rate used by `sceVideoOutSetFlipRate`. Keep `60` for standard output; set **`120`** only if you are forcing **120 Hz output** via PS Patch System (enables true hardware-locked **40 FPS** with `fps=40`). |
| **`output_res`** | `0` / `1440` / `1080` | `0` | *(Experimental)* Overrides the swapchain output buffer resolution (`0` = stock 4K `3840×2160`, `1440` = native `2560×1440`, `1080` = native `1920×1080`). Useful for 1440p/1080p monitors or extra GPU headroom on base PS5. |
| **`pro_rt_half`** | `0` / `1` | `0` | *(Experimental, PS5 Pro)* Lowers PS5 Pro's RT Reflections from full-res (Tier 3) to half-res (matching base PS5) for extra FPS and RT pool headroom while keeping **PSSR** and intact ocean water. |
| **`daemon`** | `0` / `1` | `0` | `0` = one-shot (waits up to 10 min and exits); `1` = stays in the background, reloads `fps.txt` and automatically re-patches GTAVI across game restarts. |

### `res_pct` Reference Values (4K Output)

| `res_pct` | Internal Resolution | Notes |
| --- | --- | --- |
| `100` | `3840×2160` *(Native 4K)* | **Experimental** — 2.25× more pixels than stock 1440p; very heavy on GPU and VRAM (may hit low FPS or render-target pool OOM). |
| `67` | `2560×1440` | **Stock PS5 Fat/Slim** (`TAA2U`). **Do not use on PS5 Pro** with stock RT — exceeds Pro's transient RT pool (`0x08930da7` crash on boot); use `0` for stock PS5 Pro (`2240×1260`). |
| `60` | `2304×1296` | Sharper middle ground (safe upper limit tested on **PS5 Pro** PSSR). |
| `55` | `2112×1188` | Balanced middle ground between 1080p and 1440p (works on PS5 & PS5 Pro). |
| **`50`** | **`1920×1080`** | **Default** — ~44% fewer pixels than base PS5 stock, keeps all other graphics settings 100% stock. |
| `25`–`49` | `960×540` – `1881×1058` | **Experimental** (e.g. `33` ≈ `720p`; softer image / more upscaling shimmering). |
| `0` | Stock (`2560×1440` PS5 / `2240×1260` Pro) | Leaves internal resolution completely untouched. |

Logs are saved to `/data/gtavi-unlockfps/gtavi-unlockfps.log`.

## Thermals & Temperatures

Unlocking the 30 FPS cap (especially combined with a lower `res_pct`) removes the idle time between frames, so the PS5 CPU/SoC runs at sustained full load—similar to native 60 FPS titles. Seeing **5–8 °C higher CPU temperatures** compared to stock 30 FPS is completely expected.

* The PS5's Zen 2 APU is rated up to ~100–105 °C `TjMax` and has built-in hardware thermal protection (overheating warning and automatic shutdown well before damage can occur).
* However, if your console reaches **~88–90 °C** (especially if it already runs at 80–83 °C at stock 30 FPS), it usually points to **dust buildup, restricted airflow, or dry/shifted liquid metal** on older PS5 Fat units.
* **Recommended steps if temps are high:**
  * Adjust your fan curve / lower the target temperature via **etaHEN / PS5 Fan Control**.
  * Ensure the console has open ventilation and clean the heatsink/fan of dust.
  * Optionally raise `res_pct` (e.g. `55`–`60`) or lower `fps` in `fps.txt` to reduce sustained CPU load.

## How It Works

* Watches running `eboot.bin` processes strictly for `PPSA01547`, `PPSA29660`, and `PPSA38500` without touching memory of system apps or other titles.
* Resolves the game's ASLR base address and verifies **64 bytes of surrounding context** at every patch site before writing (see [`src/gtavi-unlockfps.c`](src/gtavi-unlockfps.c) for exact RVAs).
* **FPS Unlock & Delta-Time Speed Fix:** Patches both target FPS writer sites and lowers `fwTimer`'s minimum delta-time floor from `1/30s` to `1/3000s` (`0x39aec33e`) so gameplay runs at exact 1.0× speed at any framerate above 30 FPS.
* **TAA2U & PSSR Render Resolution Scale:** Patches `TAA2U::Update()` (base PS5) and `PSSR::GetRenderDimensions()` (PS5 Pro) so `res_pct` scales internal resolution cleanly while keeping native `TAA2U` / `PSSR` upscaling and leaving all other graphics settings 100% stock.
* **120 Hz Flip-Rate, Output Resolution & Pro RT Half-Res:** Optional opt-in patches for forced 120 Hz `sceVideoOutSetFlipRate` dividers, custom swapchain output dimensions (`1440p`/`1080p`), and PS5 Pro half-res RT Reflections.
* Uses kernel DMAP page-table writes on FW >= 8.40, or `mdbg_copyin` on older firmwares.

## Building from Source

### Docker (macOS / Linux / Windows)
```sh
chmod +x build.sh
./build.sh
```
The compiled `.elf` is written to `src/GTAVI UnlockFPS_v1.15.elf`.

### Local `ps5-payload-sdk`
```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cd src
make
```

### GitHub Actions
Every code push automatically builds the payload via [`.github/workflows/build.yml`](.github/workflows/build.yml), publishes the `.elf` to **Releases**, and updates `repo.json`.

## Credits

* **[armanate](https://github.com/armanate/rdr2-60fps-runtime-patcher)** — runtime payload architecture, process/context verification, and DMAP page-table write logic.
* **a_Criminai** — additional `fwTimer` / region offsets referenced from the XML patch.

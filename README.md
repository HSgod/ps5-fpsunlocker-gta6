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

1. Load `GTAVI UnlockFPS_v1.16.elf` through your Payload Manager (PLDMGR / `elfldr`).
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
pro_highres=0
close_fix=1
pro_taa2u=0
low_shadows=0
low_volumetric=0
base_highres=0
```

Edit over FTP without rebuilding (out-of-range values are logged and fall back to their defaults):

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| **`fps`** | `30`–`120` | `60` | Target framerate cap. On a **60 Hz** TV: `30` locks to 30 FPS, `60` unlocks up to 60 FPS. On a **120 Hz** TV (with `refresh_hz=120`), you can also lock to `40` FPS or go above 60. |
| **`res_pct`** | `0`, or `25`–`100` | `50` | Internal rendering resolution (% of output). Lower = higher FPS, higher = sharper image. `50` = 1080p (default), `0` = game default. |
| **`refresh_hz`** | `60` / `120` | `60` | Match your console's output refresh rate. Keep `60` on normal TVs; set **`120`** only if you force 120 Hz via [ps-patch-system](https://github.com/illusionyy/ps-patch-system) *(using `120` on a 60 Hz TV will cut FPS in half)*. |
| **`output_res`** | `0` / `1440` / `1080` | `0` | *(Experimental)* Changes the game's output resolution (`0` = 4K, `1440` = 1440p, `1080` = 1080p) instead of upscaling to 4K. Useful for 1080p/1440p monitors or extra FPS. |
| **`pro_rt_half`** | `0` / `1` | `0` | *(Experimental, PS5 Pro)* Lowers Ray-Traced Reflections on PS5 Pro to base PS5 resolution for higher FPS while keeping PSSR and normal water. |
| **`pro_highres`** | `0` / `1` | `0` | *(PS5 Pro, 4K output)* Expands PS5 Pro video memory at startup so you can use `res_pct` above `66` (up to `90` / native 4K) without crashing. |
| **`close_fix`** | `0` / `1` | `1` | Prevents the **`CE-108255-1`** error popup when closing the game (`1` = enabled by default). |
| **`pro_taa2u`** | `0` / `1` | `0` | *(Experimental, PS5 Pro)* Uses standard TAA upscaling instead of PSSR on PS5 Pro — can improve FPS or reduce shimmering on trees/foliage. |
| **`low_shadows`** | `0` / `1` | `0` | *(Experimental)* Lowers shadow resolution for extra FPS on both PS5 and PS5 Pro. |
| **`low_volumetric`** | `0` / `1` | `0` | *(Experimental)* Lowers fog / volumetric lighting resolution for extra FPS on both PS5 and PS5 Pro. |
| **`base_highres`** | `0` / `1` | `0` | *(Experimental, Base PS5)* Expands video memory on base PS5 at startup so you can test `res_pct` above `67` without crashing. |
| **`daemon`** | `0` / `1` | `0` | `0` = runs once and closes after patching; `1` = stays in the background and automatically re-patches if you restart the game. |

### `res_pct` Reference Values (4K Output)

> **Memory limit for high `res_pct`:** With default settings, going above **`67%`** on base PS5 or **`66%`** on PS5 Pro will crash the game on startup due to its built-in memory limit. To go higher (e.g. `70`–`100`), enable **`pro_highres=1`** (PS5 Pro, 4K output) or **`base_highres=1`** *(experimental, base PS5)* and load the payload **before** starting the game.

| `res_pct` | Internal Resolution (at 4K `output_res=0`) | Notes |
| --- | --- | --- |
| `67` | `2574×1448` | **Stock PS5 Fat/Slim** (`TAA2U`). Crashes on **PS5 Pro** unless `pro_highres=1` is enabled (use `0` for stock PS5 Pro `2560×1440`). |
| `60` | `2304×1296` | Sharper middle ground (safe upper limit tested on **PS5 Pro** PSSR). |
| `55` | `2112×1188` | Balanced middle ground between 1080p and 1440p (works on PS5 & PS5 Pro). |
| **`50`** | **`1920×1080`** | **Default** — ~44% fewer pixels than base PS5 stock, keeps all other graphics settings 100% stock. |
| `25`–`49` | `960×540` – `1884×1060` | **Experimental** (e.g. `33` ≈ `720p`; softer image / more upscaling shimmering). |
| `0` | Stock (`2560×1440` PS5 / `2560×1440` Pro) | Leaves internal resolution completely untouched. |

> **Not sure what to pick?** See **[SETTINGS.md](SETTINGS.md)** for ready-to-use presets for **PS5 Fat/Slim** & **PS5 Pro** and how to combine `output_res` with `res_pct`.

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
* **TAA2U & PSSR Render Resolution Scale:** Patches `TAA2U::Update()` (base PS5) and `PSSR::GetRenderDimensions()` (PS5 Pro) with even width/height rounding so `res_pct` scales internal resolution cleanly without RT reflection speckles.
* **Close-Game `CE-108255-1` Fix (`close_fix=1`):** Bypasses the synchronous `SAVEDATAA00` / `SPACECHECK00.dat` probe (`0x65742c0`) so closing GTAVI no longer triggers `CE-108255-1`.
* **Early Boot Freeze & Optional Tweaks:** Uses `SIGSTOP`/`SIGCONT` before engine init when boot-time options are enabled (`pro_highres`, `base_highres`, `output_res`, `pro_rt_half`, `pro_taa2u`, `low_shadows`, `low_volumetric`, plus `refresh_hz=120` flip-rate dividers).
* Uses kernel DMAP page-table writes on FW >= 8.40, or `mdbg_copyin` on older firmwares.

## Building from Source

### Docker (macOS / Linux / Windows)
```sh
chmod +x build.sh
./build.sh
```
The compiled `.elf` is written to `src/GTAVI UnlockFPS_v1.16.elf`.

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
* **[OverkillLabs3](https://github.com/OverkillLabs3)** — `pro_highres` PS5 Pro heap expansion, even render-resolution rounding (`compute_resolution`), and resolution unit tests ([PR #16](https://github.com/HSgod/ps5-fpsunlocker-gta6/pull/16)).
* **Criminai** — additional `fwTimer` / region offsets referenced from the XML patch.
* **[Eizovoid](https://github.com/Eizovoid/gta-vi-ps5-patches)** — root-cause lead for the `CE-108255-1` close-game error (reimplemented here as a full `SPACECHECK00.dat` probe bypass).

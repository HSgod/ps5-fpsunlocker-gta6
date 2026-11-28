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

1. Load `GTAVI UnlockFPS_v1.17.elf` through your Payload Manager (PLDMGR / `elfldr`).
2. A PS5 notification will appear confirming the payload is ready and waiting for GTAVI (`PPSA01547` / `PPSA29660` / `PPSA38500`) to launch *(up to 10 minutes in one-shot mode, or indefinitely when `daemon=1`)*.
3. Start GTAVI *(load the payload **before** starting the game so `TAA2U` / `PSSR` scaling and optional buffer/RT/graphics tweaks apply before render targets are allocated)*.
4. Once detected, the payload verifies the target code in memory, applies the patches, confirms readback, and either exits automatically (`daemon=0`) or stays running in the background to re-patch future launches (`daemon=1`).

## Configuration & Presets

On first run, the payload creates `/data/gtavi-unlockfps/fps.txt` with inline comments for every option (defaults: **`fps=60`**, **`res_pct=50`** [`1920×1080` internal], **`close_fix=1`**). Edit it anytime over FTP without rebuilding.

* **[SETUP.md](SETUP.md)** — full list of all `fps.txt` options (grouped by category), `res_pct` resolution reference table, and high-resolution memory limit guide.
* **[SETTINGS.md](SETTINGS.md)** — ready-to-use presets for **PS5 Fat/Slim** & **PS5 Pro** and how to combine `output_res` with `res_pct`.

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
* **Early Boot Freeze & Optional Tweaks:** Uses `SIGSTOP`/`SIGCONT` before engine init when boot-time options are enabled (`pro_highres`, `base_highres`, `output_res`, `pro_rt_half`, `pro_taa2u`, `low_shadows`, `low_reflections`, `low_volumetric`, `lod_scale`, `low_bvh`, `motion_blur`, plus `refresh_hz=120` flip-rate dividers).
* Uses kernel DMAP page-table writes on FW >= 8.40, or `mdbg_copyin` on older firmwares.

## Building from Source

### Docker (macOS / Linux / Windows)
```sh
chmod +x build.sh
./build.sh
```
The compiled `.elf` is written to `src/GTAVI UnlockFPS_v1.17.elf`.

### Local `ps5-payload-sdk`
```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cd src
make
```

### GitHub Actions
Every code push automatically runs host unit tests and builds the payload via [`.github/workflows/build.yml`](.github/workflows/build.yml), publishes the `.elf` to **Releases**, and updates `repo.json`.

## Credits

* **[armanate](https://github.com/armanate/rdr2-60fps-runtime-patcher)** — runtime payload architecture, process/context verification, and DMAP page-table write logic.
* **[OverkillLabs3](https://github.com/OverkillLabs3)** — `pro_highres` PS5 Pro heap expansion, even render-resolution rounding (`compute_resolution`), resolution unit tests ([PR #16](https://github.com/HSgod/ps5-fpsunlocker-gta6/pull/16)), and full mapping of all 108 `fwGraphicsSettings` / `fwAdvancedGraphicsSettings` offsets & runtime globals ([Issue #20](https://github.com/HSgod/ps5-fpsunlocker-gta6/issues/20)).
* **Criminai** — additional `fwTimer` / region offsets referenced from the XML patch.
* **[Eizovoid](https://github.com/Eizovoid/gta-vi-ps5-patches)** — root-cause lead for the `CE-108255-1` close-game error (reimplemented here as a full `SPACECHECK00.dat` probe bypass).

# GTA 6 60 FPS Runtime Patcher (PS5 Payload)

One-shot `.elf` runtime FPS patcher payload for jailbroken **PlayStation 5** consoles, modeled after [armanate/rdr2-60fps-runtime-patcher](https://github.com/armanate/rdr2-60fps-runtime-patcher).

## How It Works
1. Launch `GTA6 60FPS_v1.0.elf` via your Payload Manager (PLDMGR / `elfldr`).
2. The payload shows a PS5 notification and **waits up to 10 minutes** for GTA 6 (`eboot.bin`) to start.
3. Once detected, it resolves the ASLR base address of `eboot.bin` and verifies **64 bytes of surrounding context** at both target FPS writer sites (`0x3f73e51` and `0x40d2be1`) before writing anything — unrelated games or mismatched versions are safely ignored.
4. Patches both sites in RAM (using DMAP page-table writes via `ptwalk` on FW >= 8.40, or `mdbg_copyin` on older firmwares), verifies readback, updates the live `g_TargetFrameTimeUs` (`0xe3be220`) and `g_RequestedFrameLock` (`0xe7bbb08`) globals so it works even if the game was already open, and exits cleanly.

## Configuring Target FPS (60 / 40 / Custom)
On first launch, the payload creates `/data/gta6-60fps/fps.txt`:
```ini
fps=60
```
You can edit this file over FTP (e.g., set `fps=40` for smoother frame pacing on 120 Hz / VRR displays, or any value between `30` and `120`) without recompiling. Runtime logs are written to `/data/gta6-60fps/gta6-60fps.log`.

## Building `.elf`

### Option 1: GitHub Actions
Push to GitHub — the included [`.github/workflows/build.yml`](.github/workflows/build.yml) workflow automatically builds `GTA6 60FPS_v1.0.elf` and uploads it under **Actions → Artifacts** (`GTA6-60FPS-Payload`).

### Option 2: Docker (macOS / Linux / Windows)
```sh
chmod +x build.sh
./build.sh
```
The compiled payload will be output to `src/GTA6 60FPS_v1.0.elf`.

### Option 3: Local `ps5-payload-sdk`
```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cd src
make
```

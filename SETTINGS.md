# Recommended Settings (`fps.txt`)

Copy any preset below into `/data/gtavi-unlockfps/fps.txt` over FTP. If `daemon=1` is enabled, changes take effect automatically the next time you restart GTAVI (no need to reload the `.elf`).

---

## PS5 Fat / Slim

### 1. Balanced FPS & Sharpness on 4K TV *(Recommended)*
```ini
fps=60
res_pct=50
refresh_hz=60
output_res=0
pro_rt_half=0
daemon=1
```
* Renders internally at **`1920×1080`** and upscales to 4K via `TAA2U` (~44–50+ FPS, pairs well with PS5 VRR 48–60 Hz).
* For a slightly sharper image at the cost of a few FPS, try `res_pct=55` (`2112×1188`).

### 2. Maximum FPS / 1080p Monitor *(Experimental)*
```ini
fps=60
res_pct=100
refresh_hz=60
output_res=1080
pro_rt_half=0
daemon=1
```
* Native 1:1 **`1920×1080`** output—skips 4K `TAA2U` upscaling and 4K post-processing for extra GPU headroom on base PS5.

### 3. 1440p Monitor (No 4K Double-Scaling) *(Experimental)*
```ini
fps=60
res_pct=75
refresh_hz=60
output_res=1440
pro_rt_half=0
daemon=1
```
* Renders internally at **`1920×1080`** and outputs natively to **`2560×1440`** (avoids `1440p → 4K → 1440p` scaling). Use `res_pct=100` for native 1:1 `2560×1440` (closer to stock 30–35 FPS).

---

## PS5 Pro

### 1. Balanced FPS & PSSR Quality (4K 60 Hz / VRR) *(Recommended)*
```ini
fps=60
res_pct=55
refresh_hz=60
output_res=0
pro_rt_half=0
daemon=1
```
* Renders internally at **`2112×1188`** and upscales to 4K via native **PSSR**.
* With stock Pro RT (`pro_rt_half=0`), keep `res_pct` at **`60` or lower** (or `0` for stock `2240×1260`); `67%+` exceeds PS5 Pro's transient RT pool (`0x08930da7` crash on boot).

### 2. Locked 40 FPS on 120 Hz Displays
```ini
fps=40
res_pct=60
refresh_hz=120
output_res=0
pro_rt_half=0
daemon=1
```
* Use only when running in **120 Hz output mode**. Locks hardware V-Sync to an even **`40.00 FPS`** (`120 / 3`) at **`2304×1296` → PSSR 4K**.

### 3. Maximum FPS on PS5 Pro *(Experimental)*
```ini
fps=60
res_pct=50
refresh_hz=60
output_res=0
pro_rt_half=1
daemon=1
```
* **`1920×1080` → PSSR 4K** with RT Reflections lowered from full-res (Tier 3) to half-res (matching base PS5) for maximum FPS while keeping PSSR and intact ocean water.

---

## Combining `output_res` with `res_pct`

`res_pct` scales relative to **`output_res`** (not always 4K), so adjust `res_pct` accordingly when changing `output_res`:

| `output_res` | `res_pct` | Effective Internal → Output | Use Case |
| --- | --- | --- | --- |
| `0` *(4K)* | `50` | `1920×1080` → `3840×2160` | **Default** — best balance on 4K TVs. |
| `1440` | `100` | `2560×1440` → `2560×1440` | Native 1:1 1440p on 1440p monitors (no 4K double-scaling). |
| `1440` | `75` | `1920×1080` → `2560×1440` | 1080p internal upscaled cleanly to 1440p (high FPS on 1440p displays). |
| `1080` | `100` | `1920×1080` → `1920×1080` | Native 1:1 1080p (skips 4K upscaling/post-FX for maximum FPS on base PS5). |

> **Tip:** Avoid leaving `res_pct=50` when using `output_res=1080` unless you intentionally want `960×540` internal resolution!

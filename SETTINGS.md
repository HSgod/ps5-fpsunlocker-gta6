# Recommended Settings (`fps.txt`)

Copy any preset below into `/data/gtavi-unlockfps/fps.txt` over FTP. If `daemon=1` is enabled, changes take effect automatically the next time you restart GTAVI (no need to reload the `.elf`).

> **Important `res_pct` limit:** GTAVI sizes its internal render target memory pool proportionally to `output_res`, expecting stock scale ratios (**`<= 67%`** on base PS5 Fat/Slim, **`<= 66%`** on PS5 Pro). Setting `res_pct` above those limits (e.g. `75` or `100`) overflows the allocator pool and crashes on boot (`OnOutOfMemory`), even when `output_res` is `1440` or `1080`. On PS5 Pro use `pro_highres=1` (preset 4) to go higher.

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
* For a slightly sharper image at the cost of a few FPS, try `res_pct=55` (`2112×1188`) or `60` (`2304×1296`).

### 2. Locked 40 FPS on 120 Hz Displays *(Balanced Sharpness & Smoothness)*
```ini
fps=40
res_pct=55
refresh_hz=120
output_res=0
pro_rt_half=0
daemon=1
```
* **Requires forcing 120 Hz output externally via [ps-patch-system](https://github.com/illusionyy/ps-patch-system)** (GTAVI does not switch HDMI to 120 Hz on its own; setting `refresh_hz=120` on a 60 Hz signal will cap FPS to 20).
* Locks hardware V-Sync to an even **`40.00 FPS`** (`120 / 3`) at **`2112×1188` → `TAA2U` 4K** (`res_pct=50` if you want extra headroom in dense city scenes).

### 3. 1440p Monitor (No 4K Double-Scaling) *(Experimental)*
```ini
fps=60
res_pct=0
refresh_hz=60
output_res=1440
pro_rt_half=0
daemon=1
```
* Outputs natively at **`2560×1440`** with stock `67%` internal scaling (`1706×960` → `1440p`), avoiding 4K upscaling/downscaling on 1440p monitors and saving GPU time on post-processing/HUD. Keep `res_pct` at `0` (or `<= 67`).

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
* Keep `res_pct` at **`66` or lower** (or `0` for stock `2560×1440`); `67%+` exceeds PS5 Pro's transient render heap (`0x08930da7` crash) unless `pro_highres=1` (preset 4).

### 2. Locked 40 FPS on 120 Hz Displays
```ini
fps=40
res_pct=60
refresh_hz=120
output_res=0
pro_rt_half=0
daemon=1
```
* **Requires forcing 120 Hz output externally via [ps-patch-system](https://github.com/illusionyy/ps-patch-system)** (GTAVI does not switch HDMI to 120 Hz on its own; setting `refresh_hz=120` on a 60 Hz signal will cap FPS to 20). Locks hardware V-Sync to an even **`40.00 FPS`** (`120 / 3`) at **`2304×1296` → PSSR 4K**.

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

### 4. Above-Stock Resolution with Full RT *(Experimental)*
```ini
fps=40
res_pct=80
refresh_hz=120
output_res=0
pro_rt_half=0
daemon=0
pro_highres=1
```
* **`3072x1728` -> PSSR 4K** with full-res RT. `fps=40` needs forced 120 Hz (see preset 2); otherwise use `fps=30`, `refresh_hz=60`.
* `pro_highres=1` enlarges the render heaps that cause the `67%+` crash (`0x08930da7`) and takes the memory from the main pool. Load the payload **before** starting GTAVI, or it falls back to `2532x1424`.
* Sizes are rounded to even width/height; odd sizes (e.g. `2572x1447` at 67%) give green speckles in RT reflections.
* Up to `res_pct=90` tested clean. 85%+ dips below 30 FPS; 100% boots but leaves almost no main pool headroom.

---

## Combining `output_res` with `res_pct`

`res_pct` always scales relative to **`output_res`** and must stay within the game's allocator ratio (**`0` or `<= 67`** on base PS5, **`0` or `<= 66`** on PS5 Pro):

| `output_res` | `res_pct` | Effective Internal → Output | Notes |
| --- | --- | --- | --- |
| `0` *(4K)* | `50` | `1920×1080` → `3840×2160` | **Default** — best balance on 4K TVs. |
| `0` *(4K)* | `55` / `60` | `2112×1188` / `2304×1296` → `3840×2160` | Sharper 4K output (works on PS5 & PS5 Pro). |
| `1440` | `0` *(stock)* | `1706×960` (PS5) / `1493×840` (Pro) → `2560×1440` | Native 1440p output without 4K upscaling/post-FX. |
| `1440` | `60` | `1536×864` → `2560×1440` | Confirmed working on PS5 Pro (`67%+` crashes with OOM). |
| `1080` | `0` *(stock)* | `1280×720` → `1920×1080` | Native 1080p output (lightest GPU load; `> 67%` crashes with OOM). |

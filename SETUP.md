# Configuration Guide (`fps.txt`)

On first run, the payload automatically creates `/data/gtavi-unlockfps/fps.txt` with inline comments for every option. You can edit it anytime over FTP without rebuilding (out-of-range values are logged and fall back to their defaults).

> **Not sure what to pick?** See **[SETTINGS.md](SETTINGS.md)** for ready-to-use presets for **PS5 Fat/Slim** & **PS5 Pro** and how to combine `output_res` with `res_pct`.

## Default `fps.txt`

```ini
fps=60
res_pct=50
refresh_hz=60
output_res=0
motion_blur=1
pro_highres=0
pro_rt_half=0
pro_taa2u=0
low_shadows=0
low_reflections=0
low_volumetric=0
lod_scale=0
low_bvh=0
base_highres=0
close_fix=1
daemon=0
```

---

## 1. Framerate & Display

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| **`fps`** | `30`–`120` | `60` | Target framerate cap. On a **60 Hz** TV: `30` locks to 30 FPS, `60` unlocks up to 60 FPS. On a **120 Hz** TV (with `refresh_hz=120`), you can also lock to `40` FPS or go above 60. |
| **`res_pct`** | `0`, or `25`–`100` | `50` | Internal rendering resolution (% of output). Lower = higher FPS, higher = sharper image. `50` = 1080p (default), `0` = game default. |
| **`refresh_hz`** | `60` / `120` | `60` | Match your console's output refresh rate. Keep `60` on normal TVs; set **`120`** only if you force 120 Hz via [ps-patch-system](https://github.com/illusionyy/ps-patch-system) *(using `120` on a 60 Hz TV will cut FPS in half)*. |
| **`output_res`** | `0` / `1440` / `1080` | `0` | *(Experimental)* Changes the game's output resolution (`0` = 4K, `1440` = 1440p, `1080` = 1080p) instead of upscaling to 4K. Useful for 1080p/1440p monitors or extra FPS. |
| **`motion_blur`** | `0` / `1` | `1` | `1` = game default; `0` = disables motion blur for a noticeably sharper image in motion (especially at `res_pct=50`) plus a small GPU savings. |

### `res_pct` Reference Values (4K Output)

> **Memory limit for high `res_pct`:** With default settings, going above **`67%`** on base PS5 or **`66%`** on PS5 Pro will crash the game on startup due to its built-in memory limit. To go higher (e.g. `70`–`100`), enable **`pro_highres=1`** (PS5 Pro, 4K output) or **`base_highres=1`** *(experimental, base PS5)* and load the payload **before** starting the game.

| `res_pct` | Internal Resolution (at 4K `output_res=0`) | Notes |
| --- | --- | --- |
| `70`–`100` | `2688×1512` – `3840×2160` | Requires **`pro_highres=1`** on PS5 Pro (4K output). Tested clean up to `90%` and native 4K. |
| `67` | `2560×1440` | **Stock PS5 Fat/Slim** (`TAA2U`). Crashes on **PS5 Pro** unless `pro_highres=1` is enabled (use `0` for stock PS5 Pro `2560×1440`). |
| `60` | `2304×1296` | Sharper middle ground (`66%` is the upper limit on **PS5 Pro** without `pro_highres`). |
| `55` | `2112×1188` | Balanced middle ground between 1080p and 1440p (works on PS5 & PS5 Pro). |
| **`50`** | **`1920×1080`** | **Default** — ~44% fewer pixels than base PS5 stock, keeps all other graphics settings 100% stock. |
| `25`–`49` | `960×540` – `1884×1060` | **Experimental** (e.g. `33` ≈ `720p`; softer image / more upscaling shimmering). |
| `0` | Stock (`2560×1440` PS5 / `2560×1440` Pro) | Leaves internal resolution completely untouched. |

---

## 2. PS5 Pro Options

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| **`pro_highres`** | `0` / `1` | `0` | *(PS5 Pro, 4K output)* Expands PS5 Pro video memory at startup so you can use `res_pct` above `66` (up to `90` / native 4K) with full RT without crashing. |
| **`pro_rt_half`** | `0` / `1` | `0` | *(Experimental, PS5 Pro)* Lowers PS5 Pro's RT / Global Illumination quality tier to base PS5 level across all 4 readers for higher FPS while keeping PSSR and normal ocean water. |
| **`pro_taa2u`** | `0` / `1` | `0` | *(Experimental, PS5 Pro)* Uses standard TAA2U upscaling instead of PSSR on PS5 Pro — can improve GPU FPS or reduce shimmering on trees/foliage. |

---

## 3. Performance & Graphics Tweaks *(Experimental)*

All options below default to **`0` (stock)** and require loading the payload **before** starting GTAVI:

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| **`low_shadows`** | `0`–`3` | `0` | `1` = lowers directional shadow map to `1280` (vs `2048`/`4096`); `2` = + fast soft-shadow filter (`12` → `2`); `3` = + disables dynamic & LOD ray-traced shadows for maximum GPU savings (keeps ocean water & GI intact). |
| **`low_reflections`** | `0` / `1` | `0` | Lowers environment reflection map resolution from `256` to `128` (4× fewer pixels). |
| **`low_volumetric`** | `0` / `1` | `0` | Lowers volumetric clouds and fog quality (raymarch + lighting) to low tier for extra GPU FPS. |
| **`lod_scale`** | `0`, or `50`–`100` | `0` | Draw distance percentage (`0` = stock `100%`, e.g. `75` = `75%`). Reduces **both CPU and GPU** load in dense city areas. |
| **`low_bvh`** | `0` / `1` | `0` | Lowers the ray-tracing BVH geometry tree quality to reduce **CPU and GPU** RT overhead. |
| **`base_highres`** | `0` / `1` | `0` | *(Base PS5)* Expands base PS5 video memory at startup so you can run higher `res_pct` ratios (e.g. `output_res=1440` + `res_pct=75–80`; `75` is the practical sweet spot, `80` dips below 30 FPS, `90–100` crashes). |

---

## 4. Stability & Behavior

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| **`close_fix`** | `0` / `1` | `1` | Prevents the **`CE-108255-1`** error popup when closing the game (`1` = enabled by default). |
| **`daemon`** | `0` / `1` | `0` | `0` = runs once and closes after patching; `1` = stays in the background, reloads `fps.txt`, and automatically re-patches if you restart the game. |

---

Logs are saved to `/data/gtavi-unlockfps/gtavi-unlockfps.log`.

> **Not sure what to pick?** See **[SETTINGS.md](SETTINGS.md)** for ready-to-use presets for **PS5 Fat/Slim** & **PS5 Pro** and how to combine `output_res` with `res_pct`.

# GTA 6 60 FPS Runtime Patcher (PS5 Payload)

Jednorazowy payload `.elf` dla zjailbreakowanego PS5, wzorowany na [armanate/rdr2-60fps-runtime-patcher](https://github.com/armanate/rdr2-60fps-runtime-patcher).

## Jak działa
1. Uruchamiasz payload `GTA6 60FPS_v1.0.elf` przez Payload Managera (PLDMGR / elfldr).
2. Payload wyświetla powiadomienie i **czeka do 10 minut** na uruchomienie GTA 6 (`eboot.bin`).
3. Po wykryciu procesu gry pobiera jego bazę ASLR w pamięci i weryfikuje **64 bajty kontekstu** wokół obu miejsc ustawiających limit FPS (`0x3f73e51` oraz `0x40d2be1`) — dzięki temu nigdy nie nadpisze złej wersji gry ani innego tytułu.
4. Nadpisuje oba miejsca w RAM-ie (z obsługą DMAP / `ptwalk` dla FW >= 8.40 oraz `mdbg_copyin` dla starszych FW), a także aktualizuje na żywo `g_TargetFrameTimeUs` (`0xe3be220`) i `g_RequestedFrameLock` (`0xe7bbb08`), po czym kończy działanie.

## Konfiguracja FPS (60 / 40 / inne)
Przy pierwszym uruchomieniu payload tworzy plik `/data/gta6-60fps/fps.txt` z zawartością:
```ini
fps=60
```
Możesz go edytować przez FTP (np. zmienić na `fps=40` dla stabilniejszego klatkażu na TV 120 Hz / VRR) — bez ponownej kompilacji. Logi zapisują się w `/data/gta6-60fps/gta6-60fps.log`.

## Kompilacja do `.elf`
### Opcja 1: Docker (macOS / Linux / Windows)
```sh
chmod +x build.sh
./build.sh
```
Gotowy plik pojawi się w `src/GTA6 60FPS_v1.0.elf`.

### Opcja 2: GitHub Actions (bez instalowania niczego lokalnie)
Wrzuć folder jako repozytorium na GitHub — workflow `.github/workflows/build.yml` automatycznie skompiluje `GTA6 60FPS_v1.0.elf` i udostępni go do pobrania w zakładce **Actions -> Artifacts**.

# Frame Counter Free

Free Geode mod for Geometry Dash 2.2081: frame/FPS HUD, per-click frame timing, colour rows, sounds, CSV export.
Targets: **Android 64-bit (arm64-v8a)**, Android 32-bit and Windows.

## Build the .geode (no PC setup needed)
1. Create a GitHub repo and upload this folder (keep the `.github` folder).
2. Open the **Actions** tab, run **Build** (or push to `main`).
3. When it finishes, download the artifact **Frame-Counter-Free** and unzip it.
   You get one `.geode` that contains the Android64, Android32 and Windows binaries.
4. Android: copy the `.geode` into `Android/media/com.geode.launcher/game/geode/mods/`
   (or open it with the Geode launcher). Windows: `Geometry Dash/geode/mods/`.

## Local build (optional)
Install the Geode CLI + SDK, then: `geode build -p android64`.

## Using v2
1. Complete the level once: the run is saved as a macro automatically.
2. Pause, **Scan windows** (runs many fast trials; lower *Scan speed* if it misbehaves).
3. Pause, **Replay**: each click shows its window in frames.

## Notes
- Timing is read from the game's level time, so it follows practice checkpoints and any TPS changes.
- Files outside `src/` were made from scratch; nothing is taken from other mods.

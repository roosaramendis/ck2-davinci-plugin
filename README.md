# ck2-davinci-plugin: CorridorKey 2.0 for DaVinci Resolve (OpenFX)

An OpenFX effect that keys a clip with CorridorKey 2.0 from inside Resolve.
It works in **Resolve 21+ Free and Studio** on **Linux, Windows and macOS**.

The plugin doesn't contain the AI model. When you press **Key clip** it runs the
CorridorKey 2.0 engine you already installed (for example with ck2-gui, though
the plugin doesn't need that app): NVIDIA (CUDA) on
Linux and Windows, or the Apple GPU on Macs. It then caches the result, so
playback and scrubbing are instant.

## The nodes

| Node | What it does |
|---|---|
| **CorridorKey 2.0** | Keys the clip (in the background) and outputs one pass: cutout, matte, foreground or checker. |
| **CorridorKey Pass** | Outputs one pass of a key, found by its **Key name**. Use several to get several passes at once; nothing is keyed twice. |
| **CorridorKey Outputs** (Fusion macro) | One node with **Cutout / Alpha / Foreground / Checker** outputs, built from four Pass nodes. Fusion page only. |
| **CorridorKey Roto** | Click-to-track roto with SAM 2.1: click / drag rectangles on whatever you want to keep (people, props, areas) in a picker window, it tracks them through the clip and outputs a **hint matte** for CorridorKey's Hint input, or the **source with everything else filled with screen colour**. |

An OpenFX node can only have one output, so several passes come from Pass nodes
(or the macro, which bundles them).

## Install

### Easiest: with the CorridorKey app (ck2-gui)

Open **Setup → DCC plugins → CorridorKey for DaVinci Resolve → Install**. The
app downloads the latest release, installs it for your system (Linux, Windows or
macOS), connects it to the engine it installed, and asks once for your password
(Windows: "allow changes"). Restart Resolve afterwards.

### By hand

Download `ck2-davinci-plugin-<version>.zip` from the
[releases](https://github.com/roosaramendis/ck2-davinci-plugin/releases) and
unpack it. It contains ready-made plugins for all three systems, so nothing
needs compiling. You also need a working CorridorKey 2.0 engine folder (the one
with `infer.py` and `venv`); on a Mac it must be the official engine on the
Apple GPU, not the MLX port.

- **Linux (any distro) and macOS:** `bash install.sh --engine /path/to/CorridorKey2.0`
- **Windows** (PowerShell):
  `powershell -ExecutionPolicy Bypass -File install.ps1 -Engine C:\path\to\CorridorKey2.0`

| Option | install.sh | install.ps1 |
|---|---|---|
| choose the engine | `--engine DIR` | `-Engine DIR` |
| engine moved: only rewrite the config | `--connect-only` | `-ConnectOnly` |
| skip the SAM 2 download | `--no-sam` | `-NoSam` |
| remove the plugin | `--uninstall` | `-Uninstall` |
| compile instead of using the ready-made plugin | `--build` | – |

Compiling (`--build`, or a source checkout without `prebuilt/`) needs `cmake`,
`git` and a C++ compiler; on a Mac, `xcode-select --install` and
`brew install cmake`.

| OS      | Plugin folder                              | Config file                                        |
|---------|--------------------------------------------|----------------------------------------------------|
| Linux   | `/usr/OFX/Plugins`                         | `~/.config/ck2-davinci-plugin/ofx.conf`            |
| macOS   | `/Library/OFX/Plugins`                     | `~/Library/Application Support/ck2-davinci-plugin/ofx.conf` |
| Windows | `C:\Program Files\Common Files\OFX\Plugins`| `%APPDATA%\ck2-davinci-plugin\ofx.conf`             |

### Problems?

Press **Report a bug** on the CorridorKey node (or in the ck2-gui app). It opens a
GitHub issue with the latest log lines filled in, plus the log folder.


## Use

1. Find **Effects → OpenFX → Keying → CorridorKey 2.0**. Use it in one of these
   places:
   - **Edit / Cut page:** drop it on a green-screen clip in the timeline.
   - **Color page:** drop it on a node. To use the alpha, right-click the node
     tree, choose **Add Alpha Output**, and wire the node's alpha into it.
   - **Fusion page:** add it as a node. The Fusion version has a second
     **Hint** input: plug in a roto or garbage matte (white = keep) to tell
     CorridorKey what to keep.
2. Press **Key clip** in the Inspector. It returns at once, and Resolve never
   freezes.
3. **Play the clip once from start to end.** The viewer shows
   `CAPTURING 120/344` while each frame is saved. Playback needs full resolution
   (proxy off). If Resolve drops frames, play it again; it only picks up the
   missing ones. Not keying the whole clip? Press **Key captured frames now** to
   key the frames saved so far.
4. When every frame is saved, keying starts by itself in the background. The
   viewer shows `KEYING 45%`. You can keep working, or even close Resolve; it
   finishes anyway. The Status line and the viewer update by themselves while
   it runs, and you get a desktop notification when it's done (turn that off
   with **Notify when done**). **Stop keying** cancels it.
5. When it's done, the key appears in place of the source by itself. Pick what the node
   outputs:
   - **Cutout:** despilled colour plus alpha. This is the normal choice.
   - **Matte:** the alpha as a black-and-white image.
   - **Foreground:** the raw colour plus alpha, without despill.
   - **Checker preview:** the key over a checkerboard.
   - **Source:** bypass.

If a frame still shows the `KEYING` banner after keying is done, press
**Refresh now** (see "If the status doesn't update by itself" below). `KEY MISSING` means the cached key is gone, for example after
moving the project to another computer or clearing the cache. Key again.

### Several passes at once

The CorridorKey 2.0 node shows a **Key name** (for example `key-3f2a`). You can
rename it.
- **CorridorKey Pass** (any page): type that name and pick a **Pass**. Add as
  many as you need.
- **Fusion page:** *Add Tool → Macros → CorridorKey Outputs*, then type the key
  name once. The node has Cutout, Alpha, Foreground and Checker outputs.

### Keep only what you want: CorridorKey Roto

Use it to keep anything in the shot (people, props, a car, a whole area) and
drop the rest.

1. Wire it up. The clip goes into **both** nodes: MediaIn → **CorridorKey Roto**,
   and MediaIn → **CorridorKey 2.0**. The Roto output goes into CorridorKey's
   **Hint** input (the second, green triangle).
2. Go to a frame where what you want to keep is clearly visible and press
   **Pick character…** (it works for any object, not only characters). A
   window opens on that frame. The frame is taken when the viewer draws it, so
   the Roto node (or the CorridorKey node after it) must be in the viewer:
   - **click** on something to keep it,
   - **right-click** on things to remove (a light stand, a crew member, a rig),
   - **drag** a rectangle around an object or area; each rectangle is its own
     object, so add as many as you need,
   - wheel zooms, middle-drag pans, Ctrl+Z undoes.

   After each change the mask is shown in red after a moment; the first time
   takes longer while SAM 2 loads. Press **Use this selection** when it's right.
3. Press **Track clip**, then **play the clip once**. It only needs the frames
   that actually play (a clip cut shorter in the edit is fine). If it ever waits
   for more, let playback loop back to the start, or press **Track captured
   frames now**. Tracking then runs in the
   background (`ROTO: TRACKING 40%`). If you pick again later, Track clip
   doesn't need another playback.
4. **Output** (default **Hint matte**). **Overlay** shows the mask over the
   picture to check it. **Garbage-filled source** fills everything outside the
   picked objects with screen colour; use it on the Edit/Color page, where there's no
   Hint input, by putting CorridorKey 2.0 *after* the Roto node.
5. Wait for **✓ Tracked** in the Roto status (it updates by itself, and you
   get a desktop notification), *then* press Key clip on the CorridorKey node.

With a Hint connected, **Hint is: First frame only** gives CorridorKey just the
first frame's matte and lets it carry it forward.

### Settings (used by the next Key clip)

- **Input is:** match your timeline. Choose display-referred for normal
  Rec.709 timelines, or scene-linear for ACES / linear workflows. The output
  comes back in the same space.
- **Despill:** how much green spill is removed from the cutout.
- **Hint trust:** how strictly the Hint matte is followed (Fusion only).
- **Temporal window:** Auto picks from free GPU memory. Smaller windows use
  less memory. **Low-VRAM mode** trades speed for memory.

### Getting the result out of Resolve

- **ProRes with alpha:** set the output to Cutout, then on the Deliver page
  choose QuickTime → ProRes 4444 and tick **Export Alpha**.
- **EXR for Fusion, Nuke or After Effects:** choose an **EXR export folder** and
  press **Export EXRs**. It writes `corridorkey_alpha.######.exr` (matte),
  `corridorkey_fg.######.exr` and `corridorkey_cutout.######.exr`. They're
  scene-linear half float, and the frame numbers match the clip.

## Free vs Studio

The plugin detects the edition by itself and shows it under **Edition**
(Resolve reports itself as "DaVinci Resolve Lite" for Free). Everything
currently works in both. If a future feature needs Studio, its button will say
so in Free instead of failing.

### If the status doesn't update by itself

Each node checks its background job every 2 seconds while one runs, and asks
Resolve to redraw when something changes. OpenFX doesn't guarantee that a host
accepts this, so:

- **Refresh now** on every node updates the Status line and the viewer by hand.
- If Resolve refuses an automatic update, that node switches to manual updates
  by itself (the Status line then says "press 'Refresh now' to update", and
  `plugin.log` says why).
- To switch automatic updates off everywhere, add a line `auto_refresh=0` to
  the config file (see the table under Install) and restart Resolve. The
  desktop notifications still work.

## Where things are kept

The Roto node runs SAM 2 in a background helper (`roto_daemon.py`, in the engine's
Python). It starts when needed and quits after 5 idle minutes to free GPU
memory. Its data is in the `roto/` folder of the cache.

Keys are cached as EXRs in `~/.cache/corridorkey-ofx` on Linux,
`~/Library/Caches/corridorkey-ofx` on macOS, and `%LOCALAPPDATA%\corridorkey-ofx`
on Windows. Each node has its own folder. Use **Forget key** to delete one, or
delete the whole folder to free space. Captured frames are uncompressed and are
deleted as soon as keying finishes. If a key fails, the status shows the path
of `engine.log`, which holds the engine's full output. `plugin.log` in the same
folder records every button press and job change, which helps when something
seems to do nothing.

## Building by hand

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The result is `build/CorridorKey2.ofx.bundle`. The first build downloads the
OpenFX SDK and tinyexr. The `.github/workflows/build.yml` workflow builds
Linux, Windows and macOS (universal) zips on GitHub.

## Licence

CorridorKey is © Corridor Digital under the Corridor Key Licence 2.0: free to
use, and derivatives must also be free and keep the CorridorKey name. This
plugin only runs the engine you installed; it doesn't include the model or its
weights.

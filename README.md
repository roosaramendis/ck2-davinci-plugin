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
| **CorridorKey Hint** | A quick helper matte for keying: pick what you want to keep (points, boxes, lassos), SAM 2.1 tracks it through the clip, and it outputs a **hint matte** for CorridorKey's Hint input, or the **source with everything else filled with screen colour**. Not meant as final roto. |
| **CorridorKey Roto** | Real roto without a green screen: the same picking, then SAM tracks it and **MatAnyone 2** (optional) makes soft edges for hair and motion blur. Outputs the **cutout** or the **matte**, and **editable shapes for Fusion**. Needs the Roto models (see Install). |

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
| set up the Roto node's models | `--roto` | `-Roto` |
| …with MatAnyone 2 soft edges (asks you to accept its licence) | `--matanyone` | `-MatAnyone` |
| Hugging Face token for SAM 3 | `--hf-token TOKEN` | `-HfToken TOKEN` |
| remove the plugin | `--uninstall` | `-Uninstall` |
| compile instead of using the ready-made plugin | `--build` | – |

**Roto models.** The Roto node runs in its own Python environment
(`roto-env` in the engine folder) because SAM 3 needs a newer `transformers`
than the engine allows. `--roto` creates it (a few GB, with PyTorch for your
GPU). `--matanyone` adds MatAnyone 2 for soft edges: it is made by S-Lab (NTU)
under the **NTU S-Lab License 1.0, free for non-commercial use only**; the
installer shows the licence and asks before downloading. **SAM 3** is gated by
Meta: request access on [huggingface.co/facebook/sam3](https://huggingface.co/facebook/sam3)
with a free account, create a read token, and pass it with `--hf-token` (or add
`hf_token=...` to the config file). SAM 2.1 needs none of this.

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

### Publishing a release (maintainer)

Push a tag like `v1.0.0`. GitHub Actions builds the plugin for Linux, Windows and
macOS and publishes `ck2-davinci-plugin-v1.0.0.zip`, which ck2-gui then offers
as an update.

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
3. **Wait while it reads the clip.** You don't need to play anything: the node
   reads the clip's frames by itself, a batch at a time, and the viewer shows
   `READING FRAMES 120/344`. This node (or one after it) must be in the viewer,
   at full resolution (proxy off). Resolve stays usable meanwhile; a slow node
   before it (like a denoise) makes this take longer. Only the frames that play
   in the edit are read. Not keying the whole clip? Press **Key captured frames
   now** to key the frames saved so far.

   If the banner says `CAPTURING … - PLAY THE CLIP ONCE` instead, Resolve
   didn't hand out the frames by itself: play the clip once from start to end
   (if it drops frames, play it again; only the missing ones are picked up).
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

### Help the key: CorridorKey Hint

A quick, tracked helper matte that tells CorridorKey what to keep (anything:
people, props, a car, an area). It isn't final roto; for that, see
**CorridorKey Roto** below.

1. Wire it up. The clip goes into **both** nodes: MediaIn → **CorridorKey Hint**,
   and MediaIn → **CorridorKey 2.0**. The Hint output goes into CorridorKey's
   **Hint** input (the second, green triangle).
2. Go to a frame where what you want to keep is clearly visible and press
   **Pick…**. A window opens on that frame. The frame is taken when the viewer
   draws it, so the Hint node (or the CorridorKey node after it) must be in the
   viewer. Choose a tool at the top (or press its key):
   - **Point (P):** click on something to keep it, right-click on things to
     remove (a light stand, a crew member, a rig);
   - **Box (B):** drag a rectangle around an object or area; each rectangle is
     its own object, so add as many as you need;
   - **Lasso (L):** draw around an area to add it, **Alt-drag or right-drag**
     to cut an area away. With **Lasso: exact edge** ticked, the lasso's own
     outline is used as it is; untick it to let SAM find the real edge inside
     the lasso;
   - wheel zooms, middle-drag pans, Ctrl+Z undoes.

   After each change the mask is shown in red after a moment; the first time
   takes longer while SAM loads. Press **Use this selection** when it's right.
3. Press **Track clip**. The node reads the clip's frames by itself (keep it, or
   the CorridorKey node after it, in the viewer), then tracking runs in the
   background (`HINT: TRACKING 40%`). It only needs the frames that actually
   play (a clip cut shorter in the edit is fine). If the banner asks you to
   play the clip instead, play it once; if it waits for more, let playback loop
   back to the start, or press **Track captured frames now**. If you pick again
   later, Track clip doesn't need to read the frames again.
4. **Output** (default **Hint matte**). **Overlay** shows the mask over the
   picture to check it. **Garbage-filled source** fills everything outside the
   picked objects with screen colour; use it on the Edit/Color page, where there's no
   Hint input, by putting CorridorKey 2.0 *after* the Hint node.
5. Wait for **✓ Tracked** in the Hint status (it updates by itself, and you
   get a desktop notification), *then* press Key clip on the CorridorKey node.

With a Hint connected, **Hint is: First frame only** gives CorridorKey just the
first frame's matte and lets it carry it forward.

Projects made with the old "CorridorKey Roto" node open with the Hint node:
it's the same node under its new name.

### Roto without a green screen: CorridorKey Roto

Needs the Roto models (`--roto`, or ck2-gui: **Setup → Roto models**).

1. MediaIn → **CorridorKey Roto**. Pick and track exactly as with the Hint node
   (**Pick…**, then **Track clip**).
2. **Edges** (used by the next Track clip):
   - **MatAnyone 2:** soft edges with hair detail and motion blur. Free for
     non-commercial use only (NTU S-Lab License 1.0). If it isn't installed,
     the node uses SAM's edges and says so.
   - **SAM only:** solid edges, no extra licence.
3. **Model:** SAM 2.1 (Large is best) or **SAM 3** (newest; needs a Hugging Face
   token, see Install).
4. **Output:** **Cutout** (the clip with the matte as alpha, straight colour),
   **Matte** (black and white), **Overlay** (to check it) or **Source**.
5. **Save shapes for Fusion** writes the tracked objects as keyframed Polygon
   masks (`CorridorKey Roto <id>.setting`) to the **Shapes folder** and opens
   it. Drag the file into Fusion's node editor to fix the roto by hand. Shapes
   have solid edges: the hair stays in the matte, so a common setup is the
   node's matte for the hair, and the shapes to clean up the body.

### Settings (used by the next Key clip)

- **Input is:** match your timeline. Choose display-referred for normal
  Rec.709 timelines, or scene-linear for ACES / linear workflows. The output
  comes back in the same space.
- **Despill:** how much green spill is removed from the cutout.
- **Hint trust:** how strictly the Hint matte is followed (Fusion only).
- **Temporal window:** Auto picks from free GPU memory. Smaller windows use
  less memory. **Low-VRAM mode** trades speed for memory.
- **Use shared GPU memory** (on by default): when the GPU's own memory is
  full, keying keeps going in shared GPU memory (system RAM). That's slower but
  it doesn't stop. Turn it off to keep everything in the GPU's own memory: if
  that runs out, keying tries once more by itself with Low-VRAM mode and a
  smaller window.

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

The Hint and Roto nodes run SAM in a background helper (`roto_daemon.py`: the
Hint node in the engine's Python, the Roto node in `roto-env`). It starts when
needed and quits after 5 idle minutes to free GPU memory. Their data is in the
`roto/` (Hint) and `rotomatte/` (Roto) folders of the cache; saved shapes go to
`shapes/` unless you choose another folder.

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

SAM 2.1 and SAM 3 are Meta's (SAM 2.1: Apache 2.0; SAM 3: the SAM Licence,
gated on Hugging Face). MatAnyone 2 is S-Lab's (NTU S-Lab License 1.0,
non-commercial). None of them is bundled: they're downloaded when you install
the Roto models, and MatAnyone 2 only after you accept its licence.

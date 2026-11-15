
# Snapmaker Orca with displacement maps

This is a fork of [Snapmaker Orca](https://github.com/Snapmaker/OrcaSlicer). It adds **displacement maps**: a real surface texture
from a pattern texture, the UV and normal maps that come with a model, or a texture generated from a prompt.
The texture is applied while slicing. The mesh is never subdivided, so a 640-triangle cylinder can print a 0.4 mm knurl and the
project stays a few kilobytes.

The work is on the branch [`displacement-maps`](https://github.com/dlvkz/OrcaSlicer/tree/displacement-maps),
which starts from Snapmaker Orca `main` (fd6e55e). This fork is not made or supported by Snapmaker. It has been built and tested on
Windows only.

![Pattern textures, a "tree bark" generated texture, low-poly models with UV map displacement and a game asset's normal map, rendered from the sliced G-code](doc/images/displacement/cover.jpg)

## Live preview

![Dragging the texture depth and offset sliders: the 3D view follows frame by frame (screen recording, real time)](doc/images/displacement/live_preview_gargoyle.webp)

The 3D view displaces the model on the GPU while you drag, before slicing. Above: a 3,000-triangle gargoyle with its UV height map.

## What it adds

**In the settings** (Process → Others → **Surface texture**, set **Texture type** to **Displacement map**; or the **Displacement map**
panel at the top left of the 3D view):

- **Patterns**, no image needed: knurl, hexagons, bricks, ribs, waves, bumps, stone and noise. All tile seamlessly.
- **Height maps**: white pushes the surface out, black pulls it in, up to 10 mm deep. PNG (8 or 16 bit, grey or colour) and JPEG
  files are read.
- **Generate from text**: describe a surface ("tree bark", "cobblestone pavement") and pick one of the seamless height maps it
  makes. It runs locally with no upload, on your graphics card (Vulkan) or else on the processor. The model is an optional add-on
  (about 0.93 GB) that the app downloads the first time you use it.
- **Projections**: box (triplanar), cylindrical, spherical, planar (top, front, side), UV and **on the surface**.
  - **UV** uses the model's own texture coordinates. When the part was loaded from a GLB, glTF or OBJ file, that file is used
    automatically.
  - **On the surface** lays the pattern out on the mesh itself instead of projecting a flat image. Honeycomb, bumps, stone and noise
    are built as cells on the surface. Images and stripes are laid down in small overlapping patches along the surface.
- **Tile size, rotation, invert and offset.** The offset works like Blender's midlevel: 0 cuts the texture into the surface, 1 raises
  it out of the surface, and 0.5 goes both ways.
- **Whole surface or per layer.** By default the texture goes on the shape itself, so tops, slopes and overhang undersides get it
  too. "Per layer only" just moves each layer's outline sideways. That is faster, about as fast as ordinary fuzzy skin, and leaves
  tops flat.
- **Per object.** Each object can have its own texture. Select it, and the panel edits that object only.
- **Normal map to relief.** When a GLB, glTF or OBJ model with a normal map is loaded, the slicer offers to turn the normal map into
  a 16-bit height map and print it as relief through the model's UVs. This is also on the right-click menu and on the command line
  (`--relief-from-normal-map`).

**In the 3D view:**

- **Live preview before slicing**, for every projection including UV. The model is displaced on the GPU by tessellation (OpenGL
  4.0), so no high-polygon copy is made and sliders update as you drag. On older OpenGL the texture is only shaded.
- The **Displacement map** panel is always visible in this fork, out of convenience.

**Slicing fixes and helpers that came out of this work:**

- **Supports ignore the texture** (optional): tree supports are planned from the untextured shape, so they don't chase every bump.
  Tree-support overhang detection is also faster.
- **Open surface thickness** (Quality → Precision): prints zero-thickness sheets and paper-thin parts (capes, flags, leaves on
  AI-generated models) as bands of the given width instead of dropping them.
- **Deterministic slicing of broken meshes**: the same project now gives the same layers every time. Before, layers of meshes with
  holes or non-manifold edges could differ from run to run.

## Quick start

1. Select an object. In the **Displacement map** panel, press **Use a displacement map**.
2. Pick a **Pattern**, or **Image** and a file, or **Generate from text…**.
3. Pick a **Projection** and a **Tile size**, then set the **Texture depth**. 0.3 to 0.6 mm reads well with a 0.4 mm nozzle.
4. Slice.

The full reference is in the [Surface texture / fuzzy skin page](doc/print_settings/others/others_settings_fuzzy_skin.md#displacement-map).

## Examples

All renders below are made from the sliced G-code: every extrusion is drawn as a bead at its real width and height, so they show
what the printer lays down, not the 3D model.

### A low-poly model and its height map

![A 3,000-triangle gargoyle alone, with its height map through UV, and the 48k-triangle original](doc/images/displacement/lowpoly_gargoyle.jpg)

A 48k-triangle model was decimated to 3,000 triangles, and what the decimation removed was baked into a 16-bit height map
(2048 × 2048 px, about 1 mm deep). Left: the low-poly mesh alone. Middle: the same mesh with the map through its UVs. Right: the
original, for reference.

![The printed gargoyles: the mesh alone and with its height map](doc/images/displacement/photo_gargoyle.jpg)

### Game assets: relief from the normal map

![Poly Haven's gothic statue: the asset, sliced from the mesh alone, and with its normal map turned into relief](doc/images/displacement/lowpoly_statue.jpg)

Game assets keep their fine detail in texture maps, not in the mesh. Poly Haven's gothic statue (CC0, 27,739 triangles) has the
pose, robes and chain in its mesh, but the face under the veil, the crown and the edges of the chain links only in its normal map.
The slicer integrates the normal map into a height map and prints it through the model's own UVs. Nothing is modelled, baked or
exported by hand.

![Poly Haven's carved wooden plate: the carving exists only in the normal map](doc/images/displacement/lowpoly_plate.jpg)

| Model | Mesh only | Mesh + its normal map |
|---|---|---|
| Carved wooden plate, 2,112 triangles, 122 mm (relief 0.9 mm) | 8.0 s, 5 h 6 min | 30.7 s, 6 h 27 min |
| Gothic statue, 27,739 triangles, 150 mm (relief 2.0 mm) | 18.5 s, 7 h 51 min | 49.2 s, 10 h 36 min |

Slice time (one run, including the normal map integration) and estimated print time on the Snapmaker U1.

### Local displacement texture generation using a lightweight SD-Turbo model (893 MB)

![Generate from text: four seamless height maps for "tree bark"](doc/images/displacement/gui_text_to_height.jpg)

![A plain cylinder and the same cylinder with the generated bark](doc/images/displacement/text_bark_grip.jpg)

On an RTX 3080 Ti the first texture of a session takes about 10 s while the card is prepared, and four more take about 1 s. On the
processor, each takes about 5 to 10 s.

## Test prints

![All 8 built-in patterns and an untextured reference, printed](doc/images/displacement/photo_plate1.jpg)

![The same PNG as a displacement map and baked into a 1.26 M-triangle mesh](doc/images/displacement/photo_map_vs_baked_closeup.jpg)

Bambu PLA Matte, 0.4 mm nozzle, 0.16 mm layers. The two cylinders' walls match row for row; the one visible difference is the top,
which only the map textures.

## Measured

Sliced for the Snapmaker U1 with the same profile (slice time is the median of 3 runs):

| Part | Method | Project size | Slice time | Est. print |
|---|---|---|---|---|
| Ø40 × 40 mm cylinder, PNG texture | 512-triangle mesh + map, whole surface | **0.022 MB** | 1.78 s | 57 min |
| | same, per layer only | **0.022 MB** | **1.53 s** | 52 min |
| | the texture baked into a 1.26 M-triangle mesh | 15.1 MB | 4.95 s | 52 min |
| Honeycomb vase, on the surface | whole surface | 0.19 MB | 6.0 s | 3 h 24 min |
| | per layer only | 0.19 MB | **4.5 s** | 3 h 2 min |
| Stone planter, on the surface | whole surface | 0.007 MB | 5.1 s | 2 h 11 min |
| | per layer only | 0.007 MB | **3.2 s** | 1 h 57 min |

The printed outer walls of the map and the baked mesh match to a median of 4.3 µm (95% of 87,480 sampled points within 26 µm).

A map only saves space and time against heavy meshes. A model that is already light (tens of thousands of triangles) plus a 2048 ×
2048 16-bit map is bigger and slower to slice than the model on its own: the 48k-triangle gargoyle is 0.65 MB and slices in 9.5 s,
its 3k mesh with the map 4.6 MB and 14.5 s. Maps pay off against meshes with millions of triangles, and when the detail only exists
as a texture, as with game assets.

## Tests

26 test cases cover the feature and run by default; a 27th runs the whole text-to-height pipeline and needs the model files.

| File | Tag | Covers |
|---|---|---|
| `tests/fff_print/test_fuzzy_skin_map.cpp` | `[FuzzySkin]` | white out and black in, offset, every projection with rotation and invert, seamless patterns, cells and charts laid on the surface, whole surface and per layer displacement, depth past 1 mm |
| `tests/fff_print/test_texture_relief.cpp` | `[TextureRelief]` | textured OBJ loading, normal map to height map (also on a mesh whose facets were reversed on loading), refusing compressed glTF |
| `tests/libslic3r/test_open_surface.cpp` | `[OpenSurface]` | open and paper-thin surfaces, the same layers every time on a broken mesh |
| `tests/libslic3r/test_text_to_height.cpp` | `[TextToHeight]` | normals to height, normalisation, DeepBump against a reference; `[.]` for the whole pipeline |

```console
build\tests\fff_print\Release\fff_print_tests.exe "[FuzzySkin],[TextureRelief]" --order rand
build\tests\libslic3r\Release\libslic3r_tests.exe "[OpenSurface],[TextToHeight]" --order rand
```

All 138 fff_print test cases pass. Of the 362 libslic3r test cases, 357 pass; the other 5 are upstream tests marked as skipped or
expected to fail.

## Known limits

- A flat image can't wrap a closed shape without a seam or stretching somewhere. **On the surface** avoids that, but where its
  patches meet you can see a soft band, and on spheres some doubled lines.
- With UV maps, the live preview can show thin spikes along the texture's seams at large depths (around 2 mm on the gargoyle). The
  slicer itself is not affected.
- glTF files with compressed geometry (EXT_meshopt, Draco) are refused with a clear message. `scripts/glb_unmeshopt.py` converts
  meshopt files.
- Fine, deep textures add print time.
- Tested on Windows only.

## Licence and credits

AGPL-3.0, like Snapmaker Orca and OrcaSlicer. Generate from text is powered by Stability AI: SD-Turbo is used under the Stability AI
Community License, with a copy and a NOTICE in the model folder. Normal maps from colour come from DeepBump by Hugo Tini (GPL-3.0),
and fast decoding from TAESD by Ollin Boer Bohan (MIT).

The carved wooden plate and the gothic statue in the examples are from [Poly Haven](https://polyhaven.com) (CC0). The gargoyle and
the chest were generated with Tripo.

The original Snapmaker Orca README follows.

---

<h1> <p "font-size:200px;"> Snapmaker Orca</p> </h1>

[![Build all](https://github.com/Snapmaker/OrcaSlicer/actions/workflows/build_all.yml/badge.svg?branch=main)](https://github.com/Snapmaker/OrcaSlicer/actions/workflows/build_all.yml)
<br>Snapmaker Orca is an open source slicer for FDM printers based on OrcaSlicer.
 


# Download

### Stable Release
📥 **[Download the Latest Stable Release](https://github.com/Snapmaker/OrcaSlicer/releases/latest)**  
Visit our GitHub Releases page for the latest stable version of Snapmaker Slicer, recommended for most users.

# How to install
**Windows**: 
1.  Download the installer for your preferred version from the [releases page](https://github.com/Snapmaker/OrcaSlicer/releases).
    - *For convenience there is also a portable build available.*
    - *If you have troubles to run the build, you might need to install following runtimes:*
      - [MicrosoftEdgeWebView2RuntimeInstallerX64](https://github.com/SoftFever/OrcaSlicer/releases/download/v1.0.10-sf2/MicrosoftEdgeWebView2RuntimeInstallerX64.exe)
          - [Details of this runtime](https://aka.ms/webview2)
          - [Alternative Download Link Hosted by Microsoft](https://go.microsoft.com/fwlink/p/?LinkId=2124703)
      - [vcredist2019_x64](https://github.com/SoftFever/OrcaSlicer/releases/download/v1.0.10-sf2/vcredist2019_x64.exe)
          -  [Alternative Download Link Hosted by Microsoft](https://aka.ms/vs/17/release/vc_redist.x64.exe)
          -  This file may already be available on your computer if you've installed visual studio.  Check the following location: `%VCINSTALLDIR%Redist\MSVC\v142`

**Mac**:
1. Download the DMG for your computer: `arm64` version for Apple Silicon and `x86_64` for Intel CPU.  
2. Drag Snapmaker_Orca.app to Application folder. 
3. *If you want to run a build from a PR, you also need to follow the instructions below:*  
    <details quarantine>
    - Option 1 (You only need to do this once. After that the app can be opened normally.):
      - Step 1: Hold _cmd_ and right click the app, from the context menu choose **Open**.
      - Step 2: A warning window will pop up, click _Open_  
      
    - Option 2:  
      Execute this command in terminal: `xattr -dr com.apple.quarantine /Applications/Snapmaker_Orca.app`
      ```console
          softfever@mac:~$ xattr -dr com.apple.quarantine /Applications/Snapmaker_Orca.app
      ```
    - Option 3:  
        - Step 1: open the app, a warning window will pop up  
            ![image](./SoftFever_doc/mac_cant_open.png)  
        - Step 2: in `System Settings` -> `Privacy & Security`, click `Open Anyway`:  
            ![image](./SoftFever_doc/mac_security_setting.png)  
    </details>
    
**Linux (Ubuntu)**:
 1. If you run into trouble executing it, try this command in the terminal:  
    `chmod +x /path_to_appimage/Snapmaker_Orca_Linux.AppImage`
    
# How to compile
- Windows 64-bit  
  - Tools needed: Visual Studio 2019, Cmake, git, git-lfs, Strawberry Perl.
      - You will require cmake version 3.14 or later, which is available [on their website](https://cmake.org/download/).
      - Strawberry Perl is [available on their GitHub repository](https://github.com/StrawberryPerl/Perl-Dist-Strawberry/releases/).
  - Run `build_release.bat` in `x64 Native Tools Command Prompt for VS 2019`
  - Note: Don't forget to run `git lfs pull` after cloning the repository to download tools on Windows

- Mac 64-bit  
  - Tools needed: Xcode, Cmake, git, gettext, libtool, automake, autoconf, texinfo
      - You can install most of them by running `brew install cmake gettext libtool automake autoconf texinfo`
  - run `build_release_macos.sh`
  - To build and debug in Xcode:
      - run `Xcode.app`
      - open ``build_`arch`/Snapmaker_Orca.Xcodeproj``
      - menu bar: Product => Scheme => Snapmaker_Orca
      - menu bar: Product => Scheme => Edit Scheme...
          - Run => Info tab => Build Configuration: `RelWithDebInfo`
          - Run => Options tab => Document Versions: uncheck `Allow debugging when browsing versions`
      - menu bar: Product => Run

- Ubuntu 
  - Dependencies **Will be auto-installed with the shell script**: `libmspack-dev libgstreamerd-3-dev libsecret-1-dev libwebkit2gtk-4.0-dev libosmesa6-dev libssl-dev libcurl4-openssl-dev eglexternalplatform-dev libudev-dev libdbus-1-dev extra-cmake-modules libgtk2.0-dev libglew-dev libudev-dev libdbus-1-dev cmake git texinfo`
  - run 'sudo ./BuildLinux.sh -u'
  - run './BuildLinux.sh -dsir'


# Note: 
If you're running Klipper, it's recommended to add the following configuration to your `printer.cfg` file.
```
# Enable object exclusion
[exclude_object]

# Enable arcs support
[gcode_arcs]
resolution: 0.1
```


## Some background
Snapmaker Orca is originally forked from Snapmaker_Orca.

Snapmaker_Orca is originally forked from Bambu Studio, it was previously known as BambuStudio-SoftFever.
Bambu Studio is forked from [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research, which is from [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community. 
Orca Slicer incorporates a lot of features from SuperSlicer by @supermerill
Orca Slicer's logo is designed by community member Justin Levine(@freejstnalxndr)  


# License
Snapmaker Orca is licensed under the GNU Affero General Public License, version 3. Snapmaker_Orca is based on Orca Slicer by SoftFever

Orca Slicer is licensed under the GNU Affero General Public License, version 3. Orca Slicer is based on Bambu Studio by BambuLab.

Bambu Studio is licensed under the GNU Affero General Public License, version 3. Bambu Studio is based on PrusaSlicer by PrusaResearch.

PrusaSlicer is licensed under the GNU Affero General Public License, version 3. PrusaSlicer is owned by Prusa Research. PrusaSlicer is originally based on Slic3r by Alessandro Ranellucci.

Slic3r is licensed under the GNU Affero General Public License, version 3. Slic3r was created by Alessandro Ranellucci with the help of many other contributors.

The GNU Affero General Public License, version 3 ensures that if you use any part of this software in any way (even behind a web server), your software must be released under the same license.

Orca Slicer includes a pressure advance calibration pattern test adapted from Andrew Ellis' generator, which is licensed under GNU General Public License, version 3. Ellis' generator is itself adapted from a generator developed by Sineos for Marlin, which is licensed under GNU General Public License, version 3.

The Bambu networking plugin is based on non-free libraries from BambuLab. It is optional to the Orca Slicer and provides extended functionalities for Bambulab printer users.

# Feedback & Contribution
We greatly value feedback and contributions from our users. Your feedback will help us to further develop Snapmaker Orca for our community.
- To submit a bug or feature request, file an issue in GitHub Issues or email us at support@snapmaker.com.
- To contribute some code, make sure you have read and followed our guidelines for contributing.

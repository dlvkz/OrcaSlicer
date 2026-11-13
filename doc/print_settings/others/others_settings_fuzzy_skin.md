# Fuzzy Skin

> In the settings these are now under **Surface texture** (where, what and how deep) and **Displacement map** (shown only for the
> displacement map texture type). The setting names changed with them: Fuzzy Skin is **Textured walls**, Noise type is
> **Texture type**, Fuzzy skin thickness is **Texture depth**, Fuzzy skin generator mode is **Texture method** (Move the nozzle,
> Vary the flow, Both), and the displacement map's own settings are **Pattern**, **Image**, **Projection**, **Tile size**,
> **Rotation**, **Invert** and **Offset**. The section names below keep the old names, so links to them still work.

Fuzzy skin randomly perturbs the wall path to produce a deliberately rough, matte appearance on the model surface.  
These settings control where the effect is applied, how the noise is generated, and how aggressive the displacement or extrusion modulation is.

Useful for creating a textures or hide surface imperfections but will increase print time and will affect dimensional accuracy.

- [Fuzzy Skin Mode](#fuzzy-skin-mode)
  - [Contour](#contour)
  - [Contour and Hole](#contour-and-hole)
  - [All Walls](#all-walls)
  - [Fuzzy Skin Generator Mode](#fuzzy-skin-generator-mode)
  - [Displacement](#displacement)
  - [Extrusion](#extrusion)
  - [Combined](#combined)
- [Noise Type](#noise-type)
  - [Classic](#classic)
  - [Perlin](#perlin)
  - [Billow](#billow)
  - [Ridged Multifractal](#ridged-multifractal)
  - [Voronoi](#voronoi)
  - [Displacement map](#displacement-map)
- [Point distance](#point-distance)
- [Skin thickness](#skin-thickness)
- [Skin feature size](#skin-feature-size)
- [Skin Noise Octaves](#skin-noise-octaves)
- [Skin Noise Persistence](#skin-noise-persistence)
- [Apply fuzzy skin to first layer](#apply-fuzzy-skin-to-first-layer)
- [Displacement map](#displacement-map)
  - [Quick start](#quick-start)
  - [Displacement texture](#displacement-texture)
  - [Projection](#projection)
  - [Size, rotation, invert](#size-rotation-invert)
  - [Displace the whole surface](#displace-the-whole-surface)
  - [Textured models and normal maps](#textured-models-and-normal-maps)
  - [Tips and limits](#tips-and-limits)
- [Credits](#credits)

## Fuzzy Skin Mode

Choose which parts of the model receive the fuzzy-skin effect.

### Contour

Apply fuzzy skin only to the outermost contour (external perimeter) of the model.  
Useful for creating a textured edge while keeping the inner surfaces smooth.

### Contour and Hole

Apply fuzzy skin to both the outer contour and interior holes. Useful when you want the rough texture to appear on negative features as well.

### All Walls

Apply fuzzy skin to every wall (external and internal). This gives the strongest overall textured appearance but will increase slicing and print time considerably.

### Fuzzy Skin Generator Mode

Select the underlying method used to produce the fuzzy effect. Each mode has different trade-offs for strength, speed and mechanical load.

### Displacement

![Fuzzy-skin-Displacement-mode](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-Displacement-mode.png?raw=true)

The classic method is when the pattern on the walls is achieved by shifting the printhead perpendicular to the wall.  
It gives a predictable result, but decreases the strength entire shells and open the pores inside the walls. It also increases the mechanical stress on the kinematics of the printer. The speed of general printing is slowing down.

### Extrusion

![Fuzzy-skin-Extrusion-mode](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-Extrusion-mode.png?raw=true)

The fuzzy skin condition is obtained by changing the amount of extruded plastic as the print head moves linearly. There is no extra load on the kinematics, there is no decrease in the printing speed, the pores do not open, but the drawing turns out to be smoother by a factor of 2. It is suitable for creating "loose" walls to reduce internal stress into extruded plastic, or masking printing defects on the side walls - a matte effect.

> [!CAUTION]
> The "Fuzzy skin thicknesses" parameter cannot be more than about 70%-125% (selected individually for different conditions) of the nozzle diameter! This is a complex condition that also depends on the height of the layer, and determines how thin the lines can be extruded.  
> [Arachne](quality_settings_wall_generator#arachne) wall generator mode should also be enabled.

### Combined

![Fuzzy-skin-Combined-mode](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-Combined-mode.png?raw=true)

This is a combination of Displacement and Extrusion modes. The clarity of the drawing is the same in the classic mode, but the walls remain strong and tight. The load on the kinematics is 2 times lower. The printing speed is faster than in Displacement mode, but the elapsed time will still be longer.

> [!WARNING]
> The limits on line thickness are the same as in the Extrusion mode.

## Noise Type

Select the noise algorithm used to generate the random offsets. Different noise types produce distinct visual textures.

### Classic

Simple uniform random noise. Produces a coarse, irregular texture.

![Fuzzy-skin-classic](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-classic.png?raw=true)

### Perlin

[Perlin noise](https://en.wikipedia.org/wiki/Perlin_noise) generates smooth, natural-looking variations with coherent structure.

![Fuzzy-skin-perlin](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-perlin.png?raw=true)

### Billow

Billow noise is similar to Perlin noise, but has a clumpier appearance. It can create more pronounced features and is often used for natural textures.

![Fuzzy-skin-billow](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-billow.png?raw=true)

### Ridged Multifractal

Creates sharp, jagged features and high-contrast detail. Useful for stone- or marble-like textures.

![Fuzzy-skin-ridged-multifractal](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-ridged-multifractal.png?raw=true)

### Voronoi

[Voronoi noise](https://en.wikipedia.org/wiki/Worley_noise) divides the surface into Voronoi cells and displaces each cell independently, creating a patchwork or cellular texture.

![Fuzzy-skin-voronoi](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-voronoi.png?raw=true)

## Displacement map

Set **Texture type** to **Displacement map** to texture the surface from a height image instead of random noise, the way a displacement map works in a 3D renderer. White pushes the surface out by the [skin thickness](#skin-thickness), black pulls it in by the same amount, and mid grey leaves it where it is.

The texture is applied while slicing: the mesh is never subdivided, so a 12-triangle box or a 640-triangle cylinder can carry a fine texture, and the project stays small. The 3D view displaces the model with the texture before you slice, and the sliced preview shows the real toolpaths. The **Displacement map** panel at the top left of the 3D view holds every setting of the selected object (or of the only object, or else the defaults for all objects); its sliders update the model as you drag.

### Quick start

1. Select the object (or set it in the process preset) and set **Textured walls** to Contour.
2. Set **Texture type** to **Displacement map**. The **Displacement map** group appears: pick a **Pattern**, a built-in one or **Image** and your own PNG or JPEG.
3. Pick a **Projection** and a **Tile size**, then set the **Texture depth**: 0.3 to 0.6 mm reads well with a 0.4 mm nozzle.
4. Keep **Displace the whole surface** on, so tops, slopes and overhangs get the texture too, then slice.

### Displacement texture

- **Image**: the file in **Image**. The **...** button next to it picks a file; **From text...** next to **Pattern** makes one from a description (see below). PNG (grey, colour or 16 bit) and JPEG are read; colour is turned into brightness. Images that tile seamlessly look best when they repeat.
- **Knurl**, **Hexagons**, **Bricks**, **Ribs**, **Waves**, **Bumps**, **Stone**, **Noise**: patterns generated by the slicer. Each one repeats seamlessly and spans the full black-to-white range, so the skin thickness is the full height of the pattern.

### Displacement map from text

**From text...** next to **Pattern** (**Generate from text...** in the displacement map panel of the 3D view) makes a seamless height map from a description, on your computer. Type what the surface should look like ("cobblestone pavement", "dragon scales", "woven wicker basket"), press **Generate**, and click the variant you like; **Use this texture** saves it as a 16-bit PNG in the data folder (`textures/text_to_height`), switches **Pattern** to an image and sets it as the displacement map. Nothing is uploaded.

- How it works: SD-Turbo draws a seamless colour texture in one step, DeepBump turns the colour into a normal map, and the normal map is integrated into heights. SD-Turbo runs with 4-bit weights (8-bit for its most sensitive layers) through stable-diffusion.cpp. Both networks run on the graphics card when one is usable (Vulkan), and otherwise on the processor; if the graphics card fails, the processor takes over. On the graphics card the first texture of a session takes about 10 s while the card is prepared, and later ones well under a second; on the processor, about 5 to 10 s each.
- The same description and seed give the same texture on the same computer; variants use the seeds that follow. The graphics card and the processor round slightly differently, so the same seed can look a little different between them.
- The model files are separate from the program: `sd-turbo-gptq.gguf`, `taesd.safetensors` and `deepbump256.gguf` (about 0.93 GB together), with their `NOTICE.txt` and licence files, go in `models/text_to_height` in the data folder (**Model folder** in the dialog opens it). Without them the dialog says which files are missing.
- The button is only there in builds made with stable-diffusion.cpp: build the dependencies with `-DDEP_TEXT2HEIGHT=ON`, or point the slicer build at an install with `-DSDCPP_ROOT=<folder>`.
- Powered by Stability AI. SD-Turbo is licensed under the Stability AI Community License (copy in the model folder); DeepBump (Hugo Tini) under GPL-3.0; TAESD (Ollin Boer Bohan) under the MIT License.

### Projection

How the image is wrapped onto the object. All of them are centred on the middle of the object.

- **Box (triplanar)**: projected from the four sides, the top and the bottom, blended by the surface's direction where it turns. A good default for boxy and organic shapes.
- **Cylindrical**: wrapped around the vertical axis. A whole number of tiles goes around the object, so the image meets itself at the back; tops and bottoms are seen from above. To wrap one image exactly once, set the size to the object's circumference.
- **Spherical**: wrapped around the object's centre like a map around a globe, also a whole number of tiles around. The image pinches at the poles.
- **Planar (top, front, side)**: projected straight through the object from one direction, like a slide projector. Surfaces facing that way show the image best; surfaces along the direction stretch it into streaks.
- **UV**: placed by the texture coordinates of the textured model the part was loaded from (OBJ, GLB or glTF), so it lands where the model's own textures are. **UV mesh** stays empty for that; set it only to take the coordinates from another file holding the same mesh (for example the original of a repaired or simplified part).
- **On the surface (seamless)**: for every pattern. For Hexagons, Bumps and Stone, instead of projecting an image, the slicer lays the pattern out on the surface itself: cells and bumps are spread evenly over the mesh (and relaxed into a honeycomb for hexagons and bumps), and each point's height comes from its distance to the nearest ones. Nothing is stretched and there are no seams: a cell on an edge folds over it, and cells keep their size round a narrow neck and a wide belly alike. Hexagons become a honeycomb laid along the surface: flat areas get a regular lattice in their own plane, and round parts get rows of cells around them that gain or lose a cell where the part widens or narrows a lot. Noise is solid 3D noise. Images and the striped patterns (Knurl, Ribs, Waves, Bricks) are laid by many small charts, spread evenly over the surface: each projects the image along its own surface direction, and they are chained from one to the next so the image continues across them (on walls the image's horizontal stays horizontal, and round a cylinder it advances by the arc length). Each point blends the charts around it, so where two chains meet from opposite sides, at the back of a round part or round a sphere, the image fades from one to the other over a few millimetres instead of leaving a seam.

Projections of a flat image always compromise on a closed shape: the cylindrical projection fits a whole number of tiles around the widest part, so they get narrower where the part is narrower, and its tops are projected separately, so the pattern is cut at the rim. Use **On the surface** whenever the part is not a plain box or cylinder.

### Size, rotation, invert

- **Tile size**: the width of one tile in mm. Its height follows the image's aspect ratio. With **On the surface**, the spacing of the cells or bumps; for images and stripes, still the tile width.
- **Rotation**: turns the image counter-clockwise on the surface. Not used by UV, nor by the cell patterns and noise on the surface. With cylindrical and spherical, angles other than 0 and 180 degrees can leave a seam at the back.
- **Invert**: swaps bumps and dents.
- **Offset**: which way the surface moves, as Blender's midlevel. At 0 it only goes in (white stays on the surface), at 1 it only goes out (black stays on the surface), at 0.5 (the default) mid grey stays put and the rest goes both ways. From black to white the surface always moves twice the **Texture depth**.

### Displace the whole surface

On (default): the texture is applied to the shape itself, as if it were modelled: walls, slopes, tops, and the undersides of overhangs. A bump on a top is printed on the layers above it, and a dent removes material from the layers below. The face the object stands on stays flat. Walls, skins, overhang detection and supports all follow the textured shape.

Off (**per layer only**): each layer's outline is moved sideways by the texture, and nothing moves up or down: tops and the undersides of overhangs stay flat, and a texture on a slope shows as steps of the layers. Every projection works. Slicing is faster, about as fast as ordinary fuzzy skin, because no material is added to or cut from tops and the layers stay simpler. Measured on the showcase parts, per layer sliced 1.2 to 1.7 times faster than the whole surface. Use it for textured walls (grips, vases, cups) when the tops don't need the texture.

**Supports ignore the texture** (tree supports, with the whole surface displaced): supports are planned from the model without the texture, so the texture's own tiny overhangs get no support of their own and supports are generated faster. Supports still keep their distance from the textured surface.

### Textured models and normal maps

GLB, glTF and OBJ (with MTL) models load with their texture coordinates. When such a model has a normal map, Snapmaker Orca offers to turn it into a surface texture: it integrates the normal map into a 16-bit height map, sets the UV projection and a depth that matches the model's size, and enables the displacement for that object. You can also do this later from the object's right-click menu (**Surface texture from normal map**), or with `--relief-from-normal-map` on the command line. Scale the model to its print size first: the depth is worked out from the model's current size.

glTF files whose geometry is compressed (`EXT_meshopt_compression`, used by current Tripo exports, or Draco) are not supported yet and are refused with a message. Export them again without mesh compression, or convert them with `python scripts/glb_unmeshopt.py in.glb out.glb` (no dependencies besides NumPy).

### Tips and limits

- The texture follows the mesh: an open or broken mesh slices with gaps whether it is textured or not. Repair it first.
- Point distance sets how finely the walls follow the texture: 0.2 mm is a good default.
- Turn off **Only one wall on top surfaces** for textured tops. Where raised texture reaches the edge of a top, that option runs the inner wall across the top between the raised parts, which shows as lines.
- A deep texture adds print time, mainly on walls and tops. Very fine textures (tiles smaller than a few mm) at large depth produce many tiny overhangs; turn on **Supports ignore the texture** if supports chase them.
- The command line accepts every setting, for example `--fuzzy_skin_texture_pattern knurl --fuzzy_skin_texture_projection cylindrical --fuzzy_skin_texture_size 4`.

## Point distance

Average distance between random sample points along each line segment.  
Smaller values add more detail and increase computation; larger values produce coarser, faster results.

## Skin thickness

Maximum lateral width (in mm) over which points can be displaced. This defines how far the wall can be jittered.  
Keep this below or near your outer wall line width and within nozzle/flow limits for reliable prints.

## Skin feature size

Base size of coherent noise features, in mm. Larger values yield bigger, more prominent structures; smaller values give fine-grained texture.

## Skin Noise Octaves

The number of octaves of coherent noise to use. Higher values increase the detail of the noise, but also increase computation time.

## Skin Noise Persistence

Controls how amplitude decays across octaves. Lower persistence results in smoother noise; higher persistence keeps finer-scale detail stronger.

## Apply fuzzy skin to first layer

Enable to apply fuzzy skin to the first layer.

> [!CAUTION]
> Can impact bed adhesion and surface contact.

## Credits

- **Generator Mode author:** [@pi-squared-studio](https://github.com/pi-squared-studio).

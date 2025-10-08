# Skybox Images

To use custom skybox images, place the following files in this folder:

## For Cubemap (6 separate images):
- `right.jpg` - Right face (+X)
- `left.jpg` - Left face (-X)
- `top.jpg` - Top face (+Y)
- `bottom.jpg` - Bottom face (-Y)
- `front.jpg` - Front face (+Z)
- `back.jpg` - Back face (-Z)

All images should be square and the same size (recommended: 1024x1024 or 2048x2048).

## For HDR Environment Map:
- Place a single equirectangular HDR image (`.hdr` format)
- Example: `environment.hdr`

## Where to find free skybox images:
- https://polyhaven.com/hdris (Free HDRIs)
- https://hdrihaven.com/ (Free HDR environment maps)
- https://www.humus.name/index.php?page=Textures (Free cubemaps)

## Note:
The engine currently supports:
1. Procedural gradient (default)
2. Loading 6 cubemap faces from JPG/PNG files
3. Loading HDR files (fallback to gradient for now)

Use the Skybox Mode dropdown in the UI to switch between modes.

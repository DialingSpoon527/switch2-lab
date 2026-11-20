# Launcher icon

`icon.svg` is the original vector artwork for the bridge. The NRO embeds
`icon.jpg`, a 256×256 JPEG with no EXIF metadata. `render-icon.py` uses CairoSVG
and Pillow to regenerate the JPEG and a local PNG preview. The build uses the
included JPEG directly.

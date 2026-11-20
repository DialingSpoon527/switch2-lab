from pathlib import Path
from io import BytesIO
import cairosvg
from PIL import Image

assets = Path(__file__).resolve().parent / "assets"
png = cairosvg.svg2png(url=str(assets / "icon.svg"), output_width=256, output_height=256)
(assets / "icon-preview.png").write_bytes(png)
Image.open(BytesIO(png)).convert("RGB").save(assets / "icon.jpg", quality=95, subsampling=0)

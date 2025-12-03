from PIL import Image
from pathlib import Path

img_path = Path(r"C:\Users\GoldGlaid\Universty\CG\testbed\images\sky_box.png")

img = Image.open(img_path)
img_resized = img.resize((736, 552), Image.LANCZOS)  # 512x512, возможна деформация

# перезаписать тот же файл
img_resized.save(img_path, optimize=True)

# или сохранить как новый файл:
# img_resized.save(img_path.with_name("geralt_512.png"), optimize=True)
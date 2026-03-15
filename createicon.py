from PIL import Image
img = Image.open("icon.png").convert("RGBA").resize((64,64), Image.LANCZOS)
data = list(img.tobytes())
with open("icon.h","w") as f:
    f.write(f"static const unsigned char icon_pixels[] = {{{','.join(map(str,data))}}};\n")
    f.write("static const int icon_w = 64, icon_h = 64;\n")

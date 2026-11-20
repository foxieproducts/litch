#!/usr/bin/env python3
"""Render the Foxie fox artwork, wordmarks, and app launcher icon.

Requires Pillow. The font is used locally, never copied into the repository.
White-matte removal preserves the source when composited back onto white.
The app icon is rendered from its checked-in PNG master.
"""

import argparse
import io
import json
from pathlib import Path

from PIL import (
    Image,
    ImageChops,
    ImageCms,
    ImageDraw,
    ImageEnhance,
    ImageFilter,
    ImageFont,
)


def remove_white_matte(source):
    # The supplied PNG has a Display ICC profile. Convert before extracting
    # alpha so untagged app assets retain its displayed colors in sRGB.
    profile = source.info.get("icc_profile")
    source = ImageCms.profileToProfile(
        source, ImageCms.ImageCmsProfile(io.BytesIO(profile)),
        ImageCms.createProfile("sRGB"), outputMode="RGB"
    ) if profile else source.convert("RGB")
    pixels = []
    for red, green, blue in (source.getpixel((x, y))
                            for y in range(source.height)
                            for x in range(source.width)):
        white = min(red, green, blue)
        alpha = 255 - white
        pixels.append(
            tuple(round((channel - white) * 255 / alpha)
                  for channel in (red, green, blue)) + (alpha,)
            if alpha else (0, 0, 0, 0)
        )
    result = Image.new("RGBA", source.size)
    result.putdata(pixels)
    restored = Image.new("RGBA", source.size, "white")
    restored.alpha_composite(result)
    error = ImageChops.difference(restored.convert("RGB"), source.convert("RGB"))
    assert max(max(pair) for pair in error.getextrema()) <= 1
    return result


def gradient_lettering(font, text, start, end):
    left, top, right, bottom = font.getbbox(text)
    mask = Image.new("L", (right - left, bottom - top))
    ImageDraw.Draw(mask).text((-left, -top), text, font=font, fill=255)
    gradient = Image.new("RGB", mask.size)
    draw = ImageDraw.Draw(gradient)
    for x in range(mask.width):
        amount = x / max(1, mask.width - 1)
        color = tuple(round(a + (b - a) * amount) for a, b in zip(start, end))
        draw.line((x, 0, x, mask.height), fill=color)
    gradient.putalpha(mask)
    return gradient


def multistop_lettering(font, text, colors):
    left, top, right, bottom = font.getbbox(text)
    mask = Image.new("L", (right - left, bottom - top))
    ImageDraw.Draw(mask).text((-left, -top), text, font=font, fill=255)
    gradient = Image.new("RGB", mask.size)
    draw = ImageDraw.Draw(gradient)
    for x in range(mask.width):
        position = x * (len(colors) - 1) / max(1, mask.width - 1)
        index = min(int(position), len(colors) - 2)
        amount = position - index
        color = tuple(
            round(a + (b - a) * amount)
            for a, b in zip(colors[index], colors[index + 1])
        )
        draw.line((x, 0, x, mask.height), fill=color)
    gradient.putalpha(mask)
    return gradient


def lettering(font_path):
    font = ImageFont.truetype(str(font_path), 240, index=1)
    assert font.getname() == ("Gurmukhi MN", "Bold")
    purple, orange = (109, 43, 218), (255, 108, 0)
    wordmark = gradient_lettering(font, "FOXIE LITCH", purple, orange)
    litch = gradient_lettering(font, "LITCH", purple, orange)
    craft = multistop_lettering(font, "CRAFT", (
        (255, 52, 82), (255, 116, 48), (255, 207, 45), (55, 215, 105),
        (34, 195, 226), (63, 106, 255), (174, 83, 255),
    ))
    gap = round(wordmark.height * 0.035)
    expanded = Image.new(
        "RGBA", (litch.width + gap + craft.width, wordmark.height)
    )
    expanded.alpha_composite(litch)
    expanded.alpha_composite(craft, (litch.width + gap, 0))
    return wordmark, expanded


def render_app_icon(master):
    with Image.open(master) as image:
        return image.convert("RGB").resize(
            (1024, 1024), Image.Resampling.LANCZOS
        )


def android_safe_icon(icon):
    """Keep all recognizable artwork inside Android's 66/108 dp safe zone."""
    background = icon.filter(ImageFilter.GaussianBlur(180))
    background = ImageEnhance.Brightness(background).enhance(0.32)
    foreground_size = 700
    foreground = icon.resize(
        (foreground_size, foreground_size), Image.Resampling.LANCZOS
    )
    # Feather only the source's already-empty outer border into the extended
    # backdrop. The tiles and supplied Foxie face remain pixel-faithful.
    mask = Image.new("L", foreground.size)
    ImageDraw.Draw(mask).rectangle(
        (36, 36, foreground_size - 37, foreground_size - 37), fill=255
    )
    mask = mask.filter(ImageFilter.GaussianBlur(24))
    offset = ((1024 - foreground_size) // 2,) * 2
    background.paste(foreground, offset, mask)
    safe_radius = 1024 * 33 / 108
    center = (1024 - 1) / 2
    violations = 0
    for y in range(1024):
        for x in range(1024):
            red, green, blue = background.getpixel((x, y))
            bright_bevel = max(red, green, blue) >= 240 and min(
                red, green, blue
            ) >= 100
            if bright_bevel and (x - center) ** 2 + (y - center) ** 2 > safe_radius ** 2:
                violations += 1
    assert violations == 0, (
        f"Android launcher bevel exceeds 66/108 safe zone: {violations} pixels"
    )
    return background


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--source", type=Path)
    parser.add_argument(
        "--icon-only", action="store_true",
        help="regenerate only the platform launcher icons from the PNG master",
    )
    parser.add_argument("--font", type=Path, default=Path(
        "/System/Library/Fonts/Supplemental/Gurmukhi MN.ttc"))
    args = parser.parse_args()
    assets = args.repo / "app/assets/branding"
    assets.mkdir(parents=True, exist_ok=True)
    if not args.icon_only:
        if args.source is None:
            parser.error("--source is required unless --icon-only is used")
        fox = remove_white_matte(Image.open(args.source))
        fox = fox.crop(fox.getchannel("A").getbbox())
        fox.save(assets / "foxie-fox.png", optimize=True)
        wordmark, litchcraft_wordmark = lettering(args.font)
        for density in (1, 2, 3):
            folder = assets if density == 1 else assets / f"{density}.0x"
            folder.mkdir(exist_ok=True)
            height = 28 * density
            wordmark.resize(
                (round(wordmark.width * height / wordmark.height), height),
                Image.Resampling.LANCZOS,
            ).save(folder / "foxie-litch-wordmark.png", optimize=True)
            litchcraft_wordmark.resize(
                (round(litchcraft_wordmark.width * height / litchcraft_wordmark.height), height),
                Image.Resampling.LANCZOS,
            ).save(folder / "foxie-litchcraft-wordmark.png", optimize=True)

    master = assets / "foxie-app-icon-master.png"
    icon = render_app_icon(master)
    icon.save(assets / "foxie-app-icon.png", optimize=True)
    ios = args.repo / "app/ios/Runner/Assets.xcassets/AppIcon.appiconset"
    for entry in json.loads((ios / "Contents.json").read_text())["images"]:
        if "filename" not in entry:
            continue
        size = round(float(entry["size"].split("x")[0]) *
                     float(entry["scale"].removesuffix("x")))
        icon.resize((size, size), Image.Resampling.LANCZOS).save(
            ios / entry["filename"], optimize=True)
    drawable = args.repo / "app/android/app/src/main/res/drawable-nodpi"
    drawable.mkdir(parents=True, exist_ok=True)
    android_icon = android_safe_icon(icon)
    android_icon.save(drawable / "ic_launcher_full.png", optimize=True)
    android_icon.save(drawable / "ic_launcher_foreground.png", optimize=True)
    for density, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96),
                          ("xxhdpi", 144), ("xxxhdpi", 192)):
        folder = args.repo / f"app/android/app/src/main/res/mipmap-{density}"
        folder.mkdir(parents=True, exist_ok=True)
        android_icon.resize((size, size), Image.Resampling.LANCZOS).save(
            folder / "ic_launcher.png", optimize=True)
    if args.icon_only:
        print(f"Rendered launcher icons from {master}")
    else:
        print(f"Rendered transparent fox, native-font wordmarks, and launcher icons in {assets}")


if __name__ == "__main__":
    main()

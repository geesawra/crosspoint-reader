#!/usr/bin/env python3
"""
Convert an image to a CrossPoint sleep-screen wallpaper BMP.

Supports both X3 and X4 devices. Outputs an uncompressed 24-bit BMP
at the correct portrait resolution for the chosen device.

Usage:
    python convert_wallpaper.py input.jpg -o sleep.bmp
    python convert_wallpaper.py input.png --device x3 --mode crop
"""

import argparse
import os
import sys

from PIL import Image

DEVICE_SPECS = {
    "x4": {"width": 480, "height": 800},
    "x3": {"width": 528, "height": 792},
}


def resize_fit(img: Image.Image, width: int, height: int) -> Image.Image:
    """Scale image down to fit inside the target box, preserving aspect ratio.
    Pads with white borders to reach the exact resolution."""
    img = img.convert("RGB")
    img.thumbnail((width, height), Image.LANCZOS)
    canvas = Image.new("RGB", (width, height), (255, 255, 255))
    x = (width - img.width) // 2
    y = (height - img.height) // 2
    canvas.paste(img, (x, y))
    return canvas


def resize_crop(img: Image.Image, width: int, height: int) -> Image.Image:
    """Scale and crop the image to completely fill the target resolution."""
    img = img.convert("RGB")
    src_ratio = img.width / img.height
    dst_ratio = width / height

    if src_ratio > dst_ratio:
        # Image is wider than target: crop width
        new_height = height
        new_width = int(new_height * src_ratio)
        img = img.resize((new_width, new_height), Image.LANCZOS)
        left = (new_width - width) // 2
        img = img.crop((left, 0, left + width, height))
    else:
        # Image is taller than target: crop height
        new_width = width
        new_height = int(new_width / src_ratio)
        img = img.resize((new_width, new_height), Image.LANCZOS)
        top = (new_height - height) // 2
        img = img.crop((0, top, width, top + height))

    return img


def resize_stretch(img: Image.Image, width: int, height: int) -> Image.Image:
    """Stretch the image to exactly match the target resolution."""
    return img.convert("RGB").resize((width, height), Image.LANCZOS)


def main():
    parser = argparse.ArgumentParser(
        description="Convert an image to a CrossPoint sleep-screen wallpaper BMP."
    )
    parser.add_argument("input", help="Path to the source image.")
    parser.add_argument(
        "-o", "--output",
        help="Output BMP path (default: sleep.bmp in the current directory).",
        default="sleep.bmp",
    )
    parser.add_argument(
        "--device",
        choices=["x3", "x4"],
        default="x4",
        help="Target device model (default: x4).",
    )
    parser.add_argument(
        "--mode",
        choices=["fit", "crop", "stretch"],
        default="fit",
        help=(
            "Resize mode: fit = scale to fit with white padding (default), "
            "crop = scale and crop to fill, stretch = distort to fill."
        ),
    )
    args = parser.parse_args()

    if not os.path.isfile(args.input):
        print(f"Error: file not found: {args.input}", file=sys.stderr)
        sys.exit(1)

    spec = DEVICE_SPECS[args.device]
    width, height = spec["width"], spec["height"]

    try:
        with Image.open(args.input) as img:
            if args.mode == "fit":
                out = resize_fit(img, width, height)
            elif args.mode == "crop":
                out = resize_crop(img, width, height)
            else:
                out = resize_stretch(img, width, height)
    except Exception as exc:
        print(f"Error loading image: {exc}", file=sys.stderr)
        sys.exit(1)

    # Pillow saves BMP as uncompressed 24-bit when the image mode is RGB.
    out.save(args.output, "BMP")
    print(f"Saved {args.device.upper()} wallpaper ({width}x{height}) to: {args.output}")


if __name__ == "__main__":
    main()

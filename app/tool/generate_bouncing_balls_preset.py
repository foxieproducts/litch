"""Generate the 256-frame format-native bouncing-balls benchmark and render.

The generated Dart data is an ordinary fixed Litchcraft animation document;
there is no runtime or firmware-side procedural rendering. The GIF previews
those exact frames at the production V3 emitter positions.
"""

import base64
import colorsys
import math
from pathlib import Path
import re
import textwrap
import zlib

from PIL import Image, ImageDraw


FRAME_COUNT = 256
FRAME_DURATION_MS = 50
RENDER_SIZE = 576
BALLS = (
    (0.52, 2, 3, 0.0, 4.0, 1),
    (0.91, 3, 2, 9.0, 1.0, 2),
    (0.12, 4, 5, 5.0, 12.0, 3),
)
STARBURSTS = ((48, 0), (128, 1), (208, 2))


def production_centers(app_root: Path):
    geometry = (app_root / "lib/src/widgets/litchcraft_tools.dart").read_text()
    geometry = geometry.split("Offset litchcraftEmitterPosition(", 1)[1]
    geometry = geometry.split("class LitchcraftTargetIcon", 1)[0]
    centers = re.findall(r"=> const Offset\(([\d.]+), ([\d.]+)\)", geometry)[:4]
    if len(centers) != 4:
        raise ValueError("Cannot resolve production V3 emitter centers")
    return [(float(x), float(y)) for x, y in centers]


def emitter_grid_position(emitter):
    square = emitter // 4
    row, column = divmod(square, 8)
    side = emitter % 4
    local = ((0, 0), (1, 0), (1, 1), (0, 1))[side]
    return column * 2 + local[0], row * 2 + local[1]


def bounce(value, maximum=13.0):
    period = maximum * 2.0
    value %= period
    return value if value <= maximum else period - value


def ball_position(frame, cycles_x, cycles_y, offset_x, offset_y):
    phase = frame / (FRAME_COUNT - 1)
    return (
        1.0 + bounce(offset_x + phase * cycles_x * 26.0),
        1.0 + bounce(offset_y + phase * cycles_y * 26.0),
    )


def ball_color(frame, base_hue, hue_cycles):
    phase = frame / (FRAME_COUNT - 1)
    red, green, blue = colorsys.hsv_to_rgb(
        (base_hue + phase * hue_cycles) % 1.0, 0.92, 1.0
    )
    return round(red * 255), round(green * 255), round(blue * 255)


def background(emitter, frame):
    # A closed dark wave remains visually subordinate while keeping the
    # transport benchmark dense enough to exercise sustained bulk admission.
    phase = (
        frame / (FRAME_COUNT - 1) * 3.0
        + emitter * 0.071
        + (emitter // 16) * 0.029
    )
    level = round((0.5 + 0.5 * math.sin(2.0 * math.pi * phase)) * 15.0)
    return level // 4, level // 3, level


def add_starbursts(pixels, frame):
    for start, ball_index in STARBURSTS:
        step = frame - start
        if step < 0 or step >= 20:
            continue
        base_hue, cycles_x, cycles_y, offset_x, offset_y, hue_cycles = BALLS[
            ball_index
        ]
        center_x, center_y = ball_position(
            start, cycles_x, cycles_y, offset_x, offset_y
        )
        color = ball_color(frame, base_hue, hue_cycles)
        progress = (step + 1) / 21.0
        radius = 0.4 + progress * 11.5
        envelope = math.sin(math.pi * progress)
        for emitter in range(256):
            pixel_x, pixel_y = emitter_grid_position(emitter)
            dx = pixel_x - center_x
            dy = pixel_y - center_y
            distance = math.hypot(dx, dy)
            angle = math.atan2(dy, dx)
            ray_error = abs(math.sin(angle * 4.0))
            ring = max(0.0, 1.0 - abs(distance - radius) / 1.2)
            ray = max(0.0, 1.0 - ray_error / 0.22) if distance <= radius else 0.0
            gain = envelope * max(ring, ray * 0.55)
            if gain < 0.025:
                continue
            for channel in range(3):
                pixels[emitter][channel] = min(
                    255,
                    pixels[emitter][channel] + round(color[channel] * gain),
                )


def render_frame(frame):
    pixels = [list(background(emitter, frame)) for emitter in range(256)]
    add_starbursts(pixels, frame)
    for base_hue, cycles_x, cycles_y, offset_x, offset_y, hue_cycles in BALLS:
        # A bright 2x2-ish core plus ten fading history positions. Continuous
        # positions keep motion smooth on the coarse physical emitter field.
        for trail in range(11):
            x, y = ball_position(
                frame - trail, cycles_x, cycles_y, offset_x, offset_y
            )
            color = ball_color(frame - trail, base_hue, hue_cycles)
            trail_gain = math.exp(-trail * 0.31)
            for emitter in range(256):
                pixel_x, pixel_y = emitter_grid_position(emitter)
                dx = abs(pixel_x - x)
                dy = abs(pixel_y - y)
                if dx >= 2.15 or dy >= 2.15:
                    continue
                spatial_gain = max(0.0, 1.0 - dx / 2.15) * max(
                    0.0, 1.0 - dy / 2.15
                )
                gain = trail_gain * spatial_gain
                if gain < 0.025:
                    continue
                for channel in range(3):
                    pixels[emitter][channel] = min(
                        255,
                        pixels[emitter][channel] + round(color[channel] * gain),
                    )
    return [tuple(pixel) for pixel in pixels]


def render_board(colors, centers):
    rendered = Image.new("RGB", (RENDER_SIZE, RENDER_SIZE), (0, 0, 0))
    draw = ImageDraw.Draw(rendered)
    square_size = RENDER_SIZE / 8
    radius = RENDER_SIZE / 62
    for row in range(8):
        for column in range(8):
            square = row * 8 + column
            for side, (center_x, center_y) in enumerate(centers):
                x = (column + center_x) * square_size
                y = (row + center_y) * square_size
                color = colors[square * 4 + side]
                draw.ellipse(
                    (x - radius, y - radius, x + radius, y + radius),
                    fill=color,
                )
    return rendered


def write_dart(output: Path, packed: bytes):
    encoded = base64.b64encode(packed).decode("ascii")
    output.write_text(
        "// Generated by tool/generate_bouncing_balls_preset.py; do not edit.\n"
        f"// {FRAME_COUNT} frames, {FRAME_DURATION_MS} ms each, "
        f"256 RGB samples/frame; {len(packed)} packed bytes.\n"
        "const String litchcraftBouncingBallsPackedRgb =\n"
        + "\n".join(f"    '{line}'" for line in textwrap.wrap(encoded, 96))
        + ";\n"
    )


def write_cpp_fixture(output: Path, samples: bytes):
    encoded = base64.b64encode(samples).decode("ascii")
    lines = textwrap.wrap(encoded, 96)
    output.write_text(
        "#pragma once\n"
        "#include <cstddef>\n"
        "namespace bouncing_balls_fixture {\n"
        "// Exact RGB payload generated with the app preset; test-only.\n"
        f"inline constexpr size_t RGB_BYTES = {len(samples)}U;\n"
        f"inline constexpr unsigned RGB_CRC32 = 0x{zlib.crc32(samples):08x}U;\n"
        "inline constexpr char RGB_BASE64[] =\n"
        + "\n".join(f'    "{line}"' for line in lines)
        + ";\n} // namespace bouncing_balls_fixture\n"
    )


def main():
    app_root = Path(__file__).resolve().parents[1]
    repository_root = app_root.parent
    centers = production_centers(app_root)
    samples = bytearray()
    rendered_frames = []
    first_colors = None
    for frame in range(FRAME_COUNT):
        colors = (
            first_colors
            if frame == FRAME_COUNT - 1
            else render_frame(frame)
        )
        if first_colors is None:
            first_colors = colors
        for color in colors:
            samples.extend(color)
        rendered_frames.append(render_board(colors, centers))

    packed = zlib.compress(samples, level=9)
    dart_output = app_root / "lib/src/models/litchcraft_bouncing_balls_data.dart"
    write_dart(dart_output, packed)
    cpp_output = (
        repository_root
        / "test/test_rickroll_virtual_litch_journey/bouncing_balls_rgb_fixture.hpp"
    )
    write_cpp_fixture(cpp_output, samples)

    render_directory = repository_root / "logs/qa/litchcraft-bouncing-balls-256"
    render_directory.mkdir(parents=True, exist_ok=True)
    render_output = render_directory / "bouncing_balls_v3_256_frame_render.gif"
    rendered_frames[0].save(
        render_output,
        save_all=True,
        append_images=rendered_frames[1:],
        duration=FRAME_DURATION_MS,
        loop=0,
        optimize=False,
    )
    print(
        f"{len(samples)} RGB bytes -> {len(packed)} packed bytes; "
        f"{dart_output}; {cpp_output}; {render_output}"
    )


if __name__ == "__main__":
    main()

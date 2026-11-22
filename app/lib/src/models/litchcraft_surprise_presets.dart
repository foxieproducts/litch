import 'dart:math' as math;

import 'litchcraft_animation.dart';
import 'litchcraft_foxie_raster.dart';

/// Ordinary editable artwork; these functions only author semantic frames.
enum LitchcraftSurprisePreset {
  pocketSunrise(1, 'Pocket Sunrise', 32),
  foxWink(2, 'Fox Wink', 63),
  koiPond(4, 'Koi Pond', 97),
  fireflyGarden(5, 'Firefly Garden', 128),
  moonLaunch(6, 'Moon Launch', 191),
  rainbowRibbon(7, 'Rainbow Ribbon', 256);

  const LitchcraftSurprisePreset(this.taps, this.name, this.frameCount);
  final int taps;
  final String name;
  final int frameCount;
  int get sparkleSeed => 0x464f5800 + taps;

  List<LitchcraftAnimationFrame> createFrames() => List.unmodifiable([
    for (var frame = 0; frame < frameCount; frame++)
      LitchcraftAnimationFrame(
        durationMs: switch (this) {
          pocketSunrise => frame == 15 || frame == 31 ? 350 : 110,
          foxWink => frame >= 26 && frame <= 32 ? 120 : 70,
          koiPond => frame % 12 == 0 ? 90 : 60,
          fireflyGarden => frame % 16 == 0 ? 80 : 50,
          moonLaunch => frame >= 88 && frame <= 100 ? 100 : 50,
          rainbowRibbon => 50,
        },
        emitters: [
          for (var emitter = 0; emitter < 256; emitter++)
            _sample(frame, emitter),
        ],
      ),
  ]);

  LitchcraftAnimatedEmitter _sample(int frame, int emitter) {
    final square = emitter ~/ 4, region = emitter % 4;
    final x = square % 8 + const [-0.29, 0.0, 0.29, 0.0][region];
    final y = square ~/ 8 + const [0.0, -0.29, 0.0, 0.29][region];
    final t = frame / frameCount, phase = t * math.pi * 2;
    var color = 0;
    var effect = LitchcraftEffectStyle.off;
    var effectMs = 0;

    switch (this) {
      case pocketSunrise:
        final height = (1 + math.cos(phase)) / 2;
        final sunY = 6.2 - 4.3 * height;
        color = _mix(0, 0xffc650, _spot(x - 3.5, y - sunY, 1.65));
        if (t > 0.1 && t < 0.6) {
          final radius = (t - 0.1) * 12;
          final ray = _ring(x - 3.5, y - 3.5, radius, 0.85);
          color = _mix(color, 0xff9340, ray * 0.85);
        }
        if (y > 5.1) {
          final wave = math.sin(x * 1.6 + phase * 2 + y * 2);
          color = _mix(
            0,
            0x36cad8,
            ((wave - 0.45) / 0.55).clamp(0.0, 1.0) * 0.75,
          );
          final reflection = _spot((x - 3.5) / 1.7, (y - 6.1) / 1.2, 1.1);
          color = _mix(
            color,
            0xffb34c,
            reflection * height * (0.6 + 0.3 * wave),
          );
        }
      case foxWink:
        // The actual Foxie mark, sampled from the canonical brand asset.
        // A small approach/perk preserves the open angular face at 8x8.
        final approach = math.pow(math.max(0.0, math.sin(phase)), 4).toDouble();
        final scale = 7.8 + approach * 1.2;
        final u = (x - 3.5 - 0.12 * math.sin(phase)) / scale + 0.5;
        var v = (y - 3.5 + 0.12 * math.sin(phase * 2)) / scale + 0.5;
        final wink = frame >= 27 && frame <= 31;
        if (wink && u > 0.5 && v > 0.42 && v < 0.575) {
          // Compress only the right gold eye; ears and jaw stay canonical.
          v = 0.49 + (v - 0.49) / 0.25;
          if (v < 0.42 || v > 0.575) v = -1;
        }
        color = _foxiePixel(u, v);
        if (t > 0.12 && t < 0.48) {
          final pulse = _ring(x - 3.5, y - 3.5, (t - 0.12) / 0.36 * 6, 0.75);
          if (color == 0) color = _mix(0, 0xfecb00, pulse * 0.85);
        }
      case koiPond:
        final radius = t * 6;
        color = _mix(0, 0x43b4c3, _ring(x - 3.5, y - 3.5, radius, 0.65) * 0.8);
        for (var fish = 0; fish < 2; fish++) {
          final angle = phase + fish * math.pi;
          final cx = 3.5 + 2.5 * math.cos(angle);
          final cy = 3.5 + 2.1 * math.sin(angle);
          final dx = x - cx, dy = y - cy;
          final along = -dx * math.sin(angle) + dy * math.cos(angle);
          final across = dx * math.cos(angle) + dy * math.sin(angle);
          final body = math.pow(along / 1.15, 2) + math.pow(across / 0.6, 2);
          if (body < 1) {
            color = along > 0.25
                ? 0xffedbd
                : fish == 0
                ? 0xff883c
                : 0xffb2c9;
            if (along > 0.7 && across.abs() < 0.16) color = 0;
          } else {
            final tail = across - 0.22 * math.sin(phase * 5 + fish);
            if (along < -0.85 &&
                along > -1.7 &&
                tail.abs() < (-along - 0.75) * 0.65) {
              color = fish == 0 ? 0xf65a2f : 0xd688c4;
            }
          }
        }
        if (_spot(x - 0.45, y - 0.65, 0.72) > 0.25 ||
            _spot(x - 6.8, y - 6.55, 0.7) > 0.25) {
          color = 0x50b875;
        }
      case fireflyGarden:
        // Keep the original quiet garden, adding one layer of taller swaying blades.
        if (y > 6.75) color = 0x288b48;
        for (var blade = 0; blade < 9; blade++) {
          final top = 4.85 + 0.5 * math.sin(blade * 1.7);
          final amount = ((7.3 - y) / (7.3 - top)).clamp(0.0, 1.0);
          final stemX =
              blade * 0.95 -
              0.3 +
              math.sin(phase + blade * 0.8) * 0.32 * amount * amount;
          if (y >= top && y <= 7.3 && (x - stemX).abs() < 0.14) {
            color = 0x56c46c;
          }
        }
        for (var flower = 0; flower < 3; flower++) {
          final sway = 0.15 * math.sin(phase + flower);
          final stemX = 1 + flower * 2.5 + sway;
          final top = 5.0 + 0.4 * math.sin(flower.toDouble());
          if ((x - stemX).abs() < 0.12 && y > top) color = 0x44b96b;
          color = _mix(
            color,
            flower == 1 ? 0xfface1 : 0xbcb0ff,
            _spot(x - stemX, y - top, 0.55),
          );
        }
        for (var fly = 0; fly < 7; fly++) {
          final angle = phase + fly * 2.39996;
          var cx = 3.5 + 3.35 * math.sin(angle * (fly % 2 + 1));
          var cy = 2.8 + 2.25 * math.cos(angle + fly);
          if (fly == 0) {
            final path = t * 8, row = path.floor(), progress = path % 1;
            cx = row.isEven ? -0.7 + progress * 8.4 : 7.7 - progress * 8.4;
            cy = row.toDouble();
          }
          final light = _spot(x - cx, y - cy, fly == 0 ? 0.9 : 0.48);
          color = _mix(color, 0xe8ff85, light);
          if (light > 0.5) {
            effect = LitchcraftEffectStyle.breathe;
            effectMs = 1500 + fly % 3 * 500;
          }
        }
      case moonLaunch:
        // Sparse stars take turns rather than tinting the empty sky.
        final age = (frame + emitter * 17) % frameCount;
        if (age < 11) color = _mix(0, 0xb8d8ff, math.sin(math.pi * age / 11));
        color = _mix(color, 0xffe2a0, _spot(x - 6.1, y - 0.6, 1.05));
        final rocketY = 3.4 - 3.4 * math.sin(phase);
        final rocketX = 3.0 + 0.5 * math.sin(phase * 2);
        final dx = (x - rocketX).abs(), dy = y - rocketY;
        final body =
            dy > -1.8 && dy < 1.15 && dx < 0.95 && dx < (dy + 1.9) * 1.2;
        if (body) {
          color = dy < -0.9 ? 0xff6964 : 0xffe9ce;
          if (dy > -0.7 && dy < 0.4 && dx < 0.52) color = 0x41d9ff;
        } else if (dy > 0.15 &&
            dy < 1.45 &&
            dx < 1.55 &&
            dx > 0.8 &&
            dx < 1.7 - dy * 0.25) {
          color = 0xff7461;
        } else if (dy > 1.1 && dy < 4.1) {
          final fireWidth =
              (4.1 - dy) * (0.36 + 0.07 * math.sin(phase * 11 + dy * 4));
          if (dx < fireWidth) {
            color = dx < fireWidth * 0.52 ? 0xffef7e : 0xff7826;
          }
          if (dy > 2.3 && dx < 0.2 && math.sin(phase * 13 + dy * 3) > 0.1) {
            color = 0xffffff;
          }
        }
      case rainbowRibbon:
        for (var ribbon = 2; ribbon >= 0; ribbon--) {
          for (var tail = 12; tail >= 0; tail--) {
            final angle = phase + ribbon * math.pi * 2 / 3 - tail * 0.055;
            final cx = 3.5 + 3.1 * math.sin(angle);
            final cy = 3.5 + 2.6 * math.sin(angle * 2);
            final weight = _spot(x - cx, y - cy, 0.64) * (1 - tail / 16);
            color = _mix(
              color,
              const [0xffb45b, 0x46ffd3, 0xd39bff][ribbon],
              math.sqrt(weight),
            );
            if (tail == 0 && weight > 0.8 && t > 0.15 && t < 0.35) {
              effect = LitchcraftEffectStyle.rainbow;
              effectMs = 4000;
            }
          }
        }
        if (t > 0.6 && t < 0.95) {
          final ring = _ring(x - 3.5, y - 3.5, (t - 0.6) / 0.35 * 6, 0.8);
          color = _mix(color, 0x75ffe0, ring * 0.8);
        }
    }
    // This authored palette deliberately uses black negative space. This is
    // specific to these six presets, never a clamp on portable user artwork.
    if (_maximumChannel(color) < 40) color = 0;
    return LitchcraftAnimatedEmitter(
      color: color,
      effect: color == 0 ? LitchcraftEffectStyle.off : effect,
      effectDurationMs: color == 0 ? 0 : effectMs,
    );
  }
}

double _spot(double x, double y, double radius) =>
    (1 - math.sqrt(x * x + y * y) / radius).clamp(0.0, 1.0);

double _ring(double x, double y, double radius, double width) =>
    (1 - (math.sqrt(x * x + y * y) - radius).abs() / width).clamp(0.0, 1.0);

int _foxiePixel(double u, double v) {
  if (u < 0 || v < 0 || u >= 1 || v >= 1) return 0;
  return litchcraftFoxiePalette[litchcraftFoxieRaster[(v * 64).floor()]
          .codeUnitAt((u * 64).floor()) -
      48];
}

int _maximumChannel(int color) =>
    math.max(color >> 16 & 255, math.max(color >> 8 & 255, color & 255));

int _mix(int a, int b, double amount) {
  final t = amount.clamp(0.0, 1.0);
  int channel(int shift) =>
      (((a >> shift) & 255) * (1 - t) + ((b >> shift) & 255) * t).round();
  return channel(16) << 16 | channel(8) << 8 | channel(0);
}

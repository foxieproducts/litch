import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/widgets/cpu_thinking_robot.dart';

void main() {
  testWidgets('CPU robot paints the canonical board icon without redesign', (
    tester,
  ) async {
    final source = File(
      '../include/modules/menu_icons_6x6.hpp',
    ).readAsStringSync();
    final definition = RegExp(
      r'CPU_PLAYER_ICON\s*=\s*\{\s*\{([\s\S]*?)\}',
    ).firstMatch(source)!.group(1)!;
    final rows = RegExp(
      r'"([.CWB]{6})"',
    ).allMatches(definition).map((match) => match.group(1)!).toList();
    expect(rows, hasLength(6));
    expect(CpuThinkingRobot.pixels, rows);

    await tester.pumpWidget(const Center(child: CpuThinkingRobot()));
    expect(tester.getSize(find.byType(CpuThinkingRobot)), const Size(36, 36));
    final paint = tester.widget<CustomPaint>(
      find.descendant(
        of: find.byType(CpuThinkingRobot),
        matching: find.byType(CustomPaint),
      ),
    );
    final recorder = ui.PictureRecorder();
    paint.painter!.paint(Canvas(recorder), const Size(36, 36));
    final picture = recorder.endRecording();
    final bytes = await tester.runAsync(() async {
      final image = await picture.toImage(36, 36);
      final bytes = await image.toByteData(format: ui.ImageByteFormat.rawRgba);
      image.dispose();
      return bytes!;
    });
    picture.dispose();
    const colors = {
      '.': 0x202A2CFF,
      'C': 0x00FFFFFF,
      'W': 0xFFFFFFFF,
      'B': 0x0000FFFF,
    };
    for (int y = 0; y < 6; ++y) {
      for (int x = 0; x < 6; ++x) {
        final offset = ((y * 6 + 3) * 36 + x * 6 + 3) * 4;
        expect(
          bytes!.getUint32(offset),
          colors[rows[y][x]],
          reason: 'Board pixel $x,$y',
        );
      }
    }
  });
}

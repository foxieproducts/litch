import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/litch_board.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/screens/home_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';

import 'health_render_capture.dart';
import 'virtual_litch/board_led_expectations.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

const _boardId = 'virtual-litch-production';
final _opening = BigInt.parse('FFFF00000000FFFF', radix: 16);

BigInt _physicalMask(BigInt logical, bool rotate) {
  if (!rotate) return logical;
  var physical = BigInt.zero;
  for (int square = 0; square < 64; ++square) {
    if ((logical & (BigInt.one << square)) != BigInt.zero) {
      physical |= BigInt.one << (63 - square);
    }
  }
  return physical;
}

Future<void> _checkpoint(
  WidgetTester tester,
  VirtualLitchBle ble,
  LitchBleService service,
  String name,
  String nextAction, {
  List<int> guided = const [],
  bool capture = false,
  bool cpuDirected = false,
  bool cpuDuckDirected = false,
}) async {
  final frame = await publishedBoardFrame(tester, ble);
  final board = frame['board'] as List<dynamic>;
  expectOrdinaryEmptyCenters(
    ble,
    frame,
    name,
    cpuDirected: cpuDirected,
    cpuDuckDirected: cpuDuckDirected,
  );
  final occupancy = BigInt.parse(frame['occupancy'] as String, radix: 16);
  final emptyCenters = <String, List<dynamic>>{
    for (var square = 0; square < 64; square++)
      if ((occupancy & (BigInt.one << square)) == BigInt.zero &&
          (board[square][1] as List).any((channel) => channel != 0))
        algebraicSquare(square): board[square][1] as List<dynamic>,
  };
  for (final square in guided) {
    expect(
      [
        ...(board[square][0] as List),
        ...(board[square][2] as List),
      ].any((channel) => channel != 0),
      isTrue,
      reason: '$name side-pair guidance ${algebraicSquare(square)}',
    );
  }
  final home = publishedVirtualHome(ble, frame['home_sequence'] as int);
  final current = service.latestHomeState!;
  expect(current.position.fen, home['fen'], reason: name);
  expect(current.physicalStatus.occupiedSquares, occupancy, reason: name);
  final evidence = <String, dynamic>{
    'name': name,
    'flutter_time': tester.binding.clock.now().toIso8601String(),
    'home': home,
    'next_action': nextAction,
    'frame': frame,
  };
  final output = Platform.environment['LITCH_VIRTUAL_EVIDENCE'];
  if (output != null) {
    await tester.runAsync(() async {
      final file = File('$output/$name-leds.json');
      await file.parent.create(recursive: true);
      await file.writeAsString(
        const JsonEncoder.withIndent('  ').convert(evidence),
      );
    });
  }
  if (capture) await captureHealthReview(tester, name);
  debugPrint(
    'CPU_DUCK_WHOLE_TURN $name native=${frame['now_ms']}ms '
    'Home=${jsonEncode(home)} occupancy=${frame['occupancy']} '
    'required=${frame['required_action']} token=${frame['pending_token']} '
    'empty_centers=${jsonEncode(emptyCenters)} '
    'display="${frame['display']}" next=$nextAction',
  );
}

void main() {
  for (final (cpuOwner, scale) in [(1, 1), (2, 1), (1, 2), (2, 2)]) {
    for (final firstPlayer in scale == 1 ? [1, 2] : [1]) {
      for (final rotate in scale == 1 ? [false, true] : [false]) {
        final name =
            'cpu-duck-p$cpuOwner-end$firstPlayer-rot${rotate ? 1 : 0}'
            '${scale == 2 ? '-2x' : ''}';
        testWidgets(
          'CpuDuckWholeTurn $name',
          (tester) async {
            tester.view.physicalSize = const Size(390, 844);
            tester.view.devicePixelRatio = 1;
            addTearDown(tester.view.resetPhysicalSize);
            addTearDown(tester.view.resetDevicePixelRatio);
            await loadHealthReviewFonts(tester, useSdkFonts: true);
            final native = (await tester.runAsync(
              () => VirtualLitchProcess.start(firstPlayer, rotate),
            ))!;
            final ble = VirtualLitchGattBle(native);
            final service = ble.createService();
            addTearDown(() => ble.disposeJourney(tester, service));
            final semantics = tester.ensureSemantics();
            final squareRotate = rotate != (firstPlayer == 2);
            var occupancy = _opening | (BigInt.one << 27); // Initial Duck d5.

            Future<void> hall() async {
              await ble.command(tester, {
                'op': 'hall',
                'mask': _physicalMask(
                  occupancy,
                  squareRotate,
                ).toRadixString(16).padLeft(16, '0'),
              });
              await ble.advance(tester, 100);
            }

            // The binary Home sequence belongs to this app connection. The
            // independently observed JSON publication has its own native sequence.
            Future<Map<String, dynamic>> nativeHome() async {
              final frame = await publishedBoardFrame(tester, ble);
              return publishedVirtualHome(ble, frame['home_sequence'] as int);
            }

            Future<void> move(int from, int to) async {
              occupancy &= ~(BigInt.one << from);
              await hall();
              occupancy |= BigInt.one << to;
              await hall();
            }

            try {
              final connected = service.connect(_boardId);
              await ble.flush(tester);
              expect(await connected, isTrue);
              await ble.advance(tester, 100);
              // Configure the logical opponent before the real Hall setup
              // becomes playable. No injected engine result or board plan.
              await ble.confirmSetting(
                tester,
                service,
                'VS_CPU_PLAYER',
                cpuOwner,
              );
              await ble.confirmSetting(tester, service, 'VS_CPU_LEVEL', 1);
              await hall();
              await ble.advance(tester, 2400); // Latch and setup sweep expire.
              var modeResolved = false;
              await tester.pumpWidget(
                MaterialApp(
                  theme: buildLitchTheme(),
                  builder: (context, child) => MediaQuery(
                    data: MediaQuery.of(
                      context,
                    ).copyWith(textScaler: TextScaler.linear(scale.toDouble())),
                    child: child!,
                  ),
                  home: RepaintBoundary(
                    child: LitchMainScaffold(
                      bleService: service,
                      boardId: _boardId,
                      boards: const [
                        LitchBoard(
                          id: _boardId,
                          name: 'Foxie_Litch',
                          rssi: -45,
                        ),
                      ],
                      onDisconnect: service.disconnect,
                      onModeResolved: () => modeResolved = true,
                      loadAppConsolePreference: () async => false,
                    ),
                  ),
                ),
              );
              await ble.flush(tester);
              await ble.advance(tester, 100);
              await ble.advanceUntil(
                tester,
                () =>
                    modeResolved &&
                    find.byType(HomeScreen).evaluate().isNotEmpty,
                'The inspected inactive mode must mount Play before its move cues',
              );
              expect(service.latestHomeState?.gameStatus.variant, 'duck');

              if (cpuOwner == 2) {
                await move(52, 36); // Human White e2-e4.
                await ble.advanceUntil(
                  tester,
                  () => service.latestHomeState?.action == HomeStateAction.duck,
                  'Human chess acknowledgement must hand off to Duck placement',
                );
                expect(find.text('YOUR MOVE'), findsOneWidget);
                expect(
                  find.text('Move the duck to a different empty square.'),
                  findsOneWidget,
                );
                await _checkpoint(
                  tester,
                  ble,
                  service,
                  '$name-human-selects',
                  'Choose a different empty square for the Duck',
                  guided: [27],
                );
                await move(27, 24); // Human chooses d5-a5, no app engine.
              }
              await ble.advanceUntil(
                tester,
                () =>
                    service.latestHomeState?.action == HomeStateAction.cpu &&
                    service.latestHomeState?.physicalMovePrompt != null,
                'Real Duck engine must select a CPU compound move',
              );
              final cpu = service.latestHomeState!.physicalMovePrompt!;
              expect(find.text('COMPUTER MOVE'), findsOneWidget);
              await _checkpoint(
                tester,
                ble,
                service,
                '$name-chess',
                'Move the computer piece ${algebraicSquare(cpu.fromSquare)} to ${algebraicSquare(cpu.toSquare)}',
                guided: [cpu.fromSquare, cpu.toSquare],
                cpuDirected: true,
              );
              if (cpu.captureSquare != null) {
                occupancy &= ~(BigInt.one << cpu.captureSquare!);
                await hall();
              }
              await move(cpu.fromSquare, cpu.toSquare);
              await ble.advanceUntil(
                tester,
                () => service.latestHomeState?.action == HomeStateAction.duck,
                'CPU chess acknowledgement must hand off to its chosen Duck target',
              );
              // This assertion is the owner-visible regression: old firmware/app
              // labelled the computer's Duck step YOUR MOVE.
              await captureHealthReview(
                tester,
                '$name-duck-handoff-before-assert',
              );
              expect(find.text('COMPUTER MOVE'), findsOneWidget);
              expect(find.text('YOUR MOVE'), findsNothing);
              expect(find.text('Place the duck'), findsOneWidget);
              final wire = await nativeHome();
              expect(wire['dr'], matches(RegExp(r'^c[a-h][1-8][a-h][1-8]$')));
              final plan = wire['dr'] as String;
              final from = tryParseAlgebraicSquare(plan.substring(1, 3))!;
              final to = tryParseAlgebraicSquare(plan.substring(3, 5))!;
              final appPlan = service.latestHomeState!.duckRelocationPrompt!;
              expect(
                (appPlan.owner, appPlan.fromSquare, appPlan.toSquare),
                (HomeDuckRelocationOwner.cpu, from, to),
              );
              expect(
                find.text(
                  'Move the duck from ${algebraicSquare(from)} to ${algebraicSquare(to)}.',
                ),
                findsOneWidget,
              );
              expect(
                find.byKey(ValueKey('cpu-move-from-${algebraicSquare(from)}')),
                findsOneWidget,
              );
              expect(
                find.byKey(ValueKey('cpu-move-to-${algebraicSquare(to)}')),
                findsOneWidget,
              );
              final retired = ble.emitted
                  .where((packet) => packet.sessionPacket?.isHomeState == true)
                  .toList();
              expect(retired, isNotEmpty);
              await _checkpoint(
                tester,
                ble,
                service,
                '$name-duck-ready',
                'Lift the Duck from ${algebraicSquare(from)}',
                guided: [from, to],
                cpuDuckDirected: true,
                capture: true,
              );
              occupancy &= ~(BigInt.one << from);
              await hall();
              expect((await nativeHome())['dr'], plan);
              await _checkpoint(
                tester,
                ble,
                service,
                '$name-duck-lifted',
                'Place the Duck on ${algebraicSquare(to)}',
                guided: [from, to],
                cpuDuckDirected: true,
              );
              final wrong = List.generate(64, (square) => square).firstWhere(
                (square) =>
                    square != from &&
                    square != to &&
                    (occupancy & (BigInt.one << square)) == BigInt.zero,
              );
              occupancy |= BigInt.one << wrong;
              await hall();
              final correctionFrame = await ble.frame(tester);
              expect(correctionFrame['required_action'], 9); // CorrectBoard.
              await _checkpoint(
                tester,
                ble,
                service,
                '$name-wrong-square',
                'Lift the Duck from the incorrect square ${algebraicSquare(wrong)}',
                guided: [wrong],
                capture: true,
              );
              final correction = await nativeHome();
              expect(correction['a'], 'corr');
              expect(correction['dr'], plan);
              expect(correction['fen'], wire['fen']);
              expect(
                service.latestHomeState!.gameStatus.duckMovePending,
                isTrue,
              );
              expect(find.text('COMPUTER MOVE'), findsNothing);
              expect(find.text('Place the duck'), findsNothing);
              expect(find.text('POSITION NEEDS CORRECTION'), findsOneWidget);
              expect(find.text('Follow the board’s guidance'), findsOneWidget);
              expect(
                find.textContaining('The board’s LEDs are guiding'),
                findsNothing,
              );
              expect(find.textContaining('expected on'), findsNothing);
              for (final square in [cpu.fromSquare, cpu.toSquare]) {
                expect(
                  tester
                      .getSemantics(
                        find.byKey(
                          ValueKey('square-${algebraicSquare(square)}'),
                        ),
                      )
                      .label,
                  isNot(contains('expected on')),
                );
              }
              expect(
                find.byKey(ValueKey('cpu-move-to-${algebraicSquare(to)}')),
                findsNothing,
              );
              occupancy &= ~(BigInt.one << wrong);
              await hall();
              expect(service.latestHomeState!.action, HomeStateAction.duck);
              expect((await nativeHome())['dr'], plan);
              expect(find.text('COMPUTER MOVE'), findsOneWidget);
              await _checkpoint(
                tester,
                ble,
                service,
                '$name-recovered',
                'Place the Duck on ${algebraicSquare(to)}',
                guided: [to],
                cpuDuckDirected: true,
              );
              occupancy |= BigInt.one << to;
              await hall();
              await ble.advance(tester, 300);
              expect(service.latestHomeState!.gameStatus.duckSquare, to);
              expect(
                service.latestHomeState!.gameStatus.duckMovePending,
                isFalse,
              );
              expect(
                service.latestHomeState!.position.sideToMove,
                cpuOwner == 1 ? HomeSide.black : HomeSide.white,
              );
              expect((await nativeHome()).containsKey('dr'), isFalse);
              expect(
                find.byKey(ValueKey('cpu-move-to-${algebraicSquare(to)}')),
                findsNothing,
              );
              final committed = await ble.frame(tester);
              expect(committed['pending_token'], 0);
              expect(committed['occupancy'], committed['expected']);
              final committedFen = service.latestHomeState!.position.fen;
              await ble.advance(tester, 300);
              expect(service.latestHomeState!.position.fen, committedFen);
              await _checkpoint(
                tester,
                ble,
                service,
                '$name-committed',
                'Human P${cpuOwner == 1 ? 2 : 1} makes the next chess move',
                capture: true,
              );

              final disconnected = service.disconnect();
              await ble.flush(tester);
              await disconnected;
              expect(service.latestHomeState, isNull);
              final reconnected = service.connect(_boardId);
              await ble.flush(tester);
              expect(await reconnected, isTrue);
              await ble.advance(tester, 100);
              await ble.advanceUntil(
                tester,
                () => service.latestHomeState?.position.fen == committedFen,
                'The new session must hydrate the committed human turn',
              );
              final current = service.latestHomeState;
              for (final packet in retired) {
                ble.deliver(packet, lateCallback: true);
              }
              await tester.pump();
              expect(identical(service.latestHomeState, current), isTrue);
              expect(
                find.byKey(ValueKey('cpu-move-to-${algebraicSquare(to)}')),
                findsNothing,
              );
              await _checkpoint(
                tester,
                ble,
                service,
                '$name-reconnected',
                'Continue the acknowledged human turn; ignore retired CPU callbacks',
              );
              expect(tester.takeException(), isNull);
            } finally {
              semantics.dispose();
              final disconnected = service.disconnect();
              await ble.flush(tester);
              await disconnected;
              await tester.pumpWidget(const SizedBox.shrink());
            }
          },
          variant: TargetPlatformVariant.only(TargetPlatform.iOS),
          skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
        );
      }
    }
  }
}

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';

void main() {
  test('bounded merge joins RGB and effects but preserves erase coupling', () {
    final common = staticLitchcraftDocument(
      LitchcraftCanvas.blank().paint([0], 15, 0x123456),
    );
    final local = LitchcraftDocumentSession(common)
      ..confirm('board', common)
      ..paintSelectedFrame(squares: [0], mask: 1, color: 0xabcdef);
    final physical = LitchcraftDocumentSession(common)
      ..applyEffect(square: 0, mask: 1, effect: LitchcraftEffectStyle.breathe);
    expect(
      local.observeBoard('board', physical.document),
      LitchcraftDocumentResolution.merge,
    );
    local.mergeBoard();
    expect(local.document.frames[0].emitters[0].color, 0xabcdef);
    expect(
      local.document.frames[0].emitters[0].effect,
      LitchcraftEffectStyle.breathe,
    );
    final erased = LitchcraftDocumentSession(common)
      ..paintSelectedFrame(squares: [0], mask: 1, color: 0, clearEffects: true);
    expect(
      mergeLitchcraftDocuments(common, erased.document, physical.document),
      isNull,
    );
    final competing = LitchcraftDocumentSession(common)
      ..paintSelectedFrame(squares: [0], mask: 1, color: 0x654321);
    expect(
      mergeLitchcraftDocuments(common, local.document, competing.document),
      isNull,
    );
    final structural = LitchcraftDocumentSession(common)..addFrame();
    expect(
      mergeLitchcraftDocuments(common, structural.document, physical.document),
      isNull,
    );
  });

  test('accepted prefix proof tolerates independent physical fields', () {
    final before = staticLitchcraftDocument(LitchcraftCanvas.blank());
    final accepted = patchLitchcraftEmitters(
      before,
      frame: 0,
      squares: [7],
      mask: 15,
      color: 0xff7430,
    );
    final board = patchLitchcraftEmitters(
      accepted,
      frame: 0,
      squares: [0],
      mask: 15,
      color: 0x123456,
    );
    expect(matchesLitchcraftDocumentPatch(before, accepted, board), isTrue);
    final overwritten = patchLitchcraftEmitters(
      board,
      frame: 0,
      squares: [7],
      mask: 15,
      color: 0x654321,
    );
    expect(
      matchesLitchcraftDocumentPatch(before, accepted, overwritten),
      isFalse,
    );
  });
  test(
    'duplicate favorite order is exact document reconciliation identity',
    () {
      final favorites = [1, 1, 2, 3, 4, 5, 6, 7];
      final source = staticLitchcraftDocument(
        LitchcraftCanvas.blank(),
        favoriteColors: favorites,
      );
      final session = LitchcraftDocumentSession(source)
        ..confirm('board', source);
      expect(session.document.favoriteColors, favorites);
      session.setFavoriteColors([1, 2, 1, 3, 4, 5, 6, 7]);
      expect(sameLitchcraftDocument(source, session.document), isFalse);
      expect(
        session.observeBoard('board', source),
        LitchcraftDocumentResolution.sendApp,
      );
    },
  );
  LitchcraftDocumentSession fresh() => LitchcraftDocumentSession(
    staticLitchcraftDocument(LitchcraftCanvas.blank()),
  );

  test('one physical footprint projects once and preserves local Undo', () {
    final session = fresh();
    session.confirm('board', session.document);
    session.paintSelectedFrame(squares: <int>[7], mask: 1, color: 0xff7430);
    var notifications = 0;
    session.addListener(() => notifications++);

    expect(
      session.applyBoardPaintBatch(
        boardId: 'board',
        frame: 0,
        edits: <LitchcraftPhysicalPaint>[
          (
            squares: List<int>.generate(64, (square) => square),
            mask: 15,
            color: 0x1288cc,
            clearEffects: false,
          ),
        ],
      ),
      isTrue,
    );
    expect(notifications, 1);
    expect(session.document.frames.single.emitters[7 * 4].color, 0xff7430);
    expect(session.document.frames.single.emitters[7 * 4 + 1].color, 0x1288cc);
    expect(session.document.frames.single.emitters[63 * 4].color, 0x1288cc);
    expect(
      session.boardDocument!.frames.single.emitters[7 * 4].color,
      0x1288cc,
    );

    session.undo();
    expect(session.document.frames.single.emitters[7 * 4].color, 0x1288cc);
    expect(session.document.frames.single.emitters[63 * 4].color, 0x1288cc);
  });

  test('confirmed history echo preserves Redo and pending local intent', () {
    final session = fresh();
    session.paintSelectedFrame(squares: [0], mask: 15, color: 0x123456);
    session.confirm('board', session.document);
    session.undo();
    session.paintSelectedFrame(
      squares: [1],
      mask: 15,
      color: 0xabcdef,
      history: false,
    );
    expect(
      session.applyBoardPaintBatch(
        boardId: 'board',
        frame: 0,
        historyPreview: true,
        edits: [
          (squares: [0], mask: 15, color: 0, clearEffects: true),
        ],
      ),
      isTrue,
    );
    expect(session.canvas.colors.take(4), everyElement(0));
    expect(session.canvas.colors.skip(4).take(4), everyElement(0xabcdef));
    expect(session.nextUndoDocument, isNull);
    // paintSelectedFrame clears Redo as a new user edit. Rebuild that proof
    // without a competing edit to check its exact reverse echo separately.
    final known = fresh();
    known.paintSelectedFrame(squares: [0], mask: 15, color: 0x123456);
    known.confirm('board', known.document);
    known.undo();
    expect(
      known.applyBoardPaintBatch(
        boardId: 'board',
        frame: 0,
        historyPreview: true,
        edits: [
          (squares: [0], mask: 15, color: 0, clearEffects: true),
        ],
      ),
      isTrue,
    );
    expect(known.canRedo, isTrue);
    known.redo();
    expect(known.canvas.colors.take(4), everyElement(0x123456));
    session.retireLocalHistory();
    expect(
      session.canvas.colors.skip(4).take(4),
      everyElement(0xabcdef),
      reason: 'Retiring preview caches cannot discard the pending draft',
    );
  });

  test('retiring board lineage preserves the selected source and history', () {
    final session = fresh()
      ..addFrame()
      ..paintSelectedFrame(squares: [7], mask: 15, color: 0xabcdef)
      ..paintSelectedFrame(squares: [8], mask: 15, color: 0x654321)
      ..undo();
    final selected = session.document;
    session.confirm('board', selected);
    final frame = session.selectedFrameIndex;
    final source = session.replacementGeneration;
    final generation = session.generation;
    final undo = session.nextUndoDocument;
    final redo = session.nextRedoDocument;
    expect(session.hasLocalIntent, isFalse);
    expect(session.hasCommonDocument, isTrue);
    expect(undo, isNotNull);
    expect(redo, isNotNull);

    session.retireBoardLineage();

    expect(session.document, same(selected));
    expect(session.selectedFrameIndex, frame);
    expect(session.replacementGeneration, source);
    expect(session.generation, generation);
    expect(session.nextUndoDocument, same(undo));
    expect(session.nextRedoDocument, same(redo));
    expect(session.hasLocalIntent, isTrue);
    expect(session.hasCommonDocument, isFalse);
    expect(session.boardDocument, isNull);
    final different = LitchcraftDocumentSession(selected)
      ..paintSelectedFrame(squares: [9], mask: 15, color: 0x123456);
    expect(
      session.observeBoard('board', different.document),
      LitchcraftDocumentResolution.conflict,
    );
    expect(
      session.observeBoard(
        'board',
        staticLitchcraftDocument(LitchcraftCanvas.blank()),
      ),
      LitchcraftDocumentResolution.sendApp,
    );
    expect(
      session.observeBoard('board', selected),
      LitchcraftDocumentResolution.equal,
    );
    expect(session.document, same(selected));
  });

  test('whole document common ancestor distinguishes equal RGB effects', () {
    final session = fresh()..addFrame();
    final common = session.document;
    session.confirm('board', common);
    session.applyEffect(
      square: 0,
      mask: 15,
      effect: LitchcraftEffectStyle.breathe,
    );
    final physical = LitchcraftDocumentSession(common)
      ..selectFrame(session.selectedFrameIndex)
      ..applyEffect(square: 0, mask: 15, effect: LitchcraftEffectStyle.rainbow);
    expect(session.canvas.colors, physical.canvas.colors);
    expect(
      session.observeBoard('board', physical.document),
      LitchcraftDocumentResolution.conflict,
    );
    expect(
      session.observeBoard('board', common),
      LitchcraftDocumentResolution.sendApp,
    );
    expect(
      session.observeBoard('board', session.document),
      LitchcraftDocumentResolution.equal,
    );
  });

  test('selection/projection does not manufacture local edits or history', () {
    final session = fresh()..addFrame();
    final generation = session.generation;
    session.selectFrame(0);
    session.replaceCanvas(session.canvas);
    expect(session.generation, generation);
    session.undo();
    expect(session.document.frames, hasLength(1));
    expect(session.canUndo, isFalse);
  });

  test('stale adoption and replacement confirmation preserve newer source', () {
    final session = fresh()..addFrame();
    final source = session.document;
    session.confirm('board', source);
    final generation = session.generation;
    session.observeBoard('board', source);
    session.applyEffect(
      square: 0,
      mask: 1,
      effect: LitchcraftEffectStyle.flash,
    );
    session.acceptBoard(expectedGeneration: generation);
    session.confirm('board', source);
    expect(
      session.document.frames[1].emitters[0].effect,
      LitchcraftEffectStyle.flash,
    );
    expect(
      session.observeBoard('board', source),
      LitchcraftDocumentResolution.sendApp,
    );
  });

  test('favorites seed preference and durations participate in equality', () {
    final session = fresh()..addFrame();
    final source = session.document;
    for (final changed in [
      LitchcraftAnimationDocument(
        frames: source.frames,
        topology: source.topology,
        favoriteColors: source.favoriteColors.reversed,
      ),
      LitchcraftAnimationDocument(
        frames: source.frames,
        topology: source.topology,
        favoriteColors: source.favoriteColors,
        sparkleSeed: 8,
      ),
      LitchcraftAnimationDocument(
        frames: source.frames,
        topology: source.topology,
        favoriteColors: source.favoriteColors,
        playbackOnEntry: true,
      ),
    ]) {
      expect(sameLitchcraftDocument(source, changed), isFalse);
    }
    session.setSelectedFrameDuration(2200);
    expect(sameLitchcraftDocument(source, session.document), isFalse);
  });

  test('counterclockwise rotation keeps effect metadata with its emitter', () {
    final emitters = List<LitchcraftAnimatedEmitter>.generate(
      256,
      (index) => LitchcraftAnimatedEmitter(
        color: index == 0 ? 0x112233 : 0,
        effect: index == 0
            ? LitchcraftEffectStyle.flash
            : LitchcraftEffectStyle.off,
        effectDurationMs: index == 0 ? 1700 : 0,
      ),
    );
    final session = LitchcraftDocumentSession(
      LitchcraftAnimationDocument(
        topology: LitchcraftTopology.v3,
        favoriteColors: litchcraftDefaultFavoriteColors,
        frames: <LitchcraftAnimationFrame>[
          LitchcraftAnimationFrame(emitters: emitters),
        ],
      ),
    );

    session.rotateSelectedFrame(-1);

    // Counterclockwise maps square 0/region 0 to square 56/region 3.
    const destinationEmitter = 56 * 4 + 3;
    final rotated = session.document.frames.single.emitters;
    expect(rotated[destinationEmitter].color, 0x112233);
    expect(rotated[destinationEmitter].effect, LitchcraftEffectStyle.flash);
    expect(rotated[destinationEmitter].effectDurationMs, 1700);
    expect(rotated[0].effect, LitchcraftEffectStyle.off);

    session.undo();
    expect(session.document.frames.single.emitters[0].color, 0x112233);
    expect(
      session.document.frames.single.emitters[0].effect,
      LitchcraftEffectStyle.flash,
    );
    expect(session.canUndo, isFalse, reason: 'rotation creates one undo step');
    session.redo();
    expect(
      session.document.frames.single.emitters[destinationEmitter].effect,
      LitchcraftEffectStyle.flash,
    );
  });

  test('fill and erase mutate exact masks with one history boundary', () {
    final session = fresh()
      ..applyEffect(
        square: 3,
        mask: 3,
        effect: LitchcraftEffectStyle.flash,
        effectDurationMs: 1700,
      );
    final beforeFill = session.document;

    session.paintSelectedFrame(
      squares: List<int>.generate(64, (index) => index),
      mask: 1,
      color: 0x123456,
    );

    final filled = session.document.frames.single.emitters;
    expect(<int>[
      for (var square = 0; square < 64; square++) filled[square * 4].color,
    ], everyElement(0x123456));
    expect(filled[3 * 4].effect, LitchcraftEffectStyle.flash);
    expect(filled[3 * 4 + 1].color, 0);
    session.undo();
    expect(sameLitchcraftDocument(session.document, beforeFill), isTrue);

    session.paintSelectedFrame(
      squares: const <int>[3],
      mask: 1,
      color: 0,
      clearEffects: true,
    );
    final erased = session.document.frames.single.emitters;
    expect(erased[3 * 4].color, 0);
    expect(erased[3 * 4].effect, LitchcraftEffectStyle.off);
    expect(erased[3 * 4].effectDurationMs, 0);
    expect(
      erased[3 * 4 + 1].effect,
      LitchcraftEffectStyle.flash,
      reason: 'erase affects only emitters selected by the mask',
    );
  });

  test('clear selected frame removes effect-only artwork atomically', () {
    final session = fresh()
      ..addFrame()
      ..applyEffect(square: 9, mask: 15, effect: LitchcraftEffectStyle.rainbow);
    final otherFrame = session.document.frames.first;

    session.clearSelectedFrame();

    expect(session.document.frames.first, same(otherFrame));
    expect(
      session.document.frames[1].emitters,
      everyElement(
        isA<LitchcraftAnimatedEmitter>()
            .having((emitter) => emitter.color, 'color', 0)
            .having(
              (emitter) => emitter.effect,
              'effect',
              LitchcraftEffectStyle.off,
            )
            .having((emitter) => emitter.effectDurationMs, 'duration', 0),
      ),
    );
    session.undo();
    expect(
      session.document.frames[1].emitters[9 * 4].effect,
      LitchcraftEffectStyle.rainbow,
    );
    expect(
      session.canUndo,
      isTrue,
      reason: 'clear is exactly one undo boundary',
    );
  });

  test('256-frame sparse strokes retain distinct bounded Undo steps', () {
    final blank = staticLitchcraftDocument(LitchcraftCanvas.blank());
    final original = LitchcraftAnimationDocument(
      frames: List.generate(
        256,
        (_) => LitchcraftAnimationFrame(emitters: blank.frames.first.emitters),
      ),
      topology: blank.topology,
      favoriteColors: blank.favoriteColors,
      playbackOnEntry: blank.playbackOnEntry,
      sparkleSeed: blank.sparkleSeed,
    );
    final session = LitchcraftDocumentSession(original);
    session.paintSelectedFrame(squares: const [7], mask: 15, color: 0xff7430);
    session.paintSelectedFrame(squares: const [8], mask: 15, color: 0xff7430);

    session.undo();
    expect(session.document.frames.first.emitters[28].color, 0xff7430);
    expect(session.document.frames.first.emitters[32].color, 0);
    session.undo();
    expect(sameLitchcraftDocument(session.document, original), isTrue);

    for (var edit = 0; edit < 20; edit++) {
      session.paintSelectedFrame(squares: [edit], mask: 15, color: edit + 1);
    }
    var retainedSteps = 0;
    while (session.canUndo) {
      session.undo();
      retainedSteps++;
    }
    expect(retainedSteps, greaterThanOrEqualTo(2));
    expect(
      retainedSteps,
      lessThan(20),
      reason: 'the 32 KiB history is bounded',
    );
  });

  test(
    '256-frame history rebase preserves physical paint and stays undoable',
    () {
      final blank = staticLitchcraftDocument(LitchcraftCanvas.blank());
      final original = LitchcraftAnimationDocument(
        frames: List.generate(
          256,
          (_) =>
              LitchcraftAnimationFrame(emitters: blank.frames.first.emitters),
        ),
        topology: blank.topology,
        favoriteColors: blank.favoriteColors,
        playbackOnEntry: blank.playbackOnEntry,
        sparkleSeed: blank.sparkleSeed,
      );
      final session = LitchcraftDocumentSession(original)
        ..confirm('board', original);
      session.paintSelectedFrame(squares: const [7], mask: 15, color: 0xff7430);
      session.paintSelectedFrame(squares: const [8], mask: 15, color: 0xff7430);
      final physical = patchLitchcraftEmitters(
        original,
        frame: 1,
        squares: const [0],
        mask: 1,
        color: 0x123456,
      );
      expect(
        session.observeBoard('board', physical),
        LitchcraftDocumentResolution.merge,
      );
      session.mergeBoard();
      session.undo();
      session.undo();
      expect(session.document.frames.first.emitters[28].color, 0);
      expect(session.document.frames[1].emitters.first.color, 0x123456);

      final patched = LitchcraftDocumentSession(original)
        ..confirm('board', original);
      patched.paintSelectedFrame(squares: const [7], mask: 15, color: 0xff7430);
      patched.paintSelectedFrame(squares: const [8], mask: 15, color: 0xff7430);
      expect(
        patched.applyBoardPaintBatch(
          boardId: 'board',
          frame: 1,
          edits: const <LitchcraftPhysicalPaint>[
            (squares: [0], mask: 1, color: 0x123456, clearEffects: false),
          ],
        ),
        isTrue,
      );
      patched.undo();
      patched.undo();
      expect(patched.document.frames.first.emitters[28].color, 0);
      expect(patched.document.frames[1].emitters.first.color, 0x123456);
    },
  );

  test(
    'different board cannot inherit common source and black effects are artwork',
    () {
      final session = fresh()..addFrame();
      final source = session.document;
      session.confirm('first', source);
      session.applyEffect(
        square: 0,
        mask: 1,
        effect: LitchcraftEffectStyle.breathe,
      );
      expect(
        session.observeBoard('second', source),
        LitchcraftDocumentResolution.conflict,
      );
    },
  );
}

import 'dart:collection';

import 'package:flutter/foundation.dart';

import 'litchcraft_animation.dart';
import 'litchcraft_state.dart';

class LitchcraftFrameLimit implements Exception {
  const LitchcraftFrameLimit();
}

enum LitchcraftDocumentResolution {
  equal,
  adoptBoard,
  sendApp,
  merge,
  conflict,
}

/// Frame slots have no identity on the wire. Only merge emitter fields when
/// all frame and document metadata still describe the same layout.
bool sameLitchcraftDocumentLayout(
  LitchcraftAnimationDocument left,
  LitchcraftAnimationDocument right,
) =>
    left.frames.length == right.frames.length &&
    left.topology == right.topology &&
    left.playbackOnEntry == right.playbackOnEntry &&
    left.sparkleSeed == right.sparkleSeed &&
    listEquals(left.favoriteColors, right.favoriteColors) &&
    List.generate(
      left.frames.length,
      (i) => i,
    ).every((i) => left.frames[i].durationMs == right.frames[i].durationMs);

bool _sameEffect(LitchcraftAnimatedEmitter a, LitchcraftAnimatedEmitter b) =>
    a.effect == b.effect && a.effectDurationMs == b.effectDurationMs;

typedef LitchcraftPhysicalPaint = ({
  List<int> squares,
  int mask,
  int color,
  bool clearEffects,
});

LitchcraftAnimatedEmitter _paintPhysicalEmitter(
  LitchcraftAnimatedEmitter prior,
  LitchcraftPhysicalPaint edit,
) => LitchcraftAnimatedEmitter(
  color: edit.color,
  effect: edit.clearEffects ? LitchcraftEffectStyle.off : prior.effect,
  effectDurationMs: edit.clearEffects ? 0 : prior.effectDurationMs,
);

LitchcraftAnimationDocument _withFrameEmitters(
  LitchcraftAnimationDocument document,
  int frame,
  List<LitchcraftAnimatedEmitter> emitters,
) {
  final frames = document.frames.toList();
  frames[frame] = LitchcraftAnimationFrame(
    emitters: emitters,
    durationMs: frames[frame].durationMs,
  );
  return LitchcraftAnimationDocument(
    frames: frames,
    topology: document.topology,
    favoriteColors: document.favoriteColors,
    playbackOnEntry: document.playbackOnEntry,
    sparkleSeed: document.sparkleSeed,
  );
}

LitchcraftAnimationDocument _patchPhysicalPaints(
  LitchcraftAnimationDocument document,
  int frame,
  List<LitchcraftPhysicalPaint> edits,
) {
  final emitters = document.frames[frame].emitters.toList();
  for (final edit in edits) {
    for (final square in edit.squares) {
      for (var region = 0; region < 4; region++) {
        if (edit.mask & (1 << region) == 0) continue;
        final index = square * 4 + region;
        emitters[index] = _paintPhysicalEmitter(emitters[index], edit);
      }
    }
  }
  return _withFrameEmitters(document, frame, emitters);
}

bool _canvasOwnsRgb(
  ({int frame, int mask, int action, BigInt effectEdits, bool preview})? canvas,
  int frame,
  int index,
) {
  if (canvas == null ||
      canvas.action >= 10 ||
      canvas.frame != frame ||
      canvas.mask & (1 << (index % 4)) == 0) {
    return false;
  }
  final square = index ~/ 4;
  return canvas.action < 5 ||
      (((square % 8) + (square ~/ 8)).isOdd) ==
          (canvas.action == 6 || canvas.action == 8);
}

bool _canvasOwnsEffect(
  ({int frame, int mask, int action, BigInt effectEdits, bool preview})? canvas,
  int frame,
  int index,
) =>
    _canvasOwnsRgb(canvas, frame, index) &&
    (canvas!.effectEdits & (BigInt.one << index)) != BigInt.zero;

/// RGB and effect/duration are independent fields, except an erase couples
/// them. A null result retains the existing explicit artwork choice.
LitchcraftAnimationDocument? mergeLitchcraftDocuments(
  LitchcraftAnimationDocument common,
  LitchcraftAnimationDocument local,
  LitchcraftAnimationDocument board, {
  ({int frame, int mask, int action, BigInt effectEdits, bool preview})?
  orderedCanvas,
}) {
  if (!sameLitchcraftDocumentLayout(common, local) ||
      !sameLitchcraftDocumentLayout(common, board)) {
    return null;
  }
  final frames = <LitchcraftAnimationFrame>[];
  for (var frame = 0; frame < common.frames.length; frame++) {
    final baseFrame = common.frames[frame];
    final appFrame = local.frames[frame];
    final physicalFrame = board.frames[frame];
    if (identical(appFrame, baseFrame) &&
        !(orderedCanvas?.frame == frame && orderedCanvas!.preview)) {
      frames.add(physicalFrame);
      continue;
    }
    if (identical(physicalFrame, baseFrame) && orderedCanvas?.frame != frame) {
      frames.add(appFrame);
      continue;
    }
    final emitters = <LitchcraftAnimatedEmitter>[];
    for (var index = 0; index < 256; index++) {
      final base = baseFrame.emitters[index];
      final app = appFrame.emitters[index];
      final physical = physicalFrame.emitters[index];
      final appColor = app.color != base.color;
      final boardColor = physical.color != base.color;
      final appEffect = !_sameEffect(app, base);
      final boardEffect = !_sameEffect(physical, base);
      final equal = app.color == physical.color && _sameEffect(app, physical);
      final appErase =
          app.color == 0 &&
          app.effect == LitchcraftEffectStyle.off &&
          (appColor || appEffect);
      final boardErase =
          physical.color == 0 &&
          physical.effect == LitchcraftEffectStyle.off &&
          (boardColor || boardEffect);
      final ownsRgb = _canvasOwnsRgb(orderedCanvas, frame, index);
      final preview = ownsRgb && orderedCanvas!.preview;
      final erasePreview =
          preview && (orderedCanvas.action == 1 || orderedCanvas.action >= 7);
      if (!ownsRgb &&
          !equal &&
          ((appColor && boardColor && app.color != physical.color) ||
              (appEffect && boardEffect && !_sameEffect(app, physical)) ||
              (appErase && (boardColor || boardEffect)) ||
              (boardErase && (appColor || appEffect)))) {
        return null;
      }
      final effect =
          _canvasOwnsEffect(orderedCanvas, frame, index) ||
              erasePreview ||
              (!ownsRgb && appEffect)
          ? app
          : physical;
      emitters.add(
        LitchcraftAnimatedEmitter(
          color: preview || appColor ? app.color : physical.color,
          effect: effect.effect,
          effectDurationMs: effect.effectDurationMs,
        ),
      );
    }
    frames.add(
      LitchcraftAnimationFrame(
        emitters: emitters,
        durationMs: baseFrame.durationMs,
      ),
    );
  }
  return LitchcraftAnimationDocument(
    frames: frames,
    topology: board.topology,
    favoriteColors: board.favoriteColors,
    playbackOnEntry: board.playbackOnEntry,
    sparkleSeed: board.sparkleSeed,
  );
}

/// A recovered snapshot can prove a lost command receipt even when physical
/// work also changed fields outside that command's footprint.
bool matchesLitchcraftDocumentPatch(
  LitchcraftAnimationDocument before,
  LitchcraftAnimationDocument accepted,
  LitchcraftAnimationDocument board,
) {
  if (!sameLitchcraftDocumentLayout(before, accepted) ||
      !sameLitchcraftDocumentLayout(before, board)) {
    return false;
  }
  var changed = false;
  for (var f = 0; f < before.frames.length; f++) {
    for (var i = 0; i < 256; i++) {
      final a = before.frames[f].emitters[i];
      final b = accepted.frames[f].emitters[i];
      final actual = board.frames[f].emitters[i];
      if (a.color != b.color) {
        changed = true;
        if (actual.color != b.color) return false;
      }
      if (!_sameEffect(a, b)) {
        changed = true;
        if (!_sameEffect(actual, b)) return false;
      }
    }
  }
  return changed;
}

/// Applies only the fields owned by one accepted minimum-ATT command.
LitchcraftAnimationDocument patchLitchcraftEmitters(
  LitchcraftAnimationDocument document, {
  required int frame,
  required Iterable<int> squares,
  required int mask,
  int? color,
  LitchcraftEffectStyle? effect,
  int effectDurationMs = 0,
}) => _patchLitchcraftEmitterMasks(
  document,
  frame: frame,
  squareMasks: <(int, int)>[for (final square in squares) (square, mask)],
  color: color,
  effect: effect,
  effectDurationMs: effectDurationMs,
);

LitchcraftAnimationDocument _patchLitchcraftEmitterMasks(
  LitchcraftAnimationDocument document, {
  required int frame,
  required Iterable<(int, int)> squareMasks,
  int? color,
  LitchcraftEffectStyle? effect,
  int effectDurationMs = 0,
}) {
  final frames = document.frames.toList();
  final old = frames[frame];
  final emitters = old.emitters.toList();
  for (final (square, mask) in squareMasks) {
    for (var region = 0; region < 4; region++) {
      if (mask & (1 << region) == 0) continue;
      final index = square * 4 + region;
      final prior = emitters[index];
      emitters[index] = LitchcraftAnimatedEmitter(
        color: color ?? prior.color,
        effect: effect ?? prior.effect,
        effectDurationMs: effect == null
            ? prior.effectDurationMs
            : effectDurationMs,
      );
    }
  }
  frames[frame] = LitchcraftAnimationFrame(
    emitters: emitters,
    durationMs: old.durationMs,
  );
  return LitchcraftAnimationDocument(
    frames: frames,
    topology: document.topology,
    favoriteColors: document.favoriteColors,
    playbackOnEntry: document.playbackOnEntry,
    sparkleSeed: document.sparkleSeed,
  );
}

bool sameLitchcraftDocument(
  LitchcraftAnimationDocument left,
  LitchcraftAnimationDocument right,
) {
  if (identical(left, right)) return true;
  if (!sameLitchcraftDocumentLayout(left, right)) return false;
  for (var f = 0; f < left.frames.length; f++) {
    if (identical(left.frames[f], right.frames[f])) continue;
    for (var i = 0; i < 256; i++) {
      final a = left.frames[f].emitters[i], b = right.frames[f].emitters[i];
      if (a.color != b.color || !_sameEffect(a, b)) return false;
    }
  }
  return true;
}

bool isEmptyLitchcraftDocument(LitchcraftAnimationDocument document) =>
    document.frames.length == 1 &&
    document.frames.single.emitters.every(
      (emitter) =>
          emitter.color == 0 && emitter.effect == LitchcraftEffectStyle.off,
    );

bool isAdvancedLitchcraftDocument(LitchcraftAnimationDocument document) =>
    document.frames.length != 1 ||
    document.frames.single.durationMs !=
        litchcraftAnimationDefaultFrameDurationMs ||
    document.playbackOnEntry ||
    document.sparkleSeed != 1 ||
    document.frames.single.emitters.any(
      (emitter) =>
          emitter.effect != LitchcraftEffectStyle.off ||
          emitter.effectDurationMs != 0,
    );

LitchcraftAnimationDocument staticLitchcraftDocument(
  LitchcraftCanvas canvas, {
  Iterable<int> favoriteColors = litchcraftDefaultFavoriteColors,
}) => LitchcraftAnimationDocument(
  frames: [
    LitchcraftAnimationFrame(
      emitters: canvas.colors.map(
        (color) => LitchcraftAnimatedEmitter(color: color),
      ),
    ),
  ],
  topology: canvas.topology,
  favoriteColors: favoriteColors,
);

/// The sole editable document owner. Transport authority is held by the BLE
/// controller; these immutable board/common documents are comparison evidence,
/// never another editable copy. Selection is presentation, not a document edit.
class LitchcraftDocumentSession extends ChangeNotifier {
  LitchcraftDocumentSession(
    LitchcraftAnimationDocument document, {
    bool localIntent = true,
  }) : _document = document,
       _hasLocalIntent = localIntent;

  LitchcraftAnimationDocument _document;
  int _selectedFrame = 0, _generation = 0, _replacementGeneration = 0;
  bool _hasLocalIntent;
  bool _overlappingBoardPaint = false;
  ({int frame, int square, int mask})? _localEffectEdit;

  /// Explicit local field provenance, present only during applyEffect's
  /// synchronous notification; board projection and history never set it.
  ({int frame, int square, int mask})? get localEffectEdit => _localEffectEdit;
  final List<LitchcraftAnimationDocument> _undo = [], _redo = [];
  LitchcraftAnimationDocument? _common, _board;
  LitchcraftAnimationDocument? _strokeBefore;
  String? _boardId;

  LitchcraftAnimationDocument get document => _document;
  LitchcraftAnimationDocument? get boardDocument => _board;
  bool get hasCommonDocument => _common != null;
  bool get hasLocalIntent => _hasLocalIntent;
  bool get overlappingBoardPaint => _overlappingBoardPaint;
  int get selectedFrameIndex => _selectedFrame;
  int get generation => _generation;
  int get replacementGeneration => _replacementGeneration;
  bool get isAdvanced => isAdvancedLitchcraftDocument(_document);
  bool get canUndo => _undo.isNotEmpty;
  bool get canRedo => _redo.isNotEmpty;
  LitchcraftAnimationDocument? get nextUndoDocument =>
      _undo.isEmpty ? null : _undo.last;
  LitchcraftAnimationDocument? get nextRedoDocument =>
      _redo.isEmpty ? null : _redo.last;
  LitchcraftCanvas get canvas => LitchcraftCanvas(
    colors: _document.frames[_selectedFrame].emitters.map((e) => e.color),
    topology: _document.topology,
  );

  LitchcraftAnimationDocument _copy({
    Iterable<LitchcraftAnimationFrame>? frames,
    Iterable<int>? favorites,
  }) => LitchcraftAnimationDocument(
    frames: frames ?? _document.frames,
    topology: _document.topology,
    favoriteColors: favorites ?? _document.favoriteColors,
    playbackOnEntry: _document.playbackOnEntry,
    sparkleSeed: _document.sparkleSeed,
  );

  /// Opening/restoring a source retires all in-flight document results, even
  /// when its contents happen to equal the preceding work.
  void open(LitchcraftAnimationDocument document, {bool localIntent = true}) {
    _document = document;
    _selectedFrame = 0;
    _generation++;
    _replacementGeneration++;
    _hasLocalIntent = localIntent;
    _overlappingBoardPaint = false;
    _undo.clear();
    _redo.clear();
    _strokeBefore = null;
    notifyListeners();
  }

  void _edit(
    LitchcraftAnimationDocument next, {
    int? selection,
    bool history = true,
  }) {
    if (sameLitchcraftDocument(_document, next)) return;
    if (history) _push(_undo, _document, current: next);
    _redo.clear();
    _document = next;
    _selectedFrame = selection ?? _selectedFrame;
    _generation++;
    _hasLocalIntent = true;
    notifyListeners();
  }

  void replaceCanvas(LitchcraftCanvas canvas, {bool history = true}) {
    if (listEquals(canvas.colors, this.canvas.colors)) return;
    final frames = _document.frames.toList();
    final frame = frames[_selectedFrame];
    frames[_selectedFrame] = LitchcraftAnimationFrame(
      durationMs: frame.durationMs,
      emitters: List.generate(
        256,
        (index) => LitchcraftAnimatedEmitter(
          color: canvas.colors[index],
          effect: frame.emitters[index].effect,
          effectDurationMs: frame.emitters[index].effectDurationMs,
        ),
      ),
    );
    _edit(_copy(frames: frames), history: history);
  }

  /// Paints exact emitter slots on the selected frame as one document edit.
  /// Erase callers set [clearEffects] so invisible effect metadata cannot
  /// survive after the user clears an emitter.
  void paintSelectedFrame({
    required Iterable<int> squares,
    required int mask,
    required int color,
    bool clearEffects = false,
    bool history = true,
  }) {
    if (mask < 1 || mask > 15 || color < 0 || color > 0xffffff) {
      throw RangeError('Invalid Litchcraft paint selection');
    }
    final selectedSquares = squares.toSet();
    if (selectedSquares.any((square) => square < 0 || square >= 64)) {
      throw RangeError('Invalid Litchcraft paint selection');
    }
    final frames = _document.frames.toList();
    final frame = frames[_selectedFrame];
    final emitters = frame.emitters.toList();
    for (final square in selectedSquares) {
      for (var region = 0; region < 4; region++) {
        if (mask & (1 << region) == 0) continue;
        final index = square * 4 + region;
        final emitter = emitters[index];
        emitters[index] = LitchcraftAnimatedEmitter(
          color: color,
          effect: clearEffects ? LitchcraftEffectStyle.off : emitter.effect,
          effectDurationMs: clearEffects ? 0 : emitter.effectDurationMs,
        );
      }
    }
    frames[_selectedFrame] = LitchcraftAnimationFrame(
      emitters: emitters,
      durationMs: frame.durationMs,
    );
    _edit(_copy(frames: frames), history: history);
  }

  void clearSelectedFrame() => paintSelectedFrame(
    squares: List<int>.generate(64, (index) => index),
    mask: 15,
    color: 0,
    clearEffects: true,
  );

  void beginStroke() => _strokeBefore = _document;
  void endStroke() {
    final previous = _strokeBefore;
    _strokeBefore = null;
    if (previous != null && !sameLitchcraftDocument(previous, _document)) {
      _push(_undo, previous);
      notifyListeners();
    }
  }

  void setFavoriteColors(Iterable<int> colors, {bool history = false}) =>
      _edit(_copy(favorites: colors), history: history);

  void selectFrame(int index) {
    RangeError.checkValidIndex(index, _document.frames, 'index');
    if (_selectedFrame == index) return;
    _selectedFrame = index;
    notifyListeners();
  }

  void addFrame() {
    if (_document.frames.length == litchcraftAnimationMaximumFrameCount) {
      throw const LitchcraftFrameLimit();
    }
    final frames = _document.frames.toList()
      ..insert(_selectedFrame + 1, _document.frames[_selectedFrame]);
    _edit(_copy(frames: frames), selection: _selectedFrame + 1);
  }

  void deleteSelectedFrame() {
    if (_document.frames.length == 1) throw const LitchcraftFrameLimit();
    final frames = _document.frames.toList()..removeAt(_selectedFrame);
    _edit(
      _copy(frames: frames),
      selection: _selectedFrame == 0 ? 0 : _selectedFrame - 1,
    );
  }

  void setSelectedFrameDuration(int durationMs) {
    final frames = _document.frames.toList();
    frames[_selectedFrame] = LitchcraftAnimationFrame(
      emitters: frames[_selectedFrame].emitters,
      durationMs: durationMs,
    );
    _edit(_copy(frames: frames));
  }

  void applyEffect({
    required int square,
    required int mask,
    required LitchcraftEffectStyle effect,
    int effectDurationMs = litchcraftAnimationDefaultEffectDurationMs,
  }) {
    if (square < 0 || square >= 64 || mask < 1 || mask > 15) {
      throw RangeError('Invalid Litchcraft effect selection');
    }
    final frames = _document.frames.toList();
    final frame = frames[_selectedFrame];
    final emitters = frame.emitters.toList();
    for (var region = 0; region < 4; region++) {
      if (mask & (1 << region) == 0) continue;
      final index = square * 4 + region;
      emitters[index] = LitchcraftAnimatedEmitter(
        color: emitters[index].color,
        effect: effect,
        effectDurationMs: effect == LitchcraftEffectStyle.off
            ? 0
            : effectDurationMs,
      );
    }
    frames[_selectedFrame] = LitchcraftAnimationFrame(
      emitters: emitters,
      durationMs: frame.durationMs,
    );
    _localEffectEdit = (frame: _selectedFrame, square: square, mask: mask);
    try {
      _edit(_copy(frames: frames));
    } finally {
      _localEffectEdit = null;
    }
  }

  /// Rotates the selected frame as one artwork operation, including emitter
  /// effects and their durations rather than projecting RGB alone.
  void rotateSelectedFrame(int quarterTurns) {
    final turns = ((quarterTurns % 4) + 4) % 4;
    if (turns == 0) return;
    final frames = _document.frames.toList();
    final frame = frames[_selectedFrame];
    final emitters = List<LitchcraftAnimatedEmitter>.filled(
      256,
      const LitchcraftAnimatedEmitter(color: 0),
    );
    for (var square = 0; square < 64; square++) {
      var x = square % 8;
      var y = square ~/ 8;
      for (var turn = 0; turn < turns; turn++) {
        final nextX = 7 - y;
        y = x;
        x = nextX;
      }
      final destinationSquare = y * 8 + x;
      for (var region = 0; region < 4; region++) {
        final destinationRegion = (region + turns) % 4;
        emitters[destinationSquare * 4 + destinationRegion] =
            frame.emitters[square * 4 + region];
      }
    }
    frames[_selectedFrame] = LitchcraftAnimationFrame(
      emitters: emitters,
      durationMs: frame.durationMs,
    );
    _edit(_copy(frames: frames));
  }

  void undo() => _history(_undo, _redo);
  void redo() => _history(_redo, _undo);

  /// Retires preview inverses when the board journal gains a foreign boundary.
  /// The draft and any in-progress stroke remain owned by this session.
  void retireLocalHistory() {
    _undo.clear();
    _redo.clear();
  }

  void _history(
    List<LitchcraftAnimationDocument> from,
    List<LitchcraftAnimationDocument> to,
  ) {
    if (from.isEmpty) return;
    _push(to, _document, current: from.last);
    _document = from.removeLast();
    _selectedFrame = _selectedFrame.clamp(0, _document.frames.length - 1);
    _generation++;
    _hasLocalIntent = true;
    notifyListeners();
  }

  void _push(
    List<LitchcraftAnimationDocument> history,
    LitchcraftAnimationDocument document, {
    LitchcraftAnimationDocument? current,
  }) {
    history.add(document);
    _trimHistory(history, current: current ?? _document);
  }

  void _trimHistory(
    List<LitchcraftAnimationDocument> history, {
    required LitchcraftAnimationDocument current,
  }) {
    // Immutable documents share their unchanged frame objects. Charge each
    // retained document's frame references and only frame bodies not already
    // held by the current document or a newer history entry.
    int retainedBytes() {
      final frames = HashSet<LitchcraftAnimationFrame>.identity()
        ..addAll(current.frames);
      var bytes = 0;
      for (final entry in history.reversed) {
        bytes += 32 + entry.frames.length * 8;
        for (final frame in entry.frames) {
          if (frames.add(frame)) bytes += 1538;
        }
      }
      return bytes;
    }

    while (history.length > 1 &&
        (history.length > 20 || retainedBytes() > 32 * 1024)) {
      history.removeAt(0);
    }
  }

  /// A changed board mode or boot ends the old ancestor's authority, not
  /// the user's selected source or local history.
  void retireBoardLineage() {
    if (_common == null && _board == null) return;
    _common = _board = null;
    _boardId = null;
    _hasLocalIntent = true;
    _overlappingBoardPaint = false;
  }

  LitchcraftDocumentResolution observeBoard(
    String boardId,
    LitchcraftAnimationDocument board,
  ) {
    if (_boardId != boardId) {
      _common = null;
      _boardId = boardId;
    }
    _board = board;
    if (sameLitchcraftDocument(_document, board)) {
      _overlappingBoardPaint = false;
      return LitchcraftDocumentResolution.equal;
    }
    if (_overlappingBoardPaint) {
      return LitchcraftDocumentResolution.conflict;
    }
    final common = _common;
    if (common != null && sameLitchcraftDocument(_document, common)) {
      return LitchcraftDocumentResolution.adoptBoard;
    }
    if (common != null && sameLitchcraftDocument(board, common)) {
      return LitchcraftDocumentResolution.sendApp;
    }
    // Empty is a semantic property of the current frame, independent of
    // retained history, timing metadata and playback-on-entry preference.
    if (_hasLocalIntent && isEmptyLitchcraftDocument(board)) {
      return LitchcraftDocumentResolution.sendApp;
    }
    if (common != null) {
      return mergeLitchcraftDocuments(common, _document, board) == null
          ? LitchcraftDocumentResolution.conflict
          : LitchcraftDocumentResolution.merge;
    }
    if (!_hasLocalIntent) return LitchcraftDocumentResolution.adoptBoard;
    return LitchcraftDocumentResolution.conflict;
  }

  void acceptBoard({required int expectedGeneration}) {
    if (_generation != expectedGeneration || _board == null) return;
    _document = _board!;
    _selectedFrame = _selectedFrame.clamp(0, _document.frames.length - 1);
    _common = _board;
    _undo.clear();
    _redo.clear();
    _generation++;
    _hasLocalIntent = false;
    _overlappingBoardPaint = false;
    notifyListeners();
  }

  /// A captured pending Canvas footprint orders its RGB preview after the
  /// board snapshot. Only explicitly newer local effect fields retain their
  /// ownership; all other effects learn the native journal/root.
  bool mergeBoard({
    ({int frame, int mask, int action, BigInt effectEdits, bool preview})?
    orderedCanvas,
  }) {
    if (orderedCanvas != null && _overlappingBoardPaint) return false;
    final common = _common!, board = _board!;
    final merged = mergeLitchcraftDocuments(
      common,
      _document,
      board,
      orderedCanvas: orderedCanvas,
    );
    if (merged == null) return false;
    // Absolute Canvas preview belongs only to the current draft. Historical
    // preimages still learn the actual board bytes needed for one native Undo.
    final historyCanvas = orderedCanvas == null
        ? null
        : (
            frame: orderedCanvas.frame,
            mask: orderedCanvas.mask,
            action: orderedCanvas.action,
            effectEdits: orderedCanvas.effectEdits,
            preview: false,
          );
    // Rebase existing bounded local history so offline Undo cannot erase a
    // physical contribution learned during this stroke.
    for (final history in [_undo, _redo]) {
      for (var i = history.length - 1; i >= 0; i--) {
        final rebased = mergeLitchcraftDocuments(
          common,
          history[i],
          board,
          orderedCanvas: historyCanvas,
        );
        if (rebased == null) {
          history.removeAt(i);
        } else {
          history[i] = rebased;
        }
      }
    }
    if (_strokeBefore != null) {
      _strokeBefore = mergeLitchcraftDocuments(
        common,
        _strokeBefore!,
        board,
        orderedCanvas: historyCanvas,
      );
    }
    _document = merged;
    _trimHistory(_undo, current: _document);
    _trimHistory(_redo, current: _document);
    _common = board;
    _overlappingBoardPaint = false;
    _generation++;
    notifyListeners();
    return true;
  }

  /// Records only the immutable bytes actually accepted by the board. A newer
  /// local edit survives and remains divergent from this common ancestor.
  void confirm(String boardId, LitchcraftAnimationDocument accepted) {
    _boardId = boardId;
    _board = accepted;
    _common = accepted;
    if (sameLitchcraftDocument(_document, accepted)) {
      _hasLocalIntent = false;
      _overlappingBoardPaint = false;
    }
  }

  /// Applies a board-originated patch to board and common history. An emitter
  /// with a pending local edit keeps that local intent until reconciliation;
  /// unrelated emitters adopt the physical change immediately. A pending
  /// gesture may retain its draft and preimage until coherent history readback;
  /// board/common projection and overlap detection still apply.
  bool applyBoardPaintBatch({
    required String boardId,
    required int frame,
    required List<LitchcraftPhysicalPaint> edits,
    bool historyPreview = false,
    bool preservePendingStroke = false,
    ({int frame, int mask, int action, BigInt effectEdits, bool preview})?
    orderedCanvas,
  }) {
    if (_boardId != boardId ||
        _board == null ||
        frame < 0 ||
        frame >= _board!.frames.length ||
        frame >= _document.frames.length ||
        edits.isEmpty ||
        edits.any(
          (edit) =>
              edit.squares.isEmpty ||
              edit.squares.any((square) => square < 0 || square >= 64) ||
              edit.mask < 1 ||
              edit.mask > 15 ||
              edit.color < 0 ||
              edit.color > 0xffffff,
        )) {
      return false;
    }
    LitchcraftAnimationDocument patch(LitchcraftAnimationDocument source) =>
        _patchPhysicalPaints(source, frame, edits);

    final localEmitters = _document.frames[frame].emitters.toList();
    final commonBefore = _common;
    final commonEmitters =
        commonBefore != null && frame < commonBefore.frames.length
        ? commonBefore.frames[frame].emitters.toList()
        : null;
    for (final edit in edits) {
      for (final square in edit.squares) {
        for (var region = 0; region < 4; region++) {
          if (edit.mask & (1 << region) == 0) continue;
          final index = square * 4 + region;
          final local = localEmitters[index];
          final common = commonEmitters?[index];
          final safe =
              common == null ||
              (local.color == common.color && _sameEffect(local, common));
          if (safe && !historyPreview) {
            final projected = _paintPhysicalEmitter(local, edit);
            final preview =
                _canvasOwnsRgb(orderedCanvas, frame, index) &&
                orderedCanvas!.preview;
            final preserveEffect =
                _canvasOwnsEffect(orderedCanvas, frame, index) ||
                (preview &&
                    (orderedCanvas.action == 1 || orderedCanvas.action >= 7));
            if (!preservePendingStroke) {
              localEmitters[index] = preview || preserveEffect
                  ? LitchcraftAnimatedEmitter(
                      color: preview ? local.color : projected.color,
                      effect: preserveEffect ? local.effect : projected.effect,
                      effectDurationMs: preserveEffect
                          ? local.effectDurationMs
                          : projected.effectDurationMs,
                    )
                  : projected;
            }
          } else if (!historyPreview &&
              !_canvasOwnsRgb(orderedCanvas, frame, index)) {
            _overlappingBoardPaint = true;
          }
          if (common != null) {
            commonEmitters![index] = _paintPhysicalEmitter(common, edit);
          }
        }
      }
    }
    _board = patch(_board!);
    if (commonBefore != null && commonEmitters != null) {
      _common = _withFrameEmitters(commonBefore, frame, commonEmitters);
    }
    final next = _withFrameEmitters(_document, frame, localEmitters);
    if (!historyPreview && !preservePendingStroke) {
      for (final history in <List<LitchcraftAnimationDocument>>[_undo, _redo]) {
        for (var index = 0; index < history.length; index++) {
          if (frame < history[index].frames.length) {
            history[index] = patch(history[index]);
          }
        }
      }
    }
    if (!historyPreview &&
        !preservePendingStroke &&
        _strokeBefore != null &&
        frame < _strokeBefore!.frames.length) {
      _strokeBefore = patch(_strokeBefore!);
    }
    if (!sameLitchcraftDocument(_document, next)) {
      _document = next;
      _generation++;
    }
    _trimHistory(_undo, current: _document);
    _trimHistory(_redo, current: _document);
    _hasLocalIntent =
        _common == null || !sameLitchcraftDocument(_document, _common!);
    notifyListeners();
    return true;
  }
}

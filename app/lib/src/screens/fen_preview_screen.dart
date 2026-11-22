import 'package:flutter/material.dart';

import '../widgets/compact_controls.dart';

import '../localization/litch_strings.dart';
import '../models/home_board_snapshot.dart';
import '../models/standard_chess_replay.dart';
import '../widgets/home_chessboard.dart';
import 'post_game_review_layout.dart';

/// Offline editor and logical-board preview for one complete standard FEN.
class FenPreviewScreen extends StatefulWidget {
  const FenPreviewScreen({super.key});

  @override
  State<FenPreviewScreen> createState() => _FenPreviewScreenState();
}

class _FenPreviewScreenState extends State<FenPreviewScreen> {
  final TextEditingController _controller = TextEditingController();
  final FocusNode _fenFocus = FocusNode();
  String? _validatedFen;
  String? _error;

  @override
  void dispose() {
    _controller.dispose();
    _fenFocus.dispose();
    super.dispose();
  }

  void _preview() {
    try {
      final chess = validatedStandardChessInitialPosition(
        _controller.text.trim(),
      );
      setState(() {
        _validatedFen = chess.fen;
        _error = null;
      });
      _fenFocus.unfocus();
    } catch (_) {
      setState(() {
        _validatedFen = null;
        _error = litchStrings(context).fenPreviewInvalid;
      });
      _fenFocus.requestFocus();
    }
  }

  void _editAgain() {
    setState(() {
      _validatedFen = null;
      _error = null;
    });
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _fenFocus.requestFocus();
    });
  }

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final fen = _validatedFen;
    return Scaffold(
      appBar: LitchCompactAppBar(
        child: AppBar(title: Text(strings.fenPreviewTitle)),
      ),
      body: SafeArea(
        child: fen != null
            ? _buildPreview(context, fen)
            : SingleChildScrollView(
                key: const ValueKey<String>('fen-preview-scroll'),
                padding: const EdgeInsets.fromLTRB(16, 16, 16, 28),
                child: Center(
                  child: ConstrainedBox(
                    constraints: const BoxConstraints(maxWidth: 620),
                    child: _buildEditor(context),
                  ),
                ),
              ),
      ),
    );
  }

  Widget _buildEditor(BuildContext context) {
    final strings = litchStrings(context);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        Semantics(
          header: true,
          child: Text(
            strings.fenPreviewEntryHeading,
            style: Theme.of(
              context,
            ).textTheme.headlineSmall?.copyWith(fontWeight: FontWeight.w700),
          ),
        ),
        const SizedBox(height: 8),
        Text(strings.fenPreviewIntro),
        const SizedBox(height: 18),
        TextField(
          key: const ValueKey<String>('fen-preview-field'),
          controller: _controller,
          focusNode: _fenFocus,
          minLines: 3,
          maxLines: 5,
          textInputAction: TextInputAction.done,
          autocorrect: false,
          enableSuggestions: false,
          decoration: InputDecoration(
            labelText: strings.fenPreviewFieldLabel,
            hintText: strings.fenPreviewFieldHint,
            hintMaxLines: 3,
            error: _error == null
                ? null
                : Text(_error!, softWrap: true, overflow: TextOverflow.visible),
          ),
          onChanged: (_) {
            if (_error != null) setState(() => _error = null);
          },
          onSubmitted: (_) => _preview(),
        ),
        if (_error != null)
          Semantics(
            liveRegion: true,
            label: _error,
            child: const SizedBox.shrink(),
          ),
        const SizedBox(height: 18),
        FilledButton.icon(
          key: const ValueKey<String>('fen-preview-validate'),
          style: FilledButton.styleFrom(minimumSize: const Size(0, 48)),
          onPressed: _preview,
          icon: const Icon(Icons.visibility_outlined),
          label: Text(strings.fenPreviewValidate),
        ),
      ],
    );
  }

  Widget _buildPreview(BuildContext context, String fen) {
    final strings = litchStrings(context);
    final position = HomeChessPosition.tryParse(fen)!;
    return GamePositionReviewLayout(
      scrollKey: const ValueKey<String>('fen-preview-scroll'),
      padding: const EdgeInsets.fromLTRB(16, 16, 16, 28),
      boardTopGap: 16,
      boardBottomGap: 16,
      heading: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          Semantics(
            header: true,
            child: Text(
              strings.fenPreviewHeading,
              style: Theme.of(
                context,
              ).textTheme.headlineSmall?.copyWith(fontWeight: FontWeight.w700),
            ),
          ),
          const SizedBox(height: 8),
          Text(strings.fenPreviewDisclaimer),
        ],
      ),
      board: Semantics(
        key: const ValueKey<String>('fen-preview-board'),
        container: true,
        explicitChildNodes: true,
        label: strings.fenPreviewBoardSemantics(
          position.sideToMove == HomeSide.white
              ? strings.reviewWhite
              : strings.reviewBlack,
          position.pieces.length,
        ),
        child: HomeChessboard(
          snapshot: HomeBoardSnapshot(position: position),
          showCoordinates: true,
          maxSize: double.infinity,
        ),
      ),
      details: <Widget>[
        Text(
          strings.fenPreviewValidatedFen,
          style: Theme.of(context).textTheme.labelLarge,
        ),
        const SizedBox(height: 4),
        SelectableText(
          fen,
          key: const ValueKey<String>('fen-preview-canonical-fen'),
        ),
        const SizedBox(height: 14),
        _FenMetadata(fen: fen),
        const SizedBox(height: 18),
        OutlinedButton.icon(
          key: const ValueKey<String>('fen-preview-edit-again'),
          style: OutlinedButton.styleFrom(minimumSize: const Size(0, 48)),
          onPressed: _editAgain,
          icon: const Icon(Icons.edit_outlined),
          label: Text(strings.fenPreviewEditAgain),
        ),
      ],
    );
  }
}

class _FenMetadata extends StatelessWidget {
  const _FenMetadata({required this.fen});

  final String fen;

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final fields = fen.split(' ');
    final rights = <String>[
      if (fields[2].contains('K')) strings.fenPreviewWhiteKingSide,
      if (fields[2].contains('Q')) strings.fenPreviewWhiteQueenSide,
      if (fields[2].contains('k')) strings.fenPreviewBlackKingSide,
      if (fields[2].contains('q')) strings.fenPreviewBlackQueenSide,
    ];
    return Semantics(
      container: true,
      child: Card(
        margin: EdgeInsets.zero,
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: <Widget>[
              Text(
                strings.fenPreviewMetadataTitle,
                style: Theme.of(
                  context,
                ).textTheme.titleMedium?.copyWith(fontWeight: FontWeight.w700),
              ),
              const SizedBox(height: 8),
              Text(
                strings.fenPreviewSideToMove(
                  fields[1] == 'w' ? strings.reviewWhite : strings.reviewBlack,
                ),
              ),
              Text(
                strings.fenPreviewCastlingRights(
                  rights.isEmpty ? strings.fenPreviewNone : rights.join(', '),
                ),
              ),
              Text(
                strings.fenPreviewEnPassantTarget(
                  fields[3] == '-' ? strings.fenPreviewNone : fields[3],
                ),
              ),
              Text(strings.fenPreviewHalfmoveClock(int.parse(fields[4]))),
              Text(strings.fenPreviewFullmoveNumber(int.parse(fields[5]))),
            ],
          ),
        ),
      ),
    );
  }
}

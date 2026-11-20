import 'dart:async';

import 'package:flutter/material.dart';

/// App-owned terminal text; never appended to the board's diagnostic history.
class ConsoleWelcome extends StatefulWidget {
  const ConsoleWelcome({
    super.key,
    required this.fontFamily,
    required this.onComplete,
    required this.onLineRevealed,
  });

  final String? fontFamily;
  final VoidCallback onComplete;
  final VoidCallback onLineRevealed;

  @override
  State<ConsoleWelcome> createState() => _ConsoleWelcomeState();
}

class _ConsoleWelcomeState extends State<ConsoleWelcome>
    with SingleTickerProviderStateMixin {
  static const _first = 'The Litch has you.';
  static const _second = 'Follow the orange fox ';
  static const _letterMs = 65;
  static const _pauseMs = 350;
  static const _firstEnd = _first.length * _letterMs;
  static const _secondStart = _firstEnd + _pauseMs;
  static const _typingEnd = _secondStart + (_second.length + 1) * _letterMs;
  static const _holdMs = 2000;
  static const _exitMs = 260;
  static const _totalMs = _typingEnd + _holdMs + _exitMs;
  static const _green = Color(0xff00e676);
  static const _orange = Color(0xffff7900);

  late final AnimationController _clock =
      AnimationController(
          vsync: this,
          // Reduced motion has its own static two-second presentation below.
          animationBehavior: AnimationBehavior.preserve,
        )
        ..addListener(_onTick)
        ..addStatusListener((status) {
          if (status == AnimationStatus.completed) widget.onComplete();
        });
  bool? _reducedMotion;
  bool _secondLineRevealed = false;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    final reduced = MediaQuery.disableAnimationsOf(context);
    if (_reducedMotion == reduced) return;
    _reducedMotion = reduced;
    unawaited(
      precacheImage(const AssetImage('assets/branding/foxie-fox.png'), context),
    );
    _clock.duration = Duration(milliseconds: reduced ? _holdMs : _totalMs);
    _clock.forward(from: 0);
  }

  void _onTick() {
    if (!_secondLineRevealed &&
        (_reducedMotion == true || _clock.value * _totalMs >= _secondStart)) {
      _secondLineRevealed = true;
      widget.onLineRevealed();
    }
  }

  @override
  void dispose() {
    _clock.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Semantics(
    container: true,
    liveRegion: true,
    label: 'The Litch has you. Follow the orange fox.',
    child: ExcludeSemantics(
      child: AnimatedBuilder(
        animation: _clock,
        builder: (context, _) {
          final reduced = _reducedMotion == true;
          final ms = _clock.value * _totalMs;
          final firstCount = reduced
              ? _first.length
              : (ms / _letterMs).floor().clamp(0, _first.length);
          final secondCount = reduced
              ? _second.length
              : ((ms - _secondStart) / _letterMs).floor().clamp(
                  0,
                  _second.length,
                );
          final showSecond = reduced || ms >= _secondStart;
          final finished = reduced || ms >= _typingEnd;
          final exit = reduced
              ? 0.0
              : Curves.easeInCubic.transform(
                  ((ms - _typingEnd - _holdMs) / _exitMs).clamp(0.0, 1.0),
                );
          final style = TextStyle(
            color: _green,
            backgroundColor: Colors.black,
            fontFamily: widget.fontFamily,
            fontSize: 12,
            height: 1.4,
          );
          return ClipRect(
            child: Align(
              alignment: Alignment.topLeft,
              heightFactor: 1 - exit,
              child: FractionalTranslation(
                translation: Offset(0, -exit),
                child: Text.rich(
                  key: const ValueKey('console-welcome-text'),
                  TextSpan(
                    children: [
                      TextSpan(text: _first.substring(0, firstCount)),
                      if (showSecond) ...[
                        TextSpan(
                          text: '\n${_second.substring(0, secondCount)}',
                        ),
                        if (finished) ...[
                          WidgetSpan(
                            alignment: PlaceholderAlignment.middle,
                            child: ColoredBox(
                              color: Colors.black,
                              child: Image.asset(
                                'assets/branding/foxie-fox.png',
                                width: MediaQuery.textScalerOf(
                                  context,
                                ).scale(16),
                                height: MediaQuery.textScalerOf(
                                  context,
                                ).scale(16),
                                color: _orange,
                                colorBlendMode: BlendMode.srcIn,
                                excludeFromSemantics: true,
                              ),
                            ),
                          ),
                          const TextSpan(text: '.'),
                        ],
                      ],
                      if (!finished) const TextSpan(text: '▌'),
                    ],
                  ),
                  style: style,
                  textDirection: TextDirection.ltr,
                ),
              ),
            ),
          );
        },
      ),
    ),
  );
}

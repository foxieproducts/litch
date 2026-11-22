import 'dart:async';
import 'dart:ui' show FlutterView;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:intl/intl.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../localization/litch_strings.dart';
import '../models/console_completion.dart';
import '../services/litch_ble_service.dart';
import '../widgets/console_appearance_sheet.dart';
import '../widgets/console_welcome.dart';
import '../widgets/home_notice_presenter.dart';
import '../widgets/litch_workspace_geometry.dart';

class AppConsoleScreen extends StatefulWidget {
  const AppConsoleScreen({
    super.key,
    required this.bleService,
    required this.boardId,
    this.notices,
  });

  final LitchBleService bleService;
  final String boardId;
  final HomeNoticePresenter? notices;

  @override
  State<AppConsoleScreen> createState() => AppConsoleScreenState();
}

class AppConsoleScreenState extends State<AppConsoleScreen>
    with WidgetsBindingObserver {
  static const _orientationChannel = MethodChannel(
    'com.foxieproducts.litch/interface_orientation',
  );
  final HomeNoticePresenter _localNotices = HomeNoticePresenter();
  HomeNoticePresenter get _notices => widget.notices ?? _localNotices;
  StateSetter? _refreshAppearance;

  final TextEditingController _inputController = TextEditingController();
  final FocusNode _inputFocusNode = FocusNode();
  final ScrollController _consoleScrollController = ScrollController();
  static const double _autoScrollThreshold = 40;
  (int, String)? _lastConsoleUpdate;
  bool _selectingOutput = false;
  (LitchBleService, String, int)? _traceCommandSession;
  bool? _traceCommandTargetEnabled;
  static const _foregroundKey = 'console_foreground_rgb';
  static const _backgroundKey = 'console_background_rgb';
  static const _timestampKey = 'console_timestamp_rgb';
  static const _fontKey = 'console_font_family';
  static const _showTimestampsKey = 'console_show_timestamps';
  static const _welcomeSeenKey = 'console_welcome_seen';
  SharedPreferences? _preferences;
  late final Future<void> _appearanceReady = _loadAppearance();
  bool _welcomeSeen = true;
  bool _showWelcome = false;
  static const _defaultForeground = Color(0xffd9f3f2);
  static const _defaultBackground = Colors.black;
  Color _foreground = _defaultForeground;
  Color _background = _defaultBackground;
  Color _timestamp = const ConsoleAppearance().timestamp;
  String _fontFamily = 'monospace';
  bool _showTimestamps = false;
  (LitchBleService, String, int)? _traceFailedSession;
  bool _active = false;
  FlutterView? _activeView;
  bool? _keyboardPreferred;
  bool _keyboardVisible = false;
  String? _timestampLocale;
  bool? _timestamp24Hour;
  DateFormat? _timestampFormat;
  static final _timestampPrefix = RegExp(r'^(\d{2}):(\d{2}):(\d{2})\.(\d{3}) ');

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _inputFocusNode.addListener(_onInputFocusChanged);
    _inputFocusNode.onKeyEvent = _onInputKeyEvent;
    unawaited(_appearanceReady);
  }

  void _onInputFocusChanged() {
    if (!mounted) return;
    if (_inputFocusNode.hasFocus) scrollConsoleToBottom(force: true);
    if (_active && _inputFocusNode.hasFocus && _keyboardPreferred != true) {
      setState(() => _keyboardPreferred = true);
    }
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    final view = View.of(context);
    if (identical(view, _activeView)) return;
    _activeView = view;
    _keyboardVisible = view.viewInsets.bottom > 0;
  }

  @override
  void deactivate() {
    _activeView = null;
    super.deactivate();
  }

  @override
  void didChangeMetrics() {
    final view = _activeView;
    if (view == null) return;
    // Keep the latest line visible as the keyboard changes the log viewport.
    if (_inputFocusNode.hasFocus) scrollConsoleToBottom();
    final visible = view.viewInsets.bottom > 0;
    if (_keyboardVisible == visible) return;
    setState(() {
      if (_active && _keyboardVisible && !visible) {
        _keyboardPreferred = false;
      }
      _keyboardVisible = visible;
    });
  }

  Future<void> _loadAppearance() async {
    final preferences = await SharedPreferences.getInstance();
    if (!mounted) return;
    setState(() {
      _preferences = preferences;
      _welcomeSeen = preferences.getBool(_welcomeSeenKey) ?? false;
      _foreground = Color(
        preferences.getInt(_foregroundKey) ?? _defaultForeground.toARGB32(),
      );
      _background = Color(
        preferences.getInt(_backgroundKey) ?? _defaultBackground.toARGB32(),
      );
      _timestamp = Color(
        preferences.getInt(_timestampKey) ??
            Color.lerp(_foreground, _background, .28)!.toARGB32(),
      );
      _fontFamily = preferences.getString(_fontKey) ?? 'monospace';
      _showTimestamps = preferences.getBool(_showTimestampsKey) ?? false;
    });
    _startWelcomeIfNeeded();
  }

  void _startWelcomeIfNeeded() {
    if (!_active || _preferences == null || _welcomeSeen) return;
    setState(() {
      _welcomeSeen = true;
      _showWelcome = true;
    });
    unawaited(_preferences!.setBool(_welcomeSeenKey, true));
    _scheduleConsoleScroll(shouldScroll: true, force: true);
  }

  Future<void> _saveAppearance() async {
    final preferences = await SharedPreferences.getInstance();
    await Future.wait(<Future<bool>>[
      preferences.setInt(_foregroundKey, _foreground.toARGB32()),
      preferences.setInt(_backgroundKey, _background.toARGB32()),
      preferences.setInt(_timestampKey, _timestamp.toARGB32()),
      preferences.setString(_fontKey, _fontFamily),
      preferences.setBool(_showTimestampsKey, _showTimestamps),
    ]);
  }

  String? _resolvedFontFamily(BuildContext context) {
    if (_fontFamily == 'monospace') {
      return Theme.of(context).platform == TargetPlatform.iOS
          ? 'Menlo'
          : 'monospace';
    }
    return _fontFamily == 'sans-serif' ? null : _fontFamily;
  }

  Future<void> _showAppearance() async {
    await _appearanceReady;
    if (!mounted) return;
    final result = await showModalBottomSheet<ConsoleAppearance>(
      context: context,
      useSafeArea: true,
      isScrollControlled: true,
      builder: (_) => StatefulBuilder(
        builder: (context, refresh) {
          _refreshAppearance = refresh;
          return StreamBuilder<ConnectionStateUpdate>(
            stream: widget.bleService.connectionStream,
            builder: (context, connection) => StreamBuilder<bool>(
              stream: widget.bleService.consoleTraceOutputStream,
              initialData: widget.bleService.consoleTraceOutputEnabled,
              builder: (context, trace) {
                final strings = litchStrings(context);
                final enabled = trace.data ?? false;
                final status = !_connectedToBoard
                    ? connection.data?.connectionState ==
                              DeviceConnectionState.connecting
                          ? strings.boardConnecting
                          : strings.boardDisconnected
                    : _traceCommandInFlight
                    ? _traceCommandTargetEnabled == true
                          ? strings.consoleTraceEnabling
                          : strings.consoleTraceDisabling
                    : _traceFailedSession == _consoleSession
                    ? strings.consoleTraceFailed
                    : enabled
                    ? strings.consoleTraceEnabled
                    : strings.consoleTraceDisabled;
                return ConsoleAppearanceSheet(
                  initial: ConsoleAppearance(
                    foreground: _foreground,
                    background: _background,
                    timestamp: _timestamp,
                    font: _fontFamily,
                    showTimestamps: _showTimestamps,
                  ),
                  traceOutputEnabled: enabled,
                  traceOutputStatus: status,
                  onTraceOutputChanged:
                      !_connectedToBoard || _traceCommandInFlight
                      ? null
                      : _setTraceOutputEnabled,
                );
              },
            ),
          );
        },
      ),
    );
    _refreshAppearance = null;
    if (!mounted || result == null) return;
    setState(() {
      _foreground = result.foreground;
      _background = result.background;
      _timestamp = result.timestamp;
      _fontFamily = result.font;
      _showTimestamps = result.showTimestamps;
      if (result.resetWelcome) {
        _welcomeSeen = false;
        _showWelcome = false;
      }
    });
    if (result.resetWelcome) {
      unawaited(_preferences?.setBool(_welcomeSeenKey, false));
    }
    unawaited(_saveAppearance());
  }

  String _displayLine(String line) {
    final timestamp = _timestampPrefix.firstMatch(line);
    const marker = '[FW] ';
    if (timestamp == null || !line.startsWith(marker, timestamp.end)) {
      return line;
    }
    return line.replaceRange(timestamp.end, timestamp.end + marker.length, '');
  }

  Widget _consoleLine(String raw, DateFormat formatter) {
    final line = _displayLine(raw);
    final match = _timestampPrefix.firstMatch(line);
    final style = TextStyle(
      color: _foreground,
      fontFamily: _resolvedFontFamily(context),
      fontSize: 12,
    );
    if (match == null) return Text(line, style: style);
    final hour = int.parse(match[1]!);
    final minute = int.parse(match[2]!);
    final second = int.parse(match[3]!);
    if (hour > 23 || minute > 59 || second > 59) {
      return Text(line, style: style);
    }
    if (!_showTimestamps) {
      return Text(line.substring(match.end), style: style);
    }
    // Service timestamps already represent local wall time. Reformat only the
    // presentation; never reinterpret them as UTC or change diagnostic history.
    final time = DateTime(
      2000,
      1,
      1,
      hour,
      minute,
      second,
      int.parse(match[4]!),
    );
    return Text.rich(
      TextSpan(
        children: [
          TextSpan(
            text: '${formatter.format(time)} ',
            style: TextStyle(color: _timestamp),
          ),
          TextSpan(text: line.substring(match.end)),
        ],
      ),
      style: style,
    );
  }

  DateFormat _timeFormatter(BuildContext context) {
    final locale = Localizations.localeOf(context).toString();
    final use24Hour = MediaQuery.alwaysUse24HourFormatOf(context);
    if (_timestampFormat == null ||
        _timestampLocale != locale ||
        _timestamp24Hour != use24Hour) {
      final time = use24Hour ? DateFormat.Hms(locale) : DateFormat.jms(locale);
      _timestampFormat = DateFormat(
        time.pattern!.replaceFirst('ss', 'ss.SSS'),
        locale,
      );
      _timestampLocale = locale;
      _timestamp24Hour = use24Hour;
    }
    return _timestampFormat!;
  }

  @override
  void didUpdateWidget(covariant AppConsoleScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.boardId != widget.boardId) {
      _inputController.clear();
      _traceCommandSession = null;
      _traceCommandTargetEnabled = null;
    }
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _inputFocusNode.removeListener(_onInputFocusChanged);
    _inputController.dispose();
    _inputFocusNode.dispose();
    _consoleScrollController.dispose();
    super.dispose();
  }

  bool _isNearBottom() {
    if (!_consoleScrollController.hasClients) {
      return true;
    }
    final double distance =
        _consoleScrollController.position.maxScrollExtent -
        _consoleScrollController.position.pixels;
    return distance <= _autoScrollThreshold;
  }

  void _scheduleConsoleScroll({
    required bool shouldScroll,
    bool force = false,
  }) {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || !_consoleScrollController.hasClients) {
        return;
      }

      if (!force && (!shouldScroll || _selectingOutput)) {
        return;
      }

      final double maxExtent =
          _consoleScrollController.position.maxScrollExtent;
      _consoleScrollController.jumpTo(maxExtent);
    });
  }

  void scrollConsoleToBottom({bool force = false}) {
    _scheduleConsoleScroll(
      shouldScroll: force ? true : _isNearBottom(),
      force: force,
    );
  }

  Future<void> _showPreparedKeyboard() async {
    // iOS may deliberately retain its landscape interface while the Flutter
    // workspace performs a half-turn. Release that native presentation lock
    // before UIKit constructs the keyboard, otherwise it can enter from the
    // landscape edge that was correct before the phone turned.
    if (Theme.of(context).platform == TargetPlatform.iOS) {
      await _orientationChannel
          .invokeMethod<void>('editing', true)
          .catchError((Object _) {});
    }
    if (!mounted || !_active || _keyboardPreferred == false) return;
    _inputFocusNode.requestFocus();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || !_active || _keyboardPreferred == false) return;
      if (!_inputFocusNode.hasFocus) _inputFocusNode.requestFocus();
      unawaited(SystemChannels.textInput.invokeMethod<void>('TextInput.show'));
    });
    WidgetsBinding.instance.ensureVisualUpdate();
  }

  void activateConsole() {
    final wasActive = _active;
    _active = true;
    if (!wasActive) _startWelcomeIfNeeded();
    // First entry opens the command keyboard. An explicit hide is sticky for
    // the rest of this mounted console session, including navigation away and
    // back, so returning to diagnostics does not surprise the user.
    _keyboardPreferred ??= true;
    scrollConsoleToBottom(force: true);
    if (!mounted || _keyboardPreferred == false) return;
    unawaited(_showPreparedKeyboard());
  }

  void deactivateConsole() {
    _active = false;
    if (_showWelcome) setState(() => _showWelcome = false);
    _inputFocusNode.unfocus();
  }

  void _hideKeyboard() {
    setState(() => _keyboardPreferred = false);
    _inputFocusNode.unfocus();
  }

  void _toggleKeyboard() {
    if (_keyboardVisible) {
      _hideKeyboard();
      return;
    }
    setState(() {
      _active = true;
      _keyboardPreferred = true;
    });
    unawaited(_showPreparedKeyboard());
  }

  void _showKeyboardFromInput() {
    _active = true;
    scrollConsoleToBottom(force: true);
    if (_keyboardPreferred != true) {
      setState(() => _keyboardPreferred = true);
    }
    unawaited(_showPreparedKeyboard());
  }

  (LitchBleService, String, int) get _consoleSession => (
    widget.bleService,
    widget.boardId,
    widget.bleService.connectionSessionToken,
  );

  bool get _traceCommandInFlight => _traceCommandSession == _consoleSession;

  bool get _connectedToBoard =>
      widget.bleService.isConnected &&
      widget.bleService.connectedId == widget.boardId;

  Future<void> _sendLineFromInput() async {
    if (!_connectedToBoard) return;
    final session = _consoleSession;
    final String rawInput = _inputController.text;
    final String targetId = widget.boardId;
    final bool sent = await widget.bleService.sendConsoleLine(
      targetId,
      rawInput,
    );
    if (sent && mounted && session == _consoleSession) {
      // A delayed send must not erase the next command being drafted.
      if (_inputController.text == rawInput) _inputController.clear();
      scrollConsoleToBottom(force: true);
    }
  }

  List<ConsoleCompletion> _completions(TextEditingValue value) =>
      completeConsoleDraft(
        value,
        settings: widget.bleService.immediateSettingsSnapshot(widget.boardId),
      );

  void _insertCompletion(ConsoleCompletion completion, TextEditingValue draft) {
    // A held suggestion must not overwrite a newer draft or an IME edit.
    if (_inputController.value != draft) return;
    _inputController.value = completion.value;
    _showKeyboardFromInput();
  }

  KeyEventResult _onInputKeyEvent(FocusNode node, KeyEvent event) {
    if (event is! KeyDownEvent ||
        event.logicalKey != LogicalKeyboardKey.tab ||
        HardwareKeyboard.instance.isShiftPressed ||
        HardwareKeyboard.instance.isControlPressed ||
        HardwareKeyboard.instance.isAltPressed ||
        HardwareKeyboard.instance.isMetaPressed) {
      return KeyEventResult.ignored;
    }
    final draft = _inputController.value;
    final choices = _completions(draft);
    if (choices.isEmpty) return KeyEventResult.ignored;
    _insertCompletion(choices.first, draft);
    return KeyEventResult.handled;
  }

  Widget _completionRow(EdgeInsets safe, double navigationClearance) {
    return ListenableBuilder(
      listenable: Listenable.merge([_inputController, _inputFocusNode]),
      builder: (context, _) {
        if (!_inputFocusNode.hasFocus) return const SizedBox.shrink();
        final draft = _inputController.value;
        final choices = _completions(draft);
        if (choices.isEmpty) return const SizedBox.shrink();
        final strings = litchStrings(context);
        return Padding(
          padding: EdgeInsets.only(
            left: safe.left + 4,
            right: safe.right + 4 + navigationClearance,
          ),
          child: Semantics(
            container: true,
            label: strings.consoleSuggestions,
            // These transient tap targets disappear when the editor loses
            // focus. Native traversal must use the surviving Console controls.
            child: ExcludeFocusTraversal(
              child: SingleChildScrollView(
                scrollDirection: Axis.horizontal,
                child: Row(
                  key: const ValueKey('console-suggestions'),
                  children: [
                    for (final choice in choices)
                      Tooltip(
                        message: strings.consoleInsertSuggestion(choice.label),
                        excludeFromSemantics: true,
                        child: TextButton(
                          key: ValueKey('console-insert-${choice.label}'),
                          style: TextButton.styleFrom(
                            foregroundColor: _foreground,
                            minimumSize: const Size(48, 48),
                            textStyle: TextStyle(
                              fontFamily: _resolvedFontFamily(context),
                            ),
                          ),
                          onPressed: () => _insertCompletion(choice, draft),
                          child: Text(
                            choice.label,
                            semanticsLabel: strings.consoleInsertSuggestion(
                              choice.label,
                            ),
                          ),
                        ),
                      ),
                  ],
                ),
              ),
            ),
          ),
        );
      },
    );
  }

  Future<void> _setTraceOutputEnabled(bool enabled) async {
    if (_traceCommandInFlight || !_connectedToBoard) {
      return;
    }

    final session = _consoleSession;
    setState(() {
      _traceFailedSession = null;
      _traceCommandSession = session;
      _traceCommandTargetEnabled = enabled;
    });
    _refreshAppearance?.call(() {});
    final String targetId = widget.boardId;
    final bool sent = await widget.bleService.setConsoleTraceOutputEnabled(
      targetId,
      enabled,
    );
    if (!mounted || _traceCommandSession != session) {
      return;
    }
    setState(() {
      _traceCommandSession = null;
      _traceCommandTargetEnabled = null;
      if (!sent && session == _consoleSession) _traceFailedSession = session;
    });
    _refreshAppearance?.call(() {});
    if (!sent &&
        _refreshAppearance == null &&
        session == _consoleSession &&
        _connectedToBoard) {
      _notices.showNotice(
        context,
        SnackBar(content: Text(litchStrings(context).consoleTraceFailed)),
      );
    }
  }

  @override
  Widget build(BuildContext context) => StreamBuilder<ConnectionStateUpdate>(
    stream: widget.bleService.connectionStream,
    builder: (context, snapshot) {
      final connected = _connectedToBoard;
      final connecting =
          snapshot.data?.deviceId == widget.boardId &&
          snapshot.data?.connectionState == DeviceConnectionState.connecting;
      return _buildConsole(context, connected, connecting);
    },
  );

  Widget _buildConsole(BuildContext context, bool connected, bool connecting) {
    final connectionLabel = connecting
        ? litchStrings(context).boardConnecting
        : litchStrings(context).boardDisconnected;
    final media = MediaQuery.of(context);
    final systemInsets = media.viewPadding;
    final safe = LitchWorkspaceGeometry.isTabletWindow(media.size)
        ? systemInsets + const EdgeInsets.symmetric(horizontal: 8)
        : systemInsets;
    final usableSize = Size(
      media.size.width - systemInsets.horizontal,
      media.size.height - systemInsets.vertical,
    );
    final landscapeShell = LitchWorkspaceGeometry.usesCompactLandscape(
      media.size,
      usableSize,
    );
    final navigationClearance = landscapeShell && !_keyboardVisible
        ? LitchWorkspaceGeometry(usableSize).compactNavigationWidth +
              LitchWorkspaceGeometry.landscapeGap
        : 0.0;
    // Console always keeps its compact navigation on the right so the command
    // prompt and terminal origin remain stable in either landscape rotation.
    final formatter = _timeFormatter(context);
    return ColoredBox(
      key: const ValueKey('console-terminal-surface'),
      color: _background,
      child: Column(
        children: <Widget>[
          Expanded(
            child: Stack(
              children: <Widget>[
                Positioned.fill(
                  child: StreamBuilder<List<String>>(
                    stream: widget.bleService.consoleStream,
                    initialData: widget.bleService.consoleLinesSnapshot,
                    builder: (context, snapshot) {
                      final lines =
                          snapshot.hasData && snapshot.data!.isNotEmpty
                          ? snapshot.data!
                          : <String>[litchStrings(context).consoleWaiting];
                      final update = (lines.length, lines.last);
                      if (update != _lastConsoleUpdate) {
                        final shouldStickToBottom = _isNearBottom();
                        _lastConsoleUpdate = update;
                        _scheduleConsoleScroll(
                          shouldScroll: shouldStickToBottom,
                        );
                      }
                      final occurrences = <String, int>{};
                      return ListView(
                        controller: _consoleScrollController,
                        padding: EdgeInsets.fromLTRB(
                          safe.left + 4,
                          56,
                          safe.right + 4,
                          8,
                        ),
                        children: [
                          SelectionArea(
                            onSelectionChanged: (selection) {
                              _selectingOutput =
                                  selection != null &&
                                  selection.plainText.isNotEmpty;
                            },
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.stretch,
                              children: [
                                for (final line in lines)
                                  KeyedSubtree(
                                    key: ValueKey((
                                      line,
                                      occurrences.update(
                                        line,
                                        (count) => count + 1,
                                        ifAbsent: () => 0,
                                      ),
                                    )),
                                    child: _consoleLine(line, formatter),
                                  ),
                                if (_showWelcome)
                                  ConsoleWelcome(
                                    key: const ValueKey('console-welcome'),
                                    fontFamily: _resolvedFontFamily(context),
                                    onLineRevealed: () =>
                                        _scheduleConsoleScroll(
                                          shouldScroll: _isNearBottom(),
                                        ),
                                    onComplete: () {
                                      if (mounted) {
                                        setState(() => _showWelcome = false);
                                      }
                                    },
                                  ),
                              ],
                            ),
                          ),
                        ],
                      );
                    },
                  ),
                ),
                Positioned(
                  top: 4,
                  right: safe.right + 4,
                  child: Material(
                    color: _background.withValues(alpha: .92),
                    borderRadius: BorderRadius.circular(24),
                    child: IconButton(
                      key: const ValueKey('console-appearance-button'),
                      tooltip: litchStrings(context).consoleAppearance,
                      constraints: const BoxConstraints(
                        minWidth: 48,
                        minHeight: 48,
                      ),
                      color: _foreground,
                      iconSize: 20,
                      icon: const Icon(Icons.settings_outlined),
                      onPressed: _showAppearance,
                    ),
                  ),
                ),
              ],
            ),
          ),
          Divider(height: 1, color: _foreground.withValues(alpha: .35)),
          if (!connected)
            Semantics(
              liveRegion: true,
              child: Padding(
                padding: EdgeInsets.fromLTRB(
                  safe.left + 4,
                  4,
                  safe.right + 4,
                  0,
                ),
                child: Align(
                  alignment: Alignment.centerLeft,
                  child: Text(
                    connectionLabel,
                    key: const ValueKey('console-connection-status'),
                    style: TextStyle(color: _foreground),
                  ),
                ),
              ),
            ),
          _completionRow(safe, navigationClearance),
          Padding(
            padding: EdgeInsets.fromLTRB(
              safe.left + 4,
              2,
              safe.right + 4 + navigationClearance,
              4,
            ),
            child: Row(
              key: const ValueKey('console-command-row'),
              children: <Widget>[
                Text(
                  '> ',
                  key: const ValueKey('console-command-prompt'),
                  style: TextStyle(
                    color: _foreground,
                    fontFamily: _resolvedFontFamily(context),
                    fontSize: 13,
                  ),
                ),
                Expanded(
                  child: TextField(
                    key: const ValueKey('console-command-input'),
                    focusNode: _inputFocusNode,
                    controller: _inputController,
                    autofocus: false,
                    autocorrect: false,
                    enableSuggestions: false,
                    enableIMEPersonalizedLearning: false,
                    autofillHints: const <String>[],
                    textCapitalization: TextCapitalization.none,
                    smartDashesType: SmartDashesType.disabled,
                    smartQuotesType: SmartQuotesType.disabled,
                    keyboardType: TextInputType.visiblePassword,
                    textInputAction: connected
                        ? TextInputAction.send
                        : TextInputAction.unspecified,
                    onTap: _showKeyboardFromInput,
                    onTapAlwaysCalled: true,
                    // Reading output and sending commands keep the terminal
                    // ready for the next command. Dismissal is explicit.
                    onTapOutside: (_) {},
                    // Flutter restarts native input after onSubmitted, even
                    // when focus stays here. Completion alone keeps it open.
                    onEditingComplete: () {
                      _inputController.clearComposing();
                      unawaited(_sendLineFromInput());
                    },
                    style: TextStyle(
                      color: _foreground,
                      fontFamily: _resolvedFontFamily(context),
                    ),
                    cursorColor: _foreground,
                    decoration: InputDecoration.collapsed(
                      hintText: litchStrings(context).consoleCommand,
                      hintStyle: TextStyle(
                        color: _foreground.withValues(alpha: .65),
                        fontFamily: _resolvedFontFamily(context),
                      ),
                    ),
                  ),
                ),
                IconButton(
                  key: const ValueKey('console-hide-keyboard'),
                  tooltip: _keyboardVisible
                      ? litchStrings(context).consoleHideKeyboard
                      : litchStrings(context).consoleShowKeyboard,
                  onPressed: _toggleKeyboard,
                  color: _foreground,
                  icon: Icon(
                    _keyboardVisible
                        ? Icons.keyboard_arrow_down_rounded
                        : Icons.keyboard_arrow_up_rounded,
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

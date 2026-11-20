import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../services/app_litchcraft_preferences.dart';
import '../services/device_landscape_side.dart';
import 'litch_workspace_geometry.dart';

/// A single owner for viewport settling and the landscape content transition.
/// The shell itself is never animated through a rotation transform.
class OrientationWorkspace extends StatefulWidget {
  const OrientationWorkspace({super.key, required this.child, this.sideReader});

  final Widget child;
  final Future<DeviceLandscapeSide?> Function()? sideReader;

  @override
  State<OrientationWorkspace> createState() => _OrientationWorkspaceState();
}

class _OrientationWorkspaceState extends State<OrientationWorkspace>
    with SingleTickerProviderStateMixin, WidgetsBindingObserver {
  static const _settleDelay = Duration(milliseconds: 100);
  static const _channel = MethodChannel(
    'com.foxieproducts.litch/interface_orientation',
  );
  late final _turn = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 220),
    value: 1,
  );
  MediaQueryData? _coherent;
  MediaQueryData? _candidate;
  Timer? _settleTimer;
  DeviceLandscapeSide? _side;
  DeviceLandscapeSide? _geometrySide;
  DeviceLandscapeSide? _pendingSide;
  int _readGeneration = 0;
  int _turnGeneration = 0;
  bool _compensateHalfTurn = false;
  bool _hasLandscapePose = false;
  bool _requestedTabletPreference = false;
  bool? _tabletToolsOnLeftOverride;

  bool get _tabletToolsOnLeft =>
      _tabletToolsOnLeftOverride ?? (_side == DeviceLandscapeSide.left);

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (!_requestedTabletPreference &&
        LitchWorkspaceGeometry.isTabletWindow(MediaQuery.sizeOf(context))) {
      _requestedTabletPreference = true;
      unawaited(_loadTabletPreference());
    }
  }

  Future<void> _loadTabletPreference() async {
    try {
      final onLeft = await AppLitchcraftPreferences.loadTabletControlsOnLeft();
      if (mounted && onLeft != null && _tabletToolsOnLeftOverride == null) {
        setState(() => _tabletToolsOnLeftOverride = onLeft);
      }
    } catch (_) {
      // The current scene side remains available without preference storage.
    }
  }

  void _moveTabletToolsToOtherSide() {
    final onLeft = !_tabletToolsOnLeft;
    setState(() => _tabletToolsOnLeftOverride = onLeft);
    // Navigation and the workbench switch together even if storage is unavailable.
    unawaited(
      AppLitchcraftPreferences.saveTabletControlsOnLeft(
        onLeft,
      ).catchError((Object _) {}),
    );
  }

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    FocusManager.instance.addListener(_focusChanged);
    if (widget.sideReader == null) {
      _channel.setMethodCallHandler((call) async {
        if (call.method == 'orientationChanged') _readSide();
      });
    }
    _readSide();
  }

  void _focusChanged() {
    if (widget.sideReader != null) return;
    final focusContext = FocusManager.instance.primaryFocus?.context;
    final editing =
        focusContext?.widget is EditableText ||
        focusContext?.findAncestorWidgetOfExactType<EditableText>() != null;
    unawaited(
      _channel.invokeMethod<void>('editing', editing).catchError((Object _) {}),
    );
  }

  @override
  void didChangeMetrics() => _readSide();

  Future<void> _readSide() async {
    final generation = ++_readGeneration;
    final snapshot = widget.sideReader != null
        ? DeviceOrientationSnapshot(side: await widget.sideReader!())
        : await const DeviceLandscapeSideReader().readSnapshot();
    final next = snapshot.side;
    if (!mounted || generation != _readGeneration) return;
    final compensate =
        snapshot.compensateHalfTurn &&
        _coherent?.orientation != Orientation.portrait;
    if (_compensateHalfTurn != compensate) {
      setState(() => _compensateHalfTurn = compensate);
    }
    // Android can briefly report an unresolved display rotation between the
    // configuration and display callbacks. Keep the last coherent side and
    // any turn already in flight instead of making that transient cancel it.
    if (next == null) return;
    final viewport = View.of(context).physicalSize;
    final landscape = viewport.width > viewport.height;
    final startsLandscapePose = landscape && !_hasLandscapePose;
    _hasLandscapePose = landscape;
    // Entering landscape uses its first reported side directly. A side kept
    // from before portrait does not represent a half-turn within landscape.
    if (startsLandscapePose ||
        _side == null ||
        _coherent?.orientation != Orientation.landscape) {
      ++_turnGeneration;
      _turn.value = 1;
      setState(() {
        _side = next;
        _pendingSide = null;
      });
      return;
    }
    if (next == _pendingSide || (next == _side && _pendingSide == null)) return;
    final turnGeneration = ++_turnGeneration;
    if (next == _side) {
      // A rapid reversal cancels the obsolete turn, never commits its side.
      _pendingSide = null;
      _turn.animateTo(1, duration: const Duration(milliseconds: 100));
      return;
    }
    _pendingSide = next;
    // Geometry follows the physical side at the start of the transition. The
    // canvas and opted-in glyphs then make one finite half-turn in their final
    // wells; the toolbar shell never rotates or snaps after the animation.
    setState(() => _side = next);
    if (MediaQuery.maybeOf(context)?.disableAnimations ?? false) {
      _turn.value = 1;
    } else {
      await _turn.forward(from: 0).orCancel.catchError((Object _) {});
    }
    if (!mounted || turnGeneration != _turnGeneration) return;
    setState(() => _pendingSide = null);
  }

  bool _sameGeometry(MediaQueryData a, MediaQueryData b) =>
      a.size == b.size && a.viewPadding == b.viewPadding;

  void _observeGeometry(MediaQueryData incoming) {
    _coherent ??= incoming;
    final sideChanged = _geometrySide != _side;
    _geometrySide = _side;
    if (_sameGeometry(_coherent!, incoming)) {
      _candidate = null;
      _settleTimer?.cancel();
      return;
    }
    // Use the current insets with a resize or a confirmed landscape turn.
    // Only inset updates outside those transitions need time to settle.
    if (_coherent!.size != incoming.size ||
        sideChanged ||
        _pendingSide != null) {
      final wasLandscape = _coherent!.orientation == Orientation.landscape;
      _settleTimer?.cancel();
      _candidate = null;
      _coherent = incoming;
      if (wasLandscape && incoming.orientation == Orientation.portrait) {
        // A landscape turn cannot keep rotating content after portrait wins.
        ++_readGeneration;
        ++_turnGeneration;
        _pendingSide = null;
        _hasLandscapePose = false;
        _compensateHalfTurn = false;
        _turn.value = 1;
      }
      return;
    }
    if (_candidate != null && _sameGeometry(_candidate!, incoming)) return;
    _candidate = incoming;
    _settleTimer?.cancel();
    _settleTimer = Timer(_settleDelay, () {
      if (!mounted || _candidate == null) return;
      setState(() {
        _coherent = _candidate;
        _candidate = null;
      });
    });
  }

  @override
  void dispose() {
    ++_readGeneration;
    ++_turnGeneration;
    _settleTimer?.cancel();
    WidgetsBinding.instance.removeObserver(this);
    FocusManager.instance.removeListener(_focusChanged);
    if (widget.sideReader == null) _channel.setMethodCallHandler(null);
    _turn.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final incoming = MediaQuery.of(context);
    _observeGeometry(incoming);
    final coherent = _coherent!;
    // Text scale, accessibility and keyboard insets remain live. Only viewport
    // geometry is retained; a keyboard must never wait on a rotation timer.
    EdgeInsets orientInsets(EdgeInsets value) => !_compensateHalfTurn
        ? value
        : EdgeInsets.fromLTRB(value.right, value.bottom, value.left, value.top);
    EdgeInsets workspaceInsets(EdgeInsets value) {
      final oriented = orientInsets(value);
      if (coherent.orientation != Orientation.landscape) return oriented;
      // Keep the workspace centered on the physical display while protecting
      // both gesture/corner edges. An asymmetric bottom inset must not shift
      // every board upward by half of that inset.
      final vertical = math.max(oriented.top, oriented.bottom);
      var left = oriented.left;
      var right = oriented.right;
      if (Theme.of(context).platform == TargetPlatform.iOS &&
          !LitchWorkspaceGeometry.isTabletWindow(coherent.size) &&
          _hasLandscapePose &&
          _side != null) {
        // UIKit on phones reports a conservative strip on both edges. Native
        // orientation identifies the camera edge, so preserve that real inset.
        // Portrait has no lateral safe strip. Match that usable-edge geometry
        // on the unobstructed landscape edge. Until the current pose is known,
        // both strips remain protected.
        // Tablet windows and Android keep both reported lateral safe areas.
        if (_side == DeviceLandscapeSide.left) {
          left = 0;
        } else {
          right = 0;
        }
      }
      return EdgeInsets.fromLTRB(left, vertical, right, vertical);
    }

    final permanentInsets = workspaceInsets(coherent.viewPadding);
    final media = incoming.copyWith(
      size: coherent.size,
      viewPadding: permanentInsets,
      // Live padding loses its gesture inset when an IME covers that edge.
      // Keep permanent landscape geometry stable; viewInsets below separately
      // carries the keyboard intrusion to the active input surface.
      padding: coherent.orientation == Orientation.landscape
          ? permanentInsets
          : orientInsets(
              _candidate != null ? coherent.padding : incoming.padding,
            ),
      viewInsets: orientInsets(incoming.viewInsets),
    );
    return ClipRect(
      child: RotatedBox(
        // An instantaneous coordinate correction, not a shell animation.
        // UIKit's actual scene state, never OS version alone, authorizes it.
        quarterTurns: _compensateHalfTurn ? 2 : 0,
        child: MediaQuery(
          data: media,
          child: OrientationWorkspaceScope(
            side: _side,
            turn: _turn,
            tabletToolsOnLeft: _tabletToolsOnLeft,
            moveTabletToolsToOtherSide: _moveTabletToolsToOtherSide,
            child: widget.child,
          ),
        ),
      ),
    );
  }
}

class OrientationWorkspaceScope extends InheritedWidget {
  const OrientationWorkspaceScope({
    super.key,
    required this.side,
    required this.turn,
    required this.tabletToolsOnLeft,
    required this.moveTabletToolsToOtherSide,
    required super.child,
  });

  final DeviceLandscapeSide? side;
  final Animation<double> turn;
  final bool tabletToolsOnLeft;
  final VoidCallback moveTabletToolsToOtherSide;

  static OrientationWorkspaceScope? maybeOf(BuildContext context) =>
      context.dependOnInheritedWidgetOfExactType<OrientationWorkspaceScope>();

  @override
  bool updateShouldNotify(OrientationWorkspaceScope oldWidget) =>
      side != oldWidget.side ||
      tabletToolsOnLeft != oldWidget.tabletToolsOnLeft ||
      turn != oldWidget.turn;
}

/// Opt-in canvas/icon animation. Labels, navigation surfaces and tool wells do
/// not rotate; after the finite turn the shared side commits for every page.
class OrientationContentTurn extends StatelessWidget {
  const OrientationContentTurn({super.key, required this.child});
  final Widget child;

  @override
  Widget build(BuildContext context) {
    final scope = OrientationWorkspaceScope.maybeOf(context);
    if (scope == null) return child;
    return AnimatedBuilder(
      animation: scope.turn,
      builder: (context, child) => Transform.rotate(
        angle: (1 - Curves.easeOutCubic.transform(scope.turn.value)) * math.pi,
        child: child,
      ),
      child: child,
    );
  }
}

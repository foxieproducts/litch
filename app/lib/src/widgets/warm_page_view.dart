import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';

/// The app has at most four retained page shells. Lay out that bounded set
/// before explicit navigation so each destination is ready when selected.
/// Controllers and page children remain the existing single instances.
class WarmPageView extends PageView {
  WarmPageView({
    super.key,
    required PageController controller,
    required super.children,
  }) : assert(children.length <= 4),
       super(
         controller: controller,
         physics: const NeverScrollableScrollPhysics(),
       );

  @override
  State<PageView> createState() => _WarmPageViewState();
}

class _WarmPageViewState extends State<PageView> {
  @override
  Widget build(BuildContext context) {
    final direction = Directionality.of(context) == TextDirection.rtl
        ? AxisDirection.left
        : AxisDirection.right;
    return Scrollable(
      axisDirection: direction,
      controller: widget.controller,
      physics: widget.physics,
      scrollBehavior: ScrollConfiguration.of(
        context,
      ).copyWith(scrollbars: false),
      viewportBuilder: (context, position) => Viewport(
        axisDirection: direction,
        offset: position,
        cacheExtent: 4,
        cacheExtentStyle: CacheExtentStyle.viewport,
        slivers: [SliverFillViewport(delegate: widget.childrenDelegate)],
      ),
    );
  }
}

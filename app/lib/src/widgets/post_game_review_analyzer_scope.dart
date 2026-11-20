import 'package:flutter/widgets.dart';

import '../services/post_game_review_analyzer.dart';

class PostGameReviewJobOwnerScope extends InheritedWidget {
  const PostGameReviewJobOwnerScope({
    super.key,
    required this.owner,
    required super.child,
  });

  final PostGameReviewJobOwner owner;

  static PostGameReviewJobOwner? maybeOf(BuildContext context) => context
      .dependOnInheritedWidgetOfExactType<PostGameReviewJobOwnerScope>()
      ?.owner;

  @override
  bool updateShouldNotify(PostGameReviewJobOwnerScope oldWidget) =>
      !identical(owner, oldWidget.owner);
}

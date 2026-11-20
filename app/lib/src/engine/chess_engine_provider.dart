import 'chess_engine_models.dart';

abstract interface class ChessEngineProvider {
  EngineCapabilities get capabilities;

  Stream<EngineSearchEvent> search(
    EngineSearchRequest request,
    EngineCancellation cancellation,
  );

  /// Releases any app-lifetime process, isolate, socket, or native engine.
  Future<void> close();
}

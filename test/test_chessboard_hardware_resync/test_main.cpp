#include <cstdio>

#include "hardware/chessboard_hardware.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

class StubBoardInput final : public modules::IBoardInput {
  public:
    uint64_t occ64 = 0;
    bool snapshotOk = true;

    uint32_t resyncCalls = 0;
    uint32_t snapshotCalls = 0;

    bool TryReadSnapshot(uint64_t& outOcc64) override {
        ++snapshotCalls;
        outOcc64 = occ64;
        return snapshotOk;
    }

    bool TryReadDelta(modules::SquareDelta&) override {
        return false;
    }

    void Resync() override {
        ++resyncCalls;
    }
};

} // namespace

int main() {
    std::puts("[CHESSBOARD_HARDWARE_RESYNC] start");

    StubBoardInput input;
    hardware::ChessboardHardware hw(input);

    input.occ64 = (1ULL << 0) | (1ULL << 7);

    hw.SetFlipSides(false);
    hw.Resync();

    Check(input.resyncCalls == 1, "calls board input resync");
    Check(input.snapshotCalls == 1, "reads snapshot once");

    Check(hw.GetOccupied(0), "raw 0 maps to logical 0 when not flipped");
    Check(hw.GetOccupied(7), "raw 7 maps to logical 7 when not flipped");
    Check(!hw.GetOccupied(63), "unoccupied square stays false");

    // Flip-sides is a 180 rotation: logical = 63 - raw.
    StubBoardInput input2;
    hardware::ChessboardHardware hw2(input2);
    input2.occ64 = (1ULL << 0) | (1ULL << 63);

    hw2.SetFlipSides(true);
    hw2.Resync();

    Check(hw2.GetOccupied(63), "raw 0 maps to logical 63 when flipped");
    Check(hw2.GetOccupied(0), "raw 63 maps to logical 0 when flipped");

    // Snapshot read failure should leave occupancy empty.
    StubBoardInput input3;
    hardware::ChessboardHardware hw3(input3);
    input3.occ64 = (1ULL << 1);
    input3.snapshotOk = false;

    hw3.SetFlipSides(false);
    hw3.Resync();

    Check(!hw3.GetOccupied(1), "failed snapshot does not set occupancy");

    if (g_failures == 0) {
        std::puts("[CHESSBOARD_HARDWARE_RESYNC] ok");
        return 0;
    }

    std::printf("[CHESSBOARD_HARDWARE_RESYNC] failures=%d\n", g_failures);
    return 1;
}

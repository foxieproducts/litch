// The aggregate already links this shared probe from its owning suite.
// Standalone async journeys need the same real allocation boundary once.
#if !defined(LITCH_ALL_NATIVE_AGGREGATE)
#include "../test_litchcraft_persistence/allocation_probe.cpp"
#endif

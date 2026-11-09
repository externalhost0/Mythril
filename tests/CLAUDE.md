# mythril tests

Two tiers. Pure tests (everything in this folder) check library logic exhaustively with no device and run anywhere. The device tier is running the real samples under validation, which today lives outside the repo as a per-machine check. It is weak at barrier hazards: lavapipe executes close to serially, so a missing barrier rarely corrupts output, and syncval cannot see bindless or BDA accesses. So barrier coverage lives in the pure tier, a passing device test is never proof that a barrier is right, and a pure test is never proof that rendering is right. Tests build in any top-level configure (`MYTH_BUILD_TESTS` defaults to `PROJECT_IS_TOP_LEVEL`); run `ctest --test-dir build/debug`. Sync rules come from `vk_sync_tables.h`, generated from the SDK's `vk.xml` at configure time by `gen_sync_tables.py`; never hand-copy stage or access tables.

Every new pure sync test follows the shape of `test_buffer_barriers.cpp`:

1. Test the function the library actually calls. If the logic is composed inline in a `cmd*` method, extract it into a pure `vkutil` function first and have the method call it. Only do this for logic the samples run.
2. The oracle (what accesses are possible for a given input) is written from the Vulkan spec, never from the code under test. Failure example: an oracle built from `kBufferUses` would agree with it if a row dropped BDA shader writes.
3. Enumerate the whole input space through the same mapping the library uses (`ToVkBufferUsage` over every usage bit subset and storage type, every valid feature set) instead of picking examples, and check the case count so an empty loop cannot pass.
4. Check validity against the vk.xml tables (no unknown bits), coverage against the oracle on both sides of each barrier, and tightness (every emitted stage and access bit is justified by some required use). ALL_COMMANDS, MEMORY_READ and MEMORY_WRITE are banned outright.
5. Add a `known_bad` case for each real bug the test is meant to catch and assert the checker flags it. Before trusting a new test, break the real code on purpose and confirm it fails (touch the file after restoring, same-second mtimes skip the rebuild).

Known limits of the pure tier: no queue family dimension (narrowed masks use graphics-only stages, fine while every `CommandBuffer` records on the graphics queue) and no feature gating beyond acceleration structures and ray tracing.

## Scope

Tests cover the code as it is and the inputs the samples actually produce. Never add or widen library functionality (new parameters, new result types, offscreen modes, sample hooks) just to make something testable. Extracting existing inline logic into a pure function the library then calls is fine. Add a test when a change is about to touch logic the samples run, or when a real bug is found.

// The compaction's rules (gpu/vk/vram_compact.h): tests/test_vram_compact.py compiles and runs this.
#include "../gpu/vk/vram_compact.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

int main() {
    using namespace vram_compact;
    const uint64_t MB = 1ull << 20;
    // pressed: the driver's count over 90% of what it allows; never without an allowance
    CHECK(!pressed(9000 * MB, 10900 * MB));
    CHECK(pressed(10023 * MB, 10900 * MB));   // (the uncompacted walk's count)
    CHECK(!pressed(10023 * MB, 0));
    // a run starts for RX_VK_COMPACT_MB of free space, for 192 MB while pressed, and never for more than was asked
    CHECK(run_from(512 * MB, false) == 512 * MB);
    CHECK(run_from(512 * MB, true) == 192 * MB);
    CHECK(run_from(64 * MB, true) == 64 * MB);
    CHECK(rest(false) == 600 && rest(true) == 120);
    // only the textures' pool's free space comes off the count
    CHECK(reclaimable(850 * MB, 215 * MB) == 215 * MB);    // (the rest is in the render targets' and the buffers' blocks)
    CHECK(reclaimable(100 * MB, 215 * MB) == 100 * MB);    // (never more than the blocks hold free in all)
    // the budget holds the count, less what a run gives back, at pct% of the allowance
    const uint64_t allowed = 10900 * MB, budget = 6144 * MB;
    CHECK(held_budget(budget, 5000 * MB, 9000 * MB, 200 * MB, allowed, 85) == budget);   // (8,800 of 9,265: under the line)
    CHECK(held_budget(budget, 5600 * MB, 10023 * MB, 215 * MB, allowed, 85) == 5600 * MB - (10023 - 215 - 9265) * MB);
    CHECK(held_budget(budget, 5600 * MB, 10023 * MB, 215 * MB, allowed, 90) == budget);   // (9,808 of 9,810: at 90 that count is let be)
    CHECK(held_budget(budget, 5600 * MB, 10323 * MB, 215 * MB, allowed, 90) == 5600 * MB - (10323 - 215 - 9810) * MB);
    CHECK(held_budget(4096 * MB, 5600 * MB, 10023 * MB, 215 * MB, allowed, 85) == 4096 * MB);   // (never more than it was)
    CHECK(held_budget(budget, 300 * MB, 10800 * MB, 0, allowed, 85) == 0);                      // (the excess is more than the textures hold)
    CHECK(held_budget(budget, 5600 * MB, 10023 * MB, 215 * MB, 0, 85) == budget);               // (no allowance known: left alone)
    CHECK(held_budget(budget, 5600 * MB, 100 * MB, 215 * MB, allowed, 85) == budget);           // (more to give back than is used)
    // the hold's pass keeps a live texture used in the last ten seconds: the eviction's own 60 presents is not enough for it
    CHECK(kHoldUnused == 600);
    CHECK(hold_keeps(0) && hold_keeps(60) && hold_keeps(599));   // (out of view for a second: a turning camera brings it back)
    CHECK(!hold_keeps(600) && !hold_keeps(3600));
    // the reserve: a line five points under the hold's, for the textures unused thirty seconds
    CHECK(reserve_pct(85) == 80 && reserve_pct(50) == 45 && reserve_pct(3) == 0);
    CHECK(kReserveUnused == 1800 && kReserveUnused > kHoldUnused);
    CHECK(hold_target(59, 5000 * MB, 4500 * MB) == UINT64_MAX);          // (used a second ago: stays, whatever the lines ask)
    CHECK(hold_target(599, 5000 * MB, 4500 * MB) == UINT64_MAX);
    CHECK(hold_target(600, 5000 * MB, 4500 * MB) == 5000 * MB);          // (unused ten seconds: for the hold's own line)
    CHECK(hold_target(1799, 5000 * MB, 4500 * MB) == 5000 * MB);
    CHECK(hold_target(1800, 5000 * MB, 4500 * MB) == 4500 * MB);         // (unused thirty: down to the reserve's line)
    CHECK(hold_target(600, 0, 4500 * MB) == UINT64_MAX);                 // (the hold's line not crossed: only the long unused go)
    CHECK(hold_target(1800, 0, 4500 * MB) == 4500 * MB);
    // a count between the two lines: the reserve asks for less than the budget, the hold for nothing
    CHECK(held_budget(budget, 5000 * MB, 9000 * MB, 200 * MB, allowed, 85) == budget);
    CHECK(held_budget(budget, 5000 * MB, 9000 * MB, 200 * MB, allowed, reserve_pct(85)) == 5000 * MB - (9000 - 200 - 8720) * MB);
    // and it is done with live textures once those chosen and the dead render targets together cover the excess
    CHECK(!hold_met(5600 * MB, 0, 0, 5000 * MB));                 // (600 MB over, nothing chosen yet)
    CHECK(!hold_met(5600 * MB, 599 * MB, 0, 5000 * MB));
    CHECK(hold_met(5600 * MB, 600 * MB, 0, 5000 * MB));
    CHECK(hold_met(5600 * MB, 0, 600 * MB, 5000 * MB));           // (the dead targets alone cover it: no live texture goes)
    CHECK(hold_met(5600 * MB, 0, 2500 * MB, 5000 * MB));          // (the walk's 2.5 GB of dead targets)
    CHECK(!hold_met(5600 * MB, 100 * MB, 400 * MB, 5000 * MB));   // (100 MB still to go)
    CHECK(hold_met(5600 * MB, 200 * MB, 400 * MB, 5000 * MB));
    CHECK(hold_met(300 * MB, 200 * MB, 200 * MB, 0));             // (more chosen than the textures hold, their sum having drifted)
    std::printf("PASS: the compaction's rules\n");
    return 0;
}

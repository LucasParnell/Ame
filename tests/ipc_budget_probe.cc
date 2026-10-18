// Host probe for the userspace poll-budget controller (pure logic, no kernel).
#include <cassert>
#include <cstdio>
#include "mochi/ipc_budget.h"

int main() {
    using mochi::BudgetController;

    // Zero budget: a full window of parks lifts it to the floor, not above.
    BudgetController c;
    for (uint32_t i = 0; i < BudgetController::kWindow; ++i) c.note_wait(true);
    assert(c.budget == BudgetController::kMin);

    // Sustained parks double it until the cap.
    for (int window = 0; window < 8; ++window)
        for (uint32_t i = 0; i < BudgetController::kWindow; ++i) c.note_wait(true);
    assert(c.budget == BudgetController::kMax);

    // Decay needs kQuietWindows empty windows, then one 1/8 step each time.
    const uint32_t before = c.budget;
    for (uint32_t i = 0; i < BudgetController::kWindow * (BudgetController::kQuietWindows - 1); ++i)
        c.note_wait(false);
    assert(c.budget == before);              // still inside the quiet run
    for (uint32_t i = 0; i < BudgetController::kWindow * 4; ++i) c.note_wait(false);
    assert(c.budget < before && c.budget >= BudgetController::kMin);
    for (uint32_t i = 0; i < BudgetController::kWindow * 400; ++i) c.note_wait(false);
    assert(c.budget == BudgetController::kMin);

    // Dead band: a minority of parks changes nothing.
    BudgetController held;
    held.seed(512);
    for (uint32_t i = 0; i < BudgetController::kWindow; ++i) held.note_wait(i < 7);
    assert(held.budget == 512);

    // A window that is half parks is already a growth signal.
    for (uint32_t i = 0; i < BudgetController::kWindow; ++i) held.note_wait(i < BudgetController::kGrowParks);
    assert(held.budget == 1024);

    // Seeds are not trusted: values above the cap are clamped.
    BudgetController seeded;
    seeded.seed(99999);
    assert(seeded.budget == BudgetController::kMax);

    std::puts("budget controller: floor, growth, cap, decay, dead band, clamped seed");
    return 0;
}

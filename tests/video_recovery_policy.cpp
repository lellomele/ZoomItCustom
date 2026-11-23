#include "../src/VideoRecovery.h"
#include <iostream>
#include <limits>
#include <stdexcept>
void Require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
int main() {
    try {
        zoomit::video::RepairWindow state;
        Require(state.Request(100, 1), "first signal requests repair");
        for (ULONGLONG now = 110; now < 1800; now += 10)
            Require(!state.Request(now, 1), "burst coalesces");
        Require(state.Delay(1800) > 0 && state.Delay(2100) == 0, "quiet wait has a firm maximum");
        state.attempts = 2;
        Require(!state.Exhausted(2200), "third attempt remains possible");
        state.attempts = 3;
        Require(state.Exhausted(2200), "failure retry count bounded");
        state.Success(2200);
        Require(!state.Request(2300, 2), "completion notifications cannot self-loop");
        Require(state.Request(2300, 2, true), "a genuinely new driver event bypasses quiet cooldown");
        state.Cancel();
        Require(!state.pending && state.generation != 2, "cancel invalidates old queued work");
        state.completed = 0;
        state.Request(2400, 3);
        state.Success(2500);
        state.Request(3600, 4);
        Require(state.Exhausted(3600), "frequent repair cycles fail closed rather than looping forever");
        zoomit::video::RepairWindow timeout;
        timeout.Request(100, 1);
        Require(timeout.Exhausted(8100), "total recovery deadline");
        RECT view{350, 316, 2030, 1436}, same{0, 0, 2520, 1680}, result{};
        Require(zoomit::video::FitViewport(view, same, same, 1.5f, result) && EqualRect(&view, &result),
                "same display preserves exact source");
        RECT smaller{0, 0, 1920, 1080};
        Require(zoomit::video::FitViewport(view, same, smaller, 1.5f, result) &&
                    result.right - result.left == 1280 && result.bottom - result.top == 720 &&
                    result.right <= 1920 && result.bottom <= 1080,
                "resized display retains factor and clamps viewport");
        RECT left{-1920, 0, 0, 1080};
        Require(zoomit::video::FitViewport(view, same, left, 2, result) && result.left >= -1920 &&
                    result.right <= 0,
                "negative monitor coordinates");
        RECT moved = same, shiftedView = view;
        OffsetRect(&moved, -3911, 213);
        OffsetRect(&shiftedView, -3911, 213);
        Require(zoomit::video::FitViewport(view, same, moved, 1.5f, result) && EqualRect(&result, &shiftedView),
                "a display moved left and down retains every relative viewport coordinate");
        Require(zoomit::video::FitViewport(shiftedView, moved, same, 1.5f, result) && EqualRect(&result, &view),
                "moving back from a negative origin restores the original viewport");
        Require(zoomit::video::FitViewport(view, same, same, 1, result) && EqualRect(&same, &result),
                "1x remains a valid active view");
        for (float invalid :
             {0.0f, 0.5f, 33.0f, std::numeric_limits<float>::quiet_NaN(), (std::numeric_limits<float>::infinity)()})
            Require(!zoomit::video::FitViewport(view, same, same, invalid, result), "invalid zoom rejected");
        Require(!zoomit::video::FitViewport(view, same, RECT{}, 2, result), "removed/empty monitor rejected");
        Require(!zoomit::video::FitViewport(view, moved, same, 2, result),
                "viewport outside the captured display rejected");
        std::cout << "PASS video recovery policy: bounded bursts/retries/deadlines, cancellation, no self-loop, "
                     "exact and rebased viewports.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

#include "capture_wait.h"
#include "check.h"
#include "../sender/win/frame_policy.h"

struct Frame { unsigned value = 0; };
struct Capture {
    enum class Result { Frame, Timeout };
    HANDLE entered = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE permit = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    unsigned timeout = 0;
    Result acquire(unsigned ms, Frame &frame) {
        timeout = ms;
        SetEvent(entered);
        if (WaitForSingleObject(permit, ms) != WAIT_OBJECT_0) return Result::Timeout;
        frame.value = 42;
        return Result::Frame;
    }
    ~Capture() { CloseHandle(entered); CloseHandle(permit); }
};

int main()
{
    Capture source;
    lls::CaptureWait<Capture, Frame> wait(source);
    CHECK(wait.open() && wait.start(1000, 7));
    CHECK(!wait.start(10, 8));
    CHECK(WaitForSingleObject(source.entered, 1000) == WAIT_OBJECT_0 && source.timeout == 100);
    HANDLE recovery = CreateEventW(nullptr, FALSE, TRUE, nullptr);
    HANDLE events[] = {recovery, wait.event()};
    CHECK(WaitForMultipleObjects(2, events, FALSE, 1000) == WAIT_OBJECT_0);
    Frame frame;
    Capture::Result result{};
    uint64_t generation = 0;
    CHECK(!wait.take(frame, result, generation)); // recovery did not wait for acquisition
    lls::FramePolicy policy;
    policy.encoded(100000, false);
    policy.encoded(117666, true); // acquisition still pending when repeat fired
    SetEvent(source.permit);
    CHECK(WaitForSingleObject(wait.event(), 1000) == WAIT_OBJECT_0);
    CHECK(wait.take(frame, result, generation));
    CHECK(frame.value == 42 && generation == 7 && result == Capture::Result::Frame);
    CHECK(!lls_frame_policy_should_encode(&policy, 117667, true, false));
    CHECK(lls_frame_policy_should_encode(&policy, 132332, true, false));
    CHECK(frame.value == 42); // the one-shot update is retained until that gate
    CHECK(!wait.pending() && !wait.take(frame, result, generation));
    CHECK(wait.start(5, 8));
    CHECK(WaitForSingleObject(wait.event(), 1000) == WAIT_OBJECT_0);
    CHECK(wait.take(frame, result, generation) && generation == 8 && result == Capture::Result::Timeout);
    CHECK(wait.start(100, 9));
    wait.close(); // outstanding acquisition drains before source destruction
    CloseHandle(recovery);
    puts("capture wait: recovery wake, frame handoff, timeout and in-flight shutdown verified");
}

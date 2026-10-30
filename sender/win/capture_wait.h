#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>

namespace lls {
// DXGI AcquireNextFrame cannot be cancelled. Isolate only that wait on a worker
// so RECOVER can wake the encoding thread immediately. All GPU conversion and
// encoding remain on the pipeline thread. Start only when the previous result
// has been consumed/released; event handoff publishes the frame and result.
template<class Source, class Frame> class CaptureWait {
public:
    using Result = typename Source::Result;
    explicit CaptureWait(Source &source) : source_(source) {}
    ~CaptureWait() { close(); }
    CaptureWait(const CaptureWait &) = delete;
    CaptureWait &operator=(const CaptureWait &) = delete;
    bool open() {
        request_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!request_ || !ready_) return false;
        thread_ = CreateThread(nullptr, 0, run, this, 0, nullptr);
        return thread_ && SetThreadPriority(thread_, THREAD_PRIORITY_HIGHEST);
    }
    bool start(unsigned timeout_ms, uint64_t generation) {
        if (pending_ || !thread_) return false;
        timeout_ = timeout_ms > 100 ? 100 : timeout_ms;
        generation_ = generation;
        frame_ = {};
        pending_ = true;
        return SetEvent(request_) != FALSE;
    }
    bool take(Frame &frame, Result &result, uint64_t &generation) {
        if (!pending_ || WaitForSingleObject(ready_, 0) != WAIT_OBJECT_0) return false;
        frame = frame_; result = result_; generation = generation_;
        ResetEvent(ready_);
        pending_ = false;
        return true;
    }
    HANDLE event() const { return pending_ ? ready_ : nullptr; }
    bool pending() const { return pending_; }
    int64_t completed_qpc() const { return completed_qpc_; } // valid after take()
    void close() {
        if (thread_) {
            stopping_.store(true);
            SetEvent(request_);
            WaitForSingleObject(thread_, INFINITE); // acquire bounded to 100 ms
            CloseHandle(thread_); thread_ = nullptr;
        }
        if (request_) { CloseHandle(request_); request_ = nullptr; }
        if (ready_) { CloseHandle(ready_); ready_ = nullptr; }
    }
private:
    static DWORD WINAPI run(void *context) {
        auto &self = *static_cast<CaptureWait *>(context);
        for (;;) {
            if (WaitForSingleObject(self.request_, INFINITE) != WAIT_OBJECT_0 || self.stopping_.load()) return 0;
            self.result_ = self.source_.acquire(self.timeout_, self.frame_);
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            self.completed_qpc_ = now.QuadPart;
            SetEvent(self.ready_);
        }
    }
    Source &source_;
    HANDLE request_ = nullptr, ready_ = nullptr, thread_ = nullptr;
    std::atomic<bool> stopping_{false};
    bool pending_ = false;
    unsigned timeout_ = 0;
    uint64_t generation_ = 0;
    int64_t completed_qpc_ = 0;
    Frame frame_{};
    Result result_{};
};
} // namespace lls

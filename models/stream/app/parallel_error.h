#pragma once

#include <atomic>
#include <exception>
#include <stdexcept>
#include <string>

namespace stream {

// FIX(2026-11-24 E1): an exception that leaves an OpenMP parallel region calls std::terminate (no message, abnormal exit).
// Each parallel body catches everything with `catch (...) { errors.capture(); }`; the first error is kept, the remaining
// iterations are skipped (`if (errors.failed()) continue;`) and, after the region, rethrow() raises one
// std::runtime_error with a clear message, so main() prints it and exits with a non-zero code.
class ParallelError {
public:
    bool failed() const noexcept { return failed_.load(std::memory_order_relaxed); }

    void capture() noexcept
    {
        #pragma omp critical(stream_parallel_error)
        {
            if (!first_) first_ = std::current_exception();
        }
        failed_.store(true, std::memory_order_relaxed);
    }

    void rethrow(const std::string& stage) const
    {
        if (!first_) return;
        std::string what = "未知异常";
        try {
            std::rethrow_exception(first_);
        } catch (const std::exception& error) {
            what = error.what();
        } catch (...) {
        }
        throw std::runtime_error(stage + "的并行计算出错并已中止：" + what);
    }

private:
    std::exception_ptr first_;
    std::atomic<bool> failed_{false};
};

}  // namespace stream

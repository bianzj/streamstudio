// ==========================================================================
// 文件：parallel_error.h
// 功能：在 OpenMP 并行区内安全地收集异常，避免异常逸出并行区导致 std::terminate
// 主要模块：
//   1. ParallelError：记录首个异常（capture）、供其余迭代快速跳过（failed）、并行区后重抛（rethrow）
// 数据流：image.cpp / inversion.cpp 的 #pragma omp parallel for 循环体捕获异常 → 循环后 rethrow
//          → main() 打印错误信息并以非零退出码结束
// 注意：只保留第一个异常；其余迭代通过 failed() 跳过，不再继续计算
// ==========================================================================

#pragma once

#include <atomic>
#include <exception>
#include <stdexcept>
#include <string>

namespace stream {

// 模块：ParallelError（线程安全的首个异常收集器）
// 用法：并行体内 try/catch(...) 调用 capture()；循环体开头 if (errors.failed()) continue;；
//       并行区结束后在串行代码里调用 rethrow(阶段名)。
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

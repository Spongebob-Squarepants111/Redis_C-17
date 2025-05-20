#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <atomic>
#include <type_traits>
#include <random>
#include <optional>
#include <shared_mutex>

class DoubleBufferThreadPool {
public:
    explicit DoubleBufferThreadPool(size_t threads = std::thread::hardware_concurrency());
    ~DoubleBufferThreadPool();
    
    DoubleBufferThreadPool(const DoubleBufferThreadPool&) = delete;
    DoubleBufferThreadPool& operator=(const DoubleBufferThreadPool&) = delete;
    DoubleBufferThreadPool(DoubleBufferThreadPool&&) noexcept = default;
    DoubleBufferThreadPool& operator=(DoubleBufferThreadPool&&) noexcept = default;

    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

    [[nodiscard]] size_t pending_tasks() const noexcept;
    void shutdown();

private:
    struct Buffer {
        std::queue<std::function<void()>> tasks;
        mutable std::shared_mutex mutex;
        std::condition_variable_any cv;
    };

    // 双缓冲核心组件
    static inline constexpr size_t BUFFER_COUNT = 2;
    std::array<Buffer, BUFFER_COUNT> buffers_;
    std::atomic<size_t> write_index_{0};  // 当前写入缓冲索引
    std::atomic<bool> stop_{false};
    std::vector<std::thread> workers_;
    
    // 动态切换参数
    static inline constexpr size_t INITIAL_THRESHOLD = 1000;
    std::atomic<size_t> switch_threshold_{INITIAL_THRESHOLD};

    void worker_thread();
    void switch_buffers() noexcept;  // 声明切换缓冲区的函数
};

template <class F, class... Args>
auto DoubleBufferThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>> {
    using return_type = std::invoke_result_t<F, Args...>;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        [f = std::forward<F>(f), ...args = std::forward<Args>(args)]() mutable {
            return std::invoke(std::forward<F>(f), std::forward<Args>(args)...);
        }
    );
    
    std::future<return_type> res = task->get_future();

    if (const size_t idx = write_index_.load(); !stop_) {
        {
            std::unique_lock lock(buffers_[idx].mutex);
            buffers_[idx].tasks.emplace([task]() { (*task)(); });
        }
        
        // 检查任务数量，决定是否切换缓冲区
        if (buffers_[idx].tasks.size() >= switch_threshold_.load()) {
            switch_buffers();
        }
        
        // 使用if constexpr优化通知逻辑
        if constexpr (BUFFER_COUNT == 2) {
            buffers_[0].cv.notify_one();
            buffers_[1].cv.notify_one();
        } else {
            buffers_[idx].cv.notify_one();
        }
        
        return res;
    }
    
    throw std::runtime_error("enqueue on stopped pool");
}
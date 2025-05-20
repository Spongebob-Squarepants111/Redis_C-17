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

class DoubleBufferThreadPool {
public:
    explicit DoubleBufferThreadPool(size_t threads = std::thread::hardware_concurrency());
    ~DoubleBufferThreadPool();

    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result_t<F, Args...>>;

    size_t pending_tasks() const noexcept;
    void shutdown();

private:
    struct Buffer {
        std::queue<std::function<void()>> tasks;
        mutable std::mutex mutex;
        std::condition_variable cv;
    };

    // 双缓冲核心组件
    Buffer buffers_[2];
    std::atomic<size_t> write_index_{0};  // 当前写入缓冲索引
    std::atomic<bool> stop_{false};
    std::vector<std::thread> workers_;
    
    // 动态切换参数
    static constexpr size_t INITIAL_THRESHOLD = 1000;
    std::atomic<size_t> switch_threshold_{INITIAL_THRESHOLD};

    void worker_thread();
    void switch_buffers() noexcept;  // 声明切换缓冲区的函数
};


template <class F, class... Args>
auto DoubleBufferThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<typename std::invoke_result_t<F, Args...>> {
    using return_type = typename std::invoke_result_t<F, Args...>;

    auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
    auto task = std::make_shared<std::packaged_task<return_type()>>(bound);
    std::future<return_type> res = task->get_future();

    int idx = write_index_.load();  // 获取当前写入的缓冲区
    {
        std::lock_guard<std::mutex> lk(buffers_[idx].mutex);
        if (stop_) throw std::runtime_error("enqueue on stopped pool");
        buffers_[idx].tasks.emplace([task]() { (*task)(); });
    }
    
    // 检查任务数量，决定是否切换缓冲区
    if (buffers_[idx].tasks.size() >= switch_threshold_.load()) {
        switch_buffers();
    }
    
    // buffers_[idx].cv.notify_one();  // 唤醒一个工作线程
    buffers_[0].cv.notify_one();
    buffers_[1].cv.notify_one();
    return res;
}
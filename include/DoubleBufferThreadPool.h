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
#include <chrono>

// 定义缓存行大小为64字节，通常CPU缓存行大小
#define CACHE_LINE_SIZE 64

class DoubleBufferThreadPool {
public:
    explicit DoubleBufferThreadPool(size_t initial_threads = std::thread::hardware_concurrency());
    ~DoubleBufferThreadPool();

    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result_t<F, Args...>>;

    size_t pending_tasks() const noexcept;
    void shutdown();

    // 性能统计接口
    struct alignas(CACHE_LINE_SIZE) PerformanceStats {
        size_t total_tasks;
        size_t completed_tasks;
        size_t buffer_switches;
        double avg_processing_time;
        size_t active_threads;
        size_t total_threads;
    };
    
    PerformanceStats get_stats() const;

private:
    // 线程池配置
    struct alignas(CACHE_LINE_SIZE) ThreadConfig {
        const size_t min_threads;
        const size_t max_threads;
        std::atomic<size_t> current_threads{0};
    };

    // 自适应阈值
    struct alignas(CACHE_LINE_SIZE) AdaptiveThreshold {
        std::atomic<size_t> current;
        std::atomic<size_t> hits{0};
        std::atomic<size_t> misses{0};
        
        explicit AdaptiveThreshold(size_t initial) : current(initial) {}
        
        void adjust() {
            size_t h = hits.load();
            size_t m = misses.load();
            if (h + m >= ADJUSTMENT_INTERVAL) {
                if (h > m * 2) {
                    current.fetch_add(current.load() / 10);
                } else if (m > h) {
                    current.fetch_sub(current.load() / 10);
                }
                hits = 0;
                misses = 0;
            }
        }
    };

    // 性能指标
    struct alignas(CACHE_LINE_SIZE) PoolMetrics {
        std::atomic<size_t> total_tasks{0};
        std::atomic<size_t> completed_tasks{0};
        std::atomic<size_t> buffer_switches{0};
        std::atomic<double> avg_processing_time{0};
        
        void update_processing_time(double time_ms) {
            double current_avg = avg_processing_time.load();
            size_t completed = completed_tasks.load();
            avg_processing_time.store(
                (current_avg * completed + time_ms) / (completed + 1)
            );
        }
    };

    // 工作线程指标
    struct alignas(CACHE_LINE_SIZE) WorkerMetrics {
        std::atomic<size_t> processed_tasks{0};
        std::atomic<size_t> active_time{0};
        
        WorkerMetrics() : processed_tasks(0), active_time(0) {}
        WorkerMetrics(const WorkerMetrics& other) 
            : processed_tasks(other.processed_tasks.load())
            , active_time(other.active_time.load()) {}
        WorkerMetrics& operator=(const WorkerMetrics& other) {
            processed_tasks.store(other.processed_tasks.load());
            active_time.store(other.active_time.load());
            return *this;
        }
    };

    struct alignas(CACHE_LINE_SIZE) Buffer {
        std::queue<std::function<void()>> tasks;
        alignas(CACHE_LINE_SIZE) mutable std::mutex mutex;
        std::condition_variable cv;
    };

    // 成员变量
    Buffer buffers_[2];
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> write_index_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<bool> stop_{false};
    std::vector<std::thread> workers_;
    std::vector<WorkerMetrics> worker_metrics_;
    ThreadConfig thread_config_;
    AdaptiveThreshold threshold_;
    PoolMetrics metrics_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> active_threads_{0};

    static constexpr size_t INITIAL_THRESHOLD = 1000;
    static constexpr size_t ADJUSTMENT_INTERVAL = 1000; // 调整阈值的样本间隔数

    // 私有方法
    void worker_thread();
    void switch_buffers() noexcept;
    bool try_steal_task(std::function<void()>& task);
    void check_and_adjust_thread_count();
    void resize_thread_pool(size_t target_size);
};



// 任务提交实现
template<class F, class... Args>
auto DoubleBufferThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<typename std::invoke_result_t<F, Args...>> {
    using return_type = typename std::invoke_result_t<F, Args...>;
    
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    
    std::future<return_type> res = task->get_future();
    
    size_t idx = write_index_.load();
    {
        std::lock_guard<std::mutex> lock(buffers_[idx].mutex);
        if (stop_) throw std::runtime_error("enqueue on stopped pool");
        
        buffers_[idx].tasks.emplace([task]() { (*task)(); });
        metrics_.total_tasks++;
    }
    
    if (buffers_[idx].tasks.size() >= threshold_.current.load()) {
        switch_buffers();
        threshold_.hits++;
    } else {
        threshold_.misses++;
    }
    
    threshold_.adjust();
    
    buffers_[0].cv.notify_one();
    buffers_[1].cv.notify_one();
    
    return res;
}
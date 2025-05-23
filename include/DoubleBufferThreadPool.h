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
#include <ostream>
#include <iostream>  // 用于std::cout默认参数
#include <limits>  // 用于std::numeric_limits

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
        // 基础统计信息
        size_t total_tasks;
        size_t completed_tasks;
        size_t buffer_switches;
        double avg_processing_time;    // 毫秒
        size_t active_threads;
        size_t total_threads;
        
        // 增强统计信息
        size_t read_buffer_tasks;      // 读缓冲区中的任务数
        size_t write_buffer_tasks;     // 写缓冲区中的任务数
        size_t peak_active_threads;    // 峰值活跃线程数
        double min_processing_time;    // 最小处理时间(毫秒)
        double max_processing_time;    // 最大处理时间(毫秒)
        std::chrono::steady_clock::time_point start_time; // 线程池启动时间
        double uptime_seconds;         // 运行时间(秒)
        size_t tasks_per_second;       // 每秒处理任务数
        
        // 打印详细统计信息
        std::ostream& print(std::ostream& os = std::cout, bool detailed = true) const {
            if (detailed) {
                os << "=== 线程池性能指标 ===\n"
                   << "运行时间: " << uptime_seconds << " 秒\n"
                   << "任务统计:\n"
                   << "  - 总提交任务数: " << total_tasks << "\n"
                   << "  - 已完成任务数: " << completed_tasks;
                   
                if (total_tasks > 0) {
                    double completion_rate = completed_tasks * 100.0 / total_tasks;
                    os << " (" << completion_rate << "%)";
                }
                
                os << "\n"
                   << "  - 处理速率: " << tasks_per_second << " 任务/秒\n"
                   << "缓冲区状态:\n"
                   << "  - 读缓冲区任务数: " << read_buffer_tasks << "\n"
                   << "  - 写缓冲区任务数: " << write_buffer_tasks << "\n"
                   << "  - 缓冲区切换次数: " << buffer_switches << "\n"
                   << "处理时间(毫秒):\n"
                   << "  - 平均: " << avg_processing_time << "\n"
                   << "  - 最小: " << min_processing_time << "\n"
                   << "  - 最大: " << max_processing_time << "\n"
                   << "线程使用:\n"
                   << "  - 当前活跃: " << active_threads << "/" << total_threads << "\n"
                   << "  - 历史峰值: " << peak_active_threads;
            } else {
                os << "线程池状态: "
                   << "任务总数=" << total_tasks 
                   << ", 已完成=" << completed_tasks
                   << ", 读缓冲区=" << read_buffer_tasks
                   << ", 写缓冲区=" << write_buffer_tasks
                   << ", 线程=" << active_threads << "/" << total_threads
                   << ", 处理时间=" << avg_processing_time << "ms";
            }
            return os;
        }
        
        // 输出运算符重载 (保留以兼容现有代码)
        friend std::ostream& operator<<(std::ostream& os, const PerformanceStats& stats) {
            os << "任务总数: " << stats.total_tasks 
               << ", 已完成任务: " << stats.completed_tasks
               << ", 缓冲区切换次数: " << stats.buffer_switches
               << ", 平均处理时间(ms): " << stats.avg_processing_time
               << ", 活跃线程数: " << stats.active_threads
               << ", 总线程数: " << stats.total_threads;
            return os;
        }
    };
    
    // 打印线程池统计信息
    void print_stats(std::ostream& os = std::cout, bool detailed = true) const {
        get_stats().print(os, detailed);
    }
    
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
        std::atomic<double> min_processing_time{std::numeric_limits<double>::max()};
        std::atomic<double> max_processing_time{0};
        std::atomic<size_t> peak_active_threads{0};
        std::chrono::steady_clock::time_point start_time;
        
        PoolMetrics() : start_time(std::chrono::steady_clock::now()) {}
        
        void update_processing_time(double time_ms) {
            // 更新平均处理时间
            double current_avg = avg_processing_time.load();
            size_t completed = completed_tasks.load();
            avg_processing_time.store(
                (current_avg * completed + time_ms) / (completed + 1)
            );
            
            // 更新最小处理时间
            double current_min = min_processing_time.load();
            if (time_ms < current_min) {
                min_processing_time.store(time_ms);
            }
            
            // 更新最大处理时间
            double current_max = max_processing_time.load();
            if (time_ms > current_max) {
                max_processing_time.store(time_ms);
            }
        }
        
        void update_peak_threads(size_t current_active) {
            size_t peak = peak_active_threads.load();
            if (current_active > peak) {
                peak_active_threads.store(current_active);
            }
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
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> write_buffer_{0}; // 写缓冲区(提交任务)
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> read_buffer_{1};  // 读缓冲区(执行任务)
    alignas(CACHE_LINE_SIZE) std::atomic<bool> stop_{false};
    std::vector<std::thread> workers_;
    std::vector<WorkerMetrics> worker_metrics_;
    ThreadConfig thread_config_;
    AdaptiveThreshold threshold_;
    PoolMetrics metrics_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> active_threads_{0};
    alignas(CACHE_LINE_SIZE) mutable std::mutex switch_mutex_; // 用于保护缓冲区切换

    static constexpr size_t INITIAL_THRESHOLD = 100;
    static constexpr size_t ADJUSTMENT_INTERVAL = 1000; // 调整阈值的样本间隔数

    // 私有方法
    void worker_thread();
    void switch_buffers() noexcept;
    bool need_switch_buffers() const noexcept;
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
    
    size_t write_idx = write_buffer_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(buffers_[write_idx].mutex);
        if (stop_) throw std::runtime_error("enqueue on stopped pool");
        
        buffers_[write_idx].tasks.emplace([task]() { (*task)(); });
        metrics_.total_tasks++;
    }
    
    // 检查是否需要切换缓冲区
    if (need_switch_buffers()) {
        std::lock_guard<std::mutex> switch_lock(switch_mutex_);
        if (need_switch_buffers()) {  // 双重检查以避免多线程竞争
            switch_buffers();
            threshold_.hits++;
        } else {
            threshold_.misses++;
        }
    } else {
        threshold_.misses++;
    }
    
    threshold_.adjust();
    
    return res;
}
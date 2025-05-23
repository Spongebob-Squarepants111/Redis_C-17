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

// 定义缓存行大小为64字节，避免伪共享
#define CACHE_LINE_SIZE 64

class ThreadPool {
public:
    explicit ThreadPool(size_t initial_threads = std::thread::hardware_concurrency());
    ~ThreadPool();

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
        double avg_processing_time;    // 毫秒
        size_t active_threads;
        size_t total_threads;
        
        // 增强统计信息
        size_t pending_tasks;          // 队列中的待处理任务
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
                   << "队列状态:\n"
                   << "  - 待处理任务数: " << pending_tasks << "\n"
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
                   << ", 待处理=" << pending_tasks
                   << ", 线程=" << active_threads << "/" << total_threads
                   << ", 处理时间=" << avg_processing_time << "ms";
            }
            return os;
        }
        
        // 输出运算符重载
        friend std::ostream& operator<<(std::ostream& os, const PerformanceStats& stats) {
            os << "任务总数: " << stats.total_tasks 
               << ", 已完成任务: " << stats.completed_tasks
               << ", 待处理任务: " << stats.pending_tasks
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

    // 性能指标
    struct alignas(CACHE_LINE_SIZE) PoolMetrics {
        std::atomic<size_t> total_tasks{0};
        std::atomic<size_t> completed_tasks{0};
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

    // 成员变量
    std::queue<std::function<void()>> tasks;
    mutable std::mutex queue_mutex; 
    std::condition_variable cv;
    alignas(CACHE_LINE_SIZE) std::atomic<bool> stop_{false};
    std::vector<std::thread> workers_;
    std::vector<WorkerMetrics> worker_metrics_;
    ThreadConfig thread_config_;
    PoolMetrics metrics_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> active_threads_{0};

    // 私有方法
    void worker_thread();
    void check_and_adjust_thread_count();
    void resize_thread_pool(size_t target_size);
};

// 任务提交实现
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<typename std::invoke_result_t<F, Args...>> {
    using return_type = typename std::invoke_result_t<F, Args...>;
    
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    
    std::future<return_type> res = task->get_future();
    
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (stop_) throw std::runtime_error("enqueue on stopped pool");
        
        tasks.emplace([task]() { (*task)(); });
        metrics_.total_tasks++;
    }
    
    // 通知一个等待中的线程有新任务
    cv.notify_one();
    
    return res;
}
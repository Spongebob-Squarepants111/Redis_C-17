#include "ThreadPool.h"

ThreadPool::ThreadPool(size_t initial_threads)
    : thread_config_{
        .min_threads = std::max<size_t>(2, initial_threads/2),
        .max_threads = initial_threads * 2
      } {
    resize_thread_pool(initial_threads);
}

ThreadPool::~ThreadPool() {
    shutdown();
}

ThreadPool::PerformanceStats ThreadPool::get_stats() const {
    // 获取当前待处理任务数
    size_t pending = 0;
    
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        pending = tasks.size();
    }
    
    // 计算运行时间(秒)
    auto now = std::chrono::steady_clock::now();
    double uptime = std::chrono::duration<double>(now - metrics_.start_time).count();
    
    // 计算每秒处理任务数
    size_t completed = metrics_.completed_tasks.load();
    size_t tasks_per_sec = uptime > 0 ? static_cast<size_t>(completed / uptime) : 0;
    
    return {
        metrics_.total_tasks,
        metrics_.completed_tasks,
        metrics_.avg_processing_time,
        active_threads_,
        workers_.size(),
        pending,
        metrics_.peak_active_threads.load(),
        metrics_.min_processing_time.load(),
        metrics_.max_processing_time.load(),
        metrics_.start_time,
        uptime,
        tasks_per_sec
    };
}

void ThreadPool::shutdown() {
    stop_ = true;
    cv.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

size_t ThreadPool::pending_tasks() const noexcept {
    std::lock_guard<std::mutex> lock(queue_mutex);
    return tasks.size();
}

void ThreadPool::check_and_adjust_thread_count() {
    size_t current_threads = workers_.size();
    size_t active = active_threads_.load();
    size_t pending = pending_tasks();
    
    size_t target_threads = current_threads;
    if (active == current_threads && pending > current_threads) {
        // 所有线程都很忙，且还有待处理任务，增加线程
        target_threads = std::min(current_threads + 2, thread_config_.max_threads);
    } else if (active < current_threads / 2 && current_threads > thread_config_.min_threads) {
        // 超过一半的线程空闲，减少线程
        target_threads = std::max(current_threads - 1, thread_config_.min_threads);
    }
    
    if (target_threads != current_threads) {
        resize_thread_pool(target_threads);
    }
}

void ThreadPool::resize_thread_pool(size_t target_size) {
    size_t current = workers_.size();
    if (target_size > current) {
        // 增加线程
        for (size_t i = current; i < target_size; ++i) {
            workers_.emplace_back([this] { worker_thread(); });
            worker_metrics_.emplace_back();
        }
    }
    // 减少线程通过自然退出实现
}

void ThreadPool::worker_thread() {
    while (!stop_) {
        std::function<void()> task;
        bool got_task = false;
        
        // 从队列获取任务
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            
            // 等待任务或超时
            cv.wait_for(lock, 
                std::chrono::milliseconds(100), 
                [this] { 
                    return stop_ || !tasks.empty(); 
                });
            
            // 检查是否有任务可执行
            if (!tasks.empty()) {
                task = std::move(tasks.front());
                tasks.pop();
                got_task = true;
            }
        }
        
        // 如果获取到任务，执行它
        if (got_task) {
            active_threads_++;
            // 更新峰值活跃线程数
            metrics_.update_peak_threads(active_threads_.load());
            
            auto start = std::chrono::steady_clock::now();
            
            task();
            
            auto end = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                end - start).count();
            
            metrics_.update_processing_time(duration);
            metrics_.completed_tasks++;
            active_threads_--;
            
            // 检查是否需要调整线程池大小
            check_and_adjust_thread_count();
        } else if (!stop_) {
            // 如果队列为空，短暂休眠避免CPU空转
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

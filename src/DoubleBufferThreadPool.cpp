#include "DoubleBufferThreadPool.h"

DoubleBufferThreadPool::DoubleBufferThreadPool(size_t initial_threads)
    : thread_config_{
        .min_threads = std::max<size_t>(2, initial_threads/2),
        .max_threads = initial_threads * 2
      }
    , threshold_(INITIAL_THRESHOLD)
    , write_buffer_(0)
    , read_buffer_(1) {
    resize_thread_pool(initial_threads);
}

DoubleBufferThreadPool::~DoubleBufferThreadPool() {
    shutdown();
}

DoubleBufferThreadPool::PerformanceStats DoubleBufferThreadPool::get_stats() const {
    // 获取当前读写缓冲区中的任务数
    size_t read_tasks = 0;
    size_t write_tasks = 0;
    
    {
        size_t read_idx = read_buffer_.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> read_lock(buffers_[read_idx].mutex);
        read_tasks = buffers_[read_idx].tasks.size();
    }
    
    {
        size_t write_idx = write_buffer_.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> write_lock(buffers_[write_idx].mutex);
        write_tasks = buffers_[write_idx].tasks.size();
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
        metrics_.buffer_switches,
        metrics_.avg_processing_time,
        active_threads_,
        workers_.size(),
        read_tasks,
        write_tasks,
        metrics_.peak_active_threads.load(),
        metrics_.min_processing_time.load(),
        metrics_.max_processing_time.load(),
        metrics_.start_time,
        uptime,
        tasks_per_sec
    };
}

void DoubleBufferThreadPool::shutdown() {
    stop_ = true;
    for (auto& buffer : buffers_) {
        buffer.cv.notify_all();
    }
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

size_t DoubleBufferThreadPool::pending_tasks() const noexcept {
    size_t count = 0;
    for (const auto& buffer : buffers_) {
        std::lock_guard<std::mutex> lock(buffer.mutex);
        count += buffer.tasks.size();
    }
    return count;
}


bool DoubleBufferThreadPool::need_switch_buffers() const noexcept {
    const size_t write_idx = write_buffer_.load(std::memory_order_acquire);
    size_t tasks_count = 0;
    
    // 用于记录首次有任务的时间，静态变量保持状态
    static std::chrono::steady_clock::time_point last_submit_time = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    
    {
        std::lock_guard<std::mutex> lock(buffers_[write_idx].mutex);
        tasks_count = buffers_[write_idx].tasks.size();
        
        // 如果有任务并且是第一批任务（时间点为默认值），更新提交时间
        if (tasks_count > 0 && last_submit_time == std::chrono::steady_clock::time_point{}) {
            last_submit_time = now;
        }
    }
    
    // 条件1: 达到任务数量阈值
    bool threshold_met = tasks_count >= threshold_.current.load(std::memory_order_relaxed);
    
    // 条件2: 任务提交后等待时间超过最大允许时间(例如50ms)
    bool time_exceeded = false;
    if (tasks_count > 0) {
        static const int MAX_WAIT_MS = 50; // 最大等待50毫秒
        time_exceeded = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_submit_time).count() > MAX_WAIT_MS;
        
        // 调试输出
        if (time_exceeded) {
            std::cout << "时间阈值触发缓冲区切换，当前任务数: " << tasks_count 
                      << "，等待时间: " << std::chrono::duration_cast<std::chrono::milliseconds>(now - last_submit_time).count() 
                      << "ms" << std::endl;
        }
    }
    
    // 任意条件满足则切换缓冲区
    if (threshold_met || time_exceeded) {
        if (time_exceeded) {
            last_submit_time = std::chrono::steady_clock::time_point{}; // 重置时间，准备下一轮计时
        }
        return true;
    }
    
    return false;
}

void DoubleBufferThreadPool::switch_buffers() noexcept {
    // 交换读写缓冲区
    size_t old_write = write_buffer_.load(std::memory_order_relaxed);
    size_t old_read = read_buffer_.load(std::memory_order_relaxed);
    
    // 原子地切换读写缓冲区
    write_buffer_.store(old_read, std::memory_order_release);
    read_buffer_.store(old_write, std::memory_order_release);
    
    // 通知所有等待读缓冲区的线程
    buffers_[old_write].cv.notify_all();
    
    metrics_.buffer_switches++;
}

void DoubleBufferThreadPool::check_and_adjust_thread_count() {
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

void DoubleBufferThreadPool::resize_thread_pool(size_t target_size) {
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

void DoubleBufferThreadPool::worker_thread() {
    while (!stop_) {
        std::function<void()> task;
        bool got_task = false;
        
        // 从读缓冲区获取任务
        {
            size_t read_idx = read_buffer_.load(std::memory_order_acquire);
            std::unique_lock<std::mutex> lock(buffers_[read_idx].mutex);
            
            // 如果读缓冲区为空，等待任务或超时
            if (buffers_[read_idx].tasks.empty()) {
                buffers_[read_idx].cv.wait_for(lock, 
                    std::chrono::milliseconds(100), 
                    [this, read_idx] { 
                        return stop_ || !buffers_[read_idx].tasks.empty(); 
                    });
            }
            
            // 再次检查是否有任务可执行
            if (!buffers_[read_idx].tasks.empty()) {
                task = std::move(buffers_[read_idx].tasks.front());
                buffers_[read_idx].tasks.pop();
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
            // 如果读缓冲区为空且写缓冲区有任务，尝试切换缓冲区
            if (need_switch_buffers()) {
                std::lock_guard<std::mutex> switch_lock(switch_mutex_);
                if (need_switch_buffers()) {
                    switch_buffers();
                }
            } else {
                // 如果两个缓冲区都为空，短暂休眠避免CPU空转
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
}

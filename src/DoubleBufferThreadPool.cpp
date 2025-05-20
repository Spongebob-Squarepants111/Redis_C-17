#include "DoubleBufferThreadPool.h"

DoubleBufferThreadPool::DoubleBufferThreadPool(size_t initial_threads)
    : thread_config_{
        .min_threads = std::max<size_t>(2, initial_threads/2),
        .max_threads = initial_threads * 2
      }
    , threshold_(INITIAL_THRESHOLD) {
    resize_thread_pool(initial_threads);
}

DoubleBufferThreadPool::~DoubleBufferThreadPool() {
    shutdown();
}

DoubleBufferThreadPool::PerformanceStats DoubleBufferThreadPool::get_stats() const {
    return {
        metrics_.total_tasks,
        metrics_.completed_tasks,
        metrics_.buffer_switches,
        metrics_.avg_processing_time,
        active_threads_,
        workers_.size()
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

void DoubleBufferThreadPool::switch_buffers() noexcept {
    write_index_.store(1 - write_index_.load());
    metrics_.buffer_switches++;
}

bool DoubleBufferThreadPool::try_steal_task(std::function<void()>& task) {
    size_t other_buffer = 1 - write_index_.load();
    std::unique_lock<std::mutex> lock(buffers_[other_buffer].mutex, std::try_to_lock);
    if (lock && !buffers_[other_buffer].tasks.empty()) {
        task = std::move(buffers_[other_buffer].tasks.front());
        buffers_[other_buffer].tasks.pop();
        return true;
    }
    return false;
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
        
        // 1. 尝试从当前活跃缓冲区获取任务
        {
            size_t current_buffer = write_index_.load();
            std::unique_lock<std::mutex> lock(buffers_[current_buffer].mutex);
            if (!buffers_[current_buffer].tasks.empty()) {
                task = std::move(buffers_[current_buffer].tasks.front());
                buffers_[current_buffer].tasks.pop();
                got_task = true;
            }
        }
        
        // 2. 如果当前缓冲区没有任务，尝试任务窃取
        if (!got_task) {
            got_task = try_steal_task(task);
        }
        
        // 3. 如果获取到任务，执行它
        if (got_task) {
            active_threads_++;
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
        } else {
            // 如果没有任务，等待条件变量
            std::unique_lock<std::mutex> lock(buffers_[write_index_.load()].mutex);
            buffers_[write_index_.load()].cv.wait_for(lock, 
                std::chrono::milliseconds(100), 
                [this] { return stop_ || !buffers_[write_index_.load()].tasks.empty(); });
        }
    }
} 
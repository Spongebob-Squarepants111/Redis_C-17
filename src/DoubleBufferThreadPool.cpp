#include "DoubleBufferThreadPool.h"
#include <stdexcept>

DoubleBufferThreadPool::DoubleBufferThreadPool(size_t threads) {
    workers_.reserve(threads);
    for (size_t i = 0; i < threads; ++i) {
        workers_.emplace_back([this] { worker_thread(); });
    }
}

DoubleBufferThreadPool::~DoubleBufferThreadPool() {
    shutdown();
}

void DoubleBufferThreadPool::worker_thread() {
    while (!stop_) {
        if (const size_t read_idx = 1 - write_index_.load(); true) {  // 选取"读缓冲"
            std::queue<std::function<void()>> local;
            {
                std::unique_lock lock(buffers_[read_idx].mutex);
                
                // 使用 std::cv.wait_for 增加超时机制
                if (buffers_[read_idx].cv.wait_for(lock, std::chrono::milliseconds(100), 
                    [this, read_idx] { return stop_ || !buffers_[read_idx].tasks.empty(); })) {
                    
                    if (stop_ && buffers_[read_idx].tasks.empty()) return;
                    
                    // 批量交换任务
                    local = std::move(buffers_[read_idx].tasks);
                    buffers_[read_idx].tasks = std::queue<std::function<void()>>();
                } 
            }

            // 执行任务
            while (!local.empty()) {
                if (auto& task = local.front(); task) {
                    task();
                }
                local.pop();
            }

            // 切换缓冲区
            switch_buffers();
        }
    }
}

void DoubleBufferThreadPool::switch_buffers() noexcept {
    // 切换缓冲区的核心逻辑：交换当前写入缓冲区
    write_index_.store(1 - write_index_.load(), std::memory_order_release);
    
    // 动态调整切换阈值（指数退避）
    const size_t new_threshold = std::min(
        switch_threshold_.load(std::memory_order_relaxed) * 2, 
        INITIAL_THRESHOLD * 16
    );
    switch_threshold_.store(new_threshold, std::memory_order_relaxed);
}

size_t DoubleBufferThreadPool::pending_tasks() const noexcept {
    size_t count = 0;
    for (size_t i = 0; i < BUFFER_COUNT; ++i) {
        std::shared_lock lock(buffers_[i].mutex);
        count += buffers_[i].tasks.size();
    }
    return count;
}

void DoubleBufferThreadPool::shutdown() {
    stop_.store(true, std::memory_order_release);
    
    for (auto& buffer : buffers_) {
        buffer.cv.notify_all();  // 唤醒所有工作线程
    }
    
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();  // 确保所有线程都退出
        }
    }
    
    // 清空任务队列
    for (auto& buffer : buffers_) {
        std::unique_lock lock(buffer.mutex);
        buffer.tasks = std::queue<std::function<void()>>();
    }
}

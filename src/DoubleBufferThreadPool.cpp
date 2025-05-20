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
    while (!stop_.load()) {
        int read_idx = 1 - write_index_.load();  // 选取“读缓冲”
        std::queue<std::function<void()>> local;
        {
            std::unique_lock<std::mutex> lk(buffers_[read_idx].mutex);
            
            // 使用 std::cv.wait_for 增加超时机制
            if (buffers_[read_idx].cv.wait_for(lk, std::chrono::milliseconds(100), [this, read_idx] {
                return stop_.load() || !buffers_[read_idx].tasks.empty();
            })) {
                if (stop_.load() && buffers_[read_idx].tasks.empty()) return;
                
                // 批量交换任务
                std::swap(local, buffers_[read_idx].tasks);
            } 
        }

        // 执行任务
        while (!local.empty()) {
            local.front()();
            local.pop();
        }

        // 切换缓冲区
        switch_buffers();
    }
}

void DoubleBufferThreadPool::switch_buffers() noexcept {
    // 切换缓冲区的核心逻辑：交换当前写入缓冲区
    write_index_.store(1 - write_index_.load());  // 切换缓冲区的索引
    
    // 动态调整切换阈值（指数退避）
    const size_t new_threshold = std::min(
        switch_threshold_.load() * 2, 
        INITIAL_THRESHOLD * 16
    );
    switch_threshold_.store(new_threshold);
}

size_t DoubleBufferThreadPool::pending_tasks() const noexcept {
    size_t count = 0;
    for (int i = 0; i < 2; ++i) {
        std::lock_guard<std::mutex> lk(buffers_[i].mutex);
        count += buffers_[i].tasks.size();
    }
    return count;
}

void DoubleBufferThreadPool::shutdown() {
    stop_ = true;
    for (auto& buffer : buffers_) {
        buffer.cv.notify_all();  // 唤醒所有工作线程
    }
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();  // 确保所有线程都退出
    }
}

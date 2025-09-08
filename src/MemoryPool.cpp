#include "MemoryPool.h"
#include <cstdlib>

MemoryBlockPool::MemoryBlockPool(size_t block_size, size_t initial_blocks)
    : block_size_(block_size)
    , blocks_per_chunk_(calculate_blocks_per_chunk(block_size))
    , next_free_(nullptr) {
    prealloc(initial_blocks);
}

MemoryBlockPool::~MemoryBlockPool() {
    clear();
}

void* MemoryBlockPool::allocate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!next_free_) {
        allocate_chunk();
        if (!next_free_) {
            throw std::bad_alloc();
        }
    }
    void* block = next_free_;
    next_free_ = *reinterpret_cast<void**>(next_free_);
    ++allocated_blocks_;
    return block;
}

void MemoryBlockPool::deallocate(void* block) {
    if (!block) return;
    std::lock_guard<std::mutex> lock(mutex_);
    *reinterpret_cast<void**>(block) = next_free_;
    next_free_ = block;
    if (allocated_blocks_ > 0) {
        --allocated_blocks_;
    }
}

void MemoryBlockPool::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (void* chunk : allocated_chunks_) {
        ::free(chunk);
    }
    allocated_chunks_.clear();
    next_free_ = nullptr;
    allocated_blocks_ = 0;
}

size_t MemoryBlockPool::block_size() const {
    return block_size_;
}

size_t MemoryBlockPool::allocated_blocks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocated_blocks_;
}

size_t MemoryBlockPool::allocated_chunks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocated_chunks_.size();
}

size_t MemoryBlockPool::calculate_blocks_per_chunk(size_t block_size) {
    if (block_size <= 32) return 512;
    if (block_size <= 64) return 256;
    if (block_size <= 128) return 128;
    if (block_size <= 256) return 64;
    if (block_size <= 512) return 32;
    if (block_size <= 1024) return 16;
    if (block_size <= 2048) return 8;
    if (block_size <= 4096) return 4;
    return 1;
}

void MemoryBlockPool::allocate_chunk() {
    size_t align = std::max<size_t>(16, block_size_);
    size_t chunk_size = blocks_per_chunk_ * block_size_;
    void* chunk = aligned_alloc(align, chunk_size);
    if (!chunk) {
        throw std::bad_alloc();
    }
    allocated_chunks_.push_back(chunk);
    char* chunk_ptr = static_cast<char*>(chunk);
    for (size_t i = 0; i < blocks_per_chunk_ - 1; ++i) {
        void* current_block = chunk_ptr + i * block_size_;
        void* next_block = chunk_ptr + (i + 1) * block_size_;
        *reinterpret_cast<void**>(current_block) = next_block;
    }
    void* last_block = chunk_ptr + (blocks_per_chunk_ - 1) * block_size_;
    *reinterpret_cast<void**>(last_block) = next_free_;
    next_free_ = chunk_ptr;
}

void MemoryBlockPool::prealloc(size_t num_blocks) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t chunks_needed = (num_blocks + blocks_per_chunk_ - 1) / blocks_per_chunk_;
    for (size_t i = 0; i < chunks_needed; ++i) {
        allocate_chunk();
    }
}



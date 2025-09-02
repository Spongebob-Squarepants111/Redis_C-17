#pragma once

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <cstring>  // for strerror
#include <cerrno>   // for errno
#endif

#include <thread>
#include <vector>
#include <iostream>
#include <algorithm>
#include <string>

/**
 * 线程亲和性管理类
 * 负责将线程绑定到特定的CPU核心，减少线程迁移开销，提高缓存局部性
 */
class ThreadAffinity {
public:
    /**
     * 获取系统CPU核心数
     */
    static unsigned int get_cpu_count() {
        return std::thread::hardware_concurrency();
    }
    
    /**
     * 绑定当前线程到指定CPU核心
     * @param cpu_id CPU核心ID (0-based)
     * @return 成功返回true，失败返回false
     */
    static bool bind_current_thread_to_cpu(int cpu_id) {
#ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);
        
        int result = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        if (result != 0) {
            std::cerr << "Failed to set thread affinity to CPU " << cpu_id 
                      << ": " << strerror(result) << std::endl;
            return false;
        }
        
        std::cout << "Thread " << std::this_thread::get_id() 
                  << " bound to CPU " << cpu_id << std::endl;
        return true;
#else
        std::cout << "Thread affinity not supported on this platform" << std::endl;
        return false;
#endif
    }
    
    /**
     * 绑定指定线程到CPU核心
     * @param thread 要绑定的线程
     * @param cpu_id CPU核心ID
     * @return 成功返回true，失败返回false
     */
    static bool bind_thread_to_cpu(std::thread& thread, int cpu_id) {
#ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);
        
        int result = pthread_setaffinity_np(thread.native_handle(), sizeof(cpu_set_t), &cpuset);
        if (result != 0) {
            std::cerr << "Failed to set thread affinity to CPU " << cpu_id 
                      << ": " << strerror(result) << std::endl;
            return false;
        }
        
        std::cout << "Thread " << thread.get_id() 
                  << " bound to CPU " << cpu_id << std::endl;
        return true;
#else
        std::cout << "Thread affinity not supported on this platform" << std::endl;
        return false;
#endif
    }
    
    /**
     * 获取当前线程绑定的CPU核心
     * @return CPU核心ID列表
     */
    static std::vector<int> get_current_thread_affinity() {
        std::vector<int> cpus;
        
#ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        
        int result = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        if (result == 0) {
            for (int i = 0; i < CPU_SETSIZE; i++) {
                if (CPU_ISSET(i, &cpuset)) {
                    cpus.push_back(i);
                }
            }
        }
#endif
        
        return cpus;
    }
    
    /**
     * 计算最优的CPU分配策略
     * @param thread_count 线程数量
     * @return CPU分配方案
     */
    static std::vector<int> calculate_optimal_cpu_assignment(size_t thread_count) {
        std::vector<int> assignment;
        unsigned int cpu_count = get_cpu_count();
        
        if (cpu_count == 0) {
            return assignment;
        }
        
        // 策略1: 如果线程数 <= CPU数，每个线程绑定一个CPU
        if (thread_count <= cpu_count) {
            for (size_t i = 0; i < thread_count; ++i) {
                assignment.push_back(static_cast<int>(i));
            }
        }
        // 策略2: 如果线程数 > CPU数，使用轮询分配
        else {
            for (size_t i = 0; i < thread_count; ++i) {
                assignment.push_back(static_cast<int>(i % cpu_count));
            }
        }
        
        return assignment;
    }
    
    /**
     * 设置线程调度优先级
     * @param policy 调度策略 (SCHED_NORMAL, SCHED_FIFO, SCHED_RR)
     * @param priority 优先级
     * @return 成功返回true
     */
    static bool set_thread_priority(int policy = 0, int priority = 0) {  // 0 = SCHED_NORMAL
#ifdef __linux__
        struct sched_param param;
        param.sched_priority = priority;
        
        int result = pthread_setschedparam(pthread_self(), policy, &param);
        if (result != 0) {
            std::cerr << "Failed to set thread scheduling: " << strerror(result) << std::endl;
            return false;
        }
        
        return true;
#else
        return false;
#endif
    }
    
    /**
     * 打印系统CPU拓扑信息
     */
    static void print_system_info() {
        std::cout << "=== System CPU Information ===" << std::endl;
        std::cout << "CPU Count: " << get_cpu_count() << std::endl;
        
        auto current_affinity = get_current_thread_affinity();
        std::cout << "Current Thread CPU Affinity: ";
        for (int cpu : current_affinity) {
            std::cout << cpu << " ";
        }
        std::cout << std::endl;
    }
};

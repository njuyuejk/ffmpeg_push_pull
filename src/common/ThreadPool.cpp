#include "common/ThreadPool.h"

ThreadPool::ThreadPool(size_t numThreads) : stop(false) {
    for (size_t i = 0; i < numThreads; ++i) {
        // 创建工作线程
        workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;

                {
                    // 等待任务或停止信号
                    std::unique_lock<std::mutex> lock(this->queueMutex);
                    this->condition.wait(lock, [this] {
                        return this->stop || !this->tasks.empty();
                    });

                    // 如果线程池停止且队列为空，线程退出
                    if (this->stop && this->tasks.empty()) {
                        return;
                    }

                    // 获取任务
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }

                // 执行任务
                task();
            }
        });
    }
}

size_t ThreadPool::size() const {
    return workers.size();
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        stop = true;
    }

    // 通知所有线程
    condition.notify_all();

    // 等待所有线程结束
    for (std::thread &worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}
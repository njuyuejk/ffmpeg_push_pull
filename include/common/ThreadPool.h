#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>

/**
 * @brief 线程池类
 * 用于管理和调度线程，处理多个任务
 */
class ThreadPool {
public:
    /**
     * @brief 构造函数
     * @param numThreads 线程池中的线程数
     */
    ThreadPool(size_t numThreads);

    /**
     * @brief 获取线程池中线程数量
     * @return 线程数量
     */
    size_t size() const;

    /**
     * @brief 提交任务到线程池
     * @param f 任务函数
     * @param args 任务函数的参数
     * @return 任务结果的future对象
     */
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
    -> std::future<typename std::result_of<F(Args...)>::type>;

    /**
     * @brief 析构函数，关闭线程池
     */
    ~ThreadPool();

private:
    // 工作线程向量
    std::vector<std::thread> workers;
    // 任务队列
    std::queue<std::function<void()>> tasks;

    // 同步相关
    std::mutex queueMutex;
    std::condition_variable condition;
    bool stop;
};

// 模板方法实现
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
-> std::future<typename std::result_of<F(Args...)>::type> {

    using return_type = typename std::result_of<F(Args...)>::type;

    // 创建一个封装了任务的shared_ptr智能指针
    auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    // 获取future对象，用于之后获取结果
    std::future<return_type> result = task->get_future();

    // 锁定并添加任务到队列
    {
        std::unique_lock<std::mutex> lock(queueMutex);

        // 不允许在线程池停止后提交任务
        if (stop) {
            throw std::runtime_error("Cannot enqueue on stopped ThreadPool");
        }

        // 添加任务到队列
        tasks.emplace([task]() { (*task)(); });
    }

    // 通知一个等待中的线程
    condition.notify_one();

    return result;
}

#endif // THREAD_POOL_H
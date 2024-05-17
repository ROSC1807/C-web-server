# C-web-server
基于C++实现的http server，基于epoll和Reactor模式实现
**项目情况**：

主要分为两个部分：

C++11新标准实现的线程池。

http-conn类。里面的成员包含读写缓冲区信息、socket连接的文件描述符，主从状态机信息、客户请求的目标文件信息等一系列信息。

**未来待添加：**

将所有的代码重构为C++11标准的实现

同步、异步日志系统

数据库连接池

定时器处理非活动连接

*目前只支持HTTP1.1，而且服务器只支持解析GET方法。需要进一步拓展。*

*畅想：支持协程*

目前只支持同步Reactor模式，暂时不支持异步Proactor模式，可以考虑添加异步Proactor模式







**项目主体逻辑**

<img src="C++%E5%AD%A6%E4%B9%A0%E8%B7%AF%E7%BA%BF/image-20240515140540746.png" alt="image-20240515140540746" style="zoom: 50%;" />

目前只支持同步Reactor模式，暂时不支持异步Proactor模式

**同步Reactor模式**：要求主线程（I/O处理单元）只负责监听文件描述符上是否有事件发生，有的话就立即将该事件通知工作线程（逻辑单元），将 socket 可读可写事件放入请求队列，交给工作线程处理。除此之外，主线程不做任何其他实质性的工作。读写数据，接受新的连接，以及处理客户请求均在工作线程中完成。

**异步Proactor模式**：Proactor 模式将所有 I/O 操作都交给主线程和内核来处理（进行读、写），工作线程仅仅负责业务逻辑。







**线程池**

```C++
#ifndef MY_THREAD_POOL_H
#define MY_THREAD_POOL_H

#include <vector>
#include <queue>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
//future是C++标准库中用于异步编程的头文件。这个头文件提供了std::async,std::future,std::promise等类和函数，用于实现异步操作和获取异步操作的结果
#include <functional>
#include <stdexcept>

class ThreadPool{
public:
    //构造函数
    ThreadPool(size_t);
    //右值引用两个功能，移动语义和完美转发，这里用来实现完美转发
    template <typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        ->std::future<typename std::result_of<F(Args...)>::type>;
    //std::result_of<F(Args...)> 是一个模板元函数，用于获取调用F传入参数“Args...”后的结果类型。
    //C++14之后可以使用更现代的std::invoke_result_t来替代。  
    //函数元模板是使用模板编写的，在编译时计算值的函数。模板元函数允许在编译期间执行一些计算，而不是在运行时执行。这种编译期计算的特性称为模板元编程。
        
    ~ThreadPool();    
private:
    //创建一个线程
    std::vector<std::thread> workers;
    //任务队列
    //std::function<void()>是C++标准库中的一个模板类，表示一个可以存储任何可调用对象
    std::queue<std::function<void()>> tasks;
    
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

inline ThreadPool::ThreadPool(size_t threads)
    :   stop(false)
{
    for(size_t i = 0; i < threads; ++i)
        //向线程池中加入线程，使用emplace_back直接在vector中创建线程，执行以下的lambda表达式函数
        workers.emplace_back(
            [this]
            {
                for(;;){
                    std::function<void()> task;
                    {   
                        //创建一个对queue_mutex的独占锁
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        //如果线程池不停止或者任务列表为空会一直阻塞
                        this->condition.wait(lock, [this]{return this->stop || !this->tasks.empty();});
                        if(this->stop && this->tasks.empty())
                            return;
                        //把任务队列中的第一个任务移动赋值给task，然后出队
                        task=std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            }
        );
}
//任务队列入队
template<typename F, typename... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
    ->std::future<typename std::result_of<F(Args...)>::type>
{
    using return_type = typename std::result_of<F(Args...)>::type;
    /*
    std::package_task和std::future是C++11中引入的两个类，用于处理异步任务的结果
    std::packaged_task是一个可调用目标，它包装了一个任务，该任务可以在另一个线程上运行。它可以捕获任务的返回值或异常，并将其存储在std::future对象中，以便以后使用
    std::future代表一个异步操作的结果。它可以用于从异步任务中获取返回值或异常。
    以下是使用std::packaged_task和std::future的基本步骤：
        1.创建一个std::packaged_task对象，该对象包装了要执行的任务。
        2.调用std::packaged_task对象的get_future()方法，该方法返回一个与任务关联的std::future对象。
        3.在另一个线程上调用std::packaged_task对象的operator()，以执行任务。
        4.在需要任务结果的地方，调用与任务关联的std::future对象的get()方法，以获取任务的返回值或异常。
    */
    auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if(stop)
        {
            throw std::runtime_error("ThreadPool stopped");
        }
        tasks.emplace([task]{(*task)();});
    }    
    condition.notify_one();
    return res;
}

inline ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
    for(std::thread &worker:workers)
        worker.join();
}


#endif

```

实现细节：

**池**：使用vector动态存储std::thread，在类对象创建时实现线程的初始化以及线程同步机制。（思考：这里的线程挑选机制是什么呢？）

**任务队列**：使用list动态存储std::function<void()>类型的函数包装器。并提供enqueue方法将任务添加至任务队列。

**enqueue方法**：是一个成员函数模板，接收一个可调用对象f和变长参数 Args...的右值引用（以实现完美转发），返回一个std::future类型的对象。由于任务队列中全部存储为std::function<void()>类型的函数包装器，需要在该方法中将 Args...参数提前绑定到f上，并使用std::future来提前获取f(Args...)的返回值。随后将lambda表达式包裹的可调用对象入队。

**线程同步**：使用<condition_variable>头文件中的条件变量类和`<mutex>` 头文件中的锁类型进行线程同步。具体来说，使用std::unique_lock锁管理类对std::mutex进行管理。使用std::condition_variable的wait方法阻塞线程，直到条件满足或者被notice系函数显示通知。



**http-conn类**

http-conn类实现了线程处理HTTP请求(解析HTTP请求报文，生成HTTP回复报文)的逻辑。主函数基于epoll多路复用实现同步Reactor模式，http-conn类中也包含epoll的逻辑。

http-conn类解析HTTP请求报文的步骤通过有限状态机来实现，并将有限状态机拆分为主、从状态机来简化步骤。

<img src="C++%E5%AD%A6%E4%B9%A0%E8%B7%AF%E7%BA%BF/image-20240516153921944.png" alt="image-20240516153921944" style="zoom: 80%;" />

<img src="C++%E5%AD%A6%E4%B9%A0%E8%B7%AF%E7%BA%BF/image-20240516153940559.png" alt="image-20240516153940559" style="zoom:80%;" />

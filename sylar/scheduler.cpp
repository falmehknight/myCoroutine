//
// Created by 谭颍豪 on 2024/6/25.
//

#include "scheduler.h"
#include "macro.h"
// #include "hook.h"
namespace sylar {

    static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

    // 当前线程的调度器，同一个调度器下的所有线程指向同一调度器实例
    static thread_local Scheduler *t_scheduler = nullptr;
    // 当前线程的调度协程，即主协程，每个线程都有独一份，包括caller线程
    static thread_local Fiber *t_scheduler_fiber = nullptr;

    Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name) {
        // 确保线程数大于0
        SYLAR_ASSERT(threads > 0);

        m_useCaller = use_caller;
        m_name = name;
        // 如果当前线程也可以作为调度线程
        if (use_caller) {
            // 需要启动的线程数减1
            --threads;
            // 启动主协程
            sylar::Fiber::GetThis();
            SYLAR_ASSERT(GetThis() == nullptr);
            t_scheduler = this;

            /**
            * caller线程的主协程不会被线程的调度协程run进行调度；而且，线程的调度协程停止时，应该返回caller线程的主协程
            * 在user caller情况下，把caller线程的主协程暂时保存起来，等调度协程结束时，再resume caller协程
            */
            // 重置调度协程m_rootFiber,将其指向新的Fiber
            m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0 , false));

            sylar::Thread::SetName(m_name);
            t_scheduler_fiber = m_rootFiber.get();
            m_rootThread = sylar::GetThreadId();
            // 当前线程id加入到线程池id中
            m_threadIds.push_back(m_rootThread);
        } else {
            m_rootThread = -1;
        }
        m_threadCount = threads;
    }

    Scheduler *Scheduler::GetThis() {
        return t_scheduler;
    }

    void Scheduler::start() {
        SYLAR_LOG_DEBUG(g_logger) << "start";
        MutexType::Lock lock(m_mutex);
        if (m_stopping) {
            SYLAR_LOG_ERROR(g_logger) << "Scheduler is stopped";
            return ;
        }
        // 确保线程池一开始是空的
        SYLAR_ASSERT(m_threads.empty());
        m_threads.resize(m_threadCount);

        // 构建线程池
        for(size_t i = 0; i < m_threadCount; i++) {
            m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this),
                                          m_name + "_" + std::to_string(i)));
            m_threadIds.push_back(m_threads[i]->getId());
        }
    }

    void Scheduler::run() {
        SYLAR_LOG_DEBUG(g_logger) << "run";
        setThis();
        // use_caller为false,即当前线程不为调度线程,这时才去构造主协程（调度协程）
        if (sylar::GetThreadId() != m_rootThread) {
            t_scheduler_fiber = sylar::Fiber::GetThis().get();
        }

        // idle协程以及任务协程
        Fiber::ptr idle_fiber(new Fiber(std::bind(&Scheduler::idle, this)));
        Fiber::ptr cb_fiber;

        // 调度任务
        ScheduleTask task;
        while (true) {
            task.reset();
            bool tickle_me = false; // 是否tickle其他线程进行任务调度
            {
                MutexType::Lock lock(m_mutex);
                auto it = m_tasks.begin();
                // 1. 遍历所有调度任务
                while (it != m_tasks.end()) {
                    // 1.1 指定了调度线程，但不是在当前线程上调度，标记一下需要通知其他线程进行调度，然后跳过这个任务，继续下一个。
                    if (it->thread != -1 && it->thread != sylar::GetThreadId()) {
                        ++it;
                        tickle_me = true;
                        continue;
                    }
                    // 1.2其他情况，即找到一个未指定线程或者是指定了当前线程的任务
                    SYLAR_ASSERT(it->fiber || it->cb);
                    if (it->fiber) {
                        // 当前任务是一个协程,确保他的状态是READY状态
                        SYLAR_ASSERT(it->fiber->getState() == Fiber::READY);
                    }
                    // 当前调度线程找到一个任务，准备开始调度，将其从任务队列中剔除，活动线程数加1
                    task = *it;
                    m_tasks.erase(it++);
                    ++m_activeThreadCount;
                    break;
                }
                // 2. 当前线程拿完一个任务之后，发现任务队列还有剩余，那个给其他线程发出通知
                tickle_me |= (it != m_tasks.end());
                if (tickle_me) {
                    tickle();
                }
                // 3.完成调度任务，分为三种
                if (task.fiber) {
                    // 3.1 协程,resume协程即可，resume返回时，协程要么执行完了，要不半路yield了，总之这个任务就算完成了，活跃线程数-1
                    task.fiber->resume();
                    --m_activeThreadCount;
                    task.reset();
                } else if (task.cb) {
                    // 3.2 函数
                    if (cb_fiber) {
                        // 复用协程
                        cb_fiber->reset(task.cb);
                    } else {
                        cb_fiber.reset(new Fiber(task.cb));
                    }
                    task.reset();
                    cb_fiber->resume();
                    --m_activeThreadCount;
                    cb_fiber.reset();
                } else {
                    // 3.3 进这个分支说明是任务队列空了，调度idle协程即可
                    if (idle_fiber->getState() == Fiber::TERM) {
                        // 调度器没有调度任务，那个idle协程会不停的resume/yield，不会结束，如果idle协程结束了，那么一定是调度器停止了
                        SYLAR_LOG_DEBUG(g_logger) << "idle fiber term";
                        break;
                    }
                    ++m_idleThreadCount;
                    idle_fiber->resume();
                    --m_idleThreadCount;
                }
            }
            SYLAR_LOG_DEBUG(g_logger) << "Scheduler::run() exit";
        }
    }

    void Scheduler::setThis() {
        t_scheduler = this;
    }

    Scheduler::~Scheduler() {
        SYLAR_LOG_DEBUG(g_logger) << "Scheduler::~Scheduler()";
        SYLAR_ASSERT(m_stopping);
        if (GetThis() == this) {
            t_scheduler = nullptr;
        }
    }

    Fiber *Scheduler::GetMainFiber() {
        return t_scheduler_fiber;
    }

    void Scheduler::stop() {
        SYLAR_LOG_DEBUG(g_logger) << "stop";
        if (stopping()) {
            return;
        }
        m_stopping = true;
        // 如果当前线程也为调度线程
        if (m_useCaller) {
            SYLAR_ASSERT(GetThis() == this);
        } else {
            SYLAR_ASSERT(GetThis() != this);
        }

        for (size_t i = 0; i < m_threadCount; i++) {
            tickle();
        }

        if (m_rootFiber) {
            tickle();
        }

        // 调度器协程结束的时候，应该返回caller协程
        if (m_rootFiber) {
            m_rootFiber->resume();
            SYLAR_LOG_DEBUG(g_logger) << "m_rootFiber end";
        }

        // 将线程池中的工作线程都运行完毕
        std::vector<Thread::ptr> threads;
        {
            MutexType::Lock lock(m_mutex);
            threads.swap(m_threads);
        }

        for (auto &i : threads) {
            i->join();
        }

    }

    void Scheduler::tickle() {
        SYLAR_LOG_DEBUG(g_logger) << "tickle";
    }

    void Scheduler::idle() {
        SYLAR_LOG_DEBUG(g_logger) << "idle";
        while(!stopping()) {
            sylar::Fiber::GetThis()->yield();
        }
    }

    bool Scheduler::stopping() {
        MutexType::Lock  lock(m_mutex);
        // 调度器正在停止且任务队列中无任务以及活跃线程数为0，则判断为可以停止。
        return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
    }
} // sylar
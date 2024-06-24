//
// Created by 谭颍豪 on 2024/6/23.
//

#include <atomic>
#include "fiber.h"
#include "log.h"

namespace sylar {

    static Logger::ptr g_logger = SYLAR_LOG_NAME("system");

    // 全局静态变量，用于统计当前的协程数
    static std::atomic<uint64_t> s_fiber_count{0};
    // 全局静态变量，用于生成协程id
    static std::atomic<uint64_t> s_fiber_id{0};

    // 线程局部变量，当前线程正在运行的协程
    static thread_local  Fiber *t_fiber = nullptr;
    // 线程局部变量，当前线程的主协程，切换到这个协程，就相当于切换到了主线程中运行，智能指针形式
    static thread_local Fiber::ptr t_thread_fiber = nullptr;

    /**
       * @brief 构造函数
       * @attention ⽆参构造函数只⽤于创建线程的第⼀个协程，也就是线程主函数对应的协程，
       * 这个协程只能由GetThis()⽅法调⽤，所以定义成私有⽅法
       */
    Fiber::Fiber() {
        SetThis(this);
        m_state = RUNNING;

        if (getcontext(&m_ctx)) {
            // 获取上下文失败
            SYLAR_ASSERT2(false, "getcontext");
        }
        // 协程数加1
        ++s_fiber_count;
        m_id = s_fiber_id++; // 协程id从0开始，用完加1

        SYLAR_LOG_DEBUG(g_logger) << "Fiber::Fiber() main id = " << m_id;
    }
} // sylar
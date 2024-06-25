//
// Created by 谭颍豪 on 2024/6/25.
//

#include "thread.h"
#include "log.h"
#include "util.h"


namespace sylar {
    static thread_local Thread *t_thread = nullptr;
    static thread_local std::string t_thread_name = "UNKNOW";

    static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");


    Thread *Thread::GetThis() {
        return t_thread;
    }

    const std::string &Thread::GetName() {
        return t_thread_name;
    }

    void Thread::SetName(const std::string &name) {
        if (name.empty()) {
            return ;
        }
        if (t_thread) {
            t_thread->m_name = name;
        }
        t_thread_name = name;
    }

    Thread::Thread(std::function<void()> cb, const std::string &name)
        : m_cb(cb)
        , m_name(name) {
        if (name.empty()) {
            m_name = "UNKNOW";
        }
        // 创建线程
        int rt = pthread_create(&m_thread, nullptr, &Thread::run, this);
        if (rt) {
            SYLAR_LOG_ERROR(g_logger) << "pthread_create thread fail, rt=" << rt
                                      << " name=" << name;
            throw std::logic_error("pthread_create error");
        }
        // 释放信号量，即线程可以执行了
        m_semaphore.wait();
    }

    Thread::~Thread() {
        if (m_thread) {
            pthread_detach(m_thread);
        }
    }

    void Thread::join() {
        if (m_thread) {
            // 等待线程完成，第二个参数代表不需要关注退出状态
            int rt = pthread_join(m_thread, nullptr);
            if (rt) {
                SYLAR_LOG_ERROR(g_logger) << "pthread_join thread fail, rt=" << rt
                                          << " name=" << m_name;
                throw std::logic_error("pthread_join error");
            }
            m_thread = 0;
        }
    }

    void *Thread::run(void *arg) {
        Thread *thread = static_cast<Thread*>(arg);
        t_thread = thread;
        t_thread_name = thread->m_name;
        thread->m_id = sylar::GetThreadId();
        // 设置线程名称，截取前 15 个字符
        pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());

        std::function<void()> cb;
        cb.swap(thread->m_cb);
        // 申请信号量，如果线程创建了（即信号量值不为0），则可以去执行函数。
        thread->m_semaphore.notify();
        cb();
        return nullptr;
    }

} // sylar
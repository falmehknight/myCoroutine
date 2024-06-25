//
// Created by 谭颍豪 on 2024/6/25.
//

#ifndef MYCOROUTINE_MUTEX_H
#define MYCOROUTINE_MUTEX_H


#include <thread>
#include <functional>
#include <memory>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <atomic>
#include <list>

#include "noncopyable.h"

namespace sylar {

    /**
    * @brief 局部锁的模板实现
    */
    template<class T>
    struct ScopedLockImpl {
    public:
        /**
         * @brief 构造函数
         * @param[in] mutex Mutex
         */
        ScopedLockImpl(T& mutex)
                :m_mutex(mutex) {
            m_mutex.lock();
            m_locked = true;
        }

        /**
         * @brief 析构函数,自动释放锁
         */
        ~ScopedLockImpl() {
            unlock();
        }

        /**
         * @brief 加锁
         */
        void lock() {
            if(!m_locked) {
                m_mutex.lock();
                m_locked = true;
            }
        }

        /**
         * @brief 解锁
         */
        void unlock() {
            if(m_locked) {
                m_mutex.unlock();
                m_locked = false;
            }
        }
    private:
        /// mutex
        T& m_mutex;
        /// 是否已上锁
        bool m_locked;
    };

    /**
     * @brief 互斥量
     */
    class Mutex : Noncopyable {
    public:
        /// 局部锁
        typedef ScopedLockImpl<Mutex> Lock;
        /**
         * @brief 构造函数
         */
        Mutex() {
            pthread_mutex_init(&m_mutex, nullptr);
        }

        /**
         * @brief 析构函数
         */
        ~Mutex() {
            pthread_mutex_destroy(&m_mutex);
        }

        /**
         * @brief 加锁
         */
        void lock() {
            pthread_mutex_lock(&m_mutex);
        }

        /**
         * @brief
         */
        void unlock() {
            pthread_mutex_unlock(&m_mutex);
        }



    private:
        // mutex
        pthread_mutex_t m_mutex;
    };

    /**
    * @brief 信号量
    */
    class Semaphore : Noncopyable {
    public:
        /**
         * @brief 构造函数
         * @param[in] count 信号量值的大小
         */
        Semaphore(uint32_t count = 0);

        /**
         * @brief 析构函数
         */
        ~Semaphore();

        /**
         * @brief 获取信号量
         */
        void wait();

        /**
         * @brief 释放信号量
         */
        void notify();
    private:
        sem_t m_semaphore;
    };

} // sylar

#endif //MYCOROUTINE_MUTEX_H

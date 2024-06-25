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
     * @brief 互斥量
     */
    class Mutex : Noncopyable {

    public:
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

} // sylar

#endif //MYCOROUTINE_MUTEX_H

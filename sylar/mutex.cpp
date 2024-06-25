//
// Created by 谭颍豪 on 2024/6/25.
//

#include <stdexcept>
#include "mutex.h"

namespace sylar {
    Semaphore::Semaphore(uint32_t count) {
        // 信号量初始化，成功返回0
        if (sem_init(&m_semaphore, 0, count)) {
            throw std::logic_error("sem_init error");
        }
    }

    Semaphore::~Semaphore() {
        sem_destroy(&m_semaphore);
    }

    void Semaphore::wait() {
        if (sem_wait(&m_semaphore)) {
            throw std::logic_error("sem_wait error");
        }
    }

    void Semaphore::notify() {
        if (sem_post(&m_semaphore)) {
            throw std::logic_error("sem_post error");
        }
    }


} // sylar
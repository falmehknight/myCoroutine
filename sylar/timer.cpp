//
// Created by 谭颍豪 on 2024/6/26.
//

#include "timer.h"
#include "util.h"
#include "macro.h"


namespace sylar {

    // 返回true说明要left在前 right在后,这里要实现最小堆
    bool Timer::Comparator::operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const {
        if(!lhs && !rhs) {
            return false;
        }
        if (!lhs) {
            return true;
        }
        if (!rhs) {
            return false;
        }
        if (lhs->m_next < rhs->m_next) {
            return true;
        }
        if (lhs->m_next > rhs->m_next) {
            return false;
        }
        return lhs.get() < rhs.get();
    }

    Timer::Timer(uint64_t next)
        :m_next(next) {
    }

    Timer::Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager *manager)
        :m_recurring(recurring)
        ,m_ms(ms)
        ,m_cb(cb)
        ,m_manager(manager){
        m_next = sylar::GetElapsedMS() + m_ms;
    }

    bool Timer::cancel() {
        TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
        // 将回调函数清空以及定时器管理器中的存储对象给清除掉
        if (m_cb) {
            m_cb = nullptr;
            auto it = m_manager->m_timers.find(shared_from_this());
            m_manager->m_timers.erase(it);
            return true;
        }
        return false;
    }

    bool Timer::refresh() {
        TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
        if (!m_cb) {
            return false;
        }
        // 先删除定时器，更新后再添加回去
        auto it = m_manager->m_timers.find(shared_from_this());
        if (it == m_manager->m_timers.end()) {
            return false;
        }
        m_manager->m_timers.erase(it);
        m_next = sylar::GetElapsedMS() + m_ms;
        m_manager->m_timers.insert(shared_from_this());
        return true;
    }

    bool Timer::reset(uint64_t ms, bool from_now) {
        // 没有任何更新，直接返回
        if (ms == m_ms && !from_now) {
            return true;
        }
        TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
        // 没有回调函数，直接失败！
        if (!m_cb) {
            return false;
        }
        // 先删除定时器，更新后再添加回去
        auto it = m_manager->m_timers.find(shared_from_this());
        if (it == m_manager->m_timers.end()) {
            return false;
        }
        m_manager->m_timers.erase(it);
        uint64_t start = 0;
        start = from_now ? sylar::GetElapsedMS() : m_next - m_ms;
        m_ms = ms;
        m_next = start + m_ms;
        m_manager->addTimer(shared_from_this(), lock);
        return true;
    }

    TimerManager::TimerManager() {
        m_previousTime = sylar::GetElapsedMS();
    }

    TimerManager::~TimerManager() {

    }

    Timer::ptr TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring) {
        Timer::ptr timer(new Timer(ms, cb, recurring, this));
        RWMutexType::WriteLock lock(m_mutex);
        addTimer(timer,lock);
        return timer;
    }

    static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb) {
        // 如果该弱指针所管理的对象已经被销毁，lock() 将返回一个空的 shared_ptr。
        std::shared_ptr<void> tmp = weak_cond.lock();
        if (tmp) {
            cb();
        }
    }

    Timer::ptr TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond,
                                               bool recurring) {
        // 这里通过std::bind形成一个新的回调函数
        return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
    }

    uint64_t TimerManager::getNextTimer() {
        RWMutexType::ReadLock lock(m_mutex);
        m_tickled = false;
        if (m_timers.empty()) {
            return ~0ull;
        }

        auto& next = *m_timers.begin();
        uint64_t now_ms = sylar::GetElapsedMS();
        // 判断当前时间是否大于等于最小的那个定时器时间，如果大于，说明定时器需要执行的时间为now！
        if (now_ms >= next->m_next) {
            return 0;
        } else {
            return next->m_next - now_ms;
        }
    }

    void TimerManager::listExpiredCb(std::vector<std::function<void()>> &cbs) {
        uint64_t now_ms = sylar::GetElapsedMS();
        std::vector<Timer::ptr> expired;
        // 1.校验
        // 判断定时器集合是否为空。
        {
            RWMutexType::ReadLock lock(m_mutex);
            if (m_timers.empty()) {
                return ;
            }
        }
        // 双重锁，在上写锁之后再判断一次是否为空
        RWMutexType::WriteLock lock(m_mutex);
        if (m_timers.empty()) {
            return ;
        }

        // 检查服务器时间是否有问题
        bool rollover = false;
        if (SYLAR_UNLIKELY(detectClockRollover(now_ms))) {
            rollover = true;
        }
        // 服务器时间没问题且最小的定时器也没到时间。
        if (!rollover && (*m_timers.begin())->m_next > now_ms) {
            return ;
        }

        Timer::ptr now_timer(new Timer(now_ms));

        // 2. 找出最小堆里面小于等于now的定时器集合
        auto it = rollover ? m_timers.end() : m_timers.lower_bound(now_timer);
        while (it != m_timers.end() && (*it)->m_next == now_ms) {
            ++it;
        }
        expired.insert(expired.begin(), m_timers.begin(), it);
        m_timers.erase(m_timers.begin(), it);
        cbs.reserve(expired.size());
        for (auto& timer : expired) {
            cbs.emplace_back(timer->m_cb);
            if (timer->m_recurring) {
                timer->m_next = now_ms + timer->m_ms;
                m_timers.insert(timer);
            } else {
                timer->m_cb = nullptr;
            }
        }

    }

    void TimerManager::addTimer(Timer::ptr val, RWMutex::WriteLock &lock) {
        auto it = m_timers.insert(val).first;
        // 被插入的对象放到了堆的最前面且之前处于不触发onTimerInsertedAtFront的情况
        bool at_front = (it == m_timers.begin()) && !m_tickled;
        if (at_front) {
            m_tickled = true;
        }
        lock.unlock();

        if (at_front) {
            onTimerInsertedAtFront();
        }
    }

    bool TimerManager::detectClockRollover(uint64_t now_ms) {
        bool rollover = false;
        if (now_ms < m_previousTime && now_ms < (m_previousTime - 60 * 60 * 1000)) {
            rollover = true;
        }
        m_previousTime = now_ms;
        return rollover;
    }

    bool TimerManager::hasTimer() {
        RWMutexType::ReadLock lock(m_mutex);
        return !m_timers.empty();
    }
} // end of namespace

//
// Created by 谭颍豪 on 2024/6/26.
//

#ifndef MYCOROUTINE_IOMANAGER_H
#define MYCOROUTINE_IOMANAGER_H


#include "scheduler.h"
#include "timer.h"

namespace sylar {

    static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

    class IOManager : public Scheduler, public TimerManager {
    public :
        typedef std::shared_ptr<IOManager> ptr;
        typedef RWMutex RWMutexType;
        /**
        * @brief IO事件，继承自epoll对事件的定义
        * @details 这里只关心socket fd的读和写事件，其他epoll事件会归类到这两类事件中
        */
        enum Event {
            /// 无事件
            NONE = 0x0,
            /// 读事件(EPOLLIN)
            READ = 0x1,
            /// 写事件(EPOLLOUT)
            WRITE = 0x4,
        };

    private:
        /**
         * @brief socket fd上下文类
         * @details 每个socket fd都对应一个FdContext,包括fd的值，fd上的事件，以及fd的读写事件上下文
         */
        struct FdContext {
            typedef Mutex MutexType;

            /**
             * @brief 事件上下文类
             * @details fd的每个事件都有一个事件上下文，保存这个事件的回调函数以及执行回调函数的调度器。fd事件这里做了简化，只预留了读、写事件。
             * 所有事件都被归类到这两类事件中
             */
            struct EventContext {
                // 执行事件回调的调度器
                Scheduler *scheduler = nullptr;
                // 事件回调协程
                Fiber::ptr fiber;
                // 事件回调函数
                std::function<void()> cb;
            };

            /**
             * @brief 获取事件上下文类
             * @param event 事件类型
             * @return 返回对应事件的上下文
             */
            EventContext &getEventContext(Event event);

            /**
             * @brief 重置事件上下文
             * @param ctx[in,out] 待重置的事件上下文对象
             */
            void resetEventContext(EventContext &ctx);

            /**
             * @brief 触发事件
             * @details 根据事件类型调用对应上下文结构中的调度器去调度回调协程或者回调函数
             * @param event 事件类型
             */
            void triggerEvent(Event event);

            // 读事件上下文
            EventContext read;
            // 写事件上下文
            EventContext write;
            // 事件关联的句柄
            int fd = 0;
            // 该fd添加了哪些事件的回调函数，或者说该fd关心哪些事件
            Event events = NONE;
            // 事件的互斥量
            MutexType mutex;
        };

    public:
        /**
         * @brief 构造函数
         * @param[in] threads 线程数量
         * @param[in] use_caller 是否将调用线程包含进去
         * @param[in] name 调度器的名称
         */
        IOManager(size_t threads = 1, bool user_caller = true, const std::string &name = "IOManager");

        /**
         * @brief 析构函数
         */
        ~IOManager();

        /**
        * @brief 添加事件
        * @details fd描述符发生了event事件时执行cb函数
        * @param[in] fd socket句柄
        * @param[in] event 事件类型
        * @param[in] cb 事件回调函数，如果为空，则默认把当前协程作为回调执行体
        * @return 添加成功返回0,失败返回-1
        */
        int addEvent(int fd, Event event, std::function<void()> cb = nullptr);

        /**
        * @brief 删除事件
        * @param[in] fd socket句柄
        * @param[in] event 事件类型
        * @attention 不会触发事件
        * @return 是否删除成功
        */
        bool delEvent(int fd, Event event);

        /**
        * @brief 取消事件
        * @param[in] fd socket句柄
        * @param[in] event 事件类型
        * @attention 如果该事件被注册过回调，那就触发一次回调事件
        * @return 是否删除成功
        */
        bool cancelEvent(int fd, Event event);

        /**
        * @brief 取消所有事件
        * @details 所有被注册的回调事件在cancel之前都会被执行一次
        * @param[in] fd socket句柄
        * @return 是否删除成功
        */
        bool cancelAll(int fd);

        /**
         * @brief 返回当前的IOManger
         */
        static IOManager *GetThis();

    protected:
        /**
         * @brief 通知调度器有任务要调度
         * @details 写pipe让idle协程从epoll_wait退出，待idle协程yield之后Scheduler::run就可以调度其他任务
         */
        void tickle() override;

        /**
         * @brief 判断是否可以停止
         * @details 判断条件是Scheduler::stopping()外加IOManager的m_pendingEventCount为0，表示没有IO事件可调度了
         */
        bool stopping() override;

        /**
         * @brief idle协程
         * @details 对于IO协程调度来说，应阻塞在等待IO事件上，idle退出的时机是epoll_wait返回，对应的操作是tickle或注册的IO事件发生
         * @Attention 调度器无调度任务时会阻塞在idle协程上，对IO调度器而言，idle状态应该关注两件事，一是有没有新的调度任务，
         * 对应Scheduler::schedule(),如果有新的调度任务，那么应该立即退出idle状态，并执行对应的任务；
         * 二是关注当前注册的所有IO事件有没有触发，如果有触发，那么应该执行IO事件对应的回调函数
         */
        void idle() override;

        /**
         * @brief 判断是否可以停止，同时获取最近一个定时器的超时时间
         * @param[out] timeout 最近一个定时器的超时时间，用于idle协程的epoll_wait
         * @return 返回是否可以停止
         */
        bool stopping(uint64_t& timeout);

        /**
         * @brief 当有定时器插入到头部时，要重新更新epoll_wait的超时时间，这里是唤醒idle协程以便于使用新的超时时间
         */
        void onTimerInsertedAtFront() override;

        /**
         * @brief 重置socket句柄上下文的容器大小
         * @param[in] size 容量大小
         */
        void contextResize(size_t size);

    private:
        // epoll 文件句柄
        int m_epfd = 0;
        // pipe 文件句柄，fd[0]读端，fd[1]写端
        int m_tickleFds[2];
        // 当前等待执行IO事件数量
        std::atomic<size_t> m_pendingEventCount = {0};
        // IOManager的Mutex互斥量
        RWMutexType m_mutex;
        // socket事件上下文容器
        std::vector<FdContext *> m_fdContexts;
    };

} // sylar

#endif //MYCOROUTINE_IOMANAGER_H

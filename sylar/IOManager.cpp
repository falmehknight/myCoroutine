//
// Created by 谭颍豪 on 2024/6/26.
//
#include <unistd.h>    // for pipe()
#include <sys/epoll.h> // for epoll_xxx()
#include <fcntl.h>     // for fcntl()
#include "IOManager.h"
#include "macro.h"
#include "log.h"

namespace sylar {
    IOManager::IOManager(size_t threads, bool user_caller, const std::string &name)
    : Scheduler(threads, user_caller, name) {
        // 创建epoll实例，参数代表 epoll 实例创建的文件描述符的数量，但在新版linux版本（2.6.8及之后）中此参数已失效
        m_epfd = epoll_create(5000);
        SYLAR_ASSERT(m_epfd > 0);

        // 创建pipe，获取m_tickleFds[2],其中0是管道的读端，1是管道的写端
        int rt = pipe(m_tickleFds);
        SYLAR_ASSERT(!rt);

        // 注册pipe读句柄的可读事件，用于tickle调度协程，通过epoll_event.data.fd保存描述符
        epoll_event event;
        memset(&event, 0, sizeof(epoll_event));
        event.events = EPOLLIN | EPOLLET; // 监视输入事件并设置边缘触发模式
        event.data.fd = m_tickleFds[0];

        // 设置文件描述符读写为非阻塞方式，配合上面设置的边缘触发
        int flags = fcntl(m_tickleFds[0], F_GETFL, 0);
        rt = fcntl(m_tickleFds[0], F_SETFL, flags | O_NONBLOCK);
        SYLAR_ASSERT(!rt);

        // 将管道的读描述符加入到epoll多路复用，如果管道可读，idle中的epoll_wait会返回
        rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
        SYLAR_ASSERT(!rt);

        contextResize(32);

        // 这里直接开启Scheduler,也就是IOManager创建即可调度协程
        start();
    }

    void IOManager::contextResize(size_t size) {
        m_fdContexts.resize(size);

        for (size_t i = 0; i < m_fdContexts.size(); ++i) {
            if (!m_fdContexts[i]) {
                m_fdContexts[i] = new FdContext;
                m_fdContexts[i]->fd = i;
            }
        }
    }

    void IOManager::tickle() {
        SYLAR_LOG_DEBUG(g_logger) << "tickle";
        // 如果当前没有空闲调度线程，那么就没有必要发通知
        if (!hasIdleThreads()) {
            return ;
        }
        // 写pipe让idle协程从epoll_wait中退出
        int rt = write(m_tickleFds[1] ,"T", 1);
        SYLAR_ASSERT(rt == 1);
    }

    void IOManager::idle() {
        SYLAR_LOG_DEBUG(g_logger) << "idle";
        // 1.初始化工作
        // 一次epoll_wait最多检测256个就绪事件，如果就绪事件超过了这个数，那么会在下轮epoll_wait的时候处理
        const uint64_t MAX_EVENTS = 256;
        epoll_event *events = new epoll_event[MAX_EVENTS]();
        // 默认情况下 std::shared_ptr 只会调用 delete，而不是 delete[]。这可能导致未定义行为，需要提供自定义删除器
        std::shared_ptr<epoll_event> shared_events(events, [](epoll_event *ptr) {
            delete[] ptr;
        });

        while(true) {
            // 获取下一个定时器的超时时间，顺便判断调度器是否停止
            uint64_t next_timeout = 0;
            if (SYLAR_UNLIKELY(stopping(next_timeout))) {
                SYLAR_LOG_DEBUG(g_logger) << "name=" << getName() << "idle stopping exit";
                break;
            }
            // 2. 阻塞在epoll_wait上，等待事件发生或者定时器超时
            int rt = 0;
            do {
                // 默认超时时间为5s，如果下一个定时器的超时时间大于5s，仍以5s来计算超时，避免超时时间太大时，epoll_wait一直阻塞
                static const int MAX_TIMEOUT = 5000;
                if (next_timeout != ~0ull) {
                    next_timeout = std::min((int)next_timeout, MAX_TIMEOUT);
                } else {
                    next_timeout = MAX_TIMEOUT;
                }
                rt = epoll_wait(m_epfd, events, MAX_EVENTS, (int)next_timeout);
                if (rt <0 && errno == EINTR) {
                    // 系统调用被中断了，应该继续去检查去
                    continue;
                } else {
                    break;
                }
            } while (true);

            // 收集所有已超时的定时器，执行回调函数
            std::vector<std::function<void()>> cbs;
            listExpiredCb(cbs);
            if (!cbs.empty()) {
                for (const auto &cb : cbs) {
                    schedule(cb);
                }
                cbs.clear();
            }

            // 3. 遍历发生的事件，根据epoll_event的私有指针找到对应的FdContext，进行事件处理
            for (int i = 0; i < rt; ++i) {
                epoll_event &event = events[i];
                // 3.0 如果是通知协程调度的消息，则处理
                if (event.data.fd == m_tickleFds[0]) {
                    // m_tickleFds[0]用于通知协程调度，这时只需要把管道的内容读完即可，本轮idle结束Scheduler::run会重新执行协程调度
                    uint8_t dummy[256];
                    while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0) ;
                    continue;
                }
                // 3.1 通过epoll_event的私有指针获取FdContext
                FdContext *fd_ctx = (FdContext *)event.data.ptr;
                FdContext::MutexType::Lock lock(fd_ctx->mutex);
                // 3.2 查看触发了哪些IO事件(读/写)
                /**
                * EPOLLERR: 出错，⽐如写读端已经关闭的pipe
                * EPOLLHUP: 套接字对端关闭
                * 出现这两种事件，应该同时触发fd的读和写事件，否则有可能出现注册的事件永远执⾏不到的情况
                */
                if (event.events & (EPOLLERR | EPOLLHUP)) {
                    event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
                }
                int real_events = NONE;
                if (event.events & EPOLLIN) {
                    real_events |= READ;
                }
                if (event.events & EPOLLOUT) {
                    real_events |= WRITE;
                }
                // 3.3 什么都没有，则继续遍历
                if ((fd_ctx->events & real_events) == NONE) {
                    continue;
                }
                // 3.4 剔除已经发生的事件，将剩下的事件重新加入到epoll_wait，如果剩下的事件为0，表示这个fd已经不需要关注了，直接从epoll中删除
                int left_events = (fd_ctx->events & ~real_events);
                // 根据剩余的事件个数决定操作类型
                int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
                event.events = EPOLLET | left_events;
                int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
                if (rt2) {
                    SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", "
                                              << (EpollCtlOp)op << ", " << fd_ctx->fd << ", " << (EPOLL_EVENTS)event.events << "):"
                                              << rt2 << " (" << errno << ") (" << strerror(errno) << ")";
                    continue;
                }
                // 3.5 处理已发生的事件，也就是让调度器调度指定的函数或者协程
                if (real_events & READ) {
                    fd_ctx->triggerEvent(READ);
                    --m_pendingEventCount;
                }
                if (real_events & WRITE) {
                    fd_ctx->triggerEvent(WRITE);
                    --m_pendingEventCount;
                }
            } //end for
            // 4. 处理完所有的事件，idle协程yield，这样可以让调度协程重新检查是否有新的任务要调度
            // 上面的triggerEvent实际上也只是把对应的fiber重新加入调度，要执行的话还得等idle协程退出
            Fiber::ptr cur = Fiber::GetThis();
            auto raw_ptr =  cur.get();
            cur.reset();

            raw_ptr->yield();
        } // end while(true)

    }

    int IOManager::addEvent(int fd, IOManager::Event event, std::function<void()> cb) {
        // 1. 找到fd对应的FdContext,如果不存在，那就分配一个
        FdContext *fd_ctx = nullptr;
        RWMutexType::ReadLock lock(m_mutex);
        if (fd < (int)m_fdContexts.size()) {
            fd_ctx = m_fdContexts[fd];
            lock.unlock();
        } else {
            lock.unlock();
            RWMutexType::WriteLock lock2(m_mutex);
            // 扩大大小为最大fd的1.5倍
            contextResize(fd * 1.5);
            fd_ctx = m_fdContexts[fd];
        }
        // 2. 添加fd的事件
        // 2.1 校验，同一fd不允许重复添加相同的事件
        FdContext::MutexType::Lock lock2(fd_ctx->mutex);
        if (SYLAR_UNLIKELY(fd_ctx->events & event)) {
            SYLAR_LOG_ERROR(g_logger) << "addEvent assert fd=" << fd
                                      << " event=" << (EPOLL_EVENTS)event
                                      << " fd_ctx.event=" << (EPOLL_EVENTS)fd_ctx->events;
            SYLAR_ASSERT(!(fd_ctx->events & event));
        }
        // 2.2 将新的事件加入到epoll_wait，使用epoll_event的私有指针存储FdContext的位置
        int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        epoll_event epevent;
        epevent.events = EPOLLET | fd_ctx->events | event;
        epevent.data.ptr = fd_ctx;
        int rt = epoll_ctl(m_epfd, op, fd, &epevent);
        if (rt) {
            SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", "
                                      << (EpollCtlOp)op << ", " << fd << ", " <<
                                      (EPOLL_EVENTS)epevent.events << "):"
                                      << rt << " (" << errno << ") (" << strerror(errno) <<
                                      ") fd_ctx->events="
                                      << (EPOLL_EVENTS)fd_ctx->events;
            return -1;
        }

        // 待执行IO事件数加1
        ++m_pendingEventCount;
        // 2.3 对event对应的EventContext中的值进行赋值
        fd_ctx->events = (Event) (fd_ctx->events | event);
        FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event);
        SYLAR_ASSERT(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);

        // 赋值scheduler和回调函数，如果回调函数为空，则把当前协程当做回调执行体
        event_ctx.scheduler = Scheduler::GetThis();
        if (cb) {
            event_ctx.cb.swap(cb);
        } else {
            event_ctx.fiber = Fiber::GetThis();
            SYLAR_ASSERT2(event_ctx.fiber->getState() == Fiber::RUNNING, "state=" << event_ctx.fiber->getState());
        }

        return 0;
    }

    bool IOManager::delEvent(int fd, IOManager::Event event) {
        // 1.找到fd对应的FdContext
        RWMutexType::ReadLock lock(m_mutex);
        if (fd >= (int)m_fdContexts.size()) {
            return false;
        }
        FdContext *fd_ctx = m_fdContexts[fd];
        lock.unlock();
        // 校验，fd不允许删除没有的事件
        FdContext::MutexType::Lock lock1(fd_ctx->mutex);
        if (SYLAR_UNLIKELY(!(fd_ctx->events & event))) {
            return false;
        }

        // 2. 清除指定的事件，表示不关注这个事件了，如果清除之后是0，则从epoll_wait中删除该文件描述符
        Event new_events = (Event) (fd_ctx->events & ~event);
        int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        epoll_event epevent;
        epevent.events = EPOLLET | new_events;
        epevent.data.ptr = fd_ctx;

        int rt = epoll_ctl(m_epfd, op, fd, &epevent);
        if (rt) {
            SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", "
                                      << (EpollCtlOp)op << ", " << fd << ", " <<
                                      (EPOLL_EVENTS)epevent.events << "):"
                                      << rt << " (" << errno << ") (" << strerror(errno) <<
                                      ") fd_ctx->events="
                                      << (EPOLL_EVENTS)fd_ctx->events;
            return false;
        }

        // 3. 更改一些变量值
        // 待执行事件数-1
        --m_pendingEventCount;
        // 重置该fd对应的event事件上下文
        fd_ctx->events = new_events;
        FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event);
        fd_ctx->resetEventContext(event_ctx);
        return true;
    }

    bool IOManager::cancelEvent(int fd, IOManager::Event event) {
        // 1.找到fd对应的FdContext
        RWMutexType::ReadLock lock(m_mutex);
        if (fd >= (int)m_fdContexts.size()) {
            return false;
        }
        FdContext *fd_ctx = m_fdContexts[fd];
        lock.unlock();

        // 校验，fd不允许取消没有的事件
        FdContext::MutexType::Lock lock1(fd_ctx->mutex);
        if (SYLAR_UNLIKELY(!(fd_ctx->events & event))) {
            return false;
        }

        // 2. 删除指定的事件，表示不关注这个事件了，如果清除之后是0，则从epoll_wait中删除该文件描述符
        Event new_events = (Event) (fd_ctx->events & ~event);
        int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        epoll_event epevent;
        epevent.events = EPOLLET | new_events;
        epevent.data.ptr = fd_ctx;

        int rt = epoll_ctl(m_epfd, op, fd, &epevent);
        if (rt) {
            SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", "
                                      << (EpollCtlOp)op << ", " << fd << ", " <<
                                      (EPOLL_EVENTS)epevent.events << "):"
                                      << rt << " (" << errno << ") (" << strerror(errno) <<
                                      ") fd_ctx->events="
                                      << (EPOLL_EVENTS)fd_ctx->events;
            return false;
        }

        // 3.取消事件和删除事件最大的区别就是这里，取消之前会先触发一次事件,重置工作在触发事件完成后已执行
        fd_ctx->triggerEvent(event);
        // 活跃事件数-1
        --m_pendingEventCount;
        return true;
    }

    bool IOManager::cancelAll(int fd) {
        // 1.找到fd对应的FdContext
        RWMutexType::ReadLock lock(m_mutex);
        if (fd >= (int)m_fdContexts.size()) {
            return false;
        }
        FdContext *fd_ctx = m_fdContexts[fd];
        lock.unlock();

        // 校验，fd不允许取消没有的事件
        FdContext::MutexType::Lock lock1(fd_ctx->mutex);
        if (!fd_ctx->events) {
            return false;
        }

        // 2.删除所有事件
        int op = EPOLL_CTL_DEL;
        epoll_event epevent;
        epevent.events = 0;
        epevent.data.ptr = fd_ctx;

        int rt = epoll_ctl(m_epfd, op, fd, &epevent);
        if (rt) {
            SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", "
                                      << (EpollCtlOp)op << ", " << fd << ", " <<
                                      (EPOLL_EVENTS)epevent.events << "):"
                                      << rt << " (" << errno << ") (" << strerror(errno) <<
                                      ") fd_ctx->events="
                                      << (EPOLL_EVENTS)fd_ctx->events;
            return false;
        }

        // 3.触发所有的已注册事件
        if (fd_ctx->events & READ) {
            fd_ctx->triggerEvent(READ);
            --m_pendingEventCount;
        }
        if (fd_ctx->events & WRITE) {
            fd_ctx->triggerEvent(WRITE);
            --m_pendingEventCount;
        }
        SYLAR_ASSERT(fd_ctx->events == 0);
        return true;
    }

    IOManager::~IOManager() {
        // 等Scheduler调度完所有的任务
        stop();
        // 关闭句柄
        close(m_epfd);
        close(m_tickleFds[0]);
        close(m_tickleFds[1]);

        for (auto & m_fdContext : m_fdContexts) {
            if (m_fdContext) {
                delete m_fdContext;
            }
        }

    }

    bool IOManager::stopping() {
        // 对于IOManager⽽⾔，必须等所有待调度的IO事件都执⾏完了才可以退出
        return m_pendingEventCount == 0 && Scheduler::stopping();
    }

    IOManager *IOManager::GetThis() {
        return dynamic_cast<IOManager *>(Scheduler::GetThis());
    }


    IOManager::FdContext::EventContext &IOManager::FdContext::getEventContext(IOManager::Event event) {
        switch (event) {
            case IOManager::READ:
                return read;
            case IOManager::WRITE:
                return write;
            default:
                SYLAR_ASSERT2(false, "getContext");
        }
        throw std::invalid_argument("getContext invalid event");
    }

    void IOManager::FdContext::resetEventContext(IOManager::FdContext::EventContext &ctx) {
        ctx.scheduler = nullptr;
        ctx.fiber.reset();
        ctx.cb = nullptr;
    }

    void IOManager::FdContext::triggerEvent(IOManager::Event event) {
        // 待触发的时间必须已经注册过了
        SYLAR_ASSERT(events & event);
        /**
        *  清除该事件，表示不再关注该事件了
        *  也就是说，注册的IO事件是一次性的，如果想持续关注某个socket fd的读写事件，那么每次触发事件之后都要重新添加
        */
        events = (Event) (events & ~event);
        // 调度对应的协程，其实就是把他加入到调度队列而已
        EventContext &ctx = getEventContext(event);
        if (ctx.cb) {
            ctx.scheduler->schedule(ctx.cb);
        } else {
            ctx.scheduler->schedule(ctx.fiber);
        }
        resetEventContext(ctx);
   }
} // sylar
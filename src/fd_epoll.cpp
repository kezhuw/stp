#include "fd.hpp"

#include "types.hpp"
#include "module.hpp"
#include "process.hpp"

#include <wild/module.hpp>
#include <wild/string.hpp>
#include <wild/scope_guard.hpp>

#include <signal.h>
#include <unistd.h>
#include <sys/epoll.h>

#include <cassert>

#include <thread>
#include <utility>
#include <type_traits>
#include <system_error>

namespace {

using namespace stp;
using namespace stp::fd;

int _epoll_create() {
    int fd = ::epoll_create(1);
    if (fd == -1) {
        throw std::system_error(errno, std::system_category(), "epoll_create(1): ");
    }
    return fd;
}

void _epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    int err = epoll_ctl(epfd, op, fd, event);
    if (err != 0) {
        throw std::system_error(errno, std::system_category(), "epoll_ctl(): ");
    }
}

struct filter {
    // no constructor is needed.
    decltype(std::declval<process_t>().Value()) source;
    decltype(std::declval<session_t>().Value()) session;
};

struct fdata {
    static_assert(static_cast<int>(Event::Read) == 0, "");
    static_assert(static_cast<int>(Event::Write) == 1, "");
    struct filter filters[2];
};

class fdset {
    struct segment {
        static constexpr int size = 16384;
        struct fdata fds[size];

        struct filter *
        selectFilter(int fd, Event event) {
            int index = fd % size;
            int mode = static_cast<int>(event);
            return &fds[index].filters[mode];
        }
    };

    static constexpr int size = 256;
    std::atomic<struct segment *> segments[size];

public:
    struct filter *
    selectFilter(int fd, Event event) {
        int index = fd / segment::size;
        assert(index < size);
        struct segment *fds = segments[index].load(std::memory_order_relaxed);
        if (fds == nullptr) {
            struct segment *new_fds = new segment;
            if (segments[index].compare_exchange_strong(fds, new_fds, std::memory_order_relaxed) == false) {
                delete new_fds;
                assert(fds != nullptr);
            } else {
                fds = new_fds;
            }
        }
        return fds->selectFilter(fd, event);
    }
} FDSET;

enum : uint32 {
    ReadEvents  = EPOLLIN | EPOLLRDHUP | EPOLLPRI | EPOLLET | EPOLLONESHOT,
    WriteEvents = EPOLLOUT | EPOLLET | EPOLLONESHOT,
};

struct event_poll {
    int epfd;
    int eventfds[2];
    uint32 events[2];
} EPOLL;

void _epoll_sigmask() {
    sigset_t sigset;
    sigfillset(&sigset);
    pthread_sigmask(SIG_SETMASK, &sigset, nullptr);
}

void _epoll_poll(int epfd) {
    _epoll_sigmask();
    struct epoll_event eps[2];
    struct epoll_event events[512];
    for (;;) {
        int n = epoll_wait(epfd, eps, 2, -1);
        if (n == -1) {
            printf("epoll_wait: %s\n", wild::os::strerror(errno));
            continue;
        }
        while (n-- > 0) {
            int fd = eps[n].data.fd;
            int nevent = epoll_wait(fd, events, std::extent<decltype(events)>::value, 0);
            while (nevent-- > 0) {
                auto fi = static_cast<struct filter*>(events[nevent].data.ptr);
                process::Response(process_t(fi->source), session_t(fi->session));
            }
        }
    }
}

void init() {
    // It is ok to leak fd in init phase.
    int epfd = _epoll_create();
    int readfd = _epoll_create();
    int writefd = _epoll_create();

    struct epoll_event event;
    event.data.u64 = 0;

    event.events = EPOLLIN;
    event.data.fd = readfd;
    _epoll_ctl(epfd, EPOLL_CTL_ADD, readfd, &event);

    event.events = EPOLLIN;
    event.data.fd = writefd;
    _epoll_ctl(epfd, EPOLL_CTL_ADD, writefd, &event);

    EPOLL.epfd = epfd;
    EPOLL.eventfds[0] = readfd;
    EPOLL.eventfds[1] = writefd;
    EPOLL.events[0] = ReadEvents;
    EPOLL.events[1] = WriteEvents;

    std::thread(_epoll_poll, epfd).detach();
}

wild::module::Definition fd_poll(module::STP, "stp:fd_epoll", init, module::Order::Fdpoll);

}

namespace stp {
namespace fd {

void Wait(int fd, Event event) {
    std::tuple<process_t, session_t> s = process::MakeSession();

    struct filter *fi = FDSET.selectFilter(fd, event);
    fi->source = std::get<process_t>(s).Value();
    fi->session = std::get<session_t>(s).Value();

    int mode = static_cast<int>(event);
    assert(mode == 0 || mode == 1);
    int epfd = EPOLL.eventfds[mode];
    struct epoll_event ev;
    ev.events = EPOLL.events[mode];
    ev.data.ptr = static_cast<void*>(fi);
    _epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    SCOPE_EXIT {
        // differenate from BSD's kqueue, epoll's ONESHOT only disable
        // reporting of fd, but no deletion is performed.
        _epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &ev);
    };
    process::Suspend(std::get<session_t>(s));
}

}
}

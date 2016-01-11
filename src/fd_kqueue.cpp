#include "fd.hpp"
#include "types.hpp"
#include "module.hpp"
#include "process.hpp"

#include "wild/module.hpp"
#include "wild/string.hpp"
#include "wild/utility.hpp"

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#include <assert.h>
#include <signal.h>
#include <pthread.h>

#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>

namespace {

using namespace stp;

void setup_sigmask() {
    sigset_t sigset;
    sigfillset(&sigset);
    pthread_sigmask(SIG_SETMASK, &sigset, nullptr);
}

void kqueue_poll(int kq) {
    setup_sigmask();
    struct kevent events[512];
    for (;;) {
        int n = kevent(kq, NULL, 0, events, static_cast<int>(wild::nelem(events)), NULL);
        if (n < 0) {
            switch (errno) {
            case EINTR:
                break;
            default:
                fprintf(stderr, "kevent(): %s\n", wild::os::strerror(errno));
                return;
            }
            continue;
        }
        while (n-- > 0) {
            struct kevent *ev = &events[n];
            auto udata = reinterpret_cast<uintptr>(ev->udata);
            auto pid = static_cast<uint32>(udata);
            auto sid = static_cast<uint32>(udata >> 32);
            process::response(process_t(pid), session_t(sid));
        }
    }
}

static int kqfd = -1;

std::thread _kqueue_thread;

void init() {
    kqfd = kqueue();
    if (kqfd == -1) {
        throw std::system_error(errno, std::system_category(), "kqueue()");
    }
    _kqueue_thread = std::thread(kqueue_poll, kqfd);
}

wild::module::Definition fd_polll(module::STP, "stp:fd_kqueue", init, module::Order::Fdpoll);

}

namespace stp {
namespace fd {

static_assert(std::is_same<decltype(std::declval<process_t>().Value()), uint32>::value, "");
static_assert(std::is_same<decltype(std::declval<session_t>().Value()), uint32>::value, "");

void stop() {
    pthread_cancel(_kqueue_thread.native_handle());
    _kqueue_thread.join();
}

void wait(int fd, Event event) {
    assert(event == Event::kRead || event == Event::kWrite);
    process::Session session = process::new_session();
    auto pid = session.Pid().Value();
    auto sid = session.Value().Value();
    void *udate = reinterpret_cast<void*>(static_cast<uintptr>(pid) | (static_cast<uintptr>(sid) << 32));
    struct kevent ev;
    short filter = event == Event::kRead ? EVFILT_READ : EVFILT_WRITE;
    EV_SET(&ev, fd, filter, EV_ADD | EV_ONESHOT, 0, 0, udate);
    if (kevent(kqfd, &ev, 1, NULL, 0, NULL) == -1) {
        throw std::system_error(errno, std::system_category(), "kevent(EV_ADD)");
    }
    process::suspend(session.Value());
}

}
}

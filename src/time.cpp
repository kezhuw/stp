#include "time.hpp"
#include "types.hpp"
#include "message.hpp"
#include "process.hpp"
#include "module.hpp"

#include <wild/Any.hpp>
#include <wild/FreeList.hpp>
#include <wild/module.hpp>

#include <assert.h>
#include <limits.h>

#include <atomic>
#include <chrono>
#include <thread>

// About algorithm See:
//
// http://blog.codingnow.com/2007/05/Timer.html
// https://github.com/cloudwu/skynet/blob/6ba28cd4420a4bbc4c65a009efdb2bd02741e7c1/skynet-src/skynet_timer.c

namespace {

using namespace stp;
using namespace stp::time;

using pid_t = decltype(std::declval<process_t>().Value());
using sid_t = decltype(std::declval<session_t>().Value());

struct timer_node {
    struct timer_node *next;
    uint64 expire;
    // user data
    pid_t source;
    sid_t session;
};

struct timer_list {
    struct timer_node *first;
    struct timer_node **last = &first;
};

inline void
_list_init(struct timer_list *l) {
    l->last = &l->first;
}

inline void
_list_append(struct timer_list *l, struct timer_node *n) {
    *(l->last) = n;
    l->last = &n->next;
}

inline struct timer_node *
_list_clear(struct timer_list *l) {
    *(l->last) = nullptr;
    auto list = l->first;
    _list_init(l);
    return list;
}

#define TIME_LEAST_SHIFT        14
#define TIME_LEAST_VALUE        (1<<TIME_LEAST_SHIFT)
#define TIME_LEAST_MASK         (TIME_LEAST_VALUE-1)

#define TIME_LEVEL_SHIFT        10
#define TIME_LEVEL_VALUE        (1<<TIME_LEVEL_SHIFT)
#define TIME_LEVEL_MASK         (TIME_LEVEL_VALUE-1)

#define TIME_BITS               (CHAR_BIT*(int)sizeof(uint64))
#define TIME_LEVEL_COUNT        ((TIME_BITS-TIME_LEAST_SHIFT)/TIME_LEVEL_SHIFT)

static_assert(TIME_BITS == TIME_LEVEL_COUNT*TIME_LEVEL_SHIFT + TIME_LEAST_SHIFT, "time bits mismatch");

struct Timer {
    uint64 time = 0;
    struct timer_list least[TIME_LEAST_VALUE];
    struct timer_list level[TIME_LEVEL_COUNT][TIME_LEVEL_VALUE-1];
    wild::FreeList<timer_node> frees;
};

inline struct timer_node *
_new_node(struct Timer *t) {
    if (auto node = t->frees.take()) {
        return node;
    }
    return new timer_node;
}

inline void
_free_node(struct Timer *t, struct timer_node *node) {
    t->frees.push(node);
}

inline void
_send(pid_t pid, sid_t session) {
    process::response(process_t(pid), session_t(session), message::Content{});
}

void
_send_list(struct Timer *t, struct timer_node *list) {
    while (list != nullptr) {
        struct timer_node *node = list;
        list = list->next;
        _send(node->source, node->session);
        _free_node(t, node);
    }
}

void
_queue(struct Timer *t, struct timer_node *node) {
    uint64 time = t->time;
    uint64 expire = node->expire;
    assert(expire >= time);
    if (expire - time < TIME_LEAST_VALUE) {
        uint64 index = expire & TIME_LEAST_MASK;
        _list_append(&t->least[index], node);
    } else {
        uint64 level = 0;
        uint64 exp2 = 1 << TIME_LEAST_SHIFT;
        do {
            exp2 <<= TIME_LEVEL_SHIFT;
            uint64 mask = exp2 - 1;
            if ((expire | mask) == (time | mask)) {
                uint64 shift = TIME_LEAST_SHIFT + level*TIME_LEVEL_SHIFT;
                uint64 value = (expire >> shift) & TIME_LEVEL_MASK;
                _list_append(&t->level[level][value-1], node);
                break;
            }
        } while (++level < TIME_LEVEL_COUNT);
        assert(level < TIME_LEVEL_COUNT);
    }
}

void
_queue_list(struct Timer *t, struct timer_node *list) {
    while (list != nullptr) {
        struct timer_node *n = list;
        list = list->next;
        _queue(t, n);
    }
}

void
_tick(struct Timer *t) {
    uint64 index = t->time & TIME_LEAST_MASK;

    _send_list(t, _list_clear(&t->least[index]));

    uint64 time = ++t->time;
    if ((time & TIME_LEAST_MASK) == 0) {
        assert(time != 0);
        time >>= TIME_LEAST_SHIFT;
        uint64 level=0;
        do {
            uint64 value = time & TIME_LEVEL_MASK;
            if (value == 0) {
                time >>= TIME_LEVEL_SHIFT;
                continue;
            }
            _queue_list(t, _list_clear(&t->level[level][value-1]));
            break;
        } while (++level < TIME_LEVEL_COUNT);
        assert(level < TIME_LEVEL_COUNT);
    }
}

void
_update(struct Timer *t, uint64 time) {
    if (time > t->time) {
        for (uint64 i=0, n = time - t->time; i<n; ++i) {
            _tick(t);
        }
    }
}

void
_timeout(struct Timer *t, process_t source, session_t session, uint64 timeout) {
    struct timer_node *node = _new_node(t);
    node->expire = t->time + timeout;
    node->source = source.Value();
    node->session = session.Value();
    _queue(t, node);
}

struct UpdateMessage {
    uint64 timestamp;
};

struct TimeoutMessage {
    uint64 timeout;
};

void
_main() {
    struct Timer t;
    process::request_callback<TimeoutMessage>([&t](TimeoutMessage *request) {
        _timeout(&t, process::sender(), process::session(), request->timeout);
    });
    process::notification_callback<UpdateMessage>([&](UpdateMessage *update) {
        _update(&t, update->timestamp);
    });
    process::serve();
}

uint64
_gettime() {
    auto now = std::chrono::steady_clock::now();
    auto msecs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return static_cast<uint64>(msecs.count());
}

uint64
_realtime() {
    auto now = std::chrono::system_clock::now();
    auto msecs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return static_cast<uint64>(msecs.count());
}

std::atomic<uint64> TIME;

// readonly after set.
std::atomic<uint64> STARTTIME;
std::atomic<uint64> STARTTIME_REALTIME;
process_t TIMER_SERVICE;

void
tick(process_t timer) {
    for (;;) {
        uint64 now = _gettime();
        assert(now >= STARTTIME.load(std::memory_order_relaxed));
        uint64 time = now - STARTTIME.load(std::memory_order_relaxed);
        if (time > TIME.load(std::memory_order_relaxed)) {
            TIME.store(time, std::memory_order_relaxed);
            process::send(timer, UpdateMessage{time});
        }
        std::this_thread::sleep_for(std::chrono::microseconds(1000));
    }
}

void
init() {
    STARTTIME.store(_gettime(), std::memory_order_relaxed);
    STARTTIME_REALTIME.store(_realtime(), std::memory_order_relaxed);
    TIMER_SERVICE = process::spawn(_main, sizeof(struct Timer));
    std::thread(tick, TIMER_SERVICE).detach();
}

wild::module::Definition Timer(module::STP, "stp:Timer", init, module::Order::Timer);

}

namespace stp {
namespace time {

void
sleep(uint64 msecs) {
    process::request(TIMER_SERVICE, TimeoutMessage{msecs});
}

uint64
startup_time() {
    return STARTTIME_REALTIME.load(std::memory_order_relaxed);
}

namespace monotonic {

uint64
now() {
    return TIME.load(std::memory_order_relaxed);
}

uint64
real_time() {
    return startup_time() + now();
}

}

}
}

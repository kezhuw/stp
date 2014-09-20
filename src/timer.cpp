#include "timer.hpp"
#include "types.hpp"
#include "message.hpp"
#include "process.hpp"
#include "module.hpp"

#include <wild/any.hpp>
#include <wild/freelist.hpp>
#include <wild/module.hpp>

#include <assert.h>
#include <limits.h>

#include <atomic>
#include <chrono>

// About algorithm See:
//
// http://blog.codingnow.com/2007/05/Timer.html
// https://github.com/cloudwu/skynet/blob/6ba28cd4420a4bbc4c65a009efdb2bd02741e7c1/skynet-src/skynet_timer.c

namespace {

using namespace stp;
using namespace stp::timer;

enum class Code {
    Update      = 1,
    Timeout     = 2,
};

message::Code make_message_code(Code code) {
    return static_cast<message::Code>(code);
}

#define FIRST_NODE_FIELD    struct timer_node *link

using pid_t = decltype(std::declval<process_t>().Value());
using sid_t = decltype(std::declval<session_t>().Value());

struct timer_node {
    FIRST_NODE_FIELD;
    uint64 expire;
    // user data
    pid_t source;
    sid_t session;
};

struct timer_list {
    struct _unused {
        FIRST_NODE_FIELD = nullptr;
    } dummy;
    struct timer_node *tail = reinterpret_cast<timer_node*>(&dummy);
};

inline void
_list_init(struct timer_list *l) {
    l->dummy.link = NULL;
    l->tail = (struct timer_node *)&l->dummy;
}

inline bool
_list_empty(struct timer_list *l) {
    return l->dummy.link == NULL;
}

inline void
_list_append(struct timer_list *l, struct timer_node *n) {
    l->tail->link = n;
    l->tail = n;
}

inline struct timer_node *
_list_clear(struct timer_list *l) {
    l->tail->link = NULL;
    struct timer_node *nodes = l->dummy.link;
    _list_init(l);
    return nodes;
}

#define TIME_LEAST_SHIFT    14
#define TIME_LEAST_VALUE    (1<<TIME_LEAST_SHIFT)
#define TIME_LEAST_MASK        (TIME_LEAST_VALUE-1)

#define TIME_LEVEL_SHIFT    10
#define TIME_LEVEL_VALUE    (1<<TIME_LEVEL_SHIFT)
#define TIME_LEVEL_MASK        (TIME_LEVEL_VALUE-1)

#define TIME_BITS        (CHAR_BIT*(int)sizeof(uint64))
#define TIME_LEVEL_COUNT    ((TIME_BITS-TIME_LEAST_SHIFT)/TIME_LEVEL_SHIFT)

static_assert(TIME_BITS == TIME_LEVEL_COUNT*TIME_LEVEL_SHIFT + TIME_LEAST_SHIFT, "time bits mismatch");

struct Timer {
    uint64 time = 0;
    struct timer_list least[TIME_LEAST_VALUE];
    struct timer_list level[TIME_LEVEL_COUNT][TIME_LEVEL_VALUE-1];
    wild::FreeListT<timer_node> frees;
};

inline struct timer_node *
_new_node(struct Timer *t) {
    if (auto node = t->frees.Take()) {
        return node;
    }
    return new timer_node;
}

inline void
_free_node(struct Timer *t, struct timer_node *node) {
    t->frees.Free(node);
}

inline void
_send(pid_t pid, sid_t session) {
    process::Response(process_t(pid), session_t(session), message::Code::None, message::Content{});
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
_tick(struct Timer *t) {
    uint64 index = t->time & TIME_LEAST_MASK;

    if (!_list_empty(&t->least[index])) {
        struct timer_node *list = _list_clear(&t->least[index]);
        do {
            struct timer_node *node = list;
            list = list->link;

            _send(node->source, node->session);
            _free_node(t, node);
        } while (list != NULL);
    }

    uint64 time = ++t->time;
    if ((time & TIME_LEAST_MASK) == 0) {
        assert(time != 0);
        time >>= TIME_LEAST_SHIFT;
        uint64 level=0;
        do {
            uint64 value = time & TIME_LEVEL_MASK;
            if (value != 0) {
                struct timer_node *list = _list_clear(&t->level[level][value-1]);
                while (list != NULL) {
                    struct timer_node *node = list;
                    list = list->link;
                    _queue(t, node);
                }
                break;
            }
            time >>= TIME_LEVEL_SHIFT;
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

void
_main() {
    struct Timer t;
    process::HandleMessage(
        message::Kind::Request,
        make_message_code(Code::Timeout),
        [&t] (process_t source, session_t session, const message::Content& content) {
            uint64 timeout = wild::Any::Cast<const uint64&>(content);
            _timeout(&t, source, session, timeout);
        });
    process::HandleMessage(
        message::Kind::Notify,
        make_message_code(Code::Update),
        [&t] (process_t, session_t, const message::Content& content) {
            uint64 timestamp = wild::Any::Cast<const uint64&>(content);
            _update(&t, timestamp);
        });
    process::Run();
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
init() {
    STARTTIME.store(_gettime(), std::memory_order_relaxed);
    STARTTIME_REALTIME.store(_realtime(), std::memory_order_relaxed);
    TIMER_SERVICE = process::Spawn(_main, sizeof(struct Timer));
}

wild::module::Definition Timer(module::STP, "stp:Timer", init, module::Order::Timer);

}

namespace stp {
namespace timer {

uint64
Time() {
    return TIME.load(std::memory_order_relaxed);
}

uint64
StartTime() {
    return STARTTIME_REALTIME.load(std::memory_order_relaxed);
}

uint64
RealTime() {
    return StartTime() + Time();
}

void
Sleep(uint64 msecs) {
    process::Request(TIMER_SERVICE, make_message_code(Code::Timeout), msecs);
}

void
Timeout(session_t session, uint64 msecs) {
    process::Send(TIMER_SERVICE, session, message::Kind::Request, make_message_code(Code::Timeout), msecs);
}

uint64
UpdateTime() {
    uint64 now = _gettime();
    assert(now >= STARTTIME.load(std::memory_order_relaxed));
    uint64 time = now - STARTTIME.load(std::memory_order_relaxed);
    if (time > TIME.load(std::memory_order_relaxed)) {
        TIME.store(time, std::memory_order_relaxed);
        process::Notify(
            TIMER_SERVICE,
            make_message_code(Code::Update),
            time);
    }
    return TIME.load(std::memory_order_relaxed);
}

}
}

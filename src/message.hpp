#pragma once

#include "types.hpp"

#include <wild/types.hpp>
#include <wild/spinlock.hpp>
#include <wild/with_lock.hpp>

#include <cassert>

#include <deque>
#include <utility>
#include <type_traits>

namespace stp {
namespace message {

enum class Kind : uint32 {
    System      = 0,        // differentiate system-level and application-level message
    Notify      = 1,
    Request     = 2,
    Response    = 3,
};

enum class Code : uint32 {
    None        = 0,
    Kill        = 1,
    Abort       = 2,
    Error       = 3,        // content.Data is process::Error ?
    Wakeup      = 4,
};

template<typename T>
struct is_message_code_enum : std::false_type {};

enum class ResourceId : uint32 {
    None            = 0,
    NEWMEM          = 1,
};

struct Content {
    Content() : res(ResourceId::None), Data(0), Meta(0) {}
    Content(uintptr data, uintptr meta) : res(ResourceId::None), Data(data), Meta(meta) {}
    Content(ResourceId resd, uintptr data, uintptr meta) : res(resd), Data(data), Meta(meta) {}
    ResourceId res;
    uintptr Data;
    uintptr Meta;
};

struct Message {
    process_t source;
    session_t session;
    Kind kind;
    Code code;
    Content content;

    struct Message *link;
};

class ForwardList {
public:

    ForwardList() : _head(nullptr), _tail(nullptr) {}

    ForwardList(const ForwardList&) = delete;

    ForwardList(ForwardList&& other)
        : _head(other._head), _tail(other._tail) {}

    ForwardList& operator=(const ForwardList&) = delete;
    ForwardList& operator=(ForwardList&& other) = delete;

    void swap(ForwardList& other) {
        std::swap(_head, other._head);
        std::swap(_tail, other._tail);
    }

    void Push(Message *msg) {
        msg->link = nullptr;
        if (_tail == nullptr) {
            _head = _tail = msg;
        } else {
            _tail->link = msg;
            _tail = msg;
        }
    }

    Message *Take() {
        Message *msg = _head;
        if (msg) {
            _head = msg->link;
            if (_head == nullptr) {
                _tail = nullptr;
            }
        }
        return msg;
    }

    Message *Front() const {
        return _head;
    }

    bool Empty() const {
        return _head == nullptr;
    }

    void Clear() {
        _head = _tail = nullptr;
    }

private:
    Message *_head;
    Message *_tail;
};

inline void swap(ForwardList& a, ForwardList& b) {
    a.swap(b);
}

Message* New(process::process_t source, process::session_t session, message::Kind kind, message::Code code, const message::Content& content);

void Delete(Message *m);

void Register(ResourceId resd, void (*freed)(uintptr data, uintptr meta));

// one reader, multiple writer, nonblocking.
// After nullptr returned from Take(), reader
// should not call Take() again util notified
// by writer.
class Mailbox {
public:

    bool Push(Message *msg) {
        WITH_LOCK(_mutex) {
            if (_wait) {
                _wait = false;
                _in.Push(msg);
                return true;
            }
            _out.Push(msg);
        }
        return false;
    }

    Message *Take() {
        if (_in.Empty()) {
            WITH_LOCK(_mutex) {
                if (_out.Empty()) {
                    _wait = true;
                    return nullptr;
                }
                using std::swap;
                swap(_in, _out);
            }
        }
        assert(!_in.Empty());
        return _in.Take();
    }

private:

    wild::SpinLock _mutex;
    bool _wait = false;
    message::ForwardList _in;
    message::ForwardList _out;
};

}
}

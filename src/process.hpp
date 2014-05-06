#pragma once

#include "types.hpp"
#include "message.hpp"

#include <wild/types.hpp>
#include <wild/spinlock.hpp>
#include <wild/forward_list.hpp>

#include <memory>
#include <queue>
#include <system_error>
#include <tuple>
#include <functional>
#include <type_traits>

namespace stp {
namespace process {

process_t Spawn(std::function<void()> func, size_t stacksize = 0);

class Callable {
public:
    virtual void operator()() = 0;

    virtual ~Callable() {}
};

template<typename Closure>
class TClosure final : public Callable {
public:

    explicit TClosure(Closure&& closure)
        : _closure(std::move(closure)) {
    }

    TClosure(const Closure&) = delete;
    TClosure& operator=(const Closure&) = delete;

    virtual void operator()() override {
        _closure();
    }

private:
    Closure _closure;
};

class PCallable {
public:

    PCallable(Callable *callable) : _callable(callable) {}

    void operator()() {
        (*_callable)();
    }

private:
    std::shared_ptr<Callable> _callable;
};

// std::function need copyable function object.
// Lambda with move-only object captured is not copyable.
template<typename Closure
       , std::enable_if_t<std::is_convertible<Closure, std::function<void()>>::value>* = nullptr
       , std::enable_if_t<!std::is_same<Closure, void()>::value>* = nullptr
       , std::enable_if_t<!std::is_same<Closure, std::function<void()>>::value>* = nullptr
       , std::enable_if_t<!std::is_copy_constructible<Closure>::value>* = nullptr
        >
process_t Spawn(Closure&& closure, size_t stacksize = 0) {
    std::function<void()> func = PCallable(new TClosure<std::remove_cv_t<Closure>>(std::forward<Closure>(closure)));
    return Spawn(func, stacksize);
}

Process *New(const char *name, uintptr argument);

uintptr Suspend(session_t);

void Resume(Process *p);

// relinquish CPU
void Yield();

Process *Running();

process_t Pid();

void Exit();
void Kill(process_t pid);

session_t NewSession();
void ReleaseSession(session_t session);

session_t NewSession(Process *p);
void ReleaseSession(Process *p, session_t session);

std::tuple<process_t, session_t> MakeSession();

Message *NextMessage();

using MessageHandler = std::function<void(process_t, session_t, message::Content const&)>;
using DefaultHandler = std::function<void(message::Kind, message::Code, process_t, session_t, message::Content const&)>;

void HandleMessage(message::Kind kind, message::Code code, MessageHandler handler);

void DefaultMessage(DefaultHandler handler);

void Run();

// Asynchronous resume, eg. insert coroutine to run queue.
void Wakeup(Coroutine *co, uintptr result = 0);

struct Error {
    enum : uintptr {
        None                = 0,
        RuntimeError        = 1,
        LocalNotExist       = 2,
        RemoteNotExist      = 3,
        UnknownMessage      = 4,
    } Value;

    /* implicit */ Error(decltype(Value) value = None) : Value(value) {}

    explicit operator bool() {
        return Value != None;
    }
};

inline bool operator==(Error a, Error b) {
    return a.Value == b.Value;
}

inline bool operator!=(Error a, Error b) {
    return !(a==b);
}

void Notify(process_t pid, message::Code code, const message::Content& content = message::Content{});

void System(process_t pid, session_t session, message::Code code, const message::Content& content = message::Content{});

Error Request(process_t pid, message::Code code, const message::Content& content, message::Content *resultp = nullptr);
void Response(process_t pid, session_t session, message::Code code = message::Code::None, const message::Content& content = message::Content{});

template<typename CodeEnum, typename std::enable_if_t<message::is_message_code_enum<CodeEnum>::value>* = nullptr>
Error Request(process_t pid, CodeEnum code, const message::Content& content, message::Content *resultp = nullptr) {
    return Request(pid, make_message_code(code), content, resultp);
}

// Asynchronous sending, always success.
//
// Error for message::Kind::Request is reported by receiving
// (message::Kind::System, message::Code::Error).
session_t Send(process_t pid, message::Kind kind, message::Code, const message::Content& content);
void ReleaseSession(session_t session);

class Mutex {
public:

    Mutex() = default;

    void lock();
    void unlock();

    bool try_lock();

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

private:
    wild::SpinLock _mutex;
    Coroutine *_owner = nullptr;
    std::queue<std::tuple<process_t, session_t>> _blocks;
};

// Only for scheduler.
class ForwardList {
public:

    ForwardList() : _head(nullptr), _tail(nullptr) {}

    void Push(Process *p);
    Process *Take();

    Process *Front() const;
    bool Empty() const;

private:
    Process *_head;
    Process *_tail;
};

}
}

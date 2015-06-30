#pragma once

#include "types.hpp"
#include "message.hpp"

#include <wild/types.hpp>
#include <wild/SpinLock.hpp>

#include <memory>
#include <system_error>
#include <tuple>
#include <functional>
#include <type_traits>

namespace stp {
namespace process {

process_t spawn(std::function<void()> func, size_t addstack = 0);

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
process_t spawn(Closure&& closure, size_t addstack = 0) {
    std::function<void()> func = PCallable(new TClosure<std::remove_cv_t<Closure>>(std::forward<Closure>(closure)));
    return spawn(func, addstack);
}

message::Content suspend(session_t);

process_t self();

void exit();
void kill(process_t pid);

class Session {
public:

    Session(session_t session);
    Session(uintptr process, session_t session);

    Session(Session&& other)
        : _process(other._process), _session(other._session) {
        other._process = 0;
        other._session = session_t(0);
    }

    ~Session() {
        if (_session) {
            close();
        }
    }

    process_t Pid() const;
    session_t Value() const {
        return _session;
    }

    Session& operator=(Session&& other) {
        if (_session) {
            close();
        }
        _process = other._process;
        _session = other._session;
        other._process = 0;
        other._session = session_t(0);
        return *this;
    }

    explicit operator session_t() const {
        return _session;
    }

    explicit operator bool() const {
        return bool(_session);
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

private:

    void close();

    uintptr _process;
    session_t _session;
};

Session new_session();

void send(process_t pid, message::Content content);
void send(process_t pid, session_t session, message::Content content);
message::Content request(process_t pid, message::Content content);
void response(process_t pid, session_t session, message::Content content = {});

void loop(std::function<void(process_t source, session_t session, message::Content&& content)> callback);

class Mutex {
public:
    Mutex();
    ~Mutex();

    void lock();
    void unlock();
    bool try_lock();

    // capture by copying
    Mutex(const Mutex&);

private:

    Mutex& operator=(const Mutex&) = delete;

    // there is no empty mutex.
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    uintptr _opaque;
};

class Condition {
public:

    Condition();
    ~Condition();

    void wait(Mutex& locker);
    void wait(Mutex& locker, std::function<bool()> pred);

    void notify_one();
    void notify_all();

    // allow capturing by copying
    Condition(const Condition&);

private:

    Condition& operator=(const Condition&) = delete;

    Condition(Condition&&) = delete;
    Condition& operator=(Condition&&) = delete;

    uintptr _opaque;
};

namespace coroutine {

void spawn(std::function<void()> func, size_t addstack = 0);
void timeout(uint64 msecs, std::function<void()> func, size_t addstack = 0);

void sleep(uint64 msecs);

// relinquish CPU
void yield();

void exit();

}

}
}

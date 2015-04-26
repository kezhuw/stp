#pragma once

#include "types.hpp"
#include "message.hpp"

#include <wild/types.hpp>
#include <wild/spinlock.hpp>
#include <wild/forward_list.hpp>

#include <memory>
#include <system_error>
#include <tuple>
#include <functional>
#include <type_traits>

namespace stp {
namespace process {

process_t Spawn(std::function<void()> func, size_t addstack = 0);

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
process_t Spawn(Closure&& closure, size_t addstack = 0) {
    std::function<void()> func = PCallable(new TClosure<std::remove_cv_t<Closure>>(std::forward<Closure>(closure)));
    return Spawn(func, addstack);
}

message::Content Suspend(session_t);

// relinquish CPU
void Yield();

process_t Pid();

void Exit();
void Kill(process_t pid);

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

Session NewSession();

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

void Send(process_t pid, message::Content content);
void Send(process_t pid, session_t session, message::Content content);
message::Content Request(process_t pid, message::Content content);
void Response(process_t pid, session_t session, message::Content content = {});

void Loop(std::function<void(process_t source, session_t session, message::Content&& content)> callback);

}
}

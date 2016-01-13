#pragma once

#include "types.hpp"

#include "wild/types.hpp"
#include "wild/Any.hpp"
#include "wild/SpinLock.hpp"

#include <memory>
#include <system_error>
#include <tuple>
#include <functional>
#include <type_traits>
#include <typeindex>

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

void send(process_t pid, wild::Any content);
void send(process_t pid, session_t session, wild::Any content);
wild::Any request(process_t pid, wild::Any content);
void response(wild::Any content = {});
void response(process_t pid, session_t session, wild::Any content = {});

process_t sender();
session_t session();

void request_callback(const std::type_info& type, std::function<void(wild::Any&& content)> handler);
void request_coroutine(const std::type_info& type, std::function<void(wild::Any&& content)> handler, size_t addstack = 0);
void notification_callback(const std::type_info& type, std::function<void(wild::Any&& content)> handler);
void notification_coroutine(const std::type_info& type, std::function<void(wild::Any&& content)> handler, size_t addstack = 0);

struct EmptyResponse {};

template<typename RequestT>
void request_callback(std::function<void(RequestT *request)> handler) {
    const std::type_info& type = typeid(RequestT);
    request_callback(type, [handler = std::move(handler)](wild::Any&& content) {
        using RequestT1 = typename std::remove_cv<RequestT>::type;
        handler(wild::Any::Cast<RequestT1>(&content));
    });
}

template<typename RequestT>
void request_coroutine(std::function<void(RequestT *request)> handler, size_t addstack = 0) {
    const std::type_info& type = typeid(RequestT);
    request_coroutine(type, [handler = std::move(handler)](wild::Any&& content) {
        using RequestT1 = typename std::remove_cv<RequestT>::type;
        handler(wild::Any::Cast<RequestT1>(&content));
    }, addstack);
}

template<typename RequestT, typename ResponseT,
         std::enable_if_t<!std::is_same<ResponseT, void>::value>* = nullptr,
         std::enable_if_t<!std::is_same<ResponseT, EmptyResponse>::value>* = nullptr>
void request_callback(std::function<ResponseT(RequestT *request)> handler) {
    const std::type_info& type = typeid(RequestT);
    request_callback(type, [handler = std::move(handler)](wild::Any&& content) {
        using RequestT1 = typename std::remove_cv<RequestT>::type;
        response(handler(wild::Any::Cast<RequestT1>(&content)));
    });
}

template<typename RequestT, typename ResponseT,
         std::enable_if_t<!std::is_same<ResponseT, void>::value>* = nullptr,
         std::enable_if_t<std::is_same<ResponseT, EmptyResponse>::value>* = nullptr>
void request_callback(std::function<ResponseT(RequestT *request)> handler) {
    const std::type_info& type = typeid(RequestT);
    request_callback(type, [handler = std::move(handler)](wild::Any&& content) {
        using RequestT1 = typename std::remove_cv<RequestT>::type;
        handler(wild::Any::Cast<RequestT1>(&content));
        response();
    });
}

template<typename RequestT, typename ResponseT,
         std::enable_if_t<!std::is_same<ResponseT, void>::value>* = nullptr,
         std::enable_if_t<!std::is_same<ResponseT, EmptyResponse>::value>* = nullptr>
void request_coroutine(std::function<ResponseT(RequestT *request)> handler, size_t addstack = 0) {
    const std::type_info& type = typeid(RequestT);
    request_coroutine(type, [handler = std::move(handler)](wild::Any&& content) {
        using RequestT1 = typename std::remove_cv<RequestT>::type;
        response(handler(wild::Any::Cast<RequestT1>(&content)));
    }, addstack);
}

template<typename RequestT, typename ResponseT,
         std::enable_if_t<!std::is_same<ResponseT, void>::value>* = nullptr,
         std::enable_if_t<std::is_same<ResponseT, EmptyResponse>::value>* = nullptr>
void request_coroutine(std::function<ResponseT(RequestT *request)> handler, size_t addstack = 0) {
    const std::type_info& type = typeid(RequestT);
    request_coroutine(type, [handler = std::move(handler)](wild::Any&& content) {
        using RequestT1 = typename std::remove_cv<RequestT>::type;
        handler(wild::Any::Cast<RequestT1>(&content));
        response();
    }, addstack);
}

template<typename MessageT>
void notification_callback(std::function<void(MessageT *message)> handler) {
    const std::type_info& type = typeid(MessageT);
    notification_callback(type, [handler = std::move(handler)](wild::Any&& content) {
        using MessageT1 = typename std::remove_cv<MessageT>::type;
        handler(wild::Any::Cast<MessageT1>(&content));
    });
}

template<typename MessageT>
void notification_coroutine(std::function<void(MessageT *message)> handler, size_t addstack = 0) {
    const std::type_info& type = typeid(MessageT);
    notification_coroutine(type, [handler = std::move(handler)](wild::Any&& content) {
        using MessageT1 = typename std::remove_cv<MessageT>::type;
        handler(wild::Any::Cast<MessageT1>(&content));
    }, addstack);
}

void serve();

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

// std::function need copyable function object.
// Lambda with move-only object captured is not copyable.
template<typename Closure
       , std::enable_if_t<std::is_convertible<Closure, std::function<void()>>::value>* = nullptr
       , std::enable_if_t<!std::is_same<Closure, void()>::value>* = nullptr
       , std::enable_if_t<!std::is_same<Closure, std::function<void()>>::value>* = nullptr
       , std::enable_if_t<!std::is_copy_constructible<Closure>::value>* = nullptr
        >
void spawn(Closure&& closure, size_t addstack = 0) {
    std::function<void()> func = PCallable(new TClosure<std::remove_cv_t<Closure>>(std::forward<Closure>(closure)));
    return spawn(func, addstack);
}

void timeout(uint64 msecs, std::function<void()> func, size_t addstack = 0);

void sleep(uint64 msecs);

// relinquish CPU
void yield();

wild::Any block(session_t);

void exit();

}

}
}

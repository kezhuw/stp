#pragma once

#include "types.hpp"
#include "SharedCallable.hpp"

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

// std::function need copyable function object.
// Lambda with move-only object captured is not copyable.
template<typename Callable, std::enable_if_t<stp::is_moveonly_callable<Callable>::value>* = nullptr>
process_t spawn(Callable&& callable, size_t addstack = 0) {
    std::function<void()> func = SharedCallable(new MoveonlyCallable<std::remove_cv_t<Callable>>(std::forward<Callable>(callable)));
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

}
}

namespace stp {
namespace coroutine {

class Coroutine;

Coroutine* spawn(std::function<void()> func, size_t addstack = 0);

// std::function need copyable function object.
// Lambda with move-only object captured is not copyable.
template<typename Callable, std::enable_if_t<stp::is_moveonly_callable<Callable>::value>* = nullptr>
Coroutine* spawn(Callable&& callable, size_t addstack = 0) {
    std::function<void()> func = SharedCallable(new MoveonlyCallable<std::remove_cv_t<Callable>>(std::forward<Callable>(callable)));
    return spawn(func, addstack);
}

Coroutine* self();

void timeout(uint64 msecs, std::function<void()> func, size_t addstack = 0);

void sleep(uint64 msecs);

// relinquish CPU
void yield();

wild::Any block(session_t);

wild::Any suspend();
void wakeup(Coroutine *co);

void exit();

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

}
}

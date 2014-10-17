#pragma once

#include "types.hpp"

#include <mutex>
#include <queue>
#include <functional>

namespace stp {
namespace coroutine {

class Coroutine;

void Spawn(std::function<void()> func, size_t addstack = 0);

void Wakeup(Coroutine *co);

void Sleep(wild::uint64 msecs);
void Timeout(uint64 msecs, std::function<void()> func, size_t addstack = 0);

Coroutine *Running();

void Exit();

class ForwardList {
public:

    void Push(Coroutine *);
    Coroutine *Take();

    Coroutine *Front() const;
    bool Empty() const;

    ForwardList& operator=(ForwardList&& other) {
        _head = other._head;
        _tail = other._tail;
        other._head = other._tail = nullptr;
        return *this;
    }

private:
    Coroutine *_head = nullptr;
    Coroutine *_tail = nullptr;
};

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
    ForwardList _coroutines;
};

class Condition {
public:

    Condition() = default;
    ~Condition() = default;

    Condition(const Condition&) = delete;
    Condition& operator=(const Condition&) = delete;

    Condition(Condition&&) = delete;
    Condition& operator=(Condition&&) = delete;

    void wait(Mutex& locker);

    void notify_one();
    void notify_all();

private:
    Mutex _mutex;
    ForwardList _blocks;
};

}
}

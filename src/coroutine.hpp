#pragma once

#include "types.hpp"

#include <mutex>
#include <functional>

namespace stp {
namespace coroutine {

void spawn(std::function<void()> func, size_t addstack = 0);
void timeout(uint64 msecs, std::function<void()> func, size_t addstack = 0);

void sleep(uint64 msecs);

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

}
}

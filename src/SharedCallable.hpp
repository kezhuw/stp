#pragma once

#include <memory>

namespace stp {

class Callable {
public:
    virtual void operator()() = 0;

    virtual ~Callable() {}
};

template<typename CallableT>
class MoveonlyCallable final : public Callable {
public:

    explicit MoveonlyCallable(CallableT&& callable)
        : _callable(std::move(callable)) {
    }

    MoveonlyCallable(const CallableT&) = delete;
    MoveonlyCallable& operator=(const CallableT&) = delete;

    virtual void operator()() override {
        _callable();
    }

private:
    CallableT _callable;
};

class SharedCallable {
public:

    SharedCallable(Callable *callable) : _callable(callable) {}

    void operator()() {
        (*_callable)();
    }

private:
    std::shared_ptr<Callable> _callable;
};

}

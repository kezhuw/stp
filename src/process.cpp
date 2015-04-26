#include "process.hpp"
#include "message.hpp"
#include "module.hpp"
#include "context.hpp"
#include "coroutine.hpp"
#include "timer.hpp"

#include <wild/BlockingQueue.hpp>
#include <wild/exception.hpp>
#include <wild/module.hpp>
#include <wild/types.hpp>
#include <wild/likely.hpp>
#include <wild/with_lock.hpp>
#include <wild/scope_guard.hpp>
#include <wild/forward_list.hpp>
#include <wild/id_allocator.hpp>
#include <wild/utility.hpp>

#include <ucontext.h>
#include <sys/mman.h>

#include <cstddef>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <deque>
#include <exception>
#include <functional>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <unistd.h>

namespace stp {

using Process = process::Process;
using Coroutine = coroutine::Coroutine;

namespace sched { static void Resume(Process *); }

enum class SystemCode {
    kKill,
    kAbort,
    kError,
};

struct SystemMessage {
    SystemCode code;
};

struct AbortedRequest {};

enum class MessageType {
    kNotify     = 0,
    kRequest    = 1,
    kResponse   = 2,
};

session_t sessionForResponse(session_t session) {
    return session_t(-session.Value());
}

MessageType typeOfMessage(message::Message *msg) {
    int32 session = static_cast<int32>(msg->session.Value());
    if (session > 0) {
        return MessageType::kRequest;
    } else if (session < 0) {
        return MessageType::kResponse;
    }
    return MessageType::kNotify;
}

namespace coroutine {

static thread_local context::Context *tContext = context::New();
static thread_local stp::coroutine::Coroutine *tCoroutine;

context::Context *
ThreadContext() {
    return tContext;
}

Coroutine *
Running() {
    return tCoroutine;
}

void
SetRunning(Coroutine *co) {
    tCoroutine = co;
}

void Die(Coroutine *co);

message::Content Suspend();

class Scope {
public:
    Scope(Coroutine *co) {
        // assert(Running() == nullptr);
        SetRunning(co);
    }
    ~Scope() {
        SetRunning(nullptr);
    }
};

struct ExitException : public std::exception {
    virtual const char *what() const noexcept final override {
        return "coroutine::ExitException";
    }
};

}

namespace process {

static thread_local Process *tProcess;

Process *
Running() {
    return tProcess;
}

void
SetRunning(Process *p) {
    tProcess = p;
}

class Scope {
public:
    Scope(Process *p) {
        assert(Running() == nullptr);
        SetRunning(p);
    }
    ~Scope() {
        SetRunning(nullptr);
    }
};

struct ExitException : public std::exception {
    virtual const char *what() const noexcept final override {
        return "process::ExitException";
    }
};

struct AbortException : public std::exception {
    virtual const char *what() const noexcept final override {
        return "process::AbortException";
    }
};

struct KillException : public std::exception {
    KillException(process_t killer) : Killer(killer) {}
    const process_t Killer;
    virtual const char *what() const noexcept final override {
        return "process::KillException";
    }
};

void Ref(Process *);
void Unref(Process *);

}

namespace coroutine {

class Result {
public:

    void set_value(message::Content value) {
        _value = std::move(value);
    }

    void set_exception(const std::exception_ptr &e) {
        _exception = e;
    }

    message::Content get_value() {
        if (_exception) {
            std::rethrow_exception(_exception);
        }
        return std::move(_value);
    }

private:
    message::Content _value;
    std::exception_ptr _exception;
};

class Coroutine {
public:

    context::Context* Context() {
        return _context;
    }

    message::Content GetResult() {
        return _result.get_value();
    }

    void SetResult(message::Content value) {
        _result.set_value(std::move(value));
    }

    void SetException(const std::exception_ptr &e) {
        _result.set_exception(e);
    }

    // TODO allocate from thread local coroutine pool.
    static Coroutine *New(std::function<void()> func, size_t addstack) {
        return new Coroutine(std::move(func), addstack);
    }

    static void Delete(Coroutine *co) {
        delete co;
    }

    void Resume() {
        coroutine::Scope enter(this);
        context::Switch(coroutine::ThreadContext(), _context);
    }

private:

    Coroutine(std::function<void()> func, size_t addstack)
        : _context(nullptr)
        , _function(std::move(func)) {
#define CoroutineMain   reinterpret_cast<void(*)(void*)>(&Coroutine::Main)
        _context = context::New(CoroutineMain, static_cast<void*>(this), addstack);
    }

    ~Coroutine() {
        context::Delete(_context);
    }

    static void Main(Coroutine *co) {
        assert(coroutine::Running() == co);
        co->Run();
        assert(coroutine::Running() == co);
        coroutine::Die(co);
    }

    void Run() {
        try {
            _function();
        } catch (coroutine::ExitException&) {
        } catch (...) {
            wild::print_exception(std::current_exception());
        }
    }

    Result _result;
    context::Context *_context;
    std::function<void()> _function;
};

void Wakeup(Coroutine *co);

void Sleep(uint64 msecs) {
    timer::Sleep(msecs);
}

void Exit() {
    Coroutine *running = Running();
    assert(running != nullptr);
    throw coroutine::ExitException();
}

message::Content Suspend() {
    Coroutine *running = Running();
    assert(running != nullptr);
    context::Switch(running->Context(), coroutine::ThreadContext());
    assert(running == Running());
    return running->GetResult();
}

void Resume(Coroutine *co) {
    co->Resume();
}

namespace detail {

class Mutex {
public:
    Mutex() = default;
    ~Mutex() = default;

    void lock();
    void unlock();
    bool try_lock();

    static Mutex* create();
    static Mutex* ref(Mutex*);
    static void unref(Mutex*);

private:
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    int64_t _refcnt;
    std::deque<Coroutine*> _coroutines;
};

Mutex* Mutex::create() {
    auto m = new Mutex;
    m->_refcnt = 1;
    return m;
}

Mutex* Mutex::ref(Mutex* m) {
    m->_refcnt += 1;
    return m;
}

void Mutex::unref(Mutex* m) {
    assert(m->_refcnt > 0);
    m->_refcnt -= 0;
    if (m->_refcnt == 0) {
        delete m;
    }
}

void Mutex::lock() {
    auto running = Running();
    assert(running != nullptr);
    if (_coroutines.empty()) {
        _coroutines.push_back(running);
    } else {
        assert(_coroutines.front() != running);
        _coroutines.push_back(running);
        try {
            coroutine::Suspend();
            assert(!_coroutines.empty());
            assert(_coroutines.front() == running);
        } catch (...) {
            wild::print_exception(std::current_exception());
            std::terminate();
        }
    }
}

bool Mutex::try_lock() {
    if (_coroutines.empty()) {
        auto running = Running();
        assert(running != nullptr);
        _coroutines.push_back(running);
        return true;
    }
    return false;
}

void Mutex::unlock() {
    auto running_ = Running();
    assert(running_ != nullptr);
    assert(!_coroutines.empty());
    assert(_coroutines.front() == running_);
    _coroutines.pop_front();
    if (!_coroutines.empty()) {
        auto pending = reinterpret_cast<Coroutine*>(_coroutines.front());
        coroutine::Wakeup(pending);
    }
}

class Condition {
public:
    Condition() = default;
    ~Condition() = default;

    void wait(Mutex& locker);
    void wait(Mutex& locker, std::function<bool()> pred);

    void notify_one();
    void notify_all();

    static Condition *create();
    static Condition *ref(Condition *m);
    static void unref(Condition *m);

private:
    Condition(const Condition&) = delete;
    Condition& operator=(const Condition&) = delete;

    Condition(Condition&&) = delete;
    Condition& operator=(Condition&&) = delete;

    int64_t _refcnt;
    std::deque<Coroutine*> _coroutines;
};

Condition* Condition::create() {
    auto c = new Condition;
    c->_refcnt = 1;
    return c;
}

Condition* Condition::ref(Condition *c) {
    c->_refcnt += 1;
    return c;
}

void Condition::unref(Condition *c) {
    assert(c->_refcnt > 0);
    c->_refcnt -= 1;
    if (c->_refcnt == 0) {
        assert(c->_coroutines.empty());
        delete c;
    }
}

void Condition::wait(Mutex& locker) {
    auto running = Running();
    assert(running != nullptr);
    locker.unlock();
    _coroutines.push_back(running);
    try {
        coroutine::Suspend();
        locker.lock();
    } catch (...) {
        wild::print_exception(std::current_exception());
        std::terminate();
    }
}

void Condition::wait(Mutex& locker, std::function<bool()> pred) {
    while (!pred()) {
        wait(locker);
    }
}

void Condition::notify_one() {
    if (!_coroutines.empty()) {
        auto pending = wild::take_front(_coroutines);
        coroutine::Wakeup(pending);
    }
}

void Condition::notify_all() {
    for (auto pending : _coroutines) {
        coroutine::Wakeup(pending);
    }
    _coroutines.clear();
}

}

Mutex::Mutex() {
    _opaque = reinterpret_cast<uintptr>(detail::Mutex::create());
}

Mutex::Mutex(const Mutex& other) {
    auto m = reinterpret_cast<detail::Mutex*>(other._opaque);
    _opaque = reinterpret_cast<uintptr>(detail::Mutex::ref(m));
}

Mutex::~Mutex() {
    auto m = reinterpret_cast<detail::Mutex*>(_opaque);
    detail::Mutex::unref(m);
}

void Mutex::lock() {
    auto m = reinterpret_cast<detail::Mutex*>(_opaque);
    m->lock();
}

void Mutex::unlock() {
    auto m = reinterpret_cast<detail::Mutex*>(_opaque);
    m->unlock();
}

bool Mutex::try_lock() {
    auto m = reinterpret_cast<detail::Mutex*>(_opaque);
    return m->try_lock();
}

Condition::Condition() {
    _opaque = reinterpret_cast<uintptr>(detail::Condition::create());
}

Condition::Condition(const Condition& other) {
    auto c = reinterpret_cast<detail::Condition*>(other._opaque);
    _opaque = reinterpret_cast<uintptr>(detail::Condition::ref(c));
}

Condition::~Condition() {
    auto c = reinterpret_cast<detail::Condition*>(_opaque);
    detail::Condition::unref(c);
}

void Condition::wait(Mutex& locker) {
    static_assert(sizeof(Mutex) == sizeof(uintptr), "");
    auto p = reinterpret_cast<uintptr*>(std::addressof(locker));
    auto m = reinterpret_cast<detail::Mutex*>(*p);
    auto c = reinterpret_cast<detail::Condition*>(_opaque);
    c->wait(*m);
}

void Condition::wait(Mutex& locker, std::function<bool()> pred) {
    while (!pred()) {
        wait(locker);
    }
}

void Condition::notify_one() {
    auto c = reinterpret_cast<detail::Condition*>(_opaque);
    c->notify_one();
}

void Condition::notify_all() {
    auto c = reinterpret_cast<detail::Condition*>(_opaque);
    c->notify_all();
}

}

namespace process {

static wild::SpinLock gProcsLocker;
static std::unordered_map<uint32, Process*> gProcsMap;

static process_t Register(Process *p) {
    static uint32 seq = 0;
    uint32 id;
    WITH_LOCK(gProcsLocker) {
        id = ++seq;
        gProcsMap[id] = p;
    }
    return process_t(id);
}

static bool Unregister(process_t pid) {
    WITH_LOCK(gProcsLocker) {
        return gProcsMap.erase(pid.Value()) == 1;
    }
}

static Process *Find(process_t pid) {
    WITH_LOCK(gProcsLocker) {
        auto it = gProcsMap.find(pid.Value());
        if (it != gProcsMap.end()) {
            Process *p = it->second;
            process::Ref(p);
            return p;
        }
    }
    return nullptr;
}

class Process {
public:

    void PushMessage(message::Message *msg) {
        bool wait = _mailbox.push(msg);
        if (wait) {
            sched::Resume(this);
        }
    }

    message::Message *PopMessage();

    message::Content Suspend(session_t session) {
        Coroutine *running = coroutine::Running();
        _block_sessions[session] = running;
        return coroutine::Suspend();
    }

    Coroutine *unblock(session_t session) {
        auto it = _block_sessions.find(session);
        if (it == _block_sessions.end()) {
            return nullptr;
        }
        Coroutine *co = it->second;
        _block_sessions.erase(it);
        return co;
    }

    void mark_zombie(Coroutine *co) {
        _zombie_coroutines.push_back(co);
    }

    void sweep_zombies() {
        for (auto co : _zombie_coroutines) {
            Coroutine::Delete(co);
        }
        _zombie_coroutines.clear();
    }

    void Schedule() {
        // running coroutine may:
        //   spawn new coroutine;
        //   wakeup suspended coroutine;
        //   block to read maibox.
        while (!_spawn_coroutines.empty()
            || !_wakeup_coroutines.empty()
            || (!_inbox.empty() && !_inbox_coroutines.empty())) {
            while (!_spawn_coroutines.empty()) {
                coroutine::Resume(wild::take_front(_spawn_coroutines));
            }
            while (!_wakeup_coroutines.empty()) {
                coroutine::Resume(wild::take_front(_wakeup_coroutines));
            }
            while (!_inbox.empty() && !_inbox_coroutines.empty()) {
                coroutine::Resume(wild::take_front(_inbox_coroutines));
            }
        }

        sweep_zombies();
    }

    bool Inactive() {
        return _spawn_coroutines.empty() && _wakeup_coroutines.empty() && _block_sessions.empty() && _inbox_coroutines.empty();
    }

    void Wakeup(Coroutine *co) {
        _wakeup_coroutines.push_back(co);
    }

    enum class ResumeResult : uintptr {
        Resume          = 0,
        Down            = 1,
        Break           = 2,
    };

    ResumeResult Resume() {
        process::Scope enter(this);
        Schedule();
        if (Inactive()) {
            return ResumeResult::Down;
        }
        auto msg = _mailbox.take();
        if (msg != nullptr) {
            MessageType msgType = typeOfMessage(msg);
            switch (msgType) {
            case MessageType::kRequest:
                ServeRequest(msg->source, msg->session);
                _inbox.push_back(msg);
                break;
            case MessageType::kResponse: {
                uint32 session = -msg->session.Value();
                if (Coroutine *co = unblock(session_t(session))) {
                    if (msg->content.type() == typeid(AbortedRequest)) {
                        co->SetException(std::make_exception_ptr(AbortException{}));
                    } else {
                        co->SetResult(std::move(msg->content));
                    }
                    Wakeup(co);
                } else {
                    printf("unknown response session[%u]\n", session);
                }
                message::Delete(msg);
                } break;
            case MessageType::kNotify:
                if (msg->content.type() == typeid(SystemMessage)) {
                    break;
                }
                _inbox.push_back(msg);
                break;
            }
            // Schedule();
            return ResumeResult::Resume;
        }
        return ResumeResult::Break;
    }

    void Response(process_t source, session_t session, message::Content content) {
        if (Process *p = Find(source)) {
            p->PushMessage(message::New(Pid(), sessionForResponse(session), std::move(content)));
            process::Unref(p);
        }
        DoneRequest(source, session);
    }

    std::unordered_set<std::tuple<process_t, session_t>> _requestsMap;

    Session NewSession() {
        uint32 id = _sessions.NewId();
        return Session(reinterpret_cast<uintptr>(this), session_t(id));
    }

    void ReleaseSession(session_t session) {
        _sessions.Erase(session.Value());
    }

    process_t Pid() const {
        return _pid;
    }

    message::Message *nextMessage() {
        Coroutine *co = coroutine::Running();
        assert(co != nullptr);
        while (_inbox.empty()) {
            _inbox_coroutines.push_back(co);
            coroutine::Suspend();
        }
        return wild::take_front(_inbox);
    }

    void Loop(std::function<void(process_t source, session_t session, message::Content&& content)> callback) {
        for (;;) {
            Message *msg = nextMessage();
            SCOPE_EXIT {
                message::Delete(msg);
            };
            callback(msg->source, msg->session, std::move(msg->content));
        }
    }

    void Spawn(std::function<void()> func, size_t addstack) {
        auto co = Coroutine::New(std::move(func), addstack);
        _spawn_coroutines.push_back(co);
    }

    static Process *New(std::function<void()> func, size_t addstack) {
        auto p = new Process();
        p->_pid = Register(p);
        p->Spawn(std::move(func), addstack);
        return p;
    }

    void ServeRequest(process_t source, session_t session) {
        _requestsMap.insert(std::make_tuple(source, session));
    }

    void DoneRequest(process_t source, session_t session) {
        _requestsMap.erase(std::make_tuple(source, session));
    }

    void AbortRequest(process_t source, session_t session) {
        process::Send(source, sessionForResponse(session), AbortedRequest{});
    }

    // XXX
    // Kill/Unref, when to delete ?
    static void Delete(Process *p) {
        printf("delete process: %p\n", p);
        delete p;
    }

private:

    Process()
        : _refcnt(1) {
    }

    ~Process() {
        for (auto request : _requestsMap) {
            AbortRequest(std::get<process_t>(request), std::get<session_t>(request));
        }
        while (!_inbox.empty()) {
            Message *msg = wild::take_front(_inbox);
            if (typeOfMessage(msg) == MessageType::kRequest) {
                AbortRequest(msg->source, msg->session);
            }
            message::Delete(msg);
        }
        while (Message *msg = _mailbox.take()) {
            if (typeOfMessage(msg) == MessageType::kRequest) {
                AbortRequest(msg->source, msg->session);
            }
            message::Delete(msg);
        }
    }

    process_t _pid;

    std::atomic<intptr> _refcnt;

    message::Mailbox _mailbox;
    std::deque<message::Message*> _inbox;

    wild::IdAllocator<uint32> _sessions;

    std::deque<Coroutine*> _inbox_coroutines;
    std::deque<Coroutine*> _spawn_coroutines;
    std::deque<Coroutine*> _wakeup_coroutines;
    std::vector<Coroutine*> _zombie_coroutines;
    std::unordered_map<session_t, Coroutine *> _block_sessions;

    friend void Ref(Process *p);
    friend void Unref(Process *p);
};

void Loop(std::function<void(process_t source, session_t session, message::Content&& content)> callback) {
    process::Running()->Loop(std::move(callback));
}

void Ref(Process *p) {
    p->_refcnt.fetch_add(1, std::memory_order_relaxed);
}

void Unref(Process *p) {
    auto refcnt = p->_refcnt.fetch_sub(1, std::memory_order_relaxed);
    if (refcnt == 1) {
        // FIXME How to retire it ?
        Process::Delete(p);
    }
}

void Retire(Process *p) {
    if (Unregister(p->Pid())) {
        process::Unref(p);
        printf("unregister succ %p\n", p);
    } else {
        printf("unregister fail %p\n", p);
    }
}

void Resume(Process *p) {
    switch (p->Resume()) {
    case Process::ResumeResult::Resume:
        sched::Resume(p);
        break;
    case Process::ResumeResult::Down:
        process::Retire(p);
        break;
    default:
        break;
    }
    process::Unref(p);
}

process_t
Spawn(std::function<void()> func, size_t addstack) {
    auto p = Process::New(std::move(func), addstack);
    process_t pid = p->Pid();
    sched::Resume(p);
    return pid;
}

process_t
Pid() {
    Process *p = Running();
    if (p) {
        return p->Pid();
    }
    return process_t();
}

void Exit() {
    Process *running = Running();
    if (UNLIKELY(!running)) {
        throw std::runtime_error("call Exit() out of process");
    }
    throw process::ExitException();
}

void Kill(process_t pid) {
    process_t self = Pid();
    if (pid == process_t(0) || pid == self) {
        Exit();
    }
    if (Process *p = Find(pid)) {
        p->PushMessage(message::New(self, session_t(), SystemMessage{SystemCode::kKill}));
        process::Unref(p);
    }
}

message::Content Suspend(session_t session) {
    return Running()->Suspend(session);
}

Session NewSession() {
    Process *running = process::Running();
    return running->NewSession();
}

void ReleaseSession(session_t session) {
    Running()->ReleaseSession(session);
}

message::Content Request(process_t pid, message::Content content) {
    Process *running = Running();
    if (UNLIKELY(!running)) {
        throw std::runtime_error("not in process");
    }
    if (Process *p = Find(pid)) {
        Session session = running->NewSession();
        p->PushMessage(message::New(running->Pid(), session.Value(), content));
        process::Unref(p);
        return running->Suspend(session.Value());
    }
    throw 5;
}

void Response(process_t source, session_t session, message::Content content) {
    if (Process *running = process::Running()) {
        running->Response(source, session, std::move(content));
    } else if (Process *p = Find(source)) {
        p->PushMessage(message::New(process_t(), sessionForResponse(session), std::move(content)));
        process::Unref(p);
    }
}

void Send(process_t pid, message::Content content) {
    Send(pid, session_t(), std::move(content));
}

void Send(process_t pid, session_t session, message::Content content) {
    if (Process *p = Find(pid)) {
        p->PushMessage(message::New(Pid(), session, std::move(content)));
    }
}

void Yield() {
    auto running = process::Running();
    assert(running != nullptr);
    Session session = running->NewSession();
    running->PushMessage(message::New(process_t(0), session.Value(), message::Content{}));
    running->Suspend(session.Value());
}

Session::Session(session_t session)
    : Session(reinterpret_cast<uintptr>(process::Running()), session) {
}

Session::Session(uintptr process, session_t session)
    : _process(process), _session(session) {
}

process_t Session::Pid() const {
    auto p = reinterpret_cast<Process*>(_process);
    return p->Pid();
}

void Session::close() {
    auto p = reinterpret_cast<Process*>(_process);
    p->ReleaseSession(_session);
}

}

namespace coroutine {

void Die(Coroutine *co) {
    auto p = process::Running();
    p->mark_zombie(co);
    context::Switch(co->Context(), coroutine::ThreadContext());
}

void Spawn(std::function<void()> func, size_t addstack) {
    auto p = process::Running();
    assert(p == nullptr);
    p->Spawn(std::move(func), addstack);
}

void Timeout(uint64 msecs, std::function<void()> func, size_t addstack) {
    Spawn([msecs, func = std::move(func)] {
        Sleep(msecs);
        func();
    }, addstack);
}

void
Wakeup(Coroutine *co) {
    auto p = process::Running();
    assert(p != nullptr);
    p->Wakeup(co);
}

}

namespace sched {

using ProcessQueue = wild::BlockingQueue<process::Process*, wild::SpinLock>;

struct Worker {
public:

    Worker() {
        _thread = std::thread(std::mem_fn(&Worker::Run), this);
    }

    void Queue(Process *p) {
        if (p == nullptr) {
            return;
        }
        _queue.push(p);
    }

private:

    void Run() {
        for (;;) {
            auto p = _queue.take();
            if (p == nullptr) {
                break;
            }
            process::Resume(p);
        }
    }

    ProcessQueue _queue;
    std::thread _thread;
};

static class {
public:

    void Start(size_t n) {
        n = std::max(size_t(2), n);
        _n = n;
        _workers = new Worker[n];
    }

    void Resume(Process *p) {
        process::Ref(p);
        uintptr randval = reinterpret_cast<uintptr>(p) + reinterpret_cast<uintptr>(&p);
        size_t index = ((randval >> 4) + (randval >> 16))%_n;
        _workers[index].Queue(p);
    }

private:
    size_t _n;
    Worker *_workers;
} scheduler;

static void Resume(Process *p) {
    scheduler.Resume(p);
}

static void init() {
    unsigned n = std::thread::hardware_concurrency();
    scheduler.Start(static_cast<size_t>(n));
}

static wild::module::Definition sched(module::STP, "stp::sched", init, module::Order::Sched);

}

}

#include "process.hpp"
#include "message.hpp"
#include "context.hpp"
#include "coroutine.hpp"
#include "sched.hpp"
#include "timer.hpp"

#include <wild/types.hpp>
#include <wild/likely.hpp>
#include <wild/with_lock.hpp>
#include <wild/scope_guard.hpp>
#include <wild/forward_list.hpp>
#include <wild/id_allocator.hpp>

#include <ucontext.h>
#include <sys/mman.h>

#include <cstddef>
#include <cstdlib>

#include <atomic>
#include <deque>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <unistd.h>


namespace stp {

using Process = process::Process;
using Coroutine = coroutine::Coroutine;

namespace coroutine {

static thread_local context::Context *tContext = context::New();
static thread_local stp::coroutine::Coroutine *tCoroutine;
static thread_local ForwardList tZombieQueue;

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

wild::uintptr Yield();

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

struct ExitException {
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

class NestedScope {
public:
    NestedScope(Process *p)
        : _origin(process::Running()) {
        SetRunning(p);
    }
    ~NestedScope() {
        SetRunning(_origin);
    }

private:
    Process *_origin;
};

struct ExitException {
};

struct AbortException {
};

struct KillException {
    KillException(process_t killer) : Killer(killer) {}
    const process_t Killer;
};

void Ref(Process *);
void Unref(Process *);

}

namespace coroutine {

class Coroutine {
public:

    context::Context* Context() {
        return _context;
    }

    uintptr Result() {
        return _result;
    }

    void SetResult(uintptr value) {
        _result = value;
    }

    void SetException(std::exception_ptr e) {
        _exception = std::move(e);
    }

    void ClearResult() {
        _result = 0;
        _exception = std::exception_ptr();
    }

    uintptr FetchResult() {
        if (UNLIKELY(bool(_exception))) {
            std::exception_ptr e;
            std::swap(e, _exception);
            std::rethrow_exception(e);
        }
        uintptr v = _result;
        _result = 0;
        return v;
    }

    void SetJoiner(Coroutine *joiner) {
        _joiner = joiner;
    }

    Coroutine *Joiner() {
        return _joiner;
    }

    void Detach() {
        assert(_joiner == nullptr);
        _joiner = reinterpret_cast<Coroutine*>(1);
    }

    // TODO allocate from thread local coroutine pool.
    static Coroutine *New(std::function<void()> func, size_t stacksize) {
        return new Coroutine(std::move(func), stacksize);
    }

    static void Delete(Coroutine *co) {
        delete co;
    }

    void Resume() {
        coroutine::Scope enter(this);
        context::Switch(coroutine::ThreadContext(), _context);
    }

private:

    Coroutine(std::function<void()> func, size_t stacksize)
        : _joiner(nullptr)
        , _result(0)
        , _context(nullptr)
        , _function(std::move(func)) {
#define CoroutineMain   reinterpret_cast<void(*)(void*)>(&Coroutine::Main)
        _context = context::New(stacksize, CoroutineMain, static_cast<void*>(this));
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
            throw coroutine::ExitException();
        } catch (coroutine::ExitException&) {
            Coroutine *joiner = _joiner;
            switch (reinterpret_cast<uintptr>(joiner)) {
            case 0:         // task, about to joining
                _joiner = reinterpret_cast<Coroutine*>(2);
                coroutine::Yield();
            case 1:         // detached
                break;
            case 2:
                abort();
                break;
            default:
                coroutine::Wakeup(joiner, Result());
                break;
            }
        } catch (...) {
            _exception = std::current_exception();
        }
    }

    // 0, task wait to join; 1, detached task; 2, result is ok; other, joiner task.
    Coroutine *_joiner;
    uintptr _result;
    std::exception_ptr _exception;
    context::Context *_context;
    std::function<void()> _function;

    Coroutine *_link;
    friend ForwardList;
};

void Cleanup() {
    while (Coroutine *co = tZombieQueue.Take()) {
        Coroutine::Delete(co);
    }
}

void ForwardList::Push(Coroutine *co) {
    co->_link = nullptr;
    if (_tail == nullptr) {
        _head = _tail = co;
    } else {
        _tail->_link = co;
        _tail = co;
    }
}

Coroutine* ForwardList::Take() {
    Coroutine *co = _head;
    if (co) {
        _head = co->_link;
        if (_head == nullptr) {
            _tail = nullptr;
        }
    }
    return co;
}

Coroutine* ForwardList::Front() const {
    return _head;
}

bool ForwardList::Empty() const {
    return _head == nullptr;
}

void Die(Coroutine *co) {
    tZombieQueue.Push(co);
    context::Switch(co->Context(), coroutine::ThreadContext());
}

Coroutine *Task(std::function<void()> func, size_t stacksize) {
    if (UNLIKELY(process::Running() == nullptr)) {
        throw std::runtime_error("try to spawn coroutine in no stp process");
    }
    auto co = Coroutine::New(std::move(func), stacksize);
    coroutine::Wakeup(co);
    return co;
}

void Spawn(std::function<void()> func, size_t stacksize) {
    Coroutine *co = Task(std::move(func), stacksize);
    co->Detach();
}

void Sleep(uint64 msecs) {
    timer::Sleep(msecs);
}

void Exit(uintptr result) {
    Coroutine *running = Running();
    assert(running != nullptr);
    running->SetResult(result);
    throw coroutine::ExitException();
}

wild::uintptr Yield() {
    Coroutine *running = Running();
    assert(running != nullptr);
    context::Switch(running->Context(), coroutine::ThreadContext());
    assert(running == Running());
    return running->FetchResult();
}

void Resume(Coroutine *co) {
    co->Resume();
}

uintptr Join(Coroutine *co) {
    Coroutine *running = Running();
    Coroutine *joiner = co->Joiner();
    assert(running != nullptr);
    switch (reinterpret_cast<uintptr>(joiner)) {
    case 0:
        co->SetJoiner(running);
        // After return from Yield(), co is invalid,
        // due to asynchronous wake up.
        return coroutine::Yield();
    case 1:
        throw std::invalid_argument("try to join detached coroutine");
    case 2: {
        SCOPE_EXIT {
            coroutine::Wakeup(co);
        };
        return co->FetchResult();
    }
    default:
        throw std::runtime_error("duplicating joiner");
    }
}

void Mutex::lock() {
    Coroutine *running = Running();
    assert(running != nullptr);
    assert(running != _coroutines.Front());
    _coroutines.Push(running);
    if (_coroutines.Front() != running) {
        coroutine::Yield();
    }
    assert(_coroutines.Front() == running);
}

bool Mutex::try_lock() {
    if (_coroutines.Empty()) {
        Coroutine *running = Running();
        assert(running != nullptr);
        _coroutines.Push(running);
        return true;
    }
    return false;
}

void Mutex::unlock() {
    Coroutine *running_ = Running();
    assert(running_ != nullptr);
    assert(_coroutines.Front() != nullptr);
    assert(_coroutines.Front() == running_);
    (void)running_;
    _coroutines.Take();
    if (Coroutine *co = _coroutines.Front()) {
        coroutine::Wakeup(co);
    }
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

static inline void Schedule(Process *p) {
    process::Ref(p);
    sched::Schedule(p);
}

static void gUnknownHandler(message::Kind, message::Code, process_t, session_t, message::Content const&) {
}

class Process {
public:

    void PushMessage(message::Message *msg) {
        bool wait = _mailbox.Push(msg);
        if (wait) {
            process::Schedule(this);
        }
    }

    message::Message *PopMessage();

    uintptr Suspend(session_t session) {
        assert(coroutine::Running() != nullptr);
        _block_sessions[session] = coroutine::Running();
        return coroutine::Yield();
    }

    void Suspend(Coroutine *co, session_t session) {
        _block_sessions[session] = co;
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

    void Schedule() {
        while (Coroutine *co = _abort_coroutines.Take()) {
            coroutine::Resume(co);
        }

        // No blocked session will be unblocked.
        while (Coroutine *co = _unblock_coroutines.Take()) {
            Message *msg = reinterpret_cast<Message*>(co->Result());
            coroutine::Resume(co);
            message::Delete(msg);
        }

        // running coroutine may:
        //   wakeup suspended coroutine;
        //   block to read maibox.
        while ((!_wakeup_coroutines.Empty()) || (!_inbox.Empty() && !_inbox_coroutines.Empty())) {
            while (Coroutine *co = _wakeup_coroutines.Take()) {
                coroutine::Resume(co);
            }
            while (!_inbox.Empty()) {
                if (Coroutine *co = _inbox_coroutines.Take()) {
                    coroutine::Resume(co);
                }
            }
        }

        coroutine::Cleanup();
    }

    bool Inactive() {
        return _wakeup_coroutines.Empty() && _block_sessions.empty() && _inbox_coroutines.Empty();
    }

    void Wakeup(Coroutine *co, uintptr result) {
        co->SetResult(result);
        _wakeup_coroutines.Push(co);
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
        auto msg = _mailbox.Take();
        if (msg != nullptr) {
            switch (msg->kind) {
            case message::Kind::System:
                switch (msg->code) {
                case message::Code::Kill:
                case message::Code::Error:
                    break;
                case message::Code::Abort:
                    if (Coroutine *co = unblock(msg->session)) {
                        printf("session[%u] aborted\n", msg->session.Value());
                        co->SetResult(static_cast<uintptr>(Error::RuntimeError));
                        _abort_coroutines.Push(co);
                    }
                    break;
                default:
                    break;
                }
                message::Delete(msg);
                break;
            case message::Kind::Response:
                if (Coroutine *co = unblock(msg->session)) {
                    co->SetResult(reinterpret_cast<uintptr>(msg));
                    _unblock_coroutines.Push(co);
                } else {
                    printf("unknown response session[%u]\n", msg->session.Value());
                    message::Delete(msg);
                }
                break;
            case message::Kind::Request:
                ServeRequest(msg->source, msg->session);
            default:
                _inbox.Push(msg);
            }
            // Schedule();
            return ResumeResult::Resume;
        }
        return ResumeResult::Break;
    }

    void Response(process_t source, session_t session, message::Code code, const message::Content& content) {
        if (Process *p = Find(source)) {
            p->PushMessage(message::New(Pid(), session, message::Kind::Response, code, content));
            process::Unref(p);
        }
        DoneRequest(source, session);
    }

    std::unordered_set<std::tuple<process_t, session_t>> _requestsMap;

    Error WaitResponse(session_t session, message::Content *resultp) {
        uintptr result = Suspend(session);
        Error error = static_cast<decltype(error.Value)>(result);
        switch (error.Value) {
        case Error::None:
            return Error::RuntimeError;
        case Error::RuntimeError:
        case Error::RemoteNotExist:
        case Error::UnknownMessage:
            return error;
        default:
            break;
        }

        if (resultp) {
            auto response = reinterpret_cast<Message*>(result);
            *resultp = std::move(response->content);
        }
        return Error::None;
    }

    Session NewSession() {
        uint32 id = _sessions.NewId();
        return Session(this, session_t(id));
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
        while (_inbox.Empty()) {
            _inbox_coroutines.Push(co);
            coroutine::Yield();
        }
        return _inbox.Take();
    }

    void Run() {
        for (;;) {
            Message *msg = nextMessage();
            SCOPE_EXIT {
                message::Delete(msg);
            };
            MessageHandler handler = selectHandler(msg->kind, msg->code);
            if (handler) {
                handler(msg->source, msg->session, msg->content);
            } else if (_defaultHandler) {
                _defaultHandler(msg->kind, msg->code, msg->source, msg->session, msg->content);
            } else {
                gUnknownHandler(msg->kind, msg->code, msg->source, msg->session, msg->content);
            }
        }
    }

    void HandleMessage(message::Kind kind, message::Code code, MessageHandler handler) {
        uint64 index = static_cast<uint64>(kind) | (static_cast<uint64>(code) << 32);
        _handlers[index] = std::move(handler);
    }

    void DefaultMessage(DefaultHandler handler) {
        _defaultHandler = handler;
    }

    static Process *New(std::function<void()> func, size_t stacksize) {
        auto p = new Process();
        p->_pid = Register(p);
        process::NestedScope enter(p);
        coroutine::Spawn(std::move(func), stacksize);
        return p;
    }

    void ServeRequest(process_t source, session_t session) {
        _requestsMap.insert(std::make_tuple(source, session));
    }

    void DoneRequest(process_t source, session_t session) {
        _requestsMap.erase(std::make_tuple(source, session));
    }

    void AbortRequest(process_t source, session_t session) {
        process::System(source, session, message::Code::Abort);
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
        while (Message *msg = _inbox.Take()) {
            if (msg->kind == message::Kind::Request) {
                AbortRequest(msg->source, msg->session);
            }
            message::Delete(msg);
        }
        while (Message *msg = _mailbox.Take()) {
            if (msg->kind == message::Kind::Request) {
                AbortRequest(msg->source, msg->session);
            }
            message::Delete(msg);
        }
    }

    MessageHandler selectHandler(message::Kind kind, message::Code code) {
        uint64 index = static_cast<uint64>(kind) | (static_cast<uint64>(code) << 32);
        auto it = _handlers.find(index);
        if (it == _handlers.end()) {
            return nullptr;
        }
        return it->second;
    }

    DefaultHandler _defaultHandler;
    std::unordered_map<uint64, MessageHandler> _handlers;

    process_t _pid;

    std::atomic<intptr> _refcnt;

    message::Mailbox _mailbox;
    message::ForwardList _inbox;

    wild::IdAllocator<uint32> _sessions;

    coroutine::ForwardList _inbox_coroutines;
    coroutine::ForwardList _wakeup_coroutines;
    coroutine::ForwardList _abort_coroutines;
    coroutine::ForwardList _unblock_coroutines;
    std::unordered_map<session_t, Coroutine *> _block_sessions;

    friend void Ref(Process *p);
    friend void Unref(Process *p);
    friend process::ForwardList;
    Process *_link;
};

void HandleMessage(message::Kind kind, message::Code code, MessageHandler handler) {
    process::Running()->HandleMessage(kind, code, std::move(handler));
}

void DefaultMessage(DefaultHandler handler) {
    process::Running()->DefaultMessage(handler);
}

void Run() {
    process::Running()->Run();
}

void ForwardList::Push(Process *p) {
    p->_link = nullptr;
    if (_tail == nullptr) {
        _head = _tail = p;
    } else {
        _tail->_link = p;
        _tail = p;
    }
}

Process* ForwardList::Take() {
    Process *head = _head;
    if (head) {
        _head = head->_link;
        if (_head == nullptr) {
            _tail = nullptr;
        }
    }
    return head;
}

Process* ForwardList::Front() const {
    return _head;
}

bool ForwardList::Empty() const {
    return _head == nullptr;
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
        sched::Schedule(p);
        break;
    case Process::ResumeResult::Down:
        process::Retire(p);
    case Process::ResumeResult::Break:
        process::Unref(p);
        break;
    }
}

process_t
Spawn(std::function<void()> func, size_t stacksize) {
    auto p = Process::New(std::move(func), stacksize);
    process_t pid = p->Pid();
    process::Schedule(p);
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
        p->PushMessage(message::New(self, session_t(), message::Kind::System, message::Code::Kill, message::Content()));
        process::Unref(p);
    }
}

uintptr Suspend(session_t session) {
    return Running()->Suspend(session);
}

Session NewSession() {
    Process *running = process::Running();
    return running->NewSession();
}

void ReleaseSession(session_t session) {
    Running()->ReleaseSession(session);
}

void
System(process_t pid, session_t session, message::Code code, const message::Content& content) {
    if (Process *p = Find(pid)) {
        p->PushMessage(message::New(Pid(), session, message::Kind::System, code, content));
        process::Unref(p);
    }
}

void Notify(process_t pid, message::Code code, const message::Content& content) {
    if (Process *p = Find(pid)) {
        p->PushMessage(message::New(Pid(), session_t(0), message::Kind::Notify, code, content));
        process::Unref(p);
    }
}

Error
Request(process_t pid, message::Code code, const message::Content& content, message::Content *resultp) {
    Process *running = Running();
    if (UNLIKELY(!running)) {
        throw std::runtime_error("not in process");
    }
    if (Process *p = Find(pid)) {
        Session session = running->NewSession();
        p->PushMessage(message::New(running->Pid(), session.Value(), message::Kind::Request, code, content));
        process::Unref(p);
        return running->WaitResponse(session.Value(), resultp);
    } else {
        return Error::LocalNotExist;
    }
}

void Response(process_t source, session_t session, message::Code code, const message::Content& content) {
    process_t self;
    if (Process *running = process::Running()) {
        self = running->Pid();
        running->DoneRequest(source, session);
    }
    if (Process *p = Find(source)) {
        p->PushMessage(message::New(self, session, message::Kind::Response, code, content));
        process::Unref(p);
    }
}

void Response(process_t pid, session_t session, const message::Content& content) {
    Response(pid, session, message::Code::None, content);
}

void Send(process_t pid, session_t session, message::Kind kind, message::Code code, const message::Content& content) {
    switch (kind) {
    case message::Kind::System:
        System(pid, session, code, content);
        break;
    case message::Kind::Notify:
        Notify(pid, code, content);
        break;
    case message::Kind::Request:
        if (Process *p = Find(pid)) {
            p->PushMessage(message::New(Pid(), session, message::Kind::Request, code, content));
            process::Unref(p);
        }
        break;
    default:
        break;
    }
}

void Yield() {
    auto running = process::Running();
    assert(running != nullptr);
    Session session = running->NewSession();
    running->PushMessage(message::New(process_t(0), session.Value(), message::Kind::Response, message::Code::None, message::Content{}));
    running->Suspend(session.Value());
}

process_t Session::Pid() const {
    return _process->Pid();
}

void Session::close() {
    _process->ReleaseSession(_session);
}

void Mutex::lock() {
    Process *p = Running();
    Coroutine *co = coroutine::Running();
    assert(p != nullptr);
    assert(co != nullptr);
    process_t pid = p->Pid();
    Session session = p->NewSession();
    // XXX In worst case, this coroutine is subject to starvation.
    for (;;) {
        WITH_LOCK(_mutex) {
            if (_owner == nullptr) {
                _owner = co;
                return;
            }
            _blocks.push(std::make_tuple(pid, session.Value()));
        }
        // spurious wakeup is possible, so loop it.
        //
        // Ok to reuse old session after wakeup.
        p->Suspend(session.Value());
    }
}

bool Mutex::try_lock() {
    Coroutine *co = coroutine::Running();
    assert(co != nullptr);
    WITH_LOCK(_mutex) {
        if (_owner == nullptr) {
            _owner = co;
            return true;
        }
    }
    return false;
}

void Mutex::unlock() {
    Coroutine *co = coroutine::Running();
    assert(co != nullptr);
    process_t pid; session_t session;
    WITH_LOCK(_mutex) {
        assert(_owner == co);
        _owner = nullptr;
        if (!_blocks.empty()) {
            std::tie(pid, session) = _blocks.front();
            _blocks.pop();
        }
    }
    if (pid) {
        Response(pid, session, message::Content());
    }
}

void Condition::wait(Mutex& locker) {
    Session session = process::NewSession();
    WITH_LOCK(_mutex) {
        locker.unlock();
        _blocks.push_back(std::make_tuple(session.Pid(), session.Value()));
    }
    SCOPE_EXIT {
        locker.lock();
    };
    process::Suspend(session.Value());
}

void Condition::notify_one() {
    process_t pid; session_t session;
    WITH_LOCK(_mutex) {
        if (_blocks.empty()) {
            return;
        }
        std::tie(pid, session) = _blocks.front();
        _blocks.pop_front();
    }
    if (pid) {
        Response(pid, session);
    }
}

void Condition::notify_all() {
    std::deque<std::tuple<process_t, session_t>> blocks;
    WITH_LOCK(_mutex) {
        if (_blocks.empty()) {
            return;
        }
        std::swap(blocks, _blocks);
    }
    for (std::tuple<process_t, session_t> session : blocks) {
        Response(std::get<process_t>(session), std::get<session_t>(session));
    }
}

}

namespace coroutine {

void
Wakeup(Coroutine *co, uintptr result) {
    auto p = process::Running();
    assert(p != nullptr);
    p->Wakeup(co, result);
}

void Timeout(uint64 msecs, std::function<void()> func, size_t stacksize) {
    auto co = Coroutine::New(std::move(func), stacksize);
    Process *running = process::Running();
    process::Session session = running->NewSession();
    timer::Timeout(session.Value(), msecs);
    running->Suspend(co, session.Value());
}

}

}

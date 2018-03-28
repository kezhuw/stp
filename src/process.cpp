#include "process.hpp"
#include "module.hpp"
#include "context.hpp"
#include "time.hpp"

#include "wild/AppendList.hpp"
#include "wild/BlockingQueue.hpp"
#include "wild/exception.hpp"
#include "wild/FreeList.hpp"
#include "wild/module.hpp"
#include "wild/types.hpp"
#include "wild/likely.hpp"
#include "wild/with_lock.hpp"
#include "wild/ScopeGuard.hpp"
#include "wild/Signaler.hpp"
#include "wild/IdAllocator.hpp"
#include "wild/utility.hpp"

#include <ucontext.h>
#include <sys/mman.h>

#include <cassert>
#include <cstddef>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <deque>
#include <vector>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <unistd.h>

namespace stp {

namespace fd {
void stop();
}

namespace time {
void stop();
}

namespace event {
void load();
}

namespace process {
class Process;
}

namespace sched {
void resume(process::Process *p);
}

}

namespace stp {
namespace process {

struct Message {
    process_t source;
    session_t session;
    wild::SharedAny content;
    struct Message *next;
};

// one reader, multiple writer, nonblocking.
// After nullptr returned from take(), reader
// should not call take() again util notified
// by writer.
class Mailbox {
public:

    Mailbox() noexcept
        : _wait(false) {}

    bool push(Message *msg) noexcept {
        WITH_LOCK(_mutex) {
            _write.push(msg);
        }
        bool expected = true;
        return _wait.compare_exchange_strong(expected, false, std::memory_order_acq_rel);
    }

    Message *take() noexcept {
        if (_read == nullptr) {
            WITH_LOCK(_mutex) {
                if ((_read = _write.clear()) == nullptr) {
                    _wait.store(true, std::memory_order_relaxed);
                    return nullptr;
                }
            }
        }
        auto msg = _read;
        _read = msg->next;
        return msg;
    }

private:

    std::atomic<bool> _wait;
    wild::SpinLock _mutex;
    Message *_read = nullptr;
    wild::AppendList<Message, &Message::next> _write;
};

namespace message {

wild::SpinLock gMutex;
wild::FreeList<Message> gFreeList;

thread_local wild::FreeList<Message> tFreeList;

Message *
allocMessage() {
    auto m = tFreeList.take();
    if (m == nullptr) {
        WITH_LOCK(gMutex) {
            m = gFreeList.take();
        }
        if (m == nullptr) {
            m = static_cast<Message*>(malloc(sizeof(Message)));
        }
    }
    return m;
}

void
deallocMessage(Message *m) {
    if (tFreeList.size() >= 10000) {
        WITH_LOCK(gMutex) {
            gFreeList.push(m);
        }
        return;
    }
    tFreeList.push(m);
}

Message *create(process_t source, session_t session, wild::SharedAny content) {
    auto m = allocMessage();
    return new (m) Message{source, session, content, nullptr};
}

void destroy(Message *msg) {
    msg->~Message();
    deallocMessage(msg);
}

}

struct KillProcess {};
struct AbortedRequest {};
struct ProcessNotExist {};

enum class MessageType {
    kNotify     = 0,
    kRequest    = 1,
    kResponse   = 2,
};

session_t sessionForResponse(session_t session) {
    return session_t(-session.Value());
}

MessageType typeOfMessage(Message *msg) {
    int32 session = static_cast<int32>(msg->session.Value());
    if (session > 0) {
        return MessageType::kRequest;
    } else if (session < 0) {
        return MessageType::kResponse;
    }
    return MessageType::kNotify;
}

class Process;

}
}

namespace stp {
namespace coroutine {

static thread_local Coroutine *tCoroutine;

context::Context *
thread_context() {
    static thread_local context::Context *tContext = context::create();
    return tContext;
}

Coroutine *
current() {
    return tCoroutine;
}

void
set_running(Coroutine *co) {
    tCoroutine = co;
}

wild::SharedAny suspend();
void wakeup(Coroutine *co);

struct ExitException : public std::exception {
    virtual const char *what() const noexcept final override {
        return "coroutine::ExitException";
    }
};

class Result {
public:

    void set_value(wild::SharedAny value) {
        _value = std::move(value);
    }

    void set_exception(const std::exception_ptr &e) {
        _exception = e;
    }

    wild::SharedAny get_value() {
        if (_exception) {
            std::rethrow_exception(_exception);
        }
        return std::move(_value);
    }

private:
    wild::SharedAny _value;
    std::exception_ptr _exception;
};

class Coroutine {
public:

    context::Context* context() {
        return _context;
    }

    wild::SharedAny get_result() {
        return _result.get_value();
    }

    void set_result(wild::SharedAny value) {
        _result.set_value(std::move(value));
    }

    void set_exception(const std::exception_ptr &e) {
        _result.set_exception(e);
    }

    void set_message_info(process_t sender, session_t session) noexcept {
        _message_sender = sender;
        _message_session = session;
    }

    process_t message_sender() noexcept {
        return _message_sender;
    }

    session_t message_session() noexcept {
        return _message_session;
    }

    // TODO allocate from thread local coroutine pool.
    static Coroutine *create(std::function<void()> func, size_t addstack) {
        return new Coroutine(std::move(func), addstack);
    }

    static void destroy(Coroutine *co) {
        delete co;
    }

    void resume() {
        class Scope {
        public:
            Scope(Coroutine *co) {
                // assert(current() == nullptr);
                coroutine::set_running(co);
            }
            ~Scope() {
                coroutine::set_running(nullptr);
            }
        } enter(this);
        context::transfer(coroutine::thread_context(), _context);
    }

    wild::SharedAny yield() {
        context::transfer(_context, coroutine::thread_context());
        assert(this == coroutine::current());
        return get_result();
    }

private:

    Coroutine(std::function<void()> func, size_t addstack)
        : _context(nullptr)
        , _function(std::move(func)) {
#define CoroutineMain   reinterpret_cast<void(*)(void*)>(&Coroutine::main)
        _context = context::create(CoroutineMain, static_cast<void*>(this), addstack);
    }

    ~Coroutine() {
        context::destroy(_context);
    }

    static void main(Coroutine *co) {
        assert(coroutine::current() == co);
        co->run();
        assert(coroutine::current() == co);
        context::transfer(co->context(), coroutine::thread_context());
    }

    void run() {
        try {
            _function();
            set_exception(std::make_exception_ptr(coroutine::ExitException{}));
        } catch (...) {
            set_exception(std::current_exception());
        }
    }

    Result _result;
    process_t _message_sender;
    session_t _message_session;
    context::Context *_context;
    std::function<void()> _function;
};

}
}

namespace stp {
namespace process {

using Coroutine = coroutine::Coroutine;

static thread_local Process *tProcess;

Process *
current() {
    return tProcess;
}

void
set_running(Process *p) {
    tProcess = p;
}

class Scope {
public:
    Scope(Process *p) {
        assert(current() == nullptr);
        set_running(p);
    }
    ~Scope() {
        set_running(nullptr);
    }
};

struct ExitException : public std::exception {
    virtual const char *what() const noexcept final override {
        return "process::ExitException";
    }
};

struct RequestAborted : public std::exception {
    virtual const char *what() const noexcept final override {
        return "process::RequestAborted";
    }
};

struct KillException : public std::exception {
    KillException(process_t killer) : Killer(killer) {}
    const process_t Killer;
    virtual const char *what() const noexcept final override {
        return "process::KillException";
    }
};

void ref(Process *);
void unref(Process *);

static wild::SpinLock gProcsLocker;
static std::unordered_map<uint32, Process*> gProcsMap;

static process_t reg(Process *p) {
    static uint32 seq = 0;
    uint32 id;
    WITH_LOCK(gProcsLocker) {
        id = ++seq;
        gProcsMap[id] = p;
    }
    return process_t(id);
}

static bool unreg(process_t pid) {
    WITH_LOCK(gProcsLocker) {
        return gProcsMap.erase(pid.Value()) == 1;
    }
    // Never goes here, suppress gcc warning.
    assert(false);
    return false;
}

struct MessageDeleter {
    void operator()(Message *m) const noexcept(noexcept(message::destroy(m))) {
        message::destroy(m);
    }
};

using MessageUniquePtr = std::unique_ptr<Message, MessageDeleter>;

struct ProcessDeleter {
    void operator()(Process *p) const noexcept {
        process::unref(p);
    }
};

using ProcessUniquePtr = std::unique_ptr<Process, ProcessDeleter>;

static ProcessUniquePtr find(process_t pid) {
    WITH_LOCK(gProcsLocker) {
        auto it = gProcsMap.find(pid.Value());
        if (it != gProcsMap.end()) {
            auto p = it->second;
            process::ref(p);
            return ProcessUniquePtr(p);
        }
    }
    return nullptr;
}

class Process {
public:

    void push_message(Message *msg) {
        bool wait = _mailbox.push(msg);
        if (wait) {
            sched::resume(this);
        }
    }

    wild::SharedAny suspend() {
        auto running = coroutine::current();
        _suspend_coroutines.insert(running);
        return running->yield();
    }

    wild::SharedAny block(session_t session) {
        Coroutine *running = coroutine::current();
        _block_sessions[session] = running;
        return running->yield();
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

    void sweep_zombies() {
        for (auto co : _zombie_coroutines) {
            Coroutine::destroy(co);
        }
        _zombie_coroutines.clear();
    }

    void abandon(std::deque<Coroutine*>& coroutines) {
        _zombie_coroutines.insert(_zombie_coroutines.end(), coroutines.begin(), coroutines.end());
        coroutines.clear();
    }

    void interrupt_block_coroutines(std::exception_ptr e) {
        for (auto value : _block_sessions) {
            auto co = value.second;
            co->set_exception(e);
            _wakeup_coroutines.push_back(co);
        }
        _block_sessions.clear();
    }

    void interrupt_suspend_coroutines(std::exception_ptr e) {
        for (auto co : _suspend_coroutines) {
            co->set_exception(e);
            _wakeup_coroutines.push_back(co);
        }
        _suspend_coroutines.clear();
    }

    void resume(Coroutine *co, bool exiting = false) {
        co->resume();
        try {
            co->get_result();
        } catch (const coroutine::ExitException&) {
            _zombie_coroutines.push_back(co);
        } catch (const process::ExitException&) {
            _zombie_coroutines.push_back(co);
            if (!exiting) {
                throw;
            }
        } catch (...) {
            _zombie_coroutines.push_back(co);
            wild::print_exception(std::current_exception());
        }
    }

    void exit() {
        auto exitException = std::make_exception_ptr(coroutine::ExitException{});
        for (;;) {
            interrupt_block_coroutines(exitException);
            interrupt_suspend_coroutines(exitException);
            if (_wakeup_coroutines.empty()) {
                break;
            }
            do {
                resume(wild::take_front(_wakeup_coroutines), true);
            } while (!_wakeup_coroutines.empty());
        }
        abandon(_spawn_coroutines);
        sweep_zombies();
    }

    bool dispatch() {
        try {
            run();
        } catch (const process::ExitException&) {
            exit();
            return false;
        }
        return !(_block_sessions.empty() && _suspend_coroutines.empty());
    }

    void run() {
        // running coroutine may:
        //   spawn new coroutine;
        //   wakeup suspended coroutine;
        //   block to read maibox.
        while (!_spawn_coroutines.empty()
            || !_wakeup_coroutines.empty()
            || (!_inbox.empty() && _inbox_coroutine != nullptr)) {
            while (!_spawn_coroutines.empty()) {
                resume(wild::take_front(_spawn_coroutines));
            }
            while (!_wakeup_coroutines.empty()) {
                resume(wild::take_front(_wakeup_coroutines));
            }
            if (!_inbox.empty() && _inbox_coroutine != nullptr) {
                wakeup(_inbox_coroutine);
                _inbox_coroutine = nullptr;
            }
        }
        sweep_zombies();
    }

    void wakeup(Coroutine *co) {
        if (_suspend_coroutines.erase(co) != 0) {
            _wakeup_coroutines.push_back(co);
        }
    }

    enum class ResumeResult : uintptr {
        kResume         = 0,
        kDown           = 1,
        kBreak          = 2,
    };

    ResumeResult resume() {
        process::Scope enter(this);
        if (!dispatch()) {
            return ResumeResult::kDown;
        }
        auto msg = _mailbox.take();
        if (msg != nullptr) {
            MessageType msgType = typeOfMessage(msg);
            switch (msgType) {
            case MessageType::kRequest:
                serve_request(msg->source, msg->session);
                _inbox.push_back(msg);
                break;
            case MessageType::kResponse: {
                uint32 session = -msg->session.Value();
                if (Coroutine *co = unblock(session_t(session))) {
                    if (auto *e = wild::SharedAny::Cast<std::exception_ptr*>(&msg->content)) {
                        co->set_exception(*e);
                    } else {
                        co->set_result(std::move(msg->content));
                    }
                    _wakeup_coroutines.push_back(co);
                } else {
                    printf("unknown response session[%u]\n", session);
                }
                message::destroy(msg);
                } break;
            case MessageType::kNotify:
                if (msg->content.type() == typeid(KillProcess)) {
                    message::destroy(msg);
                    exit();
                    return ResumeResult::kDown;
                }
                _inbox.push_back(msg);
                break;
            }
            return ResumeResult::kResume;
        }
        return ResumeResult::kBreak;
    }

    void yield() {
        Session session = new_session();
        push_message(message::create(process_t(0), sessionForResponse(session.Value()), wild::SharedAny{}));
        block(session.Value());
    }

    void response(process_t source, session_t session, wild::SharedAny content) {
        if (auto p = find(source)) {
            p->push_message(message::create(pid(), sessionForResponse(session), std::move(content)));
        }
        close_request(source, session);
    }

    std::unordered_set<std::tuple<process_t, session_t>> _requestsMap;

    Session new_session() {
        uint32 id = _sessions.NewId();
        return Session(reinterpret_cast<uintptr>(this), session_t(id));
    }

    void release_session(session_t session) {
        _sessions.Erase(session.Value());
    }

    process_t pid() const {
        return _pid;
    }

    Coroutine* spawn(std::function<void()> func, size_t addstack) {
        auto co = Coroutine::create(std::move(func), addstack);
        _spawn_coroutines.push_back(co);
        return co;
    }

    static Process *create(std::function<void()> func, size_t addstack) {
        auto p = new Process();
        p->_pid = reg(p);
        p->spawn([func = std::move(func)] { event::load(); func(); }, addstack);
        return p;
    }

    void serve_request(process_t source, session_t session) {
        _requestsMap.insert(std::make_tuple(source, session));
    }

    void close_request(process_t source, session_t session) {
        _requestsMap.erase(std::make_tuple(source, session));
    }

    void abort_request(process_t source, session_t session) {
        process::send(source, sessionForResponse(session), std::make_exception_ptr(AbortedRequest{}));
    }

    enum class HandlerMode {
        kCallback           = 1,
        kCoroutine          = 2,
    };

    struct HandlerInfo {
        HandlerMode mode;
        std::function<void(wild::SharedAny&& content)> func;
        size_t addstack;
    };

    void message_callback(const std::type_info& type, std::function<void(wild::SharedAny&& content)> func) {
        auto& handler = _message_handlers[std::type_index(type)];
        handler.mode = HandlerMode::kCallback;
        handler.func = std::move(func);
        handler.addstack = 0;
    }

    void message_coroutine(const std::type_info& type, std::function<void(wild::SharedAny&& content)> func, size_t addstack) {
        auto& handler = _message_handlers[std::type_index(type)];
        handler.mode = HandlerMode::kCoroutine;
        handler.func = std::move(func);
        handler.addstack = addstack;
    }

    void serve() {
        auto running = coroutine::current();
        for (;;) {
            if (_inbox.empty()) {
                _inbox_coroutine = running;
                coroutine::suspend();
            }
            MessageUniquePtr msg(wild::take_front(_inbox));
            auto it = _message_handlers.find(std::type_index(msg->content.type()));
            if (it == _message_handlers.end()) {
                continue;
            }
            const auto& handler = it->second;
            if (handler.mode == HandlerMode::kCallback) {
                running->set_message_info(msg->source, msg->session);
                handler.func(std::move(msg->content));
            } else {
                coroutine::spawn([func = handler.func, msg = std::move(msg)] {
                    coroutine::current()->set_message_info(msg->source, msg->session);
                    func(std::move(msg->content));
                }, handler.addstack);
            }
        }
    }

    // XXX
    // kill/unref, when to delete ?
    static void destroy(Process *p) {
        delete p;
    }

    wild::SharedAny get(wild::Atom *name) {
        auto it = _env.find(name);
        if (it == _env.end()) {
            return {};
        }
        return it->second;
    }

    wild::SharedAny set(wild::Atom *name, wild::SharedAny newValue) {
        auto& ref = _env[name];
        wild::SharedAny oldValue = std::move(ref);
        ref = std::move(newValue);
        return oldValue;
    }

private:

    Process()
        : _refcnt(1) {
    }

    ~Process() {
        for (auto request : _requestsMap) {
            abort_request(std::get<process_t>(request), std::get<session_t>(request));
        }
        while (!_inbox.empty()) {
            Message *msg = wild::take_front(_inbox);
            if (typeOfMessage(msg) == MessageType::kRequest) {
                abort_request(msg->source, msg->session);
            }
            message::destroy(msg);
        }
        while (Message *msg = _mailbox.take()) {
            if (typeOfMessage(msg) == MessageType::kRequest) {
                abort_request(msg->source, msg->session);
            }
            message::destroy(msg);
        }
    }

    process_t _pid;

    std::atomic<intptr> _refcnt;

    Mailbox _mailbox;
    std::deque<Message*> _inbox;

    wild::IdAllocator<uint32> _sessions;

    Coroutine *_inbox_coroutine = nullptr;
    std::deque<Coroutine*> _spawn_coroutines;
    std::deque<Coroutine*> _wakeup_coroutines;
    std::vector<Coroutine*> _zombie_coroutines;
    std::unordered_set<Coroutine*> _suspend_coroutines;
    std::unordered_map<session_t, Coroutine *> _block_sessions;

    std::unordered_map<wild::Atom*, wild::SharedAny> _env;

    std::unordered_map<std::type_index, HandlerInfo> _message_handlers;

    friend void ref(Process *p);
    friend void unref(Process *p);

public:
    Process *next;
};

process_t sender() {
    return coroutine::current()->message_sender();
}

session_t session() {
    return coroutine::current()->message_session();
}

void request_callback(const std::type_info& type, std::function<void(wild::SharedAny&& content)> handler) {
    process::current()->message_callback(type, std::move(handler));
}

void request_coroutine(const std::type_info& type, std::function<void(wild::SharedAny&& content)> handler, size_t addstack) {
    process::current()->message_coroutine(type, std::move(handler), addstack);
}

void notification_callback(const std::type_info& type, std::function<void(wild::SharedAny&& content)> handler) {
    process::current()->message_callback(type, std::move(handler));
}

void notification_coroutine(const std::type_info& type, std::function<void(wild::SharedAny&& content)> handler, size_t addstack) {
    process::current()->message_coroutine(type, std::move(handler), addstack);
}

void serve() {
    process::current()->serve();
}

wild::SharedAny get(wild::Atom *name) {
    return process::current()->get(name);
}

wild::SharedAny set(wild::Atom *name, wild::SharedAny value) {
    return process::current()->set(name, value);
}

void ref(Process *p) {
    p->_refcnt.fetch_add(1, std::memory_order_relaxed);
}

void unref(Process *p) {
    auto refcnt = p->_refcnt.fetch_sub(1, std::memory_order_relaxed);
    if (refcnt == 1) {
        // FIXME How to retire it ?
        Process::destroy(p);
    }
}

void retire(Process *p) {
    if (unreg(p->pid())) {
        process::unref(p);
    }
}

void resume(Process *p) {
    switch (p->resume()) {
    case Process::ResumeResult::kResume:
        sched::resume(p);
        break;
    case Process::ResumeResult::kDown:
        process::retire(p);
        break;
    default:
        break;
    }
    process::unref(p);
}

process_t
spawn(std::function<void()> func, size_t addstack) {
    auto p = Process::create(std::move(func), addstack);
    process_t pid = p->pid();
    sched::resume(p);
    return pid;
}

process_t
self() {
    Process *p = current();
    if (p) {
        return p->pid();
    }
    return process_t();
}

void exit() {
    Process *running = current();
    if (UNLIKELY(!running)) {
        throw std::runtime_error("call exit() out of process");
    }
    throw process::ExitException();
}

void kill(process_t pid) {
    process_t selfPid = self();
    if (pid == process_t(0)) {
        pid = selfPid;
    }
    if (auto p = find(pid)) {
        p->push_message(message::create(selfPid, session_t(), KillProcess{}));
    }
}

Session new_session() {
    Process *running = process::current();
    return running->new_session();
}

void release_session(session_t session) {
    current()->release_session(session);
}

wild::SharedAny request(process_t pid, wild::SharedAny content) {
    Process *running = current();
    if (UNLIKELY(!running)) {
        throw std::runtime_error("not in process");
    }
    if (auto p = find(pid)) {
        Session session = running->new_session();
        p->push_message(message::create(running->pid(), session.Value(), content));
        return running->block(session.Value());
    }
    throw ProcessNotExist{};
}

void response(process_t source, session_t session, wild::SharedAny content) {
    if (Process *running = process::current()) {
        running->response(source, session, std::move(content));
    } else if (auto p = find(source)) {
        p->push_message(message::create(process_t(), sessionForResponse(session), std::move(content)));
    }
}

void response(wild::SharedAny content) {
    response(sender(), session(), std::move(content));
}

void send(process_t pid, wild::SharedAny content) {
    send(pid, session_t(), std::move(content));
}

void send(process_t pid, session_t session, wild::SharedAny content) {
    if (auto p = find(pid)) {
        p->push_message(message::create(self(), session, std::move(content)));
    }
}

Session::Session(session_t session)
    : Session(reinterpret_cast<uintptr>(process::current()), session) {
}

Session::Session(uintptr process, session_t session)
    : _process(process), _session(session) {
}

process_t Session::Pid() const {
    auto p = reinterpret_cast<Process*>(_process);
    return p->pid();
}

void Session::close() {
    auto p = reinterpret_cast<Process*>(_process);
    p->release_session(_session);
}

}
}

namespace stp {
namespace coroutine {

Coroutine* spawn(std::function<void()> func, size_t addstack) {
    auto p = process::current();
    assert(p != nullptr);
    return p->spawn(std::move(func), addstack);
}

Coroutine* self() {
    return current();
}

void sleep(uint64 msecs) {
    time::sleep(msecs);
}

void timeout(uint64 msecs, std::function<void()> func, size_t addstack) {
    spawn([msecs, func = std::move(func)] {
        sleep(msecs);
        func();
    }, addstack);
}

void yield() {
    auto p = process::current();
    assert(p != nullptr);
    p->yield();
}

void exit() {
    Coroutine *running = current();
    assert(running != nullptr);
    throw coroutine::ExitException();
}

wild::SharedAny suspend() {
    return process::current()->suspend();
}

void wakeup(Coroutine *co) {
    process::current()->wakeup(co);
}

wild::SharedAny block(session_t session) {
    return process::current()->block(session);
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
    auto running = coroutine::current();
    assert(running != nullptr);
    if (_coroutines.empty()) {
        _coroutines.push_back(running);
    } else {
        assert(_coroutines.front() != running);
        _coroutines.push_back(running);
        try {
            coroutine::suspend();
            assert(!_coroutines.empty());
            assert(_coroutines.front() == running);
        } catch (...) {
            _coroutines.erase(std::remove(_coroutines.begin(), _coroutines.end(), running));
            throw;
        }
    }
}

bool Mutex::try_lock() {
    if (_coroutines.empty()) {
        auto running = coroutine::current();
        assert(running != nullptr);
        _coroutines.push_back(running);
        return true;
    }
    return false;
}

void Mutex::unlock() {
    auto running_ = coroutine::current();
    assert(running_ != nullptr);
    assert(!_coroutines.empty());
    assert(_coroutines.front() == running_);
    _coroutines.pop_front();
    if (!_coroutines.empty()) {
        auto pending = reinterpret_cast<Coroutine*>(_coroutines.front());
        coroutine::wakeup(pending);
    }
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

}
}

namespace stp {
namespace sched {

static class Scheduler {
public:

    void stop(int code) {
        if (_stopFired.test_and_set(std::memory_order_relaxed)) {
            return;
        }
        fd::stop();
        time::stop();
        _exitcode = code;
        _stop.store(true, std::memory_order_seq_cst);
        for (size_t i=0, n=_threads.size(); i<n; ++i) {
            _signaler.send();
        }
    }

    int serve() {
        auto n = std::max(std::thread::hardware_concurrency(), 2u);
        for (unsigned i=0; i<n; ++i) {
            _threads.emplace_back(&Scheduler::run, this);
        }
        for (auto& td : _threads) {
            td.join();
        }
        return _exitcode;
    }

    void resume(process::Process *p) {
        process::ref(p);
        bool waiting = false;
        WITH_LOCK(_resumeq_lock) {
            _resumeq.push(p);
            if (_idle_workers != 0) {
                waiting = true;
                --_idle_workers;
            }
        }
        if (waiting) {
            _signaler.send();
        }
    }

private:

    void run() {
        while (!_stop.load(std::memory_order_relaxed)) {
            process::Process *p;
            WITH_LOCK(_runq_lock) {
                if (_runq == nullptr) {
                    WITH_LOCK(_resumeq_lock) {
                        _runq = _resumeq.clear();
                        if (_runq == nullptr) {
                            ++_idle_workers;
                        }
                    }
                    if (_runq == nullptr) {
                        _signaler.wait(_runq_lock);
                        continue;
                    }
                }
                p = _runq;
                _runq = p->next;
            }
            process::resume(p);
        }
    }

    int _exitcode;
    std::atomic_flag _stopFired = ATOMIC_FLAG_INIT;
    std::atomic<bool> _stop;
    std::vector<std::thread> _threads;
    wild::SpinLock _runq_lock;
    process::Process *_runq;
    wild::SpinLock _resumeq_lock;
    wild::AppendList<process::Process, &process::Process::next> _resumeq;
    int _idle_workers;
    wild::Signaler _signaler;
} scheduler;

int serve() {
    return scheduler.serve();
}

void stop(int code) {
    scheduler.stop(code);
}

void resume(process::Process *p) {
    scheduler.resume(p);
}

}
}

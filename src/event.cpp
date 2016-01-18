#include "event.hpp"
#include "process.hpp"

#include "wild/atom.hpp"

#include <exception>
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace stp {
namespace event {

struct Module {
    static wild::Atom* Name;
    std::unordered_map<wild::Atom*, Responser> Responsers;
    std::unordered_map<wild::Atom*, std::unordered_map<wild::Atom*, Subscriber>> Subscribers;
};

wild::Atom* Module::Name = wild::atom::from("stp.event");

void load() {
    process::set(Module::Name, Module{});
}

static Module *getModule() {
    return process::get<Module*>(Module::Name);
}

wild::SharedAny request(wild::Atom *name, wild::SharedAny parameters) {
    auto m = getModule();
    auto it = m->Responsers.find(name);
    if (it == m->Responsers.end()) {
        throw std::runtime_error(string("no responser: ") + name->c_str());
    }
    const auto& f = it->second;
    return f(std::move(parameters));
}

void response(wild::Atom *name, Responser responser) {
    auto m = getModule();
    m->Responsers[name] = std::move(responser);
}

void publish(wild::Atom *name, wild::SharedAny parameters) {
    auto m = getModule();
    auto it = m->Subscribers.find(name);
    if (it == m->Subscribers.end()) {
        return;
    }
    for (const auto& value : it->second) {
        const auto& f = value.second;
        f(parameters);
    }
}

void subscribe(wild::Atom *name, wild::Atom *ident, Subscriber f) {
    auto m = getModule();
    m->Subscribers[name][ident] = std::move(f);
}

void unsubscribe(wild::Atom *name, wild::Atom *ident) {
    auto m = getModule();
    m->Subscribers[name].erase(ident);
}

}
}

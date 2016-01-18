#pragma once

#include "wild/atom.hpp"
#include "wild/SharedAny.hpp"

#include <functional>

namespace stp {
namespace event {

using Responser = std::function<wild::SharedAny(wild::SharedAny)>;
using Subscriber = std::function<void(wild::SharedAny)>;

wild::SharedAny request(wild::Atom *name, wild::SharedAny parameters);
void response(wild::Atom *name, Responser responser);

void publish(wild::Atom *name, wild::SharedAny parameters);
void subscribe(wild::Atom *name, wild::Atom *ident, Subscriber f);
void unsubscribe(wild::Atom *name, wild::Atom *ident);

}
}

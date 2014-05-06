#pragma once

#include "types.hpp"

namespace stp {
namespace context {

class Context;

Context* New();

Context* New(size_t stacksize, void (*func)(void *), void *arg);

void Switch(Context *current, Context *to);

void Delete(Context *ctx);

}
}

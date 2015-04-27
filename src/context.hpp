#pragma once

#include "types.hpp"

namespace stp {
namespace context {

class Context;

Context* create();

Context* create(void (*func)(void *), void *arg, size_t stacksize = 0);

void transfer(Context *current, Context *to);

void destroy(Context *ctx);

}
}

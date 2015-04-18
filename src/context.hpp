#pragma once

#include "types.hpp"

namespace stp {
namespace context {

class Context;

Context* New();

Context* New(void (*func)(void *), void *arg, size_t stacksize = 0);

void Switch(Context *current, Context *to);

void Delete(Context *ctx);

}
}

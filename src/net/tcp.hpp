#pragma once

#include "types.hpp"

#include <wild/fd.hpp>

#include <tuple>
#include <string>
#include <system_error>

namespace stp {
namespace net {
namespace tcp {

std::tuple<wild::Fd, std::error_condition> Listen(const char *addr);
std::tuple<wild::Fd, std::error_condition> Accept(const wild::Fd& fd, string *from);

std::tuple<wild::Fd, std::error_condition> Connect(const char *addr);

}
}
}

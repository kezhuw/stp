#pragma once

#include "types.hpp"

#include "wild/Fd.hpp"

#include <tuple>
#include <string>
#include <system_error>

namespace stp {
namespace net {
namespace tcp {

std::tuple<wild::Fd, std::error_condition> listen(const char *addr);
std::tuple<wild::Fd, std::error_condition> accept(const wild::Fd& fd, string *from);

std::tuple<wild::Fd, std::error_condition> connect(const char *addr);

}
}
}

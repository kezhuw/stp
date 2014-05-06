#pragma once

namespace stp {
namespace module {

constexpr int STP = -1;

namespace Order {

enum : int {
    Sched           = -5,
    Timer           = -4,
    Fdpoll          = -3,
};

};

}
}

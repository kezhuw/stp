#include "sched.hpp"
#include "exit.hpp"

namespace stp {

void
exit(int status) {
    stp::sched::stop(status);
}

}

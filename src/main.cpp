#include "process.hpp"
#include "sched.hpp"

#include "wild/module.hpp"
#include "wild/ScopeGuard.hpp"

#include <dlfcn.h>

#include <stdio.h>
#include <stdlib.h>

extern "C" typedef void (*main_func_t)(int argc, const char *args[]);

namespace stp {

int
main(int argc, const char *args[]) {
    wild::module::Init();

    auto entry = main_func_t(dlsym(nullptr, "stp_main"));
    if (entry == nullptr) {
        printf("main function \"stp_main\" not found!");
        exit(-1);
    }

    stp::process::spawn([entry, argc, args] { SCOPE_EXIT { sched::stop(0); }; entry(argc, args); });
    return stp::sched::serve();
}

}

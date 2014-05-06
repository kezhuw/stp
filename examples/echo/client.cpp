#include "stp.hpp"
#include "timer.hpp"
#include "net.hpp"
#include "io.hpp"

#include <wild/module.hpp>
#include <wild/string.hpp>

#include <cstdio>
#include <cstdlib>

#include <unistd.h>

using namespace stp;

int
main() {
    wild::module::Init();

    auto client = [] (string namestr) {
        const char *name = namestr.c_str();
        printf("%s: start\n", name);
        wild::Fd server;
        std::error_condition error;
        std::tie(server, error) = net::tcp::Connect("tcp4://*:3000");
        printf("%s: connected\n", name);
        if (error) {
            printf("%s: fail to connect: %s\n", name, error.message().c_str());
            process::Exit();
        }
        byte_t data[] = u8"0123456789abcdefghijklmnopqrstuvwxyz";
        for (int i=0; i<50000; ++i) {
            int err;
            std::tie(std::ignore, err) = io::Write(server.RawFd(), data, sizeof data);
            if (err) {
                printf("%s: io::Write(): %s\n", name, wild::os::strerror(err));
                break;
            }
            std::tie(std::ignore, err) = io::ReadFull(server.RawFd(), data, sizeof data);
            if (err) {
                printf("%s: io::ReadFull(): %s\n", name, wild::os::strerror(err));
                break;
            }
            coroutine::Sleep(2000);
        }
    };

    process::Spawn([&client] {
        char name[1024];
        for (int i=1; i<10000; ++i) {
            snprintf(name, sizeof name, "clinet %d\n", i);
            process::Spawn([&client, name = string(name)] {
                client(std::move(name));
            });
        }
    });

    for (;;) {
        timer::UpdateTime();
        usleep(1000);
    }
}

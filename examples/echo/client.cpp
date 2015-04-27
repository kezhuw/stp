#include "stp.hpp"
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
        fprintf(stderr, "%s: start\n", name);
        wild::Fd server;
        std::error_condition error;
        std::tie(server, error) = net::tcp::connect("tcp4://*:3000");
        fprintf(stderr, "%s: connected\n", name);
        if (error) {
            printf("%s: fail to connect: %s\n", name, error.message().c_str());
            process::exit();
        }
        byte_t data[] = u8"0123456789abcdefghijklmnopqrstuvwxyz";
        for (int i=0; i<50000; ++i) {
            int err;
            std::tie(std::ignore, err) = io::write(server.RawFd(), data, sizeof data);
            if (err) {
                printf("%s: io::write(): %s\n", name, wild::os::strerror(err));
                break;
            }
            std::tie(std::ignore, err) = io::read_full(server.RawFd(), data, sizeof data);
            if (err) {
                printf("%s: io::read_full(): %s\n", name, wild::os::strerror(err));
                break;
            }
            coroutine::sleep(500);
        }
    };

    process::spawn([&client] {
        char name[1024];
        for (int i=1; i<=10000; ++i) {
            snprintf(name, sizeof name, "client %d", i);
            process::spawn([&client, name = string(name)] {
                client(std::move(name));
            });
        }
    });

    for (;;) {
        usleep(1000);
    }
}

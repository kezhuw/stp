#include "stp.hpp"
#include "net.hpp"
#include "io.hpp"

#include <wild/module.hpp>
#include <wild/string.hpp>

#include <algorithm>
#include <atomic>
#include <random>

#include <cstdio>
#include <cstdlib>

#include <unistd.h>

using namespace stp;

int
main() {
    wild::module::Init();
    std::atomic<size_t> seqn(0);

    auto client = [&seqn] (string namestr) {
        const char *name = namestr.c_str();
        wild::Fd server;
        std::error_condition error;
        std::tie(server, error) = net::tcp::connect("tcp4://*:3000");
        if (error) {
            printf("%s: fail to connect: %s\n", name, error.message().c_str());
            process::exit();
        }
        printf("%s: connected.\n", name);
        byte_t data[] = u8"0123456789abcdefghijklmnopqrstuvwxyz";
        std::random_device rd;
        std::mt19937 g(rd());
        for (;;) {
            int err;
            size_t seqi = seqn.fetch_add(1, std::memory_order_relaxed);
            std::shuffle(std::begin(data), std::prev(std::end(data)), g);
            std::tie(std::ignore, err) = io::write(server.RawFd(), data, sizeof data);
            if (err) {
                printf("%s seq %zu: io::write(): %s\n", name, seqi, wild::os::strerror(err));
                break;
            }
            std::array<byte_t, sizeof data> buf;
            std::tie(std::ignore, err) = io::read_full(server.RawFd(), buf.data(), buf.size());
            if (err) {
                printf("%s seq %zu: io::read_full(): %s\n", name, seqi, wild::os::strerror(err));
                break;
            }
            printf("%s seq %zu: ==> %s.\n"
                   "%s seq %zu: <== %s.\n",
                   name, seqi, reinterpret_cast<char*>(data),
                   name, seqi, reinterpret_cast<char*>(buf.data()));
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

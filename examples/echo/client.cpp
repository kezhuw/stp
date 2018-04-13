#include "stp.hpp"

#include "wild/string.hpp"
#include "wild/ScopeGuard.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <random>

#include <cstdio>
#include <cstdlib>
#include <inttypes.h>

#include <unistd.h>

struct SeqRequest {
};

struct SeqResponse {
    stp::uint64 seq;
};

static void seqService() {
    stp::uint64 seqn = 0;
    stp::process::request_callback<SeqRequest>([&seqn](SeqRequest *) {
        SeqResponse response;
        response.seq = ++seqn;
        stp::process::response(response);
    });
    stp::process::serve();
}

static stp::uint64 getSeq(stp::process_t seq) {
    auto any = stp::process::request(seq, SeqRequest{});
    auto response = wild::SharedAny::Cast<SeqResponse*>(&any);
    return response->seq;
}

struct ClientExitMessage {
    stp::uint64 id;
};

static void client(stp::uint64 id, stp::process_t super, stp::process_t seq) {
    SCOPE_EXIT {
        stp::process::send(super, ClientExitMessage{id});
    };
    wild::Fd server;
    std::error_condition error;
    std::tie(server, error) = stp::net::tcp::connect("tcp4://*:3000");
    if (error) {
        printf("client %" PRIu64 ": fail to connect: %s\n", id, error.message().c_str());
        stp::process::exit();
    }
    printf("client %" PRIu64 ": connected.\n", id);
    stp::byte_t data[] = u8"0123456789abcdefghijklmnopqrstuvwxyz";
    std::random_device rd;
    std::mt19937 g(rd());
    for (;;) {
        int err;
        auto seqi = getSeq(seq);
        std::shuffle(std::begin(data), std::prev(std::end(data)), g);
        std::tie(std::ignore, err) = stp::io::write(server.RawFd(), data, sizeof data);
        if (err) {
            printf("client %" PRIu64 " seq %" PRIu64 ": stp::io::write(): %s\n", id, seqi, wild::os::strerror(err));
            break;
        }
        std::array<stp::byte_t, sizeof data> buf;
        std::tie(std::ignore, err) = stp::io::read_full(server.RawFd(), buf.data(), buf.size());
        if (err) {
            printf("client %" PRIu64 " seq %" PRIu64 ": stp::io::read_full(): %s\n", id, seqi, wild::os::strerror(err));
            break;
        }
        printf("client %" PRIu64 " seq %" PRIu64 ": ==> %s.\n"
               "client %" PRIu64 " seq %" PRIu64 ": <== %s.\n",
               id, seqi, reinterpret_cast<char*>(data),
               id, seqi, reinterpret_cast<char*>(buf.data()));
    }
}

void
entry() {
    auto self = stp::process::self();
    auto seq = stp::process::spawn(seqService);

    int n = 10000;
    for (int i=1; i<=n; ++i) {
        stp::process::spawn([id = stp::uint64(i), super = self, seq] {
            client(id, super, seq);
        });
    }
    stp::process::notification_callback<ClientExitMessage>([&n](ClientExitMessage *msg) {
        printf("client %" PRIu64 " exited\n", msg->id);
        if (--n <= 0) {
            printf("all clients done, exit.\n");
            stp::os::exit(0);
        }
    });
    stp::process::serve();
}

int
main() {
    return stp::main(entry);
}

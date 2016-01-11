cc_binary(
    name = "client",
    srcs = ["client.cpp"],
    deps = ["//:stp", "//external:wild"],
    copts = [
        "-Ideps",
        "-std=c++1y",
    ],
    linkopts = [
        "-export-dynamic",
    ],
)

cc_binary(
    name = "server",
    srcs = ["server.cpp"],
    deps = ["//:stp", "//external:wild"],
    copts = [
        "-Ideps",
        "-std=c++1y",
    ],
    linkopts = [
        "-export-dynamic",
    ],
)

COPTS = [
    "-Ideps",
    "-std=c++1y",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wconversion",
    "-Wshadow",
]

cc_binary(
    name = "client",
    srcs = ["client.cpp"],
    deps = ["//:stp", "//external:wild"],
    copts = COPTS,
    linkopts = [
        "-export-dynamic",
    ],
)

cc_binary(
    name = "server",
    srcs = ["server.cpp"],
    deps = ["//:stp", "//external:wild"],
    copts = COPTS,
    linkopts = [
        "-export-dynamic",
    ],
)

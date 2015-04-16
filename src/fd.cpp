#if defined(__linux__)
#include "fd_epoll.cpp"
#elif defined(__FreeBSD__) || defined(__APPLE__)
#include "fd_kqueue.cpp"
#endif

// 验证 GCC 11 的 C++20 协程基础能力
#include <coroutine>
#include <cstdio>
#include <exception>
struct Task {
  struct promise_type {
    Task get_return_object() { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() { std::terminate(); }
  };
};
Task co_hello() { printf("  coroutine body running\n"); co_return; }
int main(){ printf("C++20 coroutines: "); co_hello(); printf("  OK\n"); }

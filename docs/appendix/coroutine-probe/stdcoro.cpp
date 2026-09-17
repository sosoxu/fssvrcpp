#include <coroutine>
#include <version>
#include <cstdio>
int main(){
#ifdef __cpp_impl_coroutine
  printf("  __cpp_impl_coroutine = %ld\n",(long)__cpp_impl_coroutine);
#endif
#ifdef __cpp_lib_coroutine
  printf("  __cpp_lib_coroutine  = %ld\n",(long)__cpp_lib_coroutine);
#endif
#ifdef __cpp_lib_generator
  printf("  std::generator      = available\n");
#else
  printf("  std::generator      = NOT available (需 GCC 14 / C++23)\n");
#endif
#ifdef __cpp_lib_execution
  printf("  std::execution      = available\n");
#else
  printf("  std::execution      = NOT available (需 C++26)\n");
#endif
#ifdef __cpp_impl_three_way_comparison
  printf("  spaceship           = available\n");
#endif
}

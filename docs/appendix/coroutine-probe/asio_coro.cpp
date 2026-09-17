// 验证 Boost.Asio 1.74 + C++20 协程（awaitable / co_spawn）能否工作
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cstdio>
#include <thread>
namespace asio = boost::asio;
using asio::ip::tcp;

// 协程风格的"处理一个请求"：先异步读，再（模拟）异步等待，再异步写
asio::awaitable<void> handle(tcp::socket sock) {
  char buf[1024];
  std::size_t n = co_await sock.async_read_some(asio::buffer(buf), asio::use_awaitable);
  // 模拟异步存储 IO：在协程里挂起，不占用线程
  asio::steady_timer t(co_await asio::this_coro::executor);
  t.expires_after(std::chrono::milliseconds(5));
  co_await t.async_wait(asio::use_awaitable);
  const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
  co_await asio::async_write(sock, asio::buffer(resp, std::strlen(resp)), asio::use_awaitable);
  boost::system::error_code ec; sock.shutdown(tcp::socket::shutdown_both, ec);
}

asio::awaitable<void> listener() {
  auto ex = co_await asio::this_coro::executor;
  tcp::acceptor acc(ex, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 19099));
  for (;;) {
    tcp::socket sock = co_await acc.async_accept(asio::use_awaitable);
    asio::co_spawn(ex, handle(std::move(sock)), asio::detached);
  }
}
int main(){
  setvbuf(stdout,nullptr,_IONBF,0);
  asio::io_context ctx;
  asio::co_spawn(ctx, listener(), asio::detached);
  printf("Asio + C++20 coroutines 编译并运行成功；启动 %u 个工作线程\n",
         std::max(1u,std::thread::hardware_concurrency()));
  std::vector<std::thread> ts;
  for(unsigned i=0;i<std::max(1u,std::thread::hardware_concurrency());++i)
    ts.emplace_back([&]{ ctx.run(); });
  // 自测：发一个请求，看是否拿到响应
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  {
    int fd=::socket(AF_INET,SOCK_STREAM,0);
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(19099);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(::connect(fd,(sockaddr*)&a,sizeof(a))==0){
      const char* q="GET / HTTP/1.1\r\nHost: h\r\n\r\n"; ::send(fd,q,strlen(q),0);
      char b[512]; ssize_t n=::recv(fd,b,sizeof(b)-1,0);
      printf("自测响应: %.*s\n", (int)(n>0?n:0), b);
    }
    ::close(fd);
  }
  ctx.stop(); for(auto&t:ts)t.join();
  return 0;
}

// 协程模型：Boost.Asio + C++20 协程，每请求 5ms 定时器（不占线程），keep-alive
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
namespace asio=boost::asio; using asio::ip::tcp;
static std::atomic<int> g_inflight{0},g_maxif{0};

// 读取请求头（读到 \r\n\r\n）
asio::awaitable<std::string> read_headers(tcp::socket& sock){
  std::string buf; char tmp[4096];
  for(;;){
    std::size_t n=co_await sock.async_read_some(asio::buffer(tmp),asio::use_awaitable);
    buf.append(tmp,n);
    if(buf.find("\r\n\r\n")!=std::string::npos) break;
    if(buf.size()>65536) break;
  }
  co_return buf;
}
asio::awaitable<void> handle(tcp::socket sock){
  try{
    for(;;){
      std::string req=co_await read_headers(sock);
      if(req.empty()) break;
      int c=++g_inflight; int p=g_maxif.load();
      while(c>p&&!g_maxif.compare_exchange_weak(p,c)){}
      asio::steady_timer t(co_await asio::this_coro::executor);
      t.expires_after(std::chrono::milliseconds(5));
      co_await t.async_wait(asio::use_awaitable);   // ★ 挂起，不占用线程
      --g_inflight;
      const char* body="{\"k\":1}";
      char hdr[256];
      int hn=snprintf(hdr,sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nContent-Type: application/json\r\n\r\n",strlen(body));
      std::string resp(hdr,(size_t)hn); resp+=body;
      co_await asio::async_write(sock,asio::buffer(resp),asio::use_awaitable);
    }
  }catch(...){}
  boost::system::error_code ec; sock.shutdown(tcp::socket::shutdown_both,ec);
}
asio::awaitable<void> listener(unsigned short port){
  auto ex=co_await asio::this_coro::executor;
  tcp::acceptor acc(ex,tcp::endpoint(asio::ip::make_address("127.0.0.1"),port));
  for(;;){
    tcp::socket s=co_await acc.async_accept(asio::use_awaitable);
    asio::co_spawn(ex,handle(std::move(s)),asio::detached);
  }
}
int main(int argc,char**argv){
  int nthreads=atoi(argv[1]); unsigned short port=(unsigned short)atoi(argv[2]);
  asio::io_context ctx;
  asio::co_spawn(ctx,listener(port),asio::detached);
  std::vector<std::thread> ts;
  for(int i=1;i<nthreads;i++) ts.emplace_back([&]{ctx.run();});
  fprintf(stderr,"READY(coro) port=%u threads=%d\n",port,nthreads);
  ctx.run();
  for(auto&t:ts)t.join();
  return 0;
}

// 决定性对比：同一个"50ms I/O 等待"的三种实现方式
//   mode 0: 协程内【阻塞调用】(sleep)                → 并发上限 = io_context 线程数
//   mode 1: 协程内【offload 到线程池】(sleep)         → 并发上限 = 池大小
//   mode 2: 协程内【真正异步等待】(asio::steady_timer) → 并发上限 = 内存，与线程无关
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
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
static int g_mode=0; static asio::thread_pool* g_pool=nullptr;
static std::atomic<int> g_inflight{0},g_maxif{0};
static const int WAIT_MS=50;

asio::awaitable<void> handle(tcp::socket sock){
  try{
    for(;;){
      char tmp[4096];
      std::size_t n=co_await sock.async_read_some(asio::buffer(tmp),asio::use_awaitable);
      if(n==0) break;
      int c=++g_inflight; int p=g_maxif.load();
      while(c>p&&!g_maxif.compare_exchange_weak(p,c)){}
      auto io_ex=co_await asio::this_coro::executor;
      if(g_mode==0){
        std::this_thread::sleep_for(std::chrono::milliseconds(WAIT_MS));   // 阻塞 io 线程
      } else if(g_mode==1){
        co_await asio::post(*g_pool,asio::use_awaitable);
        std::this_thread::sleep_for(std::chrono::milliseconds(WAIT_MS));   // 阻塞池线程
        co_await asio::post(io_ex,asio::use_awaitable);
      } else {
        asio::steady_timer t(io_ex); t.expires_after(std::chrono::milliseconds(WAIT_MS));
        co_await t.async_wait(asio::use_awaitable);                        // 真异步：只挂起
      }
      --g_inflight;
      const char* body="{\"k\":1}";
      char hdr[200]; int hl=snprintf(hdr,sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nContent-Type: application/json\r\n\r\n",strlen(body));
      std::string resp(hdr,(size_t)hl); resp+=body;
      co_await asio::async_write(sock,asio::buffer(resp),asio::use_awaitable);
    }
  }catch(...){}
  boost::system::error_code ec; sock.shutdown(tcp::socket::shutdown_both,ec);
}
asio::awaitable<void> listener(unsigned short port){
  auto ex=co_await asio::this_coro::executor;
  tcp::acceptor acc(ex,tcp::endpoint(asio::ip::make_address("127.0.0.1"),port));
  for(;;){ tcp::socket s=co_await acc.async_accept(asio::use_awaitable);
           asio::co_spawn(ex,handle(std::move(s)),asio::detached); }
}
int main(int argc,char**argv){
  int io_threads=atoi(argv[1]); unsigned short port=(unsigned short)atoi(argv[2]);
  g_mode=atoi(argv[3]); int poolsz=argc>4?atoi(argv[4]):0;
  if(g_mode==1) g_pool=new asio::thread_pool(poolsz);
  asio::io_context ctx;
  asio::co_spawn(ctx,listener(port),asio::detached);
  std::vector<std::thread> ts;
  for(int i=1;i<io_threads;i++) ts.emplace_back([&]{ctx.run();});
  fprintf(stderr,"READY io=%d mode=%d pool=%d\n",io_threads,g_mode,poolsz);
  ctx.run(); for(auto&t:ts)t.join(); return 0;
}

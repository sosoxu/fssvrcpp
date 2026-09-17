// 关键验证：协程中的【阻塞 pread】 vs 【offload 到线程池】对并发的影响
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
namespace asio=boost::asio; using asio::ip::tcp;
static int g_fd=-1; static size_t g_chunk=8u<<20;
static int g_mode=0;                      // 0=inline 阻塞  1=offload 到线程池
static asio::thread_pool* g_pool=nullptr;
static std::atomic<long long> g_done{0};

static long long BlockingRead(){
  std::vector<char> buf(g_chunk);
  ssize_t n=::pread(g_fd,buf.data(),g_chunk,0);
  long long sum=0; for(ssize_t i=0;i<n;i+=4096) sum+=buf[(size_t)i];
  return sum+n;                            // 防止被优化掉
}
asio::awaitable<void> handle(tcp::socket sock){
  try{
    for(;;){
      char tmp[4096];
      std::size_t n=co_await sock.async_read_some(asio::buffer(tmp),asio::use_awaitable);
      if(n==0) break;
      if(std::string(tmp,n).find("GET /io")!=std::string::npos){
        long long r=0;
        if(g_mode==0){
          r=BlockingRead();                                     // ★ 直接在 io_context 线程上阻塞
        } else {
          // Boost 1.74 的 post 不支持 (executor, token, fn) 三参形式：
          // 先 hop 到线程池执行阻塞读，再 hop 回 io_context
          auto io_ex=co_await asio::this_coro::executor;
          co_await asio::post(*g_pool,asio::use_awaitable);
          r=BlockingRead();
          co_await asio::post(io_ex,asio::use_awaitable);
        }
        g_done++;
        char body[64]; int bl=snprintf(body,sizeof(body),"%lld",r);
        char hdr[256]; int hl=snprintf(hdr,sizeof(hdr),
          "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",bl);
        std::string resp(hdr,(size_t)hl); resp.append(body,(size_t)bl);
        co_await asio::async_write(sock,asio::buffer(resp),asio::use_awaitable);
      } else {
        const char* resp="HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
        co_await asio::async_write(sock,asio::buffer(resp,strlen(resp)),asio::use_awaitable);
      }
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
  int iothreads=atoi(argv[1]); unsigned short port=(unsigned short)atoi(argv[2]);
  g_mode=atoi(argv[3]); int poolsz=atoi(argv[4]);
  g_fd=::open("/tmp/coro_probe/data.bin",O_RDONLY);
  if(g_mode==1) g_pool=new asio::thread_pool(poolsz);
  asio::io_context ctx;
  asio::co_spawn(ctx,listener(port),asio::detached);
  std::vector<std::thread> ts;
  for(int i=1;i<iothreads;i++) ts.emplace_back([&]{ctx.run();});
  fprintf(stderr,"READY io_threads=%d mode=%s pool=%d\n",iothreads,
          g_mode==0?"inline-blocking":"offload",g_mode==1?poolsz:0);
  ctx.run();
  for(auto&t:ts)t.join(); return 0;
}

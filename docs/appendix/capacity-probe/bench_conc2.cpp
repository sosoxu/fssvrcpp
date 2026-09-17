// 正确配置下的并发天花板：TCP_NODELAY=ON，测小请求吞吐 + 慢 handler（模拟存储 IO）
#include <httplib.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
static void RunCase(int pool,int clients,int delay_ms,int secs,int ka_max){
  httplib::Server svr;
  svr.new_task_queue=[pool](){return new httplib::ThreadPool(size_t(pool));};
  svr.set_tcp_nodelay(true);
  svr.set_keep_alive_max_count(ka_max);
  const std::string body=R"({"FileID":"da92f52401dc4d1cb93515f159c110d4","Location":{"SignedURL":"http://x/y","FileSource":"/u/1/f"}})";
  svr.Get("/s",[&](const httplib::Request&,httplib::Response& res){
    if(delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    res.set_content(body,"application/json");});
  int port=svr.bind_to_any_port("127.0.0.1");
  std::thread sth([&]{svr.listen_after_bind();});
  std::atomic<bool> stop{false}; std::atomic<long long> cnt{0},err{0};
  std::vector<std::thread> ts;
  for(int i=0;i<clients;i++) ts.emplace_back([&]{
    httplib::Client c("127.0.0.1",port); c.set_keep_alive(true); c.set_tcp_nodelay(true);
    c.set_read_timeout(15,0); long long l=0,e=0;
    while(!stop.load(std::memory_order_relaxed)){auto r=c.Get("/s"); if(r&&r->status==200)l++; else e++;}
    cnt+=l;err+=e;});
  std::this_thread::sleep_for(std::chrono::seconds(secs));
  stop=true; for(auto&t:ts)t.join(); svr.stop(); sth.join();
  printf("  pool=%-4d clients=%-5d delay=%-3dms ka=%-6d -> %9.0f req/s (err=%lld)\n",
         pool,clients,delay_ms,ka_max,double(cnt.load())/secs,err.load());
}
int main(){
  setvbuf(stdout,nullptr,_IONBF,0);
  printf("[A] 小请求、无 IO 延迟（JSON 元数据/位置类端点）\n");
  for(int pool:{8,64,256}) RunCase(pool,64,0,3,1000000);
  printf("\n[B] ★ 每次请求阻塞 50ms 模拟存储 IO（小文件频繁读写）\n");
  for(int pool:{8,64,256,1024}) RunCase(pool,256,50,3,1000000);
  printf("\n[C] keep_alive_max_count 的影响（pool=256, delay=50ms）\n");
  for(int ka:{100,1000,1000000}) RunCase(256,256,50,3,ka);
  return 0;
}

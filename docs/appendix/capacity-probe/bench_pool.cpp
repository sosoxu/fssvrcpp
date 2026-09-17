// 验证 new_task_queue 是否生效：统计实际服务过请求的线程数
#include <httplib.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
static int Serve(int pool, int clients, int per, std::set<std::thread::id>* ids, std::mutex* mu){
  httplib::Server svr;
  if(pool>0) svr.new_task_queue=[pool](){return new httplib::ThreadPool(size_t(pool));};
  svr.set_tcp_nodelay(true); svr.set_keep_alive_max_count(1000000);
  svr.Get("/s",[&](const httplib::Request&,httplib::Response& res){
    { std::lock_guard<std::mutex> lk(*mu); ids->insert(std::this_thread::get_id()); }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));   // 模拟存储 IO
    res.set_content("{\"k\":1}","application/json"); });
  int port=svr.bind_to_any_port("127.0.0.1");
  std::thread th([&]{svr.listen_after_bind();});
  std::atomic<long long> ok{0}; std::vector<std::thread> ts;
  auto t0=std::chrono::steady_clock::now();
  for(int i=0;i<clients;i++) ts.emplace_back([&]{
    httplib::Client c("127.0.0.1",port); c.set_keep_alive(true); c.set_tcp_nodelay(true);
    c.set_read_timeout(30,0);
    for(int j=0;j<per;j++){ auto r=c.Get("/s"); if(r&&r->status==200) ok++; }});
  for(auto&t:ts)t.join();
  double s=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
  svr.stop(); th.join();
  printf("  pool=%-4s clients=%-4d -> %6.0f req/s  ok=%lld  服务线程数=%zu\n",
         pool>0?std::to_string(pool).c_str():"default",clients,ok.load()/s,ok.load(),ids->size());
  return 0;
}
int main(){
  setvbuf(stdout,nullptr,_IONBF,0);
  printf("每请求 sleep 5ms（模拟存储 IO），nproc=%u\n\n",std::thread::hardware_concurrency());
  printf("默认线程池：\n");
  for(int clients:{8,16,32,64}){ std::set<std::thread::id> ids; std::mutex mu; Serve(0,clients,10,&ids,&mu); }
  printf("\n显式 ThreadPool(256)：\n");
  for(int clients:{8,16,32,64}){ std::set<std::thread::id> ids; std::mutex mu; Serve(256,clients,10,&ids,&mu); }
  return 0;
}

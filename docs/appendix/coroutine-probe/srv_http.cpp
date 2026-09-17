// 线程模型：httplib 线程池，每请求 5ms 阻塞（模拟存储 IO），keep-alive
#include <httplib.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
int main(int argc,char**argv){
  int pool=atoi(argv[1]); int port=atoi(argv[2]);
  std::atomic<int> inflight{0},maxif{0};
  httplib::Server svr;
  svr.new_task_queue=[pool](){return new httplib::ThreadPool(size_t(pool));};
  svr.set_tcp_nodelay(true); svr.set_keep_alive_max_count(1000000);
  svr.Get("/s",[&](const httplib::Request&,httplib::Response& res){
    int c=++inflight; int p=maxif.load(); while(c>p&&!maxif.compare_exchange_weak(p,c)){}
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    res.set_content("{\"k\":1}","application/json");
    --inflight; });
  svr.Get("/stat",[&](const httplib::Request&,httplib::Response& res){
    res.set_content("{\"max_inflight\":"+std::to_string(maxif.load())+"}", "application/json"); });
  fprintf(stderr,"READY(thread) port=%d pool=%d\n",port,pool);
  svr.listen("127.0.0.1",port);
  return 0;
}

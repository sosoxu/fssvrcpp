// 验证 TCP_NODELAY 假设：小请求 + keep-alive 场景下的 40ms delayed-ACK 停顿
#include <httplib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
using clk=std::chrono::steady_clock;
static double ms(clk::time_point a,clk::time_point b){return std::chrono::duration<double,std::milli>(b-a).count();}

// 用原始 socket 顺序发 N 个请求并**正确解析**每个响应（按 Content-Length 收全）
static void raw_seq(int port,int N,const char* tag,bool nodelay){
  int fd=::socket(AF_INET,SOCK_STREAM,0);
  if(nodelay){int on=1;::setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&on,sizeof(on));}
  sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  ::connect(fd,(sockaddr*)&a,sizeof(a));
  const std::string req="GET /s HTTP/1.1\r\nHost: h\r\n\r\n";
  auto t0=clk::now(); int ok=0; std::string buf;
  for(int i=0;i<N;i++){
    ::send(fd,req.data(),req.size(),0);
    // 读到 headers 结束
    size_t he;
    while((he=buf.find("\r\n\r\n"))==std::string::npos){char t[4096];ssize_t n=::recv(fd,t,sizeof(t),0);if(n<=0)goto done;buf.append(t,(size_t)n);}
    size_t clpos=buf.find("Content-Length: ");
    size_t cl=0; if(clpos!=std::string::npos) cl=strtoul(buf.c_str()+clpos+16,nullptr,10);
    size_t need=he+4+cl;
    while(buf.size()<need){char t[4096];ssize_t n=::recv(fd,t,sizeof(t),0);if(n<=0)goto done;buf.append(t,(size_t)n);}
    if(buf.compare(0,12,"HTTP/1.1 200")==0) ok++;
    buf.erase(0,need);
  }
 done:
  auto t1=clk::now(); ::close(fd);
  printf("  %-34s %8.0f req/s  avg=%7.3f ms  (ok=%d/%d)\n",tag,N/(ms(t0,t1)/1000.0),ms(t0,t1)/N,ok,N);
}

int main(){
  setvbuf(stdout,nullptr,_IONBF,0);
  const int N=1000;
  for(bool srv_nd : {false,true}){
    httplib::Server svr;
    svr.new_task_queue=[](){return new httplib::ThreadPool(64);};
    svr.set_tcp_nodelay(srv_nd);
    const std::string body=R"({"k":"v"})";
    svr.Get("/s",[&](const httplib::Request&,httplib::Response& res){res.set_content(body,"application/json");});
    int port=svr.bind_to_any_port("127.0.0.1");
    std::thread th([&]{svr.listen_after_bind();});
    printf("server TCP_NODELAY=%s\n", srv_nd?"ON ":"OFF");
    raw_seq(port,N,"raw socket + TCP_NODELAY",true);
    raw_seq(port,N,"raw socket + Nagle(off)",false);
    { // httplib 客户端
      httplib::Client c("127.0.0.1",port); c.set_keep_alive(true); c.set_tcp_nodelay(true);
      auto t0=clk::now();int ok=0;
      for(int i=0;i<N;i++){auto r=c.Get("/s");if(r&&r->status==200)ok++;}
      auto t1=clk::now();
      printf("  %-34s %8.0f req/s  avg=%7.3f ms  (ok=%d/%d)\n","httplib client + TCP_NODELAY",N/(ms(t0,t1)/1000.0),ms(t0,t1)/N,ok,N);
    }
    svr.stop(); th.join();
  }
  return 0;
}

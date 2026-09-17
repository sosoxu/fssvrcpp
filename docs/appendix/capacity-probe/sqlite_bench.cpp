// 小文件场景的存储侧固定成本：位置记录 + 元数据记录的写入吞吐
#include <sqlite3.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
using clk=std::chrono::steady_clock;
static double secs(clk::time_point a,clk::time_point b){return std::chrono::duration<double>(b-a).count();}

static void Bench(const char* mode,const char* sync,int nthreads,int per,const char* path){
  ::remove(path);
  sqlite3* db=nullptr;
  if(sqlite3_open(path,&db)!=SQLITE_OK){printf("open fail\n");return;}
  char* e=nullptr;
  sqlite3_exec(db,"PRAGMA journal_mode=WAL;",nullptr,nullptr,&e);
  std::string p=std::string("PRAGMA synchronous=")+sync+";";
  sqlite3_exec(db,p.c_str(),nullptr,nullptr,&e);
  sqlite3_exec(db,"PRAGMA busy_timeout=10000;",nullptr,nullptr,&e);
  sqlite3_exec(db,"CREATE TABLE file_locations(file_id TEXT PRIMARY KEY, file_source TEXT, container TEXT,"
                   " object_key TEXT, zone TEXT, driver TEXT, partition_id TEXT, created_by TEXT,"
                   " created_at INTEGER, data TEXT);",nullptr,nullptr,&e);
  sqlite3_exec(db,"CREATE TABLE file_metadata_records(partition_id TEXT,id TEXT,version INTEGER,kind TEXT,"
                   " is_latest INTEGER,created_at INTEGER,created_by TEXT,acl_viewers TEXT,acl_owners TEXT,"
                   " legal_tags TEXT,file_source TEXT,data TEXT,PRIMARY KEY(partition_id,id,version));",nullptr,nullptr,&e);
  sqlite3_close(db);

  std::atomic<long long> done{0}; std::vector<std::thread> ts;
  auto t0=clk::now();
  for(int t=0;t<nthreads;t++) ts.emplace_back([&,t]{
    sqlite3* d=nullptr; sqlite3_open(path,&d);
    sqlite3_exec(d,"PRAGMA journal_mode=WAL;",nullptr,nullptr,nullptr);
    sqlite3_exec(d,p.c_str(),nullptr,nullptr,nullptr);
    sqlite3_exec(d,"PRAGMA busy_timeout=10000;",nullptr,nullptr,nullptr);
    for(int i=0;i<per;i++){
      std::string fid="p:dataset--File.Generic:"+std::to_string(t)+"-"+std::to_string(i);
      sqlite3_exec(d,"BEGIN IMMEDIATE;",nullptr,nullptr,nullptr);
      sqlite3_stmt* s=nullptr;
      sqlite3_prepare_v2(d,"INSERT OR REPLACE INTO file_locations VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);",-1,&s,nullptr);
      sqlite3_bind_text(s,1,fid.c_str(),-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(s,2,"/osdu-user/1/f",-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(s,3,"opendes-staging",-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(s,4,"2025/09/16/f",-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(s,5,"staging",-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(s,6,"posix",-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(s,7,"opendes",-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(s,8,"u@x.com",-1,SQLITE_TRANSIENT);
      sqlite3_bind_int64(s,9,1700000000);
      sqlite3_bind_text(s,10,"{\"k\":\"v\"}",-1,SQLITE_TRANSIENT);
      sqlite3_step(s); sqlite3_finalize(s);
      sqlite3_prepare_v2(d,"INSERT OR REPLACE INTO file_metadata_records VALUES(?1,?2,1,?3,1,1,?4,?5,?6,?7,?8,?9);",-1,&s,nullptr);
      sqlite3_bind_text(s,1,"opendes",-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(s,2,fid.c_str(),-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(s,3,"opendes:wks:dataset--File.Generic:1.0.0",-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(s,4,"u@x.com",-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(s,5,"[]",-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(s,6,"[]",-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(s,7,"[]",-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(s,8,"/osdu-user/1/f",-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(s,9,"{\"data\":\"...\"}",-1,SQLITE_TRANSIENT);
      sqlite3_step(s); sqlite3_finalize(s);
      sqlite3_exec(d,"COMMIT;",nullptr,nullptr,nullptr);
      done++;
    }
    sqlite3_close(d);
  });
  for(auto&t:ts)t.join();
  double s=secs(t0,clk::now());
  printf("  %-38s -> %8.0f tx/s  (%d 线程 × %d 事务, 每事务=1 位置+1 元数据)\n",
         mode,done.load()/s,nthreads,per);
}
int main(){
  setvbuf(stdout,nullptr,_IONBF,0);
  printf("SQLite 写入吞吐（每事务 = 位置记录 + 元数据记录，模拟 1 个小文件的固定 DB 成本）\n");
  Bench("单线程 / synchronous=NORMAL","NORMAL",1,5000,"/tmp/fss_bench/t1.db");
  Bench("8 线程  / synchronous=NORMAL","NORMAL",8,2000,"/tmp/fss_bench/t8.db");
  Bench("8 线程  / synchronous=FULL(每事务 fsync)","FULL",8,500,"/tmp/fss_bench/t8f.db");
  Bench("32 线程 / synchronous=NORMAL","NORMAL",32,500,"/tmp/fss_bench/t32.db");
  return 0;
}

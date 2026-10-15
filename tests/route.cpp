// Unauthenticated SSH-banner timings: compare direct TCP and HTTP CONNECT routes.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
using Clock = std::chrono::steady_clock;
void ready(int fd, short event, Clock::time_point end) {
 for (;;) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-Clock::now()).count();
  if (ms <= 0) throw std::runtime_error("timeout");
  pollfd p{fd,event,0}; int n = poll(&p,1,static_cast<int>(ms));
  if (n < 0 && errno == EINTR) continue;
  if (n <= 0) throw std::runtime_error("timeout");
  return;
 }
}
char byte(int fd, Clock::time_point end) {
 for (;;) {
  ready(fd,POLLIN,end); char c; auto n=recv(fd,&c,1,0);
  if(n<0&&(errno==EINTR||errno==EAGAIN))continue;
  if(n!=1)throw std::runtime_error("connection closed");
  return c;
 }
}
// Time-limited, bounded byte echo. No filesystem, commands or credentials.
int serve(const char* port, bool public_listener) {
 int number=std::stoi(port);
 if(number<1||number>65535)throw std::runtime_error("invalid port");
 int listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0),one=1;
 if(listener<0)throw std::runtime_error("socket");
 setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(public_listener?INADDR_ANY:INADDR_LOOPBACK);address.sin_port=htons(number);
 if(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof address)||listen(listener,8)){
  close(listener);throw std::runtime_error("probe port unavailable");
 }
 std::cout<<"PROBE_READY port="<<number<<std::endl;
 auto deadline=Clock::now()+std::chrono::seconds(120);
 while(Clock::now()<deadline){
  int client=-1;
  try{
   ready(listener,POLLIN,deadline);
   client=accept4(listener,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);
   if(client<0)continue;
   setsockopt(client,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
   const std::string greeting="CCPROXY_ROUTE\n";
   if(send(client,greeting.data(),greeting.size(),MSG_NOSIGNAL)!=static_cast<ssize_t>(greeting.size()))throw std::runtime_error("greeting");
   for(int i=0;i<256&&Clock::now()<deadline;++i){
    char c=byte(client,std::min(deadline,Clock::now()+std::chrono::seconds(3)));
    ready(client,POLLOUT,deadline);
    if(send(client,&c,1,MSG_NOSIGNAL)!=1)break;
   }
  }catch(const std::exception&){}
  if(client>=0)close(client);
 }
 close(listener);return 0;
}
int main(int argc,char**argv) {
 if(argc==3&&(std::string(argv[1])=="--serve"||std::string(argv[1])=="--serve-public")){
  try{return serve(argv[2],std::string(argv[1])=="--serve-public");}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
 }
 bool echo=argc>1&&std::string(argv[1])=="--echo";
 if(echo){--argc;++argv;}
 if(argc!=3&&argc!=5){std::cerr<<"target port [HTTP-proxy-host port]\n";return 1;}
 std::vector<double> times; int failures=0,persistent=-1;
 for(int sample=0;sample<10;++sample){
  int fd=persistent; addrinfo* addresses=nullptr;bool failed=false;
  auto start=Clock::now(), end=start+std::chrono::seconds(3);
  try {
   if(fd<0){
   addrinfo hints{};hints.ai_socktype=SOCK_STREAM;
   int error=getaddrinfo(argc==5?argv[3]:argv[1],argc==5?argv[4]:argv[2],&hints,&addresses);
   if(error)throw std::runtime_error(gai_strerror(error));
   for(auto a=addresses;a;a=a->ai_next){
    fd=socket(a->ai_family,a->ai_socktype|SOCK_NONBLOCK,a->ai_protocol);if(fd<0)continue;
    if(connect(fd,a->ai_addr,a->ai_addrlen)==0)break;
    if(errno==EINPROGRESS){
     ready(fd,POLLOUT,end);int status=0;socklen_t size=sizeof status;
     if(!getsockopt(fd,SOL_SOCKET,SO_ERROR,&status,&size)&&!status)break;
    }
    close(fd);fd=-1;
   }
   if(fd<0)throw std::runtime_error("connect failed");
   if(argc==5){
    std::string authority=std::string(argv[1])+":"+argv[2];
    if(authority.find_first_of("\r\n")!=std::string::npos)throw std::runtime_error("invalid target");
    std::string request="CONNECT "+authority+" HTTP/1.1\r\nHost: "+authority+"\r\n\r\n";
    size_t offset=0;
    while(offset<request.size()){
     ready(fd,POLLOUT,end);auto n=send(fd,request.data()+offset,request.size()-offset,MSG_NOSIGNAL);
     if(n<0&&(errno==EINTR||errno==EAGAIN))continue;
     if(n<=0)throw std::runtime_error("proxy write failed");
     offset+=n;
    }
    std::string response;
    while(response.size()<8192&&response.find("\r\n\r\n")==std::string::npos)response+=byte(fd,end);
    if(response.find("HTTP/1.")!=0||response.size()<12||response.substr(9,3)!="200")throw std::runtime_error("proxy CONNECT rejected");
   }
   std::string banner;
   for(int lines=0;lines<20;++lines){
    banner.clear();while(banner.size()<512){char c=byte(fd,end);banner+=c;if(c=='\n')break;}
    if(banner.rfind(echo?"CCPROXY_ROUTE":"SSH-",0)==0)break;
   }
   if(banner.rfind(echo?"CCPROXY_ROUTE":"SSH-",0)!=0)throw std::runtime_error("missing probe banner");
   }
   if(echo){
    int one=1;setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
    start=Clock::now();end=start+std::chrono::seconds(3);
    char c=static_cast<char>(sample+40);
    ready(fd,POLLOUT,end);
    if(send(fd,&c,1,MSG_NOSIGNAL)!=1||byte(fd,end)!=c)throw std::runtime_error("echo failed");
    persistent=fd;
   }
   times.push_back(std::chrono::duration<double,std::milli>(Clock::now()-start).count());
  }catch(const std::exception& e){failed=true;++failures;std::cerr<<"sample "<<sample+1<<": "<<e.what()<<'\n';}
  if((!echo||failed)&&fd>=0){close(fd);persistent=-1;}
  if(addresses)freeaddrinfo(addresses);
  if(echo)usleep(250000);
 }
 if(persistent>=0)close(persistent);
 std::sort(times.begin(),times.end());
 std::cout<<(argc==5?std::string("proxy:")+argv[4]:"direct")<<" successes="<<times.size()<<" failures="<<failures;
 if(!times.empty())std::cout<<(echo?" echo_p50_ms=":" banner_p50_ms=")<<(times[(times.size()-1)/2]+times[times.size()/2])/2<<" max_ms="<<times.back();
 std::cout<<'\n';return times.empty()?1:0;
}

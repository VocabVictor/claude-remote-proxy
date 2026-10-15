// Loopback-only HTTP CONNECT fixture; only loopback destinations are accepted.
#include <cerrno>
#include <csignal>
#include <fstream>
#include <iostream>
#include <string>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
bool send_all(int fd, const char* data, size_t size) {
 while(size) {
  auto n=send(fd,data,size,MSG_NOSIGNAL);
  if(n<0&&errno==EINTR)continue;
  if(n<=0)return false;
  data+=n;size-=static_cast<size_t>(n);
 }
 return true;
}
void forward(int client) {
 std::string request;
 while(request.size()<8192&&request.find("\r\n\r\n")==std::string::npos){
  char c;if(recv(client,&c,1,0)!=1)return;request+=c;
 }
 const std::string prefix="CONNECT 127.0.0.1:";
 if(request.rfind(prefix,0)!=0)return;
 size_t end=request.find(' ',prefix.size());
 if(end==std::string::npos)return;
 std::string text=request.substr(prefix.size(),end-prefix.size());
 if(text.empty()||text.size()>5||text.find_first_not_of("0123456789")!=std::string::npos)return;
 int port=std::stoi(text);if(port<1||port>65535)return;
 int remote=socket(AF_INET,SOCK_STREAM,0);if(remote<0)return;
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
 if(connect(remote,reinterpret_cast<sockaddr*>(&address),sizeof address)){close(remote);return;}
 const std::string response="HTTP/1.1 200 Connection established\r\n\r\n";
 if(!send_all(client,response.data(),response.size())){close(remote);return;}
 pollfd descriptors[2]={{client,POLLIN,0},{remote,POLLIN,0}};
 bool running=true;
 while(running){
  int n=poll(descriptors,2,15000);if(n<0&&errno==EINTR)continue;if(n<=0)break;
  for(int i=0;i<2;++i)if(descriptors[i].revents){
   char buffer[8192];auto count=recv(descriptors[i].fd,buffer,sizeof buffer,0);
   if(count<=0||!send_all(descriptors[1-i].fd,buffer,static_cast<size_t>(count))){running=false;break;}
  }
 }
 close(remote);
}
int main(int argc,char** argv){
 if(argc!=2)return 1;
 std::signal(SIGCHLD,SIG_IGN);
 int server=socket(AF_INET,SOCK_STREAM,0);if(server<0)return 1;
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 if(bind(server,reinterpret_cast<sockaddr*>(&address),sizeof address)||listen(server,8))return 1;
 socklen_t size=sizeof address;if(getsockname(server,reinterpret_cast<sockaddr*>(&address),&size))return 1;
 std::ofstream file(argv[1]);file<<ntohs(address.sin_port)<<'\n';file.close();
 for(;;){
  int client=accept(server,nullptr,nullptr);if(client<0){if(errno==EINTR)continue;return 1;}
  pid_t child=fork();
  if(!child){close(server);forward(client);close(client);_exit(0);}
  close(client);
 }
}

// Real PTY benchmark: elapsed time until tmux redraws the ready marker.
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <pty.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char** argv) {
 try {
  if (argc == 2 && std::string(argv[1]) == "--port") {
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   sockaddr_in address{}; address.sin_family = AF_INET;
   address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address)) throw std::runtime_error("bind");
   socklen_t length = sizeof address;
   if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length)) throw std::runtime_error("getsockname");
   std::cout << ntohs(address.sin_port) << '\n'; close(fd); return 0;
  }
  if (argc < 3) throw std::runtime_error("expected --drop|--bench|--rtt command [args]");
  bool drop = std::string(argv[1]) == "--drop";
  bool rtt = std::string(argv[1]) == "--rtt";
  int rtt_samples = 50;
  if (const char* configured = std::getenv("CCPROXY_RTT_SAMPLES")) {
   std::string count = configured; size_t used;
   rtt_samples = std::stoi(count, &used);
   if (used != count.size() || rtt_samples < 2 || rtt_samples > 1000) throw std::runtime_error("invalid RTT sample count");
  }
  if (!drop && !rtt && std::string(argv[1]) != "--bench") throw std::runtime_error("unknown test mode");
  std::vector<double> samples;
  for (int i = 0; i < (drop || rtt ? 1 : 21); ++i) {
   int terminal;
   winsize size{40, 160, 0, 0};
   auto start = std::chrono::steady_clock::now();
   pid_t child = forkpty(&terminal, nullptr, nullptr, &size);
   if (child < 0) throw std::runtime_error("forkpty");
   if (!child) { execvp(argv[2], argv + 2); _exit(127); }
   std::string output;
   bool ready = false;
   while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    pollfd descriptor{terminal, POLLIN, 0};
    int result = poll(&descriptor, 1, 100);
    if (result < 0 && errno == EINTR) continue;
    if (result > 0) {
     char buffer[8192]; auto n = read(terminal, buffer, sizeof buffer);
     if (n <= 0) break;
     output.append(buffer, n);
     if (output.find("TEST_CLAUDE_READY") != std::string::npos) { ready = true; break; }
    }
   }
   double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
   if (!ready) {
    kill(child, SIGKILL); waitpid(child, nullptr, 0); close(terminal);
    throw std::runtime_error("ready marker missing: " + output);
   }
   if (rtt) {
    for (int sample = 0; sample <= rtt_samples; ++sample) {
     char sent = static_cast<char>(33 + sample), received = 0;
     auto before = std::chrono::steady_clock::now();
     pollfd descriptor{terminal, POLLIN, 0};
     if (write(terminal, &sent, 1) != 1 || poll(&descriptor, 1, 10000) <= 0 ||
         read(terminal, &received, 1) != 1 || received != sent) {
      kill(child, SIGKILL); close(terminal); waitpid(child, nullptr, 0);
      throw std::runtime_error("interactive echo failed");
     }
     if (sample) samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count());
    }
   }
   if (drop || rtt) { kill(child, SIGHUP); close(terminal); }
   else {
    if (write(terminal, "\002d", 2) != 2) throw std::runtime_error("detach write");
    // Drain until EOF; otherwise a full PTY buffer could block SSH exit.
    char buffer[8192]; while (read(terminal, buffer, sizeof buffer) > 0) {}
    close(terminal);
   }
   int status; while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
   if (!drop && !rtt && (!WIFEXITED(status) || WEXITSTATUS(status))) throw std::runtime_error("attach failed");
   if (i && !rtt) samples.push_back(ms); // discard first attach (warm-up)
  }
  if (rtt) {
   std::sort(samples.begin(), samples.end());
   std::cout << "interactive_echo: n=" << samples.size() << " p50_ms=" << (samples[(samples.size()-1)/2]+samples[samples.size()/2])/2
             << " p95_ms=" << samples[(samples.size()*95+99)/100-1] << " max_ms=" << samples.back() << '\n';
  } else if (!drop) {
   std::sort(samples.begin(), samples.end());
   std::cout << argv[2] << ": n=20 warm_attach p50_ms=" << (samples[9]+samples[10])/2
             << " p95_ms=" << samples[18] << '\n';
  }
 } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

// Linux/WSL client and remote helper. OpenSSH owns transport and terminal I/O.
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include "http_connect.hpp"

namespace fs = std::filesystem;
using Strings = std::vector<std::string>;
std::string home() {
    const char* h = std::getenv("HOME");
    if (h && *h) return h;
    const auto* p = getpwuid(geteuid());
    if (!p) throw std::runtime_error("cannot resolve home directory");
    return p->pw_dir;
}
std::string quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? "'\"'\"'" : std::string(1, c);
    return out + "'";
}
[[noreturn]] void execute(const Strings& args) {
    std::vector<char*> argv;
    for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    throw std::runtime_error("exec " + args[0] + ": " + std::strerror(errno));
}
bool valid_session(const std::string& s) {
    return !s.empty() && s.size() <= 128 && s.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos;
}
bool valid_uuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}
std::string hash(const std::string& text) {
    uint64_t value = 14695981039346656037ULL;
    for (unsigned char c : text) { value ^= c; value *= 1099511628211ULL; }
    std::ostringstream result;
    result << std::hex << std::setw(16) << std::setfill('0') << value;
    return result.str();
}
void private_directory(const fs::path& path) {
    struct stat st{};
    if (lstat(path.c_str(), &st)) {
        if (errno != ENOENT) throw std::runtime_error("cannot inspect state directory");
        fs::create_directories(path);
        if (lstat(path.c_str(), &st)) throw std::runtime_error("cannot inspect new state directory");
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid())
        throw std::runtime_error("unsafe state directory: " + path.string());
    if ((st.st_mode & 077) && chmod(path.c_str(), 0700))
        throw std::runtime_error("cannot restrict state directory");
}
// Diagnostic only. Normal interactive sessions still exec SSH without a relay.
int network_test(const Strings& args) {
    int input[2], output[2];
    if (pipe2(input, O_CLOEXEC)) throw std::runtime_error("probe pipe failed");
    if (pipe2(output, O_CLOEXEC)) { close(input[0]); close(input[1]); throw std::runtime_error("probe pipe failed"); }
    auto start = std::chrono::steady_clock::now();
    pid_t child = fork();
    if (child < 0) throw std::runtime_error("probe fork failed");
    if (!child) {
        if (dup2(input[0], STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0) _exit(126);
        try { execute(args); } catch (...) { _exit(127); }
    }
    close(input[0]); close(output[1]);
    auto previous = std::signal(SIGPIPE, SIG_IGN);
    std::vector<double> samples;
    double first = 0;
    bool ok = true;
    for (int i = 0; i < 51; ++i) {
        auto sent = std::chrono::steady_clock::now();
        unsigned char value = static_cast<unsigned char>(i + 32), response = 0;
        ssize_t wrote;
        do { wrote = write(input[1], &value, 1); } while (wrote < 0 && errno == EINTR);
        pollfd descriptor{output[0], POLLIN, 0};
        int ready;
        do { ready = poll(&descriptor, 1, 15000); } while (ready < 0 && errno == EINTR);
        ssize_t got = -1;
        if (ready > 0) do { got = read(output[0], &response, 1); } while (got < 0 && errno == EINTR);
        if (wrote != 1 || got != 1 || response != value) { ok = false; break; }
        auto now = std::chrono::steady_clock::now();
        if (!i) first = std::chrono::duration<double, std::milli>(now - start).count();
        else samples.push_back(std::chrono::duration<double, std::milli>(now - sent).count());
    }
    close(input[1]); close(output[0]);
    if (!ok) kill(child, SIGTERM);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    std::signal(SIGPIPE, previous);
    if (!ok || !WIFEXITED(status) || WEXITSTATUS(status)) throw std::runtime_error("SSH echo probe failed (timeout, banner, or remote helper error)");
    std::sort(samples.begin(), samples.end());
    std::cout << "SSH echo: samples=50 first_response_ms=" << first
              << " p50_rtt_ms=" << (samples[24] + samples[25]) / 2
              << " p95_rtt_ms=" << samples[47] << " max_rtt_ms=" << samples.back()
              << " (no Claude/model calls; excludes tmux/rendering)\n";
    return 0;
}
std::string read_uuid(const fs::path& state) {
    int fd = open(state.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        // Do not perform a second existence check: another initializer may
        // atomically publish the file between open(ENOENT) and that check.
        if (errno == ENOENT) return {};
        throw std::runtime_error("cannot read session state");
    }
    char buffer[38]; size_t size = 0;
    while (size < sizeof buffer) {
        ssize_t n = read(fd, buffer + size, sizeof buffer - size);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); throw std::runtime_error("cannot read session state"); }
        if (!n) break;
        size += static_cast<size_t>(n);
    }
    close(fd);
    if (size == 37 && buffer[36] == '\n') --size;
    std::string id(buffer, size);
    if (!valid_uuid(id)) throw std::runtime_error("invalid session UUID: " + state.string());
    return id;
}
std::string new_uuid() {
    unsigned char bytes[16];
    size_t count = 0;
    while (count < sizeof bytes) {
        auto n = getrandom(bytes + count, sizeof bytes - count, 0);
        if (n < 0) { if (errno == EINTR) continue; throw std::runtime_error("getrandom failed"); }
        count += static_cast<size_t>(n);
    }
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}
std::string session_uuid(const fs::path& state) {
    // Warm path: no flock process, lock syscall, random generation or writes.
    auto id = read_uuid(state);
    if (!id.empty()) return id;
    int fd = open((state.string() + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) throw std::runtime_error("cannot open initialization lock");
    while (flock(fd, LOCK_EX)) {
        if (errno != EINTR) { close(fd); throw std::runtime_error("cannot lock state"); }
    }
    id = read_uuid(state);
    if (id.empty()) {
        id = new_uuid();
        auto tmp = state.string() + "." + std::to_string(getpid()) + ".tmp";
        int out = open(tmp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (out < 0) { close(fd); throw std::runtime_error("cannot create state"); }
        std::string content = id + "\n";
        size_t wrote = 0;
        while (wrote < content.size()) {
            ssize_t n = write(out, content.data() + wrote, content.size() - wrote);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { close(out); unlink(tmp.c_str()); close(fd); throw std::runtime_error("cannot write state"); }
            wrote += static_cast<size_t>(n);
        }
        close(out);
        if (rename(tmp.c_str(), state.c_str())) {
            int saved = errno;
            unlink(tmp.c_str()); close(fd);
            throw std::runtime_error("cannot publish state: " + std::string(std::strerror(saved)));
        }
    }
    close(fd);
    return id;
}
[[noreturn]] void serve(const std::string& project, const std::string& session,
                       const std::string& binary, const std::string& permission) {
    if (!fs::path(project).is_absolute() || !valid_session(session) || binary.empty() ||
        (permission != "standard" && permission != "bypass"))
        throw std::runtime_error("invalid remote arguments");
    if (chdir(project.c_str())) throw std::runtime_error("cannot enter remote project: " + project);
    auto state_dir = fs::path(home()) / ".claude/remote-proxy";
    private_directory(state_dir);
    auto id = session_uuid(state_dir / (session + ".session-id"));
    std::string key = project;
    for (char& c : key)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !(c >= '0' && c <= '9') && c != '-') c = '-';
    bool resume = fs::exists(fs::path(home()) / ".claude/projects" / key / (id + ".jsonl"));
    std::string launch = "exec " + quote(binary);
    if (permission == "bypass") launch += " --dangerously-skip-permissions";
    launch += resume ? " --resume " : " --session-id ";
    launch += quote(id);
    execute({"tmux", "new-session", "-A", "-s", session, "-c", project, launch});
}
std::string trim(std::string s) {
    auto first = s.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}
std::map<std::string, std::string> config() {
    std::map<std::string, std::string> values;
    const char* override = std::getenv("CLAUDE_PROXY_CONFIG");
    fs::path file = override ? override : ".claude-remote-proxy.env";
    std::ifstream in(file);
    if (!in && override) throw std::runtime_error("cannot open configuration");
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) throw std::runtime_error("expected KEY=value configuration");
        auto name = trim(line.substr(0, pos)), value = trim(line.substr(pos + 1));
        if (name.empty() || name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
            throw std::runtime_error("invalid configuration key");
        if (value.size() >= 2 && (value.front() == '\'' || value.front() == '"') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        // No shell evaluation, expansion, command substitution or sourcing.
        if (value.find_first_of("\n\r") != std::string::npos) throw std::runtime_error("invalid configuration value");
        values[name] = value;
    }
    return values;
}
int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string(argv[1]) == "--http-connect")
            return http_connect_fd(argv[2], argv[3], argv[4]);
        if (argc == 2 && std::string(argv[1]) == "--echo") {
            if (isatty(STDIN_FILENO)) {
                termios mode{};
                if (tcgetattr(STDIN_FILENO, &mode)) throw std::runtime_error("echo terminal read failed");
                cfmakeraw(&mode);
                if (tcsetattr(STDIN_FILENO, TCSANOW, &mode)) throw std::runtime_error("echo terminal setup failed");
                const char marker[] = "TEST_CLAUDE_READY";
                if (write(STDOUT_FILENO, marker, sizeof marker - 1) != sizeof marker - 1)
                    throw std::runtime_error("echo terminal marker failed");
            }
            char buffer[4096];
            for (;;) {
                ssize_t n = read(STDIN_FILENO, buffer, sizeof buffer);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) throw std::runtime_error("echo read failed");
                if (!n) return 0;
                for (ssize_t offset = 0; offset < n;) {
                    ssize_t wrote = write(STDOUT_FILENO, buffer + offset, n - offset);
                    if (wrote < 0 && errno == EINTR) continue;
                    if (wrote <= 0) throw std::runtime_error("echo write failed");
                    offset += wrote;
                }
            }
        }
        if (argc == 6 && std::string(argv[1]) == "--serve")
            serve(argv[2], argv[3], argv[4], argv[5]);
        std::string command = argc > 1 ? argv[1] : "start";
        if (command == "--version" || command == "version") {
            std::cout << "claude-remote-proxy-native 0.4.0\n"; return 0;
        }
        if (command == "--help" || command == "help") {
            std::cout << "Usage: claude-remote-proxy-native [start|run|network-test|disconnect|version]\n"
                         "Remote mode only. Configure SSH_TARGET, REMOTE_PROJECT_DIR, "
                         "REMOTE_PROXY_BIN (absolute remote helper path).\n";
            return 0;
        }
        if (argc > 2 || (command != "start" && command != "run" && command != "disconnect" && command != "network-test"))
            throw std::runtime_error("unsupported command; use --help");
        auto values = config();
        auto get = [&](const std::string& name, std::string fallback = "") {
            const char* value = std::getenv(name.c_str());
            if (value) return std::string(value);
            auto it = values.find(name);
            return it == values.end() ? fallback : it->second;
        };
        auto target = get("SSH_TARGET"), project = get("REMOTE_PROJECT_DIR");
        if (target.empty() || target[0] == '-' || target.find_first_of(" \t\r\n") != std::string::npos ||
            !fs::path(project).is_absolute()) throw std::runtime_error("invalid SSH_TARGET or REMOTE_PROJECT_DIR");
        auto mode = get("CLAUDE_PROXY_MODE", "remote");
        if (mode != "remote") throw std::runtime_error("native client only supports remote mode");
        auto session = get("REMOTE_TMUX_SESSION");
        if (session.empty()) session = "claude-native-" + hash(project);
        if (!valid_session(session)) throw std::runtime_error("invalid tmux session name");
        auto socket_dir = fs::path(home()) / ".cache/claude-remote-proxy";
        auto socket = get("CLAUDE_PROXY_SOCKET");
        auto ssh_config = get("CLAUDE_PROXY_SSH_CONFIG");
        auto compression = get("SSH_COMPRESSION"), qos = get("SSH_IP_QOS"), ciphers = get("SSH_CIPHERS");
        auto obscure = get("SSH_OBSCURE_KEYSTROKE_TIMING");
        auto proxy = get("SSH_HTTP_PROXY");
        if (!proxy.empty()) proxy_address(proxy);
        if (!compression.empty() && compression != "yes" && compression != "no")
            throw std::runtime_error("SSH_COMPRESSION must be yes or no");
        if (!obscure.empty() && obscure != "yes" && obscure != "no")
            throw std::runtime_error("SSH_OBSCURE_KEYSTROKE_TIMING must be yes or no");
        if (socket.empty()) {
            private_directory(socket_dir);
            // OpenSSH expands %C using resolved host, port, user and jump host.
            // Profile-specific masters prevent a new option silently reusing old settings.
            socket = (socket_dir / ("%C-" + hash(ssh_config + "\n" + compression + "\n" + qos + "\n" + ciphers + "\n" + obscure + "\n" + proxy).substr(0, 8))).string();
            if (socket.size() + 38 > 100) throw std::runtime_error("expanded SSH socket path too long; set CLAUDE_PROXY_SOCKET");
        }
        if (socket.size() > 100) throw std::runtime_error("SSH control socket path too long; set CLAUDE_PROXY_SOCKET");
        Strings args{"ssh"};
        if (!ssh_config.empty()) { args.push_back("-F"); args.push_back(ssh_config); }
        args.insert(args.end(), {"-S", socket});
        auto user = get("REMOTE_USER");
        if (!user.empty()) { args.push_back("-l"); args.push_back(user); }
        if (command == "disconnect") { args.insert(args.end(), {"-O", "exit", target}); execute(args); }
        if (!proxy.empty()) {
            // Escape literal percent signs before OpenSSH expands its tokens.
            auto escape = [](const std::string& text) { std::string out; for (char c : text) { out += c; if (c == '%') out += '%'; } return out; };
            auto executable = fs::canonical("/proc/self/exe").string();
            args.insert(args.end(), {"-o", "ProxyUseFdpass=yes", "-o",
                "ProxyCommand=" + quote(escape(executable)) + " --http-connect " + quote(proxy) + " '%h' '%p'"});
        }
        if (!compression.empty()) args.insert(args.end(), {"-o", "Compression=" + compression});
        if (!qos.empty()) args.insert(args.end(), {"-o", "IPQoS=" + qos});
        if (!ciphers.empty()) args.insert(args.end(), {"-o", "Ciphers=" + ciphers});
        if (!obscure.empty()) args.insert(args.end(), {"-o", "ObscureKeystrokeTiming=" + obscure});
        if (command == "network-test") args.insert(args.end(), {"-o", "BatchMode=yes"});
        auto helper = get("REMOTE_PROXY_BIN");
        if (!fs::path(helper).is_absolute()) throw std::runtime_error("REMOTE_PROXY_BIN must be an absolute remote path");
        auto binary = get("REMOTE_CLAUDE_BIN", "claude");
        auto permission = get("CLAUDE_PERMISSION_MODE", "standard");
        if (permission != "standard" && permission != "bypass") throw std::runtime_error("invalid permission mode");
        // OpenSSH probes/reuses/creates the master internally. No preflight SSH,
        // hashing commands, shell launcher or intermediate relay loop.
        args.insert(args.end(), {"-o", "ControlMaster=auto", "-o", "ControlPersist=" + get("SSH_CONTROL_PERSIST", "8h"),
                     "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3",
                     "-o", "ConnectTimeout=10", command == "network-test" ? "-T" : "-tt", target});
        args.push_back("exec " + quote(helper) + (command == "network-test" ? " --echo" :
                     " --serve " + quote(project) + " " + quote(session) + " " + quote(binary) + " " + quote(permission)));
        if (command == "network-test") return network_test(args);
        execute(args);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n'; return 1;
    }
}

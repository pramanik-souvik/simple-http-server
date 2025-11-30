#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std;

static atomic<bool> keep_running(true);

void handle_sigint(int) {
    keep_running = false;
}

// ---------- ThreadPool ----------
class ThreadPool {
public:
    ThreadPool(size_t n) : stop(false) {
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                        if (this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    ~ThreadPool() {
        {
            unique_lock<mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (thread &worker : workers) worker.join();
    }

    void enqueue(function<void()> f) {
        {
            unique_lock<mutex> lock(queue_mutex);
            tasks.push(std::move(f));
        }
        condition.notify_one();
    }

private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queue_mutex;
    condition_variable condition;
    bool stop;
};

// ---------- Utilities ----------
string url_decode(const string &src) {
    string ret;
    char ch;
    int i, ii;
    for (i = 0; i < (int)src.length(); ++i) {
        if (src[i] == '%') {
            sscanf(src.substr(i + 1, 2).c_str(), "%x", &ii);
            ch = static_cast<char>(ii);
            ret += ch;
            i = i + 2;
        } else if (src[i] == '+') {
            ret += ' ';
        } else {
            ret += src[i];
        }
    }
    return ret;
}

string sanitize_path(const string &path) {
    // Remove query and fragment
    string p = path;
    size_t q = p.find_first_of("?#");
    if (q != string::npos) p = p.substr(0, q);

    // Decode URL-encoding
    p = url_decode(p);

    // Prevent directory traversal
    // Collapse "/.." segments and remove leading '/'
    vector<string> parts;
    string token;
    stringstream ss(p);
    while (getline(ss, token, '/')) {
        if (token.empty() || token == ".") continue;
        if (token == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(token);
        }
    }
    string out;
    for (const auto &part : parts) {
        out += "/" + part;
    }
    if (out.empty()) out = "/";
    return out;
}

string get_extension(const string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == string::npos) return "";
    return path.substr(dot + 1);
}

map<string, string> default_mime_types() {
    map<string, string> m;
    m["html"] = "text/html";
    m["htm"] = "text/html";
    m["css"] = "text/css";
    m["js"] = "application/javascript";
    m["json"] = "application/json";
    m["png"] = "image/png";
    m["jpg"] = "image/jpeg";
    m["jpeg"] = "image/jpeg";
    m["gif"] = "image/gif";
    m["svg"] = "image/svg+xml";
    m["txt"] = "text/plain";
    m["pdf"] = "application/pdf";
    m["ico"] = "image/x-icon";
    return m;
}

// Read entire file into vector<char>. Return false on failure.
bool read_file(const string &path, vector<char> &out) {
    ifstream ifs(path, ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, ios::end);
    size_t len = ifs.tellg();
    ifs.seekg(0, ios::beg);
    out.resize(len);
    ifs.read(out.data(), len);
    return true;
}

// Send all bytes on socket
bool send_all(int sock, const char *data, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t sent = send(sock, data + total, len - total, 0);
        if (sent <= 0) return false;
        total += sent;
    }
    return true;
}

// ---------- HTTP Handling ----------
struct HttpRequest {
    string method;
    string path;
    map<string, string> headers;
};

bool parse_request(const string &raw, HttpRequest &req) {
    stringstream ss(raw);
    string line;
    if (!getline(ss, line)) return false;
    // remove CR if present
    if (!line.empty() && line.back() == '\r') line.pop_back();
    stringstream start(line);
    start >> req.method >> req.path; // ignore version for now
    // read headers
    while (getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon != string::npos) {
            string name = line.substr(0, colon);
            string value = line.substr(colon + 1);
            // trim
            auto ltrim = [](string &s) {
                size_t p = s.find_first_not_of(" \t");
                if (p != string::npos) s = s.substr(p); else s.clear();
            };
            auto rtrim = [](string &s) {
                size_t p = s.find_last_not_of(" \t");
                if (p != string::npos) s = s.substr(0, p + 1); else s.clear();
            };
            ltrim(value); rtrim(value);
            req.headers[name] = value;
        }
    }
    return true;
}

void handle_client(int client_sock, const string &doc_root, const map<string, string> &mime_map) {
    // Read request (simple: read until blank line or until buffer full)
    const size_t BUF_SZ = 8192;
    string raw;
    raw.reserve(1024);
    char buffer[BUF_SZ];
    ssize_t n;
    // set a short timeout to avoid blocking forever
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    while (true) {
        n = recv(client_sock, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        raw.append(buffer, buffer + n);
        if (raw.find("\r\n\r\n") != string::npos) break;
        if (raw.size() > 64 * 1024) break; // too large
    }

    if (raw.empty()) {
        close(client_sock);
        return;
    }

    HttpRequest req;
    if (!parse_request(raw, req)) {
        close(client_sock);
        return;
    }

    // Only support GET
    if (req.method != "GET") {
        string resp = "HTTP/1.1 501 Not Implemented\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(client_sock, resp.c_str(), resp.size());
        close(client_sock);
        return;
    }

    string path = sanitize_path(req.path);
    if (path.back() == '/') path += "index.html";

    string fullpath = doc_root + path;

    // If file doesn't exist or isn't a regular file -> 404
    struct stat st;
    if (stat(fullpath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        string body = "<html><body><h1>404 Not Found</h1></body></html>";
        stringstream ss;
        ss << "HTTP/1.1 404 Not Found\r\n";
        ss << "Content-Type: text/html\r\n";
        ss << "Content-Length: " << body.size() << "\r\n";
        ss << "Connection: close\r\n\r\n";
        ss << body;
        string resp = ss.str();
        send_all(client_sock, resp.c_str(), resp.size());
        close(client_sock);
        return;
    }

    vector<char> body;
    if (!read_file(fullpath, body)) {
        string body_s = "<html><body><h1>500 Internal Server Error</h1></body></html>";
        stringstream ss;
        ss << "HTTP/1.1 500 Internal Server Error\r\n";
        ss << "Content-Type: text/html\r\n";
        ss << "Content-Length: " << body_s.size() << "\r\n";
        ss << "Connection: close\r\n\r\n";
        ss << body_s;
        string resp = ss.str();
        send_all(client_sock, resp.c_str(), resp.size());
        close(client_sock);
        return;
    }

    string ext = get_extension(fullpath);
    string content_type = "application/octet-stream";
    auto it = mime_map.find(ext);
    if (it != mime_map.end()) content_type = it->second;

    // Build response headers
    stringstream ss;
    ss << "HTTP/1.1 200 OK\r\n";
    ss << "Content-Type: " << content_type << "\r\n";
    ss << "Content-Length: " << body.size() << "\r\n";
    ss << "Connection: close\r\n";
    ss << "Cache-Control: no-cache\r\n";
    ss << "\r\n";

    string headers = ss.str();
    if (!send_all(client_sock, headers.c_str(), headers.size())) {
        close(client_sock);
        return;
    }
    if (!body.empty()) {
        if (!send_all(client_sock, body.data(), body.size())) {
            close(client_sock);
            return;
        }
    }

    close(client_sock);
}

// ---------- Server ----------
class Server {
public:
    Server(int port, const string &doc_root, size_t threads)
        : port(port), doc_root(doc_root), pool(threads), mime_map(default_mime_types()), listen_sock(-1) {}

    bool start() {
        listen_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock < 0) {
            perror("socket");
            return false;
        }

        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (::bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind");
            close(listen_sock);
            return false;
        }

        if (listen(listen_sock, SOMAXCONN) < 0) {
            perror("listen");
            close(listen_sock);
            return false;
        }

        cout << "Server started on port " << port << " serving " << doc_root << "\n";

        // Accept loop
        while (keep_running) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_len);
            if (client_sock < 0) {
                if (errno == EINTR) continue; // likely due to signal
                if (!keep_running) break;
                perror("accept");
                continue;
            }

            // Make socket non-blocking? we'll keep blocking operations simple per-connection
            // Dispatch to thread pool
            pool.enqueue([client_sock, this] {
                handle_client(client_sock, this->doc_root, this->mime_map);
            });
        }

        close(listen_sock);
        cout << "Server stopped.\n";
        return true;
    }

private:
    int port;
    string doc_root;
    ThreadPool pool;
    map<string, string> mime_map;
    int listen_sock;
};

// ---------- main ----------
int main(int argc, char *argv[]) {
    signal(SIGINT, handle_sigint);
    int port = 8080;
    string doc_root = ".";
    size_t threads = thread::hardware_concurrency();
    if (threads == 0) threads = 4;

    if (argc >= 2) port = stoi(argv[1]);
    if (argc >= 3) doc_root = argv[2];
    if (argc >= 4) threads = stoi(argv[3]);

    // Ensure doc_root has no trailing slash (we'll use / when joining)
    if (!doc_root.empty() && doc_root.back() == '/') doc_root.pop_back();

    Server s(port, doc_root, threads);
    if (!s.start()) return 1;
    return 0;
}
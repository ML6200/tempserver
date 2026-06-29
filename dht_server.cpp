// dht_server.cpp  (v2)
// Arduino AM2302/DHT22 -> serial -> disk-backed history -> dashboard.
//
// Features over v1:
//   - Reads a config file (key=value) for port, paths, thresholds, window.
//   - Disk is the source of truth: appends every reading to a CSV log.
//     On startup it loads the recent window back from that file.
//   - Recognizes "nan,nan" from the sketch as a sensor-read error: recorded
//     with status "error" and surfaced as "connected, sensor read error".
//   - Two stacked charts (humidity, temperature). Humidity chart draws dashed
//     threshold lines for the warningimal/critical bounds from the config.
//   - /api/export streams the full CSV log for download.
//
// Build:  g++ -std=c++17 -O2 -pthread dht_server.cpp -o dht_server
// Run:    ./dht_server [config_file]      (default: ./dht.conf)

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <filesystem>
#include <netinet/in.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;

extern const char *kDashboardHtml; // defined in dashboard.cpp

// ===========================================================================
// Config
// ===========================================================================

struct Config
{
  std::string serial_port;
  int http_port = 8080;
  std::string log_file = "readings.csv";
  std::string hsrc = "dashboard.html";
  long long window_ms = 6LL * 3600 * 1000;

  double hum_crit_low = 30;
  double hum_warning_low = 40;
  double hum_warning_high = 60;
  double hum_crit_high = 70;

  double temp_crit_low = 16;
  double temp_warning_low = 18;
  double temp_warning_high = 24;
  double temp_crit_high = 28;
};

static std::string trim(const std::string &s)
{
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos)
    return "";

  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// Parse a duration like "30s", "10m", "6h", "1d", or a bare number (seconds).
static long long parseDuration(const std::string &v)
{
  if (v.empty())
    return 6LL * 3600 * 1000;

  char suffix = v.back();
  bool hasSuffix = !std::isdigit((unsigned char)suffix);
  double num = std::stod(hasSuffix ? v.substr(0, v.size() - 1) : v);

  long long mult = 1000; // default: seconds
  if (hasSuffix)
  {
    switch (std::tolower(suffix))
    {
    case 's':
      mult = 1000LL;
      break;
    case 'm':
      mult = 60LL * 1000;
      break;
    case 'h':
      mult = 3600LL * 1000;
      break;
    case 'd':
      mult = 86400LL * 1000;
      break;
    default:
      mult = 1000LL;
      break;
    }
  }

  return (long long)(num * mult);
}

static Config loadConfig(const std::string &path)
{
  Config c;

  std::ifstream f(path);
  if (!f)
  {
    std::cerr << "[config] " << path << " not found, using defaults\n";
    return c;
  }

  std::string line;
  while (std::getline(f, line))
  {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#')
      continue;

    size_t eq = t.find('=');
    if (eq == std::string::npos)
      continue;

    std::string k = trim(t.substr(0, eq));
    std::string v = trim(t.substr(eq + 1));

    if (k == "serial_port")
      c.serial_port = v;
    else if (k == "http_port")
      c.http_port = std::stoi(v);
    else if (k == "log_file")
      c.log_file = v;
    else if (k == "chart_window")
      c.window_ms = parseDuration(v);
    else if (k == "hum_crit_low")
      c.hum_crit_low = std::stod(v);
    else if (k == "hum_warning_low")
      c.hum_warning_low = std::stod(v);
    else if (k == "hum_warning_high")
      c.hum_warning_high = std::stod(v);
    else if (k == "hum_crit_high")
      c.hum_crit_high = std::stod(v);
    else if (k == "temp_crit_low")
      c.temp_crit_low = std::stod(v);
    else if (k == "temp_warning_low")
      c.temp_warning_low = std::stod(v);
    else if (k == "temp_warning_high")
      c.temp_warning_high = std::stod(v);
    else if (k == "temp_crit_high")
      c.temp_crit_high = std::stod(v);
  }

  std::cerr << "[config] loaded from " << path << "\n";
  return c;
}

// ===========================================================================
// Data model + disk-backed store
// ===========================================================================

enum class Status
{
  OK,
  Error
};

struct Reading
{
  long long epoch_ms;
  double humidity;
  double temperature;
  Status status;
};

static long long nowMs()
{
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

class DataStore
{
public:
  DataStore(std::string logPath, long long windowMs)
      : logPath_(std::move(logPath)), windowMs_(windowMs)
  {
    // Ensure the log's parent directory exists before we read or write.
    // ofstream will NOT create missing directories on its own.
    std::error_code ec;
    fs::path parent = fs::path(logPath_).parent_path();
    if (!parent.empty() && !fs::exists(parent, ec))
    {
      if (fs::create_directories(parent, ec))
        std::cerr << "[store] created log directory " << parent << "\n";
      else
        std::cerr << "[store] WARNING: could not create " << parent << ": "
                  << ec.message() << "\n";
    }

    loadFromDisk();

    bool existed = fs::exists(logPath_);
    out_.open(logPath_, std::ios::app);
    if (!out_)
    {
      std::cerr << "[store] ERROR: cannot open " << logPath_
                << " for append (cwd=" << fs::current_path()
                << ") — readings will NOT be persisted\n";
      return;
    }
    if (!existed)
      out_ << "epoch_ms,humidity,temperature,status\n";
    std::cerr << "[store] logging to " << fs::absolute(logPath_) << "\n";
  }

  void record(double h, double t, Status st)
  {
    Reading r{nowMs(), h, t, st};

    std::lock_guard<std::mutex> lk(mtx_);
    latest_ = r;
    window_.push_back(r);
    trimWindow();

    if (!out_)
      return;

    out_ << r.epoch_ms << ',';
    if (st == Status::OK)
      out_ << r.humidity << ',' << r.temperature;
    else
      out_ << "nan,nan";
    out_ << ',' << (st == Status::OK ? "ok" : "error") << '\n';
    out_.flush();
  }

  std::optional<Reading> latest()
  {
    std::lock_guard<std::mutex> lk(mtx_);
    return latest_;
  }

  std::vector<Reading> window()
  {
    std::lock_guard<std::mutex> lk(mtx_);
    return {
        window_.begin(),
        window_.end()};
  }

  std::string logPath() const { return logPath_; }

private:
  void trimWindow()
  {
    long long cutoff = nowMs() - windowMs_;
    while (!window_.empty() && window_.front().epoch_ms < cutoff)
      window_.pop_front();
  }

  void loadFromDisk()
  {
    std::ifstream f(logPath_);
    if (!f)
    {
      std::cerr << "[store] no existing log at " << logPath_
                << " (will create on first write)\n";
      return;
    }

    long long cutoff = nowMs() - windowMs_;
    bool first = true;

    std::string line;
    while (std::getline(f, line))
    {
      // Skip the CSV header if present.
      if (first)
      {
        first = false;
        if (line.rfind("epoch_ms", 0) == 0)
          continue;
      }

      std::stringstream ss(line);
      std::string a, b, c, d;
      if (!std::getline(ss, a, ','))
        continue;
      std::getline(ss, b, ',');
      std::getline(ss, c, ',');
      std::getline(ss, d, ',');

      try
      {
        long long ts = std::stoll(a);
        if (ts < cutoff)
          continue;

        Status st = (trim(d) == "ok") ? Status::OK : Status::Error;

        double h = 0, t = 0;
        if (st == Status::OK)
        {
          h = std::stod(b);
          t = std::stod(c);
        }

        Reading r{ts, h, t, st};
        window_.push_back(r);
        latest_ = r;
      }
      catch (...)
      {
        // Malformed line: skip it.
      }
    }

    std::cerr << "[store] loaded " << window_.size()
              << " readings within window from disk\n";
  }

  std::string logPath_;
  long long windowMs_;
  std::mutex mtx_;
  std::ofstream out_;
  std::optional<Reading> latest_;
  std::deque<Reading> window_;
};

// ===========================================================================
// Serial
// ===========================================================================

static bool nameLooksLikeArduino(const std::string &s)
{
  std::string l;
  for (char c : s)
    l.push_back(std::tolower((unsigned char)c));

  return l.find("arduino") != std::string::npos ||
         l.find("ch340") != std::string::npos ||
         l.find("ch341") != std::string::npos ||
         l.find("ftdi") != std::string::npos ||
         l.find("usbserial") != std::string::npos ||
         l.find("acm") != std::string::npos;
}

static std::optional<std::string> autodetectPort()
{
  const fs::path byId = "/dev/serial/by-id";
  std::error_code ec;

  // Prefer the stable by-id symlinks if available.
  if (fs::exists(byId, ec))
  {
    for (const auto &e : fs::directory_iterator(byId, ec))
    {
      if (nameLooksLikeArduino(e.path().filename().string()))
      {
        fs::path real = fs::canonical(e.path(), ec);
        return ec ? e.path().string() : real.string();
      }
    }
  }

  // Fall back to scanning the common device nodes.
  for (const char *pat : {"/dev/ttyACM", "/dev/ttyUSB"})
  {
    for (int i = 0; i < 8; ++i)
    {
      std::string p = std::string(pat) + std::to_string(i);
      if (fs::exists(p, ec))
        return p;
    }
  }

  return std::nullopt;
}

static int openSerial(const std::string &dev)
{
  int fd = ::open(dev.c_str(), O_RDONLY | O_NOCTTY);
  if (fd < 0)
    return -1;

  termios tty{};
  if (tcgetattr(fd, &tty) != 0)
  {
    ::close(fd);
    return -1;
  }

  cfsetispeed(&tty, B9600);
  cfsetospeed(&tty, B9600);

  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_cflag &= ~PARENB;
  tty.c_cflag &= ~CSTOPB;
  tty.c_cflag &= ~CSIZE;
  tty.c_cflag |= CS8;
  tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
  tty.c_iflag &= ~(IXON | IXOFF | IXANY | INLCR | ICRNL);
  tty.c_oflag &= ~OPOST;
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 10;

  if (tcsetattr(fd, TCSANOW, &tty) != 0)
  {
    ::close(fd);
    return -1;
  }

  return fd;
}

// Parse one serial line into a reading.
// Returns false for blank lines, the "boot" banner, or garbage.
static bool parseLine(const std::string &line, double &h, double &t,
                      Status &st)
{
  std::string s = trim(line);
  if (s.empty())
    return false;

  std::string low;
  for (char c : s)
    low.push_back(std::tolower((unsigned char)c));

  if (low == "nan,nan")
  {
    st = Status::Error;
    return true;
  }
  if (low == "boot")
    return false;

  std::istringstream iss(s);
  char comma;
  if ((iss >> h >> comma >> t) && comma == ',')
  {
    st = Status::OK;
    return true;
  }

  return false;
}

static void serialLoop(DataStore &store, std::string fixedPort,
                       std::atomic<bool> &running)
{
  std::string lineBuf;

  while (running)
  {
    // Resolve the port (fixed from config, or autodetected).
    std::string port = fixedPort;
    if (port.empty())
    {
      auto p = autodetectPort();
      if (!p)
      {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        continue;
      }
      port = *p;
    }

    int fd = openSerial(port);
    if (fd < 0)
    {
      std::cerr << "[serial] cannot open " << port << ", retrying...\n";
      std::this_thread::sleep_for(std::chrono::seconds(3));
      continue;
    }
    std::cerr << "[serial] connected: " << port << "\n";

    // Read byte-by-byte, splitting on CR/LF into lines.
    char buf[256];
    while (running)
    {
      ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n < 0)
        break;
      if (n == 0)
        continue;

      for (ssize_t i = 0; i < n; ++i)
      {
        char c = buf[i];
        if (c == '\n' || c == '\r')
        {
          if (!lineBuf.empty())
          {
            double h = 0, t = 0;
            Status st;
            if (parseLine(lineBuf, h, t, st))
              store.record(h, t, st);
            lineBuf.clear();
          }
        }
        else if (lineBuf.size() < 128)
        {
          lineBuf.push_back(c);
        }
      }
    }

    ::close(fd);
    std::cerr << "[serial] disconnected, will retry\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
}

// ===========================================================================
// HTTP
// ===========================================================================

static void sendAll(int cfd, const std::string &s)
{
  size_t off = 0;
  while (off < s.size())
  {
    ssize_t n = ::write(cfd, s.data() + off, s.size() - off);
    if (n <= 0)
      break;
    off += n;
  }
}

static void httpRespond(int cfd, const std::string &status,
                        const std::string &ctype, const std::string &body,
                        const std::string &extra = "")
{
  std::ostringstream os;
  os << "HTTP/1.1 " << status << "\r\n"
     << "Content-Type: " << ctype << "\r\n"
     << "Content-Length: " << body.size() << "\r\n"
     << "Access-Control-Allow-Origin: *\r\n"
     << extra << "Connection: close\r\n\r\n"
     << body;
  sendAll(cfd, os.str());
}

static std::string jsonLatest(DataStore &store)
{
  auto r = store.latest();
  if (!r)
    return R"({"ok":false})";

  std::ostringstream os;
  os << "{\"ok\":true,\"status\":\""
     << (r->status == Status::OK ? "ok" : "error")
     << "\",\"age_ms\":" << (nowMs() - r->epoch_ms);

  if (r->status == Status::OK)
    os << ",\"humidity\":" << r->humidity
       << ",\"temperature\":" << r->temperature;

  os << "}";
  return os.str();
}

static std::string jsonHistory(DataStore &store)
{
  auto w = store.window();

  // Build four parallel JSON arrays: timestamps, humidity, temp, status.
  std::ostringstream ts, hs, tps, sts;
  ts << "[";
  hs << "[";
  tps << "[";
  sts << "[";

  bool first = true;
  for (auto &r : w)
  {
    if (!first)
    {
      ts << ",";
      hs << ",";
      tps << ",";
      sts << ",";
    }
    first = false;

    ts << r.epoch_ms;
    sts << "\"" << (r.status == Status::OK ? "ok" : "error") << "\"";

    if (r.status == Status::OK)
    {
      hs << r.humidity;
      tps << r.temperature;
    }
    else
    {
      hs << "null";
      tps << "null";
    }
  }

  ts << "]";
  hs << "]";
  tps << "]";
  sts << "]";

  return "{\"t\":" + ts.str() + ",\"humidity\":" + hs.str() +
         ",\"temperature\":" + tps.str() + ",\"status\":" + sts.str() + "}";
}

static std::string jsonConfig(const Config &c)
{
  std::ostringstream os;
  os << "{\"hum_crit_low\":" << c.hum_crit_low
     << ",\"hum_warning_low\":" << c.hum_warning_low
     << ",\"hum_warning_high\":" << c.hum_warning_high
     << ",\"hum_crit_high\":" << c.hum_crit_high
     << ",\"temp_crit_low\":" << c.temp_crit_low
     << ",\"temp_warning_low\":" << c.temp_warning_low
     << ",\"temp_warning_high\":" << c.temp_warning_high
     << ",\"temp_crit_high\":" << c.temp_crit_high
     << ",\"window_ms\":" << c.window_ms << "}";
  return os.str();
}

static std::string readFileToString(const std::string &path)
{
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return "";

  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Extract the request path from the first line of an HTTP request.
static std::string parseRequestPath(const std::string &req)
{
  size_t sp = req.find(' ');
  if (sp == std::string::npos)
    return "/";

  size_t sp2 = req.find(' ', sp + 1);
  if (sp2 == std::string::npos)
    return "/";

  return req.substr(sp + 1, sp2 - sp - 1);
}

static void httpLoop(DataStore &store, const Config &cfg,
                     std::atomic<bool> &running)
{
  int sfd = ::socket(AF_INET, SOCK_STREAM, 0);
  int yes = 1;
  setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(cfg.http_port);

  if (bind(sfd, (sockaddr *)&addr, sizeof(addr)) < 0)
  {
    std::cerr << "[http] bind failed on port " << cfg.http_port << "\n";
    running = false;
    return;
  }

  listen(sfd, 16);
  std::cerr << "[http] listening on http://0.0.0.0:" << cfg.http_port << "\n";

  while (running)
  {
    int cfd = ::accept(sfd, nullptr, nullptr);
    if (cfd < 0)
      continue;

    char req[2048];
    ssize_t n = ::read(cfd, req, sizeof(req) - 1);
    if (n <= 0)
    {
      ::close(cfd);
      continue;
    }
    req[n] = 0;

    std::string path = parseRequestPath(req);
    std::string kDashboardHtml = readFileToString(cfg.hsrc);

    if (path == "/" || path == "/index.html")
      httpRespond(cfd, "200 OK", "text/html; charset=utf-8", kDashboardHtml);
    else if (path == "/api/latest")
      httpRespond(cfd, "200 OK", "application/json", jsonLatest(store));
    else if (path == "/api/history")
      httpRespond(cfd, "200 OK", "application/json", jsonHistory(store));
    else if (path == "/api/config")
      httpRespond(cfd, "200 OK", "application/json", jsonConfig(cfg));
    else if (path == "/api/export" || path == "/api/export.csv")
      httpRespond(
          cfd, "200 OK", "text/csv", readFileToString(store.logPath()),
          "Content-Disposition: attachment; filename=\"readings.csv\"\r\n");
    else
      httpRespond(cfd, "404 Not Found", "text/plain", "not found");

    ::close(cfd);
  }

  ::close(sfd);
}

// ===========================================================================
// Entry point
// ===========================================================================

int main(int argc, char **argv)
{
  std::string cfgPath = (argc >= 2) ? argv[1] : "dht.conf";
  Config cfg = loadConfig(cfgPath);

  DataStore store(cfg.log_file, cfg.window_ms);
  std::atomic<bool> running{true};

  std::thread serial(serialLoop, std::ref(store), cfg.serial_port,
                     std::ref(running));
  std::thread http(httpLoop, std::ref(store), std::ref(cfg), std::ref(running));

  serial.join();
  http.join();
  return 0;
}

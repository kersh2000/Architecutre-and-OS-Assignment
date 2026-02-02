#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <bcrypt.h>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

// -----------------------------
// Job model (kept close to Python)
// -----------------------------
struct Job {
    fs::path path;

    int arrival = 0;
    int est_cost = 1;
    int remaining = 1;
    int queue_level = 0;
};

// -----------------------------
// Small helpers
// -----------------------------
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);

    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

static void runFind(const fs::path& root, unsigned long long minBytes) {
    std::error_code ec;

    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator();
         it.increment(ec)) {

        if (ec) { ec.clear(); continue; }

        const auto& e = *it;
        if (!e.is_regular_file()) continue;

        auto sz = e.file_size(ec);
        if (ec) { ec.clear(); continue; }

        if (sz > minBytes) {
            std::cout << e.path() << " (" << sz << " bytes)\n";
        }
    }
}


static std::wstring algoToBcryptId(const std::string& algo) {
    if (algo == "sha256") return BCRYPT_SHA256_ALGORITHM;
    if (algo == "sha1")   return BCRYPT_SHA1_ALGORITHM;
    if (algo == "md5")    return BCRYPT_MD5_ALGORITHM;
    throw std::runtime_error("Unknown hash algorithm: " + algo + " (use sha256, sha1, md5)");
}

static std::string bytesToHex(const std::vector<unsigned char>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char b : bytes) oss << std::setw(2) << (int)b;
    return oss.str();
}

static std::string hashFile(const fs::path& file, const std::string& algo) {
    std::wstring algId = algoToBcryptId(algo);

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;

    DWORD objLen = 0, cbData = 0, hashLen = 0;

    if (BCryptOpenAlgorithmProvider(&hAlg, algId.c_str(), nullptr, 0) != 0) {
        throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    }

    if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                          (PUCHAR)&objLen, sizeof(objLen), &cbData, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        throw std::runtime_error("BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed");
    }

    if (BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                          (PUCHAR)&hashLen, sizeof(hashLen), &cbData, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        throw std::runtime_error("BCryptGetProperty(BCRYPT_HASH_LENGTH) failed");
    }

    std::vector<unsigned char> obj(objLen);
    std::vector<unsigned char> hash(hashLen);

    if (BCryptCreateHash(hAlg, &hHash, obj.data(), objLen, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        throw std::runtime_error("BCryptCreateHash failed");
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        throw std::runtime_error("Could not open file: " + file.string());
    }

    std::vector<unsigned char> buf(1 << 15); // 32 KiB
    while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)buf.size());
        std::streamsize got = in.gcount();
        if (got <= 0) break;

        if (BCryptHashData(hHash, buf.data(), (ULONG)got, 0) != 0) {
            BCryptDestroyHash(hHash);
            BCryptCloseAlgorithmProvider(hAlg, 0);
            throw std::runtime_error("BCryptHashData failed");
        }
    }

    if (BCryptFinishHash(hHash, hash.data(), (ULONG)hash.size(), 0) != 0) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        throw std::runtime_error("BCryptFinishHash failed");
    }

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    return bytesToHex(hash);
}


static std::string permsToString(fs::perms p) {
    auto bit = [&](fs::perms b) { return (p & b) != fs::perms::none; };

    std::string s;
    s += bit(fs::perms::owner_read)  ? 'r' : '-';
    s += bit(fs::perms::owner_write) ? 'w' : '-';
    s += bit(fs::perms::owner_exec)  ? 'x' : '-';

    s += bit(fs::perms::group_read)  ? 'r' : '-';
    s += bit(fs::perms::group_write) ? 'w' : '-';
    s += bit(fs::perms::group_exec)  ? 'x' : '-';

    s += bit(fs::perms::others_read)  ? 'r' : '-';
    s += bit(fs::perms::others_write) ? 'w' : '-';
    s += bit(fs::perms::others_exec)  ? 'x' : '-';

    return s;
}

static long long fileTimeToEpochSeconds(fs::file_time_type ft) {
    using namespace std::chrono;
    auto sctp = time_point_cast<system_clock::duration>(
        ft - fs::file_time_type::clock::now() + system_clock::now()
    );
    return duration_cast<seconds>(sctp.time_since_epoch()).count();
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// -----------------------------
// Indexing work (similar to Python's index())
// -----------------------------
static std::string scanOnePathJson(const fs::path& p) {
    std::string pathStr = p.string();
    std::string nameStr = p.filename().string();
    std::string extStr  = toLower(p.extension().string());

    bool is_file = false;
    bool is_dir = false;
    bool is_symlink = false;

    unsigned long long size_bytes = 0;
    long long mtime_epoch = 0;

    std::string perms_rwx = "---------";
    std::string error;

    try {
        fs::file_status st = fs::symlink_status(p);

        is_symlink = fs::is_symlink(st);
        is_file    = fs::is_regular_file(st);
        is_dir     = fs::is_directory(st);

        if (is_file) {
            size_bytes = fs::file_size(p);
        }

        mtime_epoch = fileTimeToEpochSeconds(fs::last_write_time(p));
        perms_rwx   = permsToString(st.permissions());
    }
    catch (const fs::filesystem_error& e) {
        error = e.what();
    }
    catch (const std::exception& e) {
        error = e.what();
    }

    std::string json = "{";
    json += "\"path\":\"" + jsonEscape(pathStr) + "\",";
    json += "\"name\":\"" + jsonEscape(nameStr) + "\",";
    json += "\"is_file\":" + std::string(is_file ? "true" : "false") + ",";
    json += "\"is_dir\":" + std::string(is_dir ? "true" : "false") + ",";
    json += "\"is_symlink\":" + std::string(is_symlink ? "true" : "false") + ",";
    json += "\"size_bytes\":" + std::to_string(size_bytes) + ",";
    json += "\"mtime_epoch\":" + std::to_string(mtime_epoch) + ",";
    json += "\"extension\":\"" + jsonEscape(extStr) + "\",";
    json += "\"perms_rwx\":\"" + jsonEscape(perms_rwx) + "\"";

    if (!error.empty()) {
        json += ",\"error\":\"" + jsonEscape(error) + "\"";
    }

    json += "}";
    return json;
}

// -----------------------------
// Job creation (similar to build_jobs())
// -----------------------------
static std::vector<Job> buildJobs(const fs::path& root) {
    std::vector<Job> jobs;
    int tick = 0;

    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        throw std::runtime_error("Root directory not found: " + root.string() +
                                 " (CWD: " + fs::current_path().string() + ")");
    }

    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator();
         it.increment(ec)) {

        if (ec) { ec.clear(); continue; }

        const auto& entry = *it;

        std::error_code ec2;
        if (!entry.is_regular_file(ec2)) { ec2.clear(); continue; }

        ++tick;
        fs::path p = entry.path();

        unsigned long long size_bytes = entry.file_size(ec2);
        if (ec2) { ec2.clear(); size_bytes = 0; }

        int est = 1;
        if (size_bytes < 100000ULL) est = 1;
        else if (size_bytes < 10000000ULL) est = 2;
        else est = 3;

        Job j;
        j.path = p;
        j.arrival = tick;
        j.est_cost = est;
        j.remaining = est;
        j.queue_level = 0;

        jobs.push_back(std::move(j));
    }

    // Keep arrival sort as in template (we'll do SJF selection in chooseNextJob).
    std::sort(jobs.begin(), jobs.end(),
              [](const Job& a, const Job& b) { return a.arrival < b.arrival; });

    return jobs;
}

// -----------------------------
// Scheduler hook (SJF now)
// -----------------------------
static std::optional<Job> chooseNextJob(std::deque<Job>& ready, int /*tick*/) {
    if (ready.empty()) return std::nullopt;

    // SJF: choose job with smallest est_cost; tie-break by arrival.
    auto it = std::min_element(ready.begin(), ready.end(),
        [](const Job& a, const Job& b) {
            if (a.est_cost != b.est_cost) return a.est_cost < b.est_cost;
            return a.arrival < b.arrival;
        });

    Job j = *it;
    ready.erase(it);
    return j;
}

static void onJobFeedback(Job& /*job*/, const std::string& /*jsonRecord*/) {
    // Optional: adjust job.queue_level or est_cost based on record/error
}

// -----------------------------
// Thread-pool (Variant A)
// -----------------------------
class ThreadPool {
public:
    explicit ThreadPool(size_t n) : stopping_(false) {
        threads_.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            threads_.emplace_back([this]() {
                for (;;) {
                    std::function<void()> fn;
                    {
                        std::unique_lock<std::mutex> lk(mu_);
                        cv_.wait(lk, [&] { return stopping_ || !q_.empty(); });
                        if (stopping_ && q_.empty()) return;
                        fn = std::move(q_.front());
                        q_.pop();
                    }
                    fn();
                }
            });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }

    void submit(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_.push(std::move(fn));
        }
        cv_.notify_one();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> q_;
    std::vector<std::thread> threads_;
    bool stopping_;
};

// -----------------------------
// Simulation loop (now concurrent)
// - Uses same function name runIndexer()
// - Uses SJF chooseNextJob() to feed an in-process queue
// - Prints elapsed time (and optionally writes JSONL)
// -----------------------------
static double runIndexer(const fs::path& root, const fs::path& outputJsonl, int workers) {
    constexpr bool WRITE_JSONL = false; // set true if you must write file records

    std::vector<Job> jobs = buildJobs(root);
    std::deque<Job> ready(jobs.begin(), jobs.end());

    std::ofstream out;
    if constexpr (WRITE_JSONL) {
        out.open(outputJsonl, std::ios::binary);
        if (!out) throw std::runtime_error("Could not open output file for writing.");
    }

    std::mutex ready_mu;
    std::mutex out_mu; // protect output stream if enabled
    std::atomic<uint64_t> sink{0}; // prevents optimizer from deleting the work

    auto t0 = std::chrono::steady_clock::now();
    {
        ThreadPool pool(static_cast<size_t>(workers));

        for (int w = 0; w < workers; ++w) {
            pool.submit([&]() {
                int tick_local = 0;

                for (;;) {
                    Job job;
                    {
                        std::lock_guard<std::mutex> lk(ready_mu);
                        if (ready.empty()) return;
                        ++tick_local; // local tick for worker (global tick isn't meaningful with concurrency)

                        auto next = chooseNextJob(ready, tick_local);
                        if (!next.has_value()) return;
                        job = next.value();
                    }

                    // "Run" job
                    std::string record = scanOnePathJson(job.path);

                    // Append scheduling fields (like Python task())
                    if (!record.empty() && record.back() == '}') {
                        record.pop_back();
                        record += ",\"arrival\":" + std::to_string(job.arrival);
                        record += ",\"est_cost\":" + std::to_string(job.est_cost);
                        record += ",\"queue_level\":" + std::to_string(job.queue_level);
                        record += "}";
                    }

                    onJobFeedback(job, record);

                    // Optional output (disabled by default to avoid I/O dominating benchmarks)
                    if constexpr (WRITE_JSONL) {
                        std::lock_guard<std::mutex> lk(out_mu);
                        out << record << "\n";
                    }

                    // Cheap “use” of data to avoid full DCE under optimization flags
                    sink.fetch_add(static_cast<uint64_t>(record.size()), std::memory_order_relaxed);
                }
            });
        }
    }
    auto t1 = std::chrono::steady_clock::now();

    std::chrono::duration<double> elapsed = t1 - t0;
    (void)sink; // keep sink alive

    return elapsed.count();
}

// -----------------------------
// main: prints elapsed time only
// Variant B is achieved by recompiling with -O0 vs -O2 etc.
// -----------------------------
int main(int argc, char** argv) {
    try {
        fs::path root = "testFiles";

        if (argc >= 2) {
            std::string cmd = argv[1];

            // --------------------
            // find > X
            // --------------------
            if (cmd == "find" && argc >= 3) {
                unsigned long long minBytes =
                    std::stoull(argv[2]);
                runFind(root, minBytes);
                return 0;
            }

            // --------------------
            // checksum file [--hash algo]
            // --------------------
            if (cmd == "checksum" && argc >= 3) {
                fs::path file = argv[2];
                std::string algo = "sha256";

                if (argc >= 5 && std::string(argv[3]) == "--hash") {
                    algo = argv[4];
                }

                std::string h = hashFile(file, algo);
                std::cout << algo << "(" << file << ") = " << h << "\n";
                return 0;
            }
        }

        // --------------------
        // Default: run indexer benchmark
        // --------------------
        int workers = (argc >= 2)
            ? std::max(1, std::atoi(argv[1]))
            : (int)std::thread::hardware_concurrency();

        if (workers <= 0) workers = 4;

        double elapsed = runIndexer(root, "index_results.jsonl", workers);

        std::cout << "variant=A(thread_pool)"
                  << " workers=" << workers
                  << " elapsed_sec=" << elapsed
                  << "\n";
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

#include <iostream>           // std::cout, std::cerr - for console output
#include <cstring>            // strlen() - for string manipulation
#include <sys/socket.h>       // socket(), bind(), listen(), accept() - for socket operations
#include <sys/sendfile.h>     // sendfile - for efficient file sending
#include <sys/stat.h>         // fstat - to get file status
#include <sys/uio.h>          // writev - to write to multiple buffers
#include <sys/mman.h>         // mmap - for memory-mapped file access
#include <arpa/inet.h>        // inet_ntoa - for converting IP addresses
#include <netinet/tcp.h>      // TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT - TCP connection keepalive options
#include <netinet/in.h>       // sockaddr_in - structure for IPv4 addresses
#include <unistd.h>           // close() function - to close file descriptors
#include <sys/epoll.h>        // epoll - for scalable I/O event notification
#include <fstream>            // file reading operations
#include <sstream>            // string stream manipulations
#include <filesystem>         // filesystem operations
#include <ctime>              // handling timestamps
#include <fcntl.h>            // file control options
#include <map>                // ordered associative container (Red-Black Tree)
#include <set>                // ordered unique elements (Red-Black Tree)
#include <deque>              // double-ended queue
#include <list>               // doubly linked list for cache implementation
#include <thread>             // multithreading support
#include <vector>             // dynamic array
#include <algorithm>          // applying transformations
#include <queue>              // queue data structure
#include <mutex>              // thread synchronization
#include <condition_variable> // blocking thread synchronization
#include <functional>         // function objects
#include <unordered_map>      // unordered associative container (Hash Table)
#include <unordered_set>      // unordered unique elements (Hash Table)
#include <chrono>             // measuring time
#include <shared_mutex>       // shared mutexes
#include <memory>             // smart pointers
#include <memory_resource>    // memory resource management
#include <array>              // fixed-size arrays
#include <string_view>        // efficient string handling without ownership
#include <new>                // memory alignment
#include <future>             // asynchronous tasks
#include <csignal>            // signal handling
#include <atomic>             // atomic operations
#include <zlib.h>             // zlib compression
#include <stdexcept>          // standard exceptions like std::runtime_error
#include <nlohmann/json.hpp>  // JSON parsing

#include "terminal_utils.h"

namespace fs = std::filesystem; // Alias for filesystem namespace
using json = nlohmann::json;    // Alias for JSON namespace

struct Config
{
    int port;                 // prot for server
    std::string staticFolder; // path for static files
    int threadCount;          // thread count of worker threads
    struct
    {
        int maxRequests; // maximum number of requests allowed within time window
        int timeWindow;  // duration of time window for rate limiting
    } rateLimit;
    struct
    {
        size_t sizeMB;     // maximum size of cache in MB
        int maxAgeSeconds; // maximum age of cache entries in seconds
    } cache;
};

// Connection information structure
struct ConnectionInfo
{
    std::chrono::steady_clock::time_point startTime; // connection start time
    std::string ip;                                  // client IP address
    bool isLogged;                                   // flag to track if connection is logged
    bool isClosureLogged;                            // flag to track if connection closure is logged
    uint64_t bytesReceived;                          // bytes received from client
    uint64_t bytesSent;                              // bytes sent to client
    std::vector<std::string> logBuffer;              // buffer for storing logs

    ConnectionInfo(const std::chrono::steady_clock::time_point &time,
                   const std::string &ipAddr,
                   bool logged = false,
                   bool closureLogged = false,
                   uint64_t received = 0,
                   uint64_t sent = 0)
        : startTime(time), ip(ipAddr), isLogged(logged),
          isClosureLogged(closureLogged), bytesReceived(received),
          bytesSent(sent) {}
};
class Logger
{
private:
    // basic logger members
    std::mutex logMutex;   // mutex to protect log file access
    std::ofstream logFile; // log file stream
    static Logger *instance;
    bool isWaitingForEvents;                                                 // new flag to track event waiting state
    std::chrono::steady_clock::time_point lastEventWaitLog;                  // track last event wait log time
    static constexpr auto EVENT_WAIT_LOG_INTERVAL = std::chrono::seconds(5); // log interval for waiting events

    // structure to hold log message data
    struct LogMessage
    {
        std::string message;                             // actual log message
        std::string level;                               // log level (INFO, ERROR, etc.)
        std::string ip;                                  // iP address associated with log
        std::chrono::system_clock::time_point timestamp; // when log was created

        LogMessage(const std::string &msg, const std::string &lvl, const std::string &clientIp)
            : message(msg), level(lvl), ip(clientIp),
              timestamp(std::chrono::system_clock::now()) {}
    };

    // async logging members
    std::queue<LogMessage> messageQueue; // queue to store pending log messages
    std::mutex queueMutex;               // mutex to protect message queue
    std::condition_variable queueCV;     // condition variable for queue synchronization
    std::thread loggerThread;            // background thread for processing logs
    std::atomic<bool> running{true};     // flag to control background thread

    Logger() : isWaitingForEvents(false)
    {
        logFile.open("pgs.log", std::ios::app); // open log file in append mode
        // start background logging thread
        loggerThread = std::thread(&Logger::processLogs, this);
        // deliberately don't call info() here to avoid recursion
        writeLogMessage(LogMessage("Logger initialized", "SUCCESS", "-"));
    }

    // process logs in background thread
    void processLogs()
    {
        while (running)
        {
            std::vector<LogMessage> messages; // batch of log messages to write
            {
                // wait for new messages or shutdown signal
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCV.wait_for(lock, std::chrono::seconds(1),
                                 [this]
                                 { return !messageQueue.empty() || !running; });

                // batch process up to 100 messages at once for better performance
                while (!messageQueue.empty() && messages.size() < 100)
                {
                    messages.push_back(std::move(messageQueue.front()));
                    messageQueue.pop();
                }
            }

            // write batched messages to file
            if (!messages.empty())
            {
                std::lock_guard<std::mutex> lock(logMutex);
                for (const auto &msg : messages)
                {
                    writeLogMessage(msg);
                }
                logFile.flush();
            }
        }
    }

    // format and write a single log message
    void writeLogMessage(const LogMessage &msg)
    {
        std::string logMessage = formatLogMessage(msg);

        logFile << logMessage << std::endl;

        // Output to console with appropriate color
        if (msg.level == "ERROR")
        {
            std::cout << TerminalUtils::error(logMessage) << std::endl;
        }
        else if (msg.level == "WARNING")
        {
            std::cout << TerminalUtils::warning(logMessage) << std::endl;
        }
        else if (msg.level == "SUCCESS")
        {
            std::cout << TerminalUtils::success(logMessage) << std::endl;
        }
        else
        {
            std::cout << TerminalUtils::info(logMessage) << std::endl;
        }
    }

    // format timestamp and log message
    [[nodiscard]]
    std::string formatLogMessage(const LogMessage &msg)
    {
        auto time = std::chrono::system_clock::to_time_t(msg.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      msg.timestamp.time_since_epoch()) %
                  1000;

        char timestamp[32]; // buffer for timestamp
        std::strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S]",
                      std::localtime(&time)); // format timestamp

        return std::string(timestamp) + "." + std::to_string(ms.count()) +
               " [" + msg.level + "] [" + msg.ip + "] " + msg.message;
    }

public:
    // singleton pattern implementation
    static Logger *getInstance()
    {
        if (instance == nullptr)
        {
            instance = new Logger();
        }
        return instance;
    }

    static void destroyInstance() // clean up
    {
        delete instance;
        instance = nullptr;
    }

    // ensure proper cleanup in destructor
    ~Logger()
    {
        running = false;      // signal background thread to stop
        queueCV.notify_one(); // wake up background thread

        if (loggerThread.joinable())
        {
            loggerThread.join(); // wait for background thread to finish
        }

        if (logFile.is_open())
        {
            logFile.close();
        }
    }

    // main logging function
    void log(const std::string &message, const std::string &level = "INFO", const std::string &ip = "-")
    {
        // handle special case for "Waiting for events" messages
        if (message == "Waiting for events...")
        {
            std::lock_guard<std::mutex> lock(logMutex);
            auto now = std::chrono::steady_clock::now();

            if (!isWaitingForEvents ||
                (now - lastEventWaitLog) >= EVENT_WAIT_LOG_INTERVAL) // check if interval has passed
            {
                isWaitingForEvents = true;
                lastEventWaitLog = now;
            }
            else
            {
                return; // skip logging if we're already waiting and interval hasn't passed
            }
        }
        else
        {
            isWaitingForEvents = false; // reset waiting flag
        }

        // queue log message for async processing
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            messageQueue.emplace(message, level, ip);
        }
        queueCV.notify_one(); // notify background thread
    }

    // convenience methods for different log levels
    void error(const std::string &message, const std::string &ip = "-")
    {
        log(message, "ERROR", ip);
    }

    void warning(const std::string &message, const std::string &ip = "-")
    {
        log(message, "WARNING", ip);
    }

    void success(const std::string &message, const std::string &ip = "-")
    {
        log(message, "SUCCESS", ip);
    }

    void info(const std::string &message, const std::string &ip = "-")
    {
        log(message, "INFO", ip);
    }
};

Logger *Logger::instance = nullptr; // initialize static singleton instance

class Cache
{
private:
    // Cache entry structure for storing file content and metadata - O(1) access time
    struct CacheEntry
    {
        std::vector<char> data;                       // actual content of cached file
        std::string mimeType;                         // MIME type of cached content
        time_t lastModified;                          // last modification time of file
        std::list<std::string>::iterator lruIterator; // iterator pointing to key's position in LRU list

        CacheEntry() : lastModified(0) {}

        // constructor for standard vector - O(n) for data copy
        CacheEntry(const std::vector<char> &d, const std::string &m, time_t lm,
                   std::list<std::string>::iterator it)
            : data(d), mimeType(m), lastModified(lm), lruIterator(it) {}

        // constructor for any vector-like container - O(n) for data copy
        template <typename Vector>
        CacheEntry(const Vector &d, const std::string &m, time_t lm,
                   std::list<std::string>::iterator it)
            : data(d.begin(), d.end()), mimeType(m), lastModified(lm), lruIterator(it) {}
    };

    std::unordered_map<std::string, CacheEntry> cache; // main cache storage (key -> entry mapping)
    std::list<std::string> lruList;                    // LRU order tracking list (most recent -> least recent)
    mutable std::shared_mutex mutex;                   // mutex for thread-safe operations
    size_t maxSize;                                    // maximum size of cache in bytes
    size_t currentSize;                                // current size of cache in bytes
    std::chrono::seconds maxAge;                       // maximum age of cache entries

    // helper function to update LRU order - O(1) operation
    void updateLRU(const std::string &key)
    {
        lruList.erase(cache[key].lruIterator);    // remove from current position
        lruList.push_front(key);                  // add to front (most recently used)
        cache[key].lruIterator = lruList.begin(); // update iterator
    }

public:
    // constructor with overflow check - O(1)
    explicit Cache(size_t maxSizeMB, std::chrono::seconds maxAge)
        : maxSize(static_cast<size_t>(maxSizeMB) * 1024 * 1024),
          currentSize(0),
          maxAge(maxAge)
    {
        // check for cache size overflow
        if (maxSize / (1024 * 1024) != maxSizeMB) // calculate: 1024*1024=1048576(1MB)
        {
            throw std::overflow_error("Cache size overflow");
        }
    }

    // retrieve an item from cache - O(1) average case
    template <typename Vector>
    bool get(const std::string &key, Vector &data, std::string &mimeType, time_t &lastModified)
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end())
        {
            data.assign(it->second.data.begin(), it->second.data.end());
            mimeType = it->second.mimeType;
            lastModified = it->second.lastModified;

            // update LRU order under exclusive lock
            lock.unlock();
            std::unique_lock<std::shared_mutex> uniqueLock(mutex);
            updateLRU(key);
            return true;
        }
        return false; // cache miss
    }

    // add or update an item in cache - O(1) average case
    template <typename Vector>
    void set(const std::string &key, const Vector &data,
             const std::string &mimeType, time_t lastModified)
    {
        std::unique_lock<std::shared_mutex> lock(mutex);

        // if entry already exists, remove it first
        auto it = cache.find(key);
        if (it != cache.end())
        {
            currentSize -= it->second.data.size();
            lruList.erase(it->second.lruIterator);
            cache.erase(it);
        }

        // if new entry is too large, don't cache it
        if (data.size() > maxSize)
        {
            return;
        }

        // remove least recently used entries until we have enough space
        while (!lruList.empty() && currentSize + data.size() > maxSize)
        {
            const std::string &lruKey = lruList.back();
            currentSize -= cache[lruKey].data.size();
            cache.erase(lruKey);
            lruList.pop_back();
        }

        // add new entry to front of LRU list and cache
        lruList.push_front(key);
        try
        {
            cache.emplace(key, CacheEntry(data, mimeType, lastModified, lruList.begin()));
            currentSize += data.size();
        }
        catch (const std::exception &e)
        {
            lruList.pop_front(); // rollback on failure
            Logger::getInstance()->error("Cache allocation failed: " + std::string(e.what()));
        }
    }

    // clear all items from cache - O(1)
    void clear()
    {
        std::unique_lock<std::shared_mutex> lock(mutex);
        cache.clear();
        lruList.clear();
        currentSize = 0;
    }

    // remove a specific item from cache - O(1) average case
    bool remove(const std::string &key)
    {
        std::unique_lock<std::shared_mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end())
        {
            currentSize -= it->second.data.size();
            lruList.erase(it->second.lruIterator);
            cache.erase(it);
            return true;
        }
        return false;
    }

    // check if an item exists in cache - O(1) average case
    bool exists(const std::string &key)
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return cache.find(key) != cache.end();
    }

    // get current size of cache in bytes - O(1)
    size_t size() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return currentSize;
    }

    // get number of items in cache - O(1)
    size_t count() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return cache.size();
    }

    // get maximum age of cache entries - O(1)
    std::chrono::seconds getMaxAge() const
    {
        return maxAge; // no lock needed for immutable value
    }

    // set a new maximum age for cache entries - O(1)
    void setMaxAge(std::chrono::seconds newMaxAge)
    {
        std::unique_lock<std::shared_mutex> lock(mutex);
        maxAge = newMaxAge;
    }

    // structure to hold cache statistics
    struct CacheStats
    {
        size_t currentSize;          // current cache size in bytes
        size_t maxSize;              // maximum cache size in bytes
        size_t itemCount;            // number of items in cache
        std::chrono::seconds maxAge; // maximum age of cache entries
    };

    // get cache statistics - O(1)
    CacheStats getStats() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return {currentSize, maxSize, cache.size(), maxAge};
    }
};

class Middleware
{
public:
    virtual ~Middleware() = default;
    virtual std::string process(const std::string &data) = 0;
};

class RateLimiter : public Middleware
{
public:
    RateLimiter(size_t maxRequests, std::chrono::seconds timeWindow)
        : maxRequests(maxRequests), timeWindow(timeWindow) {}

    std::string process(const std::string &data) override // override process method
    {
        std::lock_guard<std::mutex> lock(rateMutex); // lock mutex to protect clientRequests map

        auto now = std::chrono::steady_clock::now(); // get current timestamp

        auto &timestamps = clientRequests[data]; // retrieve reference to list of timestamps

        while (!timestamps.empty() && now - timestamps.front() > timeWindow) // remove expired timestamps
        {
            timestamps.pop_front(); // remove oldest timestamp
        }

        // check if number of requests in time window exceeds allowed limit
        if (timestamps.size() >= maxRequests)
        {
            // if exceeded, return a 429 Too Many Requests response
            return "HTTP/1.1 429 Too Many Requests\r\n"
                   "Content-Type: text/plain\r\n"
                   "Content-Length: 19\r\n"
                   "\r\n"
                   "Too Many Requests";
        }

        timestamps.push_back(now); // add current timestamp to list

        return data; // return original data if rate limit is not exceeded
    }

private:
    size_t maxRequests;              // maximum number of requests allowed within time window
    std::chrono::seconds timeWindow; // duration of time window for rate limiting
    // map to store timestamps of requests for each client, identified by their data
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> clientRequests;
    std::mutex rateMutex; // mutex to protect access to clientRequests
};

class Compression : public Middleware
{
public:
    [[nodiscard]]
    static bool shouldCompress(const std::string &mimeType, size_t contentLength)
    {
        // define non-compressible mime types using unordered_set for efficient look-up
        static const std::unordered_set<std::string> nonCompressibleTypes = {
            "image/png", "image/gif", "image/svg+xml", "image/x-icon", "image/webp",
            "audio/mpeg", "video/mp4", "video/webm", "application/zip", "font/woff",
            "font/woff2", "font/ttf", "application/vnd.ms-fontobject"};

        // define compressible mime types using unordered_set for efficient look-up
        static const std::unordered_set<std::string> compressibleTypes = {
            "text/", "application/javascript", "application/json",
            "application/xml", "application/x-yaml", "application/x-www-form-urlencoded"};

        // check if it's a non-compressible type
        if (nonCompressibleTypes.find(mimeType) != nonCompressibleTypes.end())
        {
            return false;
        }

        // don't compress if content length is less than 1kb
        if (contentLength < 1024)
        {
            return false;
        }

        // Check if MIME type is compressible
        for (const auto &type : compressibleTypes)
        {
            if (mimeType.find(type) == 0) // Check for prefix match
            {
                return true;
            }
        }

        return false; // default case: do not compress
    }

    [[nodiscard]]
    static bool clientAcceptsGzip(const std::string &request)
    {
        return request.find("Accept-Encoding: ") != std::string::npos &&
               request.find("gzip") != std::string::npos;
    }

    [[nodiscard]]
    std::string process(const std::string &data) override
    {
        std::string compressed = compressData(data);
        if (!compressed.empty()) // check if compression was successful
        {
            Logger::getInstance()->info("Compressed data: " + std::to_string(data.size()) + " -> " + std::to_string(compressed.size()));
            return compressed;
        }
        return data; // Ensure a string is returned in all cases
    }

private:
    [[nodiscard]]
    std::string compressData(const std::string &data)
    {
        z_stream zs;                // create a z_stream object for compression
        memset(&zs, 0, sizeof(zs)); // zero-initialize z_stream structure

        // Initialize the zlib compression
        if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION,  // set compression level
                         Z_DEFLATED,                  // use deflate compression method
                         15 | 16,                     // 15 | 16 for gzip encoding
                         8,                           // set window size
                         Z_DEFAULT_STRATEGY) != Z_OK) // use default compression strategy
        {
            throw std::runtime_error("Failed to initialize zlib");
        }

        // set input data for compression
        zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data())); // input data
        zs.avail_in = data.size();
        // size of input data

        int ret;                         // variable to hold return status of compression
        const size_t bufferSize = 32768; // define size of output buffer (32KB)
        std::string compressed;          // string to hold final compressed data
        char outbuffer[bufferSize];      // buffer for compressed output

        // compress data in a loop until all data is processed
        do
        {
            zs.next_out = reinterpret_cast<Bytef *>(outbuffer); // output buffer for compressed data
            zs.avail_out = bufferSize;                          // size of output buffer

            ret = deflate(&zs, Z_FINISH); // perform compression operation

            if (compressed.size() < zs.total_out) // append compressed data to output string
            {
                compressed.append(outbuffer, zs.total_out - compressed.size());
            }
        } while (ret == Z_OK); // continue until no more data to compress

        // check if compression completed successfully
        if (ret != Z_STREAM_END)
        {
            deflateEnd(&zs); // clean up z_stream object
            throw std::runtime_error("Failed to compress data");
        }

        deflateEnd(&zs);   // clean up and free resources allocated by zlib
        return compressed; // return compressed data
    }
};

class ThreadPool
{
public:
    explicit ThreadPool(size_t numThreads)
    {
        start(numThreads); // initialize thread pool with specified number of threads
    }

    ~ThreadPool()
    {
        stop();
    }

    // delete copy constructor and assignment operator to prevent multiple instances sharing resources
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    // template method to enqueue tasks and get future results
    template <typename F, typename... Args>
    auto enqueue(F &&f, Args &&...args)
        -> std::future<typename std::invoke_result_t<F, Args...>>
    {
        using return_type = typename std::invoke_result_t<F, Args...>;

        // create a packaged task that will store result
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<return_type> res = task->get_future();

        {
            std::unique_lock<std::mutex> lock(queueMutex);
            if (stop_flag)
            {
                throw std::runtime_error("Cannot enqueue on stopped ThreadPool");
            }

            // wrap packaged task in a void function for queue
            tasks.emplace([task]()
                          { (*task)(); });
        }

        condition.notify_one(); // notify one thread that a task is available
        return res;
    }

    // stop thread pool and wait for all tasks to complete
    void stop()
    {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            stop_flag = true;
        }

        condition.notify_all();

        // join all worker threads
        for (std::thread &worker : workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }

        workers.clear();

        // clear any remaining tasks
        std::queue<std::function<void()>> empty;
        std::swap(tasks, empty);
    }

    // get number of worker threads
    size_t threadCount() const
    {
        return workers.size();
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queueMutex;
    std::condition_variable condition;
    std::atomic<bool> stop_flag{false};

    // initialize thread pool with specified number of threads
    void start(size_t numThreads)
    {
        workers.reserve(numThreads); // prevent vector reallocation

        for (size_t i = 0; i < numThreads; ++i)
        {
            workers.emplace_back([this]
                                 {
                while (true) {
                    std::function<void()> task;// define a task to execute

                    {
                        std::unique_lock<std::mutex> lock(queueMutex);

                        condition.wait(lock, [this] {
                            return stop_flag || !tasks.empty();// wait for a task or stop flag
                        });

                        if (stop_flag && tasks.empty()) {// check if thread pool is stopped
                            return;
                        }

                        if (!tasks.empty()) {
                            task = std::move(tasks.front());// get next task
                            tasks.pop();
                        }
                    }

                    if (task) {
                        task();// execute task
                    }
                } });
        }
    }
};

class Socket
{
public:
    Socket(int port);
    ~Socket();
    void bind();
    void listen();
    void closeSocket();
    int acceptConnection(std::string &clientIp);
    int getSocketFd() const;

    static std::string durationToString(const std::chrono::steady_clock::duration &duration)
    {
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
        auto minutes = seconds / 60;
        seconds %= 60;
        return std::to_string(minutes) + "m " + std::to_string(seconds) + "s";
    }

private:
    int server_fd; // Server socket file descriptor
    int port;      // Port number
};

Socket::Socket(int port) : port(port)
{
    // Create a dual-stack socket
    server_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (server_fd == -1)
    {
        Logger::getInstance()->error("Socket creation failed: " + std::string(strerror(errno)));
        throw std::runtime_error("Socket creation failed!");
    }

    int no = 0;
    if (setsockopt(server_fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no)) < 0)
    {
        Logger::getInstance()->error("Failed to set IPV6_V6ONLY to 0: " + std::string(strerror(errno)));
        close(server_fd);
        throw std::runtime_error("Failed to configure dual-stack socket!");
    }
}

Socket::~Socket()
{
    Logger::getInstance()->info("Closing server socket");
    close(server_fd);
}

void Socket::bind()
{
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0) // Set socket options
    {
        Logger::getInstance()->error("Failed to set socket options: " + std::string(strerror(errno)));
        throw std::runtime_error("Failed to set socket options");
    }

    struct sockaddr_in6 address;
    memset(&address, 0, sizeof(address));
    address.sin6_family = AF_INET6;  // Set address family to IPv6
    address.sin6_addr = in6addr_any; // Bind to all available interfaces
    address.sin6_port = htons(port); // Set port

    if (::bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) // Bind socket to address and port
    {
        std::string errorMsg = "Bind failed: " + std::string(strerror(errno));
        Logger::getInstance()->error(errorMsg);
        throw std::runtime_error(errorMsg);
    }

    Logger::getInstance()->success("Socket successfully bound to port " + std::to_string(port));
}

void Socket::listen()
{
    ::listen(server_fd, 42); // 42 is the ultimate answer to life, the universe, and everything
}
void Socket::closeSocket()
{
    if (server_fd != -1) // Check if socket is valid
    {
        close(server_fd);
        server_fd = -1;
    }
}

[[nodiscard]]
int Socket::acceptConnection(std::string &clientIp)
{
    struct sockaddr_in6 address;
    socklen_t addrlen = sizeof(address);
    int new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen); // Accept a new connection

    if (new_socket >= 0)
    {
        char ipstr[INET6_ADDRSTRLEN];
        if (address.sin6_family == AF_INET6)
        {
            inet_ntop(AF_INET6, &address.sin6_addr, ipstr, sizeof(ipstr)); // Convert IPv6 address to string
        }
        clientIp = ipstr;
        Logger::getInstance()->success("New connection accepted from " + clientIp);

        int flags = fcntl(new_socket, F_GETFL, 0);
        if (flags == -1)
        {
            Logger::getInstance()->error("Failed to get socket flags");
            close(new_socket);
            return -1;
        }
        if (fcntl(new_socket, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            Logger::getInstance()->error("Failed to set socket to non-blocking mode");
            close(new_socket);
            return -1;
        }
    }
    else
    {
        clientIp = "-";
        Logger::getInstance()->error("Failed to accept connection");
    }
    return new_socket;
}

[[nodiscard]]
int Socket::getSocketFd() const
{
    return server_fd;
}

class Http
{
public:
    static std::string getRequestPath(const std::string &request);
    static void sendResponse(int client_socket, const std::string &content,
                             const std::string &mimeType, int statusCode,
                             const std::string &clientIp, bool isIndex = false,
                             Middleware *middleware = nullptr, Cache *cache = nullptr);
    static bool isAssetRequest(const std::string &path);

private:
    // Constants for optimized I/O
    static constexpr size_t BUFFER_SIZE = 65536;      // 64KB buffer size
    static constexpr size_t ALIGNMENT = 512;          // memory alignment boundary
    static constexpr size_t SENDFILE_CHUNK = 1048576; // 1MB sendfile chunk size
    static constexpr int MAX_IOV = IOV_MAX;           // maximum iovec array size

    // Socket option settings
    struct SocketSettings
    {
        static constexpr int keepAlive = 1;
        static constexpr int keepIdle = 60;
        static constexpr int keepInterval = 10;
        static constexpr int keepCount = 3;
    };

    // RAII wrappers
    class SocketOptionGuard
    {
        int &cork;
        int client_socket;

    public:
        SocketOptionGuard(int &c, int cs) : cork(c), client_socket(cs) {}
        ~SocketOptionGuard()
        {
            cork = 0;
            setsockopt(client_socket, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
        }
    };

    class FileGuard
    {
        int fd;

    public:
        FileGuard() : fd(-1) {}
        explicit FileGuard(int f) : fd(f) {}
        ~FileGuard()
        {
            if (fd != -1)
                close(fd);
        }
        int get() const { return fd; }
        void reset(int f = -1)
        {
            if (fd != -1)
                close(fd);
            fd = f;
        }
    };

    class MMapGuard
    {
        void *addr;
        size_t length;

    public:
        MMapGuard(void *a, size_t l) : addr(a), length(l) {}
        ~MMapGuard()
        {
            if (addr != MAP_FAILED)
                munmap(addr, length);
        }
    };

    // Helper functions declarations
    static bool setupSocketOptions(int client_socket, int cork, const std::string &clientIp);
    static bool handleFileContent(FileGuard &fileGuard,
                                  const std::string &filePath,
                                  std::pmr::vector<char> &fileContent,
                                  size_t &fileSize,
                                  time_t &lastModified,
                                  const std::string &clientIp);
    static bool compressContent(Middleware *middleware,
                                const std::string &mimeType,
                                size_t fileSize,
                                std::pmr::vector<char> &fileContent,
                                std::pmr::string &compressedContent,
                                bool cacheHit,
                                FileGuard &fileGuard,
                                std::pmr::monotonic_buffer_resource &pool);
    static std::string generateHeaders(int statusCode,
                                       const std::string &mimeType,
                                       size_t fileSize,
                                       time_t lastModified,
                                       bool isCompressed);
    static size_t sendWithWritev(int client_socket,
                                 const std::string &headerStr,
                                 const std::pmr::string &compressedContent,
                                 const std::pmr::vector<char> &fileContent,
                                 bool isCompressed,
                                 bool cacheHit,
                                 const std::string &clientIp);
    static size_t sendLargeFile(int client_socket,
                                FileGuard &fileGuard,
                                size_t fileSize,
                                const std::string &clientIp);
    static void updateCache(Cache *cache,
                            const std::string &filePath,
                            const std::string &mimeType,
                            time_t lastModified,
                            FileGuard &fileGuard,
                            size_t fileSize,
                            std::pmr::monotonic_buffer_resource &pool);
};
bool Http::isAssetRequest(const std::string &path)
{
    // cache string length to avoid multiple calls
    const size_t pathLen = path.length();
    if (pathLen < 4)
        return false; // Minimum length for an asset (.css)

    // convert last 10 chars (max extension length) to lowercase once
    // this avoids converting the entire path
    alignas(8) char lastChars[10]; // Aligned for better memory access
    const size_t checkLen = std::min(pathLen, size_t(10));
    std::transform(
        path.end() - checkLen, path.end(),
        lastChars,
        ::tolower);
    std::string_view lastView(lastChars, checkLen);

    // Most common image types and web assets first (ordered by frequency)
    static constexpr std::array<std::string_view, 16> commonExts = {
        ".jpg", ".png", ".gif", // most frequent image types
        ".jpeg", ".webp",       // less frequent image types
        ".css", ".js",          // essential web assets
        ".ico", ".svg",         // common icons
        ".woff2", ".woff",      // modern fonts first
        ".ttf",                 // legacy font
        ".mp4", ".webm",        // video files
        ".json", ".xml"         // data files
    };

    // fast path: check most common extensions first using simd-friendly loop
    const auto commonExtsSize = commonExts.size();
    for (size_t i = 0; i < commonExtsSize; ++i)
    {
        const auto &ext = commonExts[i];
        if (checkLen >= ext.length() &&
            lastView.compare(checkLen - ext.length(), ext.length(), ext) == 0)
        {
            return true;
        }
        // early exit after checking most common types
        if (i == 7 && pathLen > 6)
            break; // skip less common types for longer paths
    }

    // less common extensions - now includes previously separate ones
    static constexpr std::array<std::string_view, 5> rareExts = {
        ".eot", ".map", ".pdf", ".mp3", ".wav"};

    // check rare extensions only for specific path patterns
    if (pathLen > 8)
    { // only check for longer paths
        size_t dotPos = path.find_last_of('.');
        if (dotPos != std::string::npos)
        {
            std::string_view ext(lastChars + (dotPos - (pathLen - checkLen)),
                                 checkLen - (dotPos - (pathLen - checkLen)));
            for (const auto &rareExt : rareExts)
            {
                if (ext == rareExt)
                    return true;
            }
        }
    }

    // directory check optimization using string_view and branch prediction
    std::string_view pathView(path);

    // hot path: check most common asset directories first
    static constexpr std::array<std::string_view, 6> hotDirs = {
        "/img/", "/images/",   // image directories
        "/css/", "/js/",       // essential asset directories
        "/assets/", "/static/" // common resource directories
    };

    // use likely/unlikely for better branch prediction
    for (const auto &dir : hotDirs)
    {
        if (pathView.starts_with(dir)) [[likely]]
        {
            return true;
        }
    }

    // cold path: less common directories
    static constexpr std::array<std::string_view, 3> coldDirs = {
        "/fonts/", "/media/", "/photos/"};

    // check cold directories only if hot directories didn't match
    if (pathLen > 7) [[unlikely]]
    {
        for (const auto &dir : coldDirs)
        {
            if (pathView.find(dir) != std::string_view::npos)
            {
                return true;
            }
        }
    }

    return false;
}

[[nodiscard]]
std::string Http::getRequestPath(const std::string &request)
{
    size_t pos1 = request.find("GET ");
    size_t pos2 = request.find(" HTTP/");
    if (pos1 == std::string::npos || pos2 == std::string::npos) // Check if request is valid
    {
        return "/";
    }
    return request.substr(pos1 + 4, pos2 - (pos1 + 4));
}
void Http::sendResponse(int client_socket, const std::string &filePath,
                        const std::string &mimeType, int statusCode,
                        const std::string &clientIp, bool isIndex,
                        Middleware *middleware, Cache *cache)
{
    // Create a memory resource for this request
    std::pmr::monotonic_buffer_resource pool(64 * 1024); // 64KB initial size

    // Performance metrics
    auto startTime = std::chrono::steady_clock::now();
    size_t totalBytesSent = 0;

    // Socket options
    int cork = 1;
    SocketOptionGuard sockGuard(cork, client_socket);

    // Set socket options including TCP_CORK
    if (!setupSocketOptions(client_socket, cork, clientIp))
    {
        return;
    }

    // File content and cache handling
    std::pmr::vector<char> fileContent{&pool};
    size_t fileSize;
    time_t lastModified;
    bool cacheHit = false;

    // Try to get content from cache
    if (cache && statusCode == 200)
    {
        std::string cachedMimeType;
        cacheHit = cache->get(filePath, fileContent, cachedMimeType, lastModified);
        if (cacheHit)
        {
            fileSize = fileContent.size();
            Logger::getInstance()->info("Cache hit for: " + filePath, clientIp);
        }
    }

    // Handle file if not in cache
    FileGuard fileGuard;
    if (!cacheHit)
    {
        if (!handleFileContent(fileGuard, filePath, fileContent, fileSize, lastModified, clientIp))
        {
            return;
        }
    }

    // Compression handling
    bool isCompressed = false;
    std::pmr::string compressedContent{&pool};

    if (middleware && Compression::shouldCompress(mimeType, fileSize) &&
        mimeType.find("image/") == std::string::npos)
    {
        isCompressed = compressContent(middleware, mimeType, fileSize, fileContent,
                                       compressedContent, cacheHit, fileGuard, pool);
        if (isCompressed)
        {
            fileSize = compressedContent.size();
        }
    }

    // Generate response headers
    std::string headerStr = generateHeaders(statusCode, mimeType, fileSize, lastModified, isCompressed);

    // Send headers and content using writev
    totalBytesSent += sendWithWritev(client_socket, headerStr, compressedContent,
                                     fileContent, isCompressed, cacheHit, clientIp);

    // Handle large file transfer using sendfile or mmap
    if (!isCompressed && !cacheHit && fileGuard.get() != -1)
    {
        totalBytesSent += sendLargeFile(client_socket, fileGuard, fileSize, clientIp);

        // Update cache if needed
        if (cache && statusCode == 200)
        {
            updateCache(cache, filePath, mimeType, lastModified, fileGuard, fileSize, pool);
        }
    }

    // Record performance metrics
    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

    if (isIndex)
    {
        Logger::getInstance()->info(
            "Response sent: status=" + std::to_string(statusCode) +
                ", path=" + filePath +
                ", size=" + std::to_string(fileSize) +
                ", type=" + mimeType +
                ", cache=" + (cacheHit ? "HIT" : "MISS") +
                ", time=" + std::to_string(duration.count()) + "µs" +
                ", bytes=" + std::to_string(totalBytesSent),
            clientIp);
    }
}
bool Http::setupSocketOptions(int client_socket, int cork, const std::string &clientIp)
{
    auto setSocketOption = [&](int level, int optname, const void *optval, socklen_t optlen)
    {
        if (setsockopt(client_socket, level, optname, optval, optlen) < 0)
        {
            Logger::getInstance()->error("Failed to set socket option: " + std::string(strerror(errno)), clientIp);
            close(client_socket);
            return false;
        }
        return true;
    };

    return setSocketOption(SOL_SOCKET, SO_KEEPALIVE, &SocketSettings::keepAlive, sizeof(SocketSettings::keepAlive)) &&
           setSocketOption(IPPROTO_TCP, TCP_KEEPIDLE, &SocketSettings::keepIdle, sizeof(SocketSettings::keepIdle)) &&
           setSocketOption(IPPROTO_TCP, TCP_KEEPINTVL, &SocketSettings::keepInterval, sizeof(SocketSettings::keepInterval)) &&
           setSocketOption(IPPROTO_TCP, TCP_KEEPCNT, &SocketSettings::keepCount, sizeof(SocketSettings::keepCount)) &&
           setSocketOption(IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
}

bool Http::handleFileContent(FileGuard &fileGuard,
                             const std::string &filePath,
                             std::pmr::vector<char> &fileContent,
                             size_t &fileSize,
                             time_t &lastModified,
                             const std::string &clientIp)
{
    // Open file with O_DIRECT for large files to bypass system cache
    int flags = O_RDONLY;
    struct stat statBuf;
    if (stat(filePath.c_str(), &statBuf) == 0 && statBuf.st_size > 10 * 1024 * 1024) // 10MB threshold
    {
        flags |= O_DIRECT;
    }

    int fd = open(filePath.c_str(), flags);
    if (fd == -1 && errno == EINVAL) // O_DIRECT not supported, fallback to normal open
    {
        fd = open(filePath.c_str(), O_RDONLY);
    }

    if (fd == -1)
    {
        Logger::getInstance()->error(
            "Failed to open file: errno=" + std::to_string(errno),
            clientIp);
        return false;
    }
    fileGuard.reset(fd);

    // Advise kernel about access pattern for optimal I/O performance
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    struct stat file_stat;
    if (fstat(fd, &file_stat) == -1)
    {
        Logger::getInstance()->error(
            "Fstat failed: errno=" + std::to_string(errno),
            clientIp);
        return false;
    }

    fileSize = file_stat.st_size;
    lastModified = file_stat.st_mtime;
    return true;
}

bool Http::compressContent(Middleware *middleware,
                           const std::string &mimeType,
                           size_t fileSize,
                           std::pmr::vector<char> &fileContent,
                           std::pmr::string &compressedContent,
                           bool cacheHit,
                           FileGuard &fileGuard,
                           std::pmr::monotonic_buffer_resource &pool)
{
    if (cacheHit)
    {
        compressedContent = middleware->process(std::string(fileContent.begin(), fileContent.end()));
    }
    else
    {
        // Use aligned buffer with RAII
        struct AlignedBuffer
        {
            void *ptr;
            AlignedBuffer(size_t size, size_t alignment) : ptr(nullptr)
            {
                if (posix_memalign(&ptr, alignment, size) != 0)
                {
                    throw std::bad_alloc();
                }
            }
            ~AlignedBuffer() { free(ptr); }
            void *get() { return ptr; }
        } alignedBuffer(BUFFER_SIZE, ALIGNMENT);

        std::pmr::vector<char> buffer{&pool};
        buffer.reserve(fileSize);

        size_t totalRead = 0;
        while (totalRead < fileSize)
        {
            ssize_t bytesRead = read(fileGuard.get(), alignedBuffer.get(),
                                     std::min(BUFFER_SIZE, fileSize - totalRead));
            if (bytesRead <= 0)
                break;
            buffer.insert(buffer.end(), static_cast<char *>(alignedBuffer.get()),
                          static_cast<char *>(alignedBuffer.get()) + bytesRead);
            totalRead += bytesRead;
        }

        compressedContent = middleware->process(std::string(buffer.begin(), buffer.end()));
    }
    return true;
}
std::string Http::generateHeaders(int statusCode,
                                  const std::string &mimeType,
                                  size_t fileSize,
                                  time_t lastModified,
                                  bool isCompressed)
{
    // Pre-allocate header string capacity
    std::string headerStr;
    headerStr.reserve(1024); // Reserve reasonable space for headers

    // Generate time strings
    char timeBuffer[128], lastModifiedBuffer[128];
    time_t now = time(nullptr);
    struct tm tmBuf;
    struct tm *tm_info = gmtime_r(&now, &tmBuf); // Thread-safe version
    strftime(timeBuffer, sizeof(timeBuffer), "%a, %d %b %Y %H:%M:%S GMT", tm_info);

    tm_info = gmtime_r(&lastModified, &tmBuf);
    strftime(lastModifiedBuffer, sizeof(lastModifiedBuffer), "%a, %d %b %Y %H:%M:%S GMT", tm_info);

    // Status message lookup using array for better performance
    static const char *const STATUS_MESSAGES[] = {
        "OK",                    // 200
        nullptr,                 // 201-299
        nullptr,                 // 300-303
        "Not Modified",          // 304
        nullptr,                 // 305-399
        "Bad Request",           // 400
        "Unauthorized",          // 401
        nullptr,                 // 402
        "Forbidden",             // 403
        "Not Found",             // 404
        nullptr,                 // 405-499
        "Internal Server Error", // 500
        nullptr,                 // 501-502
        "Service Unavailable",   // 503
    };

    const char *statusMessage;
    if (statusCode >= 200 && statusCode < static_cast<int>(sizeof(STATUS_MESSAGES) / sizeof(char *)) + 200)
    {
        statusMessage = STATUS_MESSAGES[statusCode - 200];
        if (!statusMessage)
            statusMessage = "Unknown Status";
    }
    else
    {
        statusMessage = "Unknown Status";
    }

    // Assemble response headers using string
    headerStr = "HTTP/1.1 " + std::to_string(statusCode) + " " + statusMessage + "\r\n"
                                                                                 "Server: RobustHTTP/1.0\r\n"
                                                                                 "Date: " +
                std::string(timeBuffer) + "\r\n"
                                          "Content-Type: " +
                mimeType + "\r\n"
                           "Content-Length: " +
                std::to_string(fileSize) + "\r\n"
                                           "Last-Modified: " +
                std::string(lastModifiedBuffer) + "\r\n"
                                                  "Connection: keep-alive\r\n"
                                                  "Keep-Alive: timeout=60, max=1000\r\n"
                                                  "Accept-Ranges: bytes\r\n"
                                                  "Cache-Control: public, max-age=31536000\r\n"
                                                  "X-Content-Type-Options: nosniff\r\n"
                                                  "X-Frame-Options: SAMEORIGIN\r\n"
                                                  "X-XSS-Protection: 1; mode=block\r\n";

    if (isCompressed)
    {
        headerStr += "Content-Encoding: gzip\r\n"
                     "Vary: Accept-Encoding\r\n";
    }
    headerStr += "\r\n";

    return headerStr;
}

size_t Http::sendWithWritev(int client_socket,
                            const std::string &headerStr,
                            const std::pmr::string &compressedContent,
                            const std::pmr::vector<char> &fileContent,
                            bool isCompressed,
                            bool cacheHit,
                            const std::string &clientIp)
{
    // Optimize writev using maximum allowed iovec structures
    std::array<struct iovec, MAX_IOV> iov;
    int iovcnt = 0;

    // Add header to iovec
    iov[iovcnt].iov_base = const_cast<char *>(headerStr.c_str());
    iov[iovcnt].iov_len = headerStr.size();
    iovcnt++;

    // Add content to iovec if compressed or cached
    if (isCompressed)
    {
        iov[iovcnt].iov_base = const_cast<char *>(compressedContent.c_str());
        iov[iovcnt].iov_len = compressedContent.size();
        iovcnt++;
    }
    else if (cacheHit)
    {
        iov[iovcnt].iov_base = const_cast<char *>(fileContent.data());
        iov[iovcnt].iov_len = fileContent.size();
        iovcnt++;
    }

    // Send headers and content using writev with retry logic
    size_t totalSent = 0;
    const size_t totalSize = headerStr.size() +
                             (isCompressed ? compressedContent.size() : (cacheHit ? fileContent.size() : 0));

    while (totalSent < totalSize)
    {
        ssize_t sent = writev(client_socket, iov.data(), iovcnt);
        if (sent <= 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(1000));
                continue;
            }
            Logger::getInstance()->error(
                "Failed to send response: errno=" + std::to_string(errno),
                clientIp);
            return totalSent;
        }
        totalSent += sent;

        // Update iovec structures with zero-copy approach
        while (sent > 0 && iovcnt > 0)
        {
            if (static_cast<size_t>(sent) >= iov[0].iov_len)
            {
                sent -= iov[0].iov_len;
                iovcnt--;
                std::copy(iov.begin() + 1, iov.begin() + 1 + iovcnt, iov.begin());
            }
            else
            {
                iov[0].iov_base = static_cast<char *>(iov[0].iov_base) + sent;
                iov[0].iov_len -= sent;
                break;
            }
        }
    }
    return totalSent;
}
size_t Http::sendLargeFile(int client_socket,
                           FileGuard &fileGuard,
                           size_t fileSize,
                           const std::string &clientIp)
{
    size_t totalSent = 0;
    off_t offset = 0;
    bool useMmap = false;

    // Try sendfile with optimal chunk size and retry logic
    while (offset < static_cast<off_t>(fileSize))
    {
        size_t chunk = std::min(SENDFILE_CHUNK, fileSize - offset);
        ssize_t sent = sendfile(client_socket, fileGuard.get(), &offset, chunk);

        if (sent == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(1000));
                continue;
            }
            else if (errno == EINVAL || errno == ENOSYS)
            {
                useMmap = true;
                break;
            }
            Logger::getInstance()->error(
                "Failed to send file: errno=" + std::to_string(errno),
                clientIp);
            return totalSent;
        }
        else if (sent == 0)
        {
            break;
        }
        totalSent += sent;
    }

    // Fall back to mmap for large files with huge pages support
    if (useMmap)
    {
        int flags = MAP_PRIVATE;
        if (fileSize >= 2 * 1024 * 1024) // 2MB threshold for huge pages
        {
            flags |= MAP_HUGETLB;
        }

        void *mmapAddr = mmap(nullptr, fileSize, PROT_READ, flags, fileGuard.get(), 0);
        if (mmapAddr == MAP_FAILED && (flags & MAP_HUGETLB))
        {
            flags &= ~MAP_HUGETLB;
            mmapAddr = mmap(nullptr, fileSize, PROT_READ, flags, fileGuard.get(), 0);
        }

        if (mmapAddr == MAP_FAILED)
        {
            Logger::getInstance()->error(
                "Mmap failed: errno=" + std::to_string(errno),
                clientIp);
            return totalSent;
        }

        // RAII for mmap
        MMapGuard mmapGuard(mmapAddr, fileSize);
        madvise(mmapAddr, fileSize, MADV_SEQUENTIAL);

        const char *fileContent = static_cast<const char *>(mmapAddr);
        size_t remainingBytes = fileSize;
        size_t bytesSent = 0;

        while (remainingBytes > 0)
        {
            size_t chunk = std::min(BUFFER_SIZE, remainingBytes);
            ssize_t sent = send(client_socket, fileContent + bytesSent, chunk, MSG_NOSIGNAL);
            if (sent == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(1000));
                    continue;
                }
                Logger::getInstance()->error(
                    "Failed to send mmap data: errno=" + std::to_string(errno),
                    clientIp);
                return totalSent + bytesSent;
            }
            bytesSent += sent;
            remainingBytes -= sent;
        }
        totalSent += bytesSent;
    }

    return totalSent;
}

void Http::updateCache(Cache *cache,
                       const std::string &filePath,
                       const std::string &mimeType,
                       time_t lastModified,
                       FileGuard &fileGuard,
                       size_t fileSize,
                       std::pmr::monotonic_buffer_resource &pool)
{
    std::pmr::vector<char> content{&pool};
    content.reserve(fileSize);

    if (lseek(fileGuard.get(), 0, SEEK_SET) != -1)
    {
        // Use aligned buffer for optimal read performance
        std::unique_ptr<char[]> alignedBuffer(new (std::align_val_t{ALIGNMENT}) char[BUFFER_SIZE]);
        size_t totalRead = 0;

        while (totalRead < fileSize)
        {
            ssize_t bytesRead = read(fileGuard.get(), alignedBuffer.get(),
                                     std::min(BUFFER_SIZE, fileSize - totalRead));
            if (bytesRead <= 0)
                break;

            content.insert(content.end(), alignedBuffer.get(),
                           alignedBuffer.get() + bytesRead);
            totalRead += bytesRead;
        }

        // Only update cache if we read the entire file
        if (totalRead == fileSize)
        {
            cache->set(filePath, content, mimeType, lastModified);
        }
    }
}

class Router
{
public:
    Router(const std::string &staticFolder) : staticFolder(staticFolder)
    {
        Logger::getInstance()->success("Router initialized with static folder: " + staticFolder);
    }
    void route(const std::string &path, int client_socket, const std::string &clientIp, Middleware *middleware, Cache *cache);
    [[nodiscard]]
    std::string getStaticFolder() const
    {
        return staticFolder;
    }

private:
    std::string staticFolder;                         // path to static files
    std::string getMimeType(const std::string &path); // get MIME type based on file extension
};

[[nodiscard]]
std::string Router::getMimeType(const std::string &path)
{
    // cache frequently used MIME types for quick access
    static const std::string TEXT_HTML = "text/html";
    static const std::string TEXT_PLAIN = "text/plain";
    static const std::string IMAGE_JPEG = "image/jpeg";

    // define sets of extensions that map to the same MIME type
    static const std::set<std::string_view> htmlExts = {".html", ".htm"};
    static const std::set<std::string_view> jpegExts = {".jpg", ".jpeg"};

    // define valid extensions set for quick validation
    static const std::set<std::string_view> validExts = {
        ".html", ".htm", ".css", ".js", ".json", ".png",
        ".jpg", ".jpeg", ".gif", ".svg", ".ico", ".txt",
        ".pdf", ".xml", ".zip", ".woff", ".woff2", ".ttf",
        ".eot", ".mp3", ".mp4", ".webm", ".webp"};

    // define MIME type mappings for validated extensions
    static const std::unordered_map<std::string_view, std::string_view> mimeTypes = {
        {".css", "text/css"},
        {".js", "application/javascript"},
        {".json", "application/json"},
        {".png", "image/png"},
        {".gif", "image/gif"},
        {".svg", "image/svg+xml"},
        {".ico", "image/x-icon"},
        {".txt", "text/plain"},
        {".pdf", "application/pdf"},
        {".xml", "application/xml"},
        {".zip", "application/zip"},
        {".woff", "font/woff"},
        {".woff2", "font/woff2"},
        {".ttf", "font/ttf"},
        {".eot", "application/vnd.ms-fontobject"},
        {".mp3", "audio/mpeg"},
        {".mp4", "video/mp4"},
        {".webm", "video/webm"},
        {".webp", "image/webp"}};

    // find last dot in path
    size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos)
    {
        return TEXT_PLAIN;
    }

    // extract extension and convert to lowercase using string_view
    // this avoids string allocation for the extension
    char lastChars[10]; // max extension length
    const size_t extLen = std::min(path.length() - dotPos, size_t(10));
    std::transform(
        path.begin() + dotPos,
        path.begin() + dotPos + extLen,
        lastChars,
        ::tolower);
    std::string_view ext(lastChars, extLen);

    // quick validation check
    if (!validExts.contains(ext))
    {
        return TEXT_PLAIN;
    }

    // fast path for most common types
    if (htmlExts.contains(ext))
    {
        return TEXT_HTML;
    }
    if (jpegExts.contains(ext))
    {
        return IMAGE_JPEG;
    }

    // look up MIME type for other extensions
    auto it = mimeTypes.find(ext);
    return it != mimeTypes.end() ? std::string(it->second) : TEXT_PLAIN;
}

void Router::route(const std::string &path, int client_socket, const std::string &clientIp,
                   Middleware *middleware, Cache *cache)
{
    // pre-allocate string capacity to avoid reallocation
    // +11 accounts for potential "/index.html" addition
    std::string filePath;
    filePath.reserve(staticFolder.size() + path.size() + 11);
    filePath = staticFolder;
    filePath += path;

    // normalize path to lowercase for case-insensitive comparison
    // using a separate string to preserve original path for logging
    std::string normalizedPath = path;
    std::transform(normalizedPath.begin(), normalizedPath.end(), normalizedPath.begin(), ::tolower);

    // use string_view for efficient string operations
    // this avoids string copies during comparisons
    std::string_view pathView(normalizedPath);

    // cache filesystem check results to avoid redundant calls
    bool isDir = fs::is_directory(filePath);
    bool isIndex = (pathView == "/index.html" || pathView == "/" || isDir);
    bool isAsset = Http::isAssetRequest(normalizedPath);

    // append index.html for directory paths
    if (isDir)
    {
        filePath += "/index.html";
    }

    // log non-asset index requests
    if (!isAsset && isIndex)
    {
        Logger::getInstance()->info("Processing request: " + path, clientIp);
    }

    // static response for 404 errors
    // this avoids creating the string repeatedly
    static const char *SIMPLE_404_RESPONSE =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 9\r\n"
        "\r\n"
        "Not Found";

    // cache for 404.html content
    // using static variables and std::once_flag for thread-safe initialization
    static std::string cached404Content;
    static std::once_flag flag404;
    static bool has404File = false;

    // check if file exists and handle 404 errors
    if (!fs::exists(filePath) || (isDir && !fs::exists(filePath)))
    {
        // log warning for non-asset requests
        if (!isAsset)
        {
            Logger::getInstance()->warning("File not found: " + filePath, clientIp);
        }

        // initialize 404.html content only once
        // this is thread-safe due to std::call_once
        std::call_once(flag404, [&]()
                       {
            std::ifstream file("404.html", std::ios::binary);
            if (file) {
                has404File = true;
                // efficient file reading using iterators
                cached404Content.assign(
                    std::istreambuf_iterator<char>(file),
                    std::istreambuf_iterator<char>()
                );
            } });

        // send simple 404 response if custom 404.html doesn't exist
        if (!has404File)
        {
            // use MSG_NOSIGNAL to prevent SIGPIPE
            send(client_socket, SIMPLE_404_RESPONSE, strlen(SIMPLE_404_RESPONSE), MSG_NOSIGNAL);
            return;
        }

        // static response header for 404 errors
        // this is shared across all 404 responses
        static const std::string RESPONSE_404_HEADER =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n"
            "Content-Length: ";

        // use writev for efficient response sending
        // this minimizes the number of system calls
        struct iovec iov[4]; // array of io vectors for writev
        std::string contentLength = std::to_string(cached404Content.size());
        static const char *CRLF = "\r\n\r\n";

        // set up io vectors for response components
        iov[0].iov_base = const_cast<char *>(RESPONSE_404_HEADER.data());
        iov[0].iov_len = RESPONSE_404_HEADER.size();
        iov[1].iov_base = const_cast<char *>(contentLength.data());
        iov[1].iov_len = contentLength.size();
        iov[2].iov_base = const_cast<char *>(CRLF);
        iov[2].iov_len = 4; // Length of "\r\n\r\n"
        iov[3].iov_base = const_cast<char *>(cached404Content.data());
        iov[3].iov_len = cached404Content.size();

        // send all components in a single system call
        if (writev(client_socket, iov, 4) == -1)
        {
            Logger::getInstance()->error("Failed to send 404 response: " + std::string(strerror(errno)), clientIp);
        }
        return;
    }

    // get mime type and send response
    // note: getmimetype is assumed to be case-insensitive
    std::string mimeType = getMimeType(filePath);

    // send the response using the optimized http::sendresponse method
    // the !isasset && isindex parameter determines whether to log the response
    Http::sendResponse(client_socket, filePath, mimeType, 200, clientIp,
                       !isAsset && isIndex, middleware, cache);
}

class Parser
{
public:
    static Config parseConfig(const std::string &configFilePath);
};

Config Parser::parseConfig(const std::string &configFilePath)
{
    // log start of configuration reading process
    Logger::getInstance()->info("Reading configuration from: " + configFilePath);

    // open  configuration file
    std::ifstream file(configFilePath);
    if (!file.is_open())
    {
        Logger::getInstance()->error("Could not open config file: " + configFilePath);
        throw std::runtime_error("Could not open config file!");
    }

    // parse JSON configuration
    json configJson;
    try
    {
        file >> configJson;
    }
    catch (const json::parse_error &e)
    {
        Logger::getInstance()->error("JSON parsing error: " + std::string(e.what()));
        throw std::runtime_error("Failed to parse configuration file");
    }

    // check if all required fields exist and are not null
    if (!configJson.contains("port") || configJson["port"].is_null() ||
        !configJson.contains("static_folder") || configJson["static_folder"].is_null() ||
        !configJson.contains("thread_count") || configJson["thread_count"].is_null() ||
        !configJson.contains("rate_limit") || configJson["rate_limit"].is_null() ||
        !configJson["rate_limit"].contains("max_requests") || configJson["rate_limit"]["max_requests"].is_null() ||
        !configJson["rate_limit"].contains("time_window") || configJson["rate_limit"]["time_window"].is_null() ||
        !configJson.contains("cache") || configJson["cache"].is_null() ||
        !configJson["cache"].contains("size_mb") || configJson["cache"]["size_mb"].is_null() ||
        !configJson["cache"].contains("max_age_seconds") || configJson["cache"]["max_age_seconds"].is_null())
    {
        Logger::getInstance()->error("Configuration file is missing required fields or contains null values");
        throw std::runtime_error("Incomplete configuration file");
    }

    // create a Config object and populate it with values from JSON
    Config config;
    config.port = configJson["port"];
    config.staticFolder = configJson["static_folder"];
    config.threadCount = configJson["thread_count"];
    config.rateLimit.maxRequests = configJson["rate_limit"]["max_requests"];
    config.rateLimit.timeWindow = configJson["rate_limit"]["time_window"];
    config.cache.sizeMB = configJson["cache"]["size_mb"].get<size_t>();
    config.cache.maxAgeSeconds = configJson["cache"]["max_age_seconds"].get<int>();

    // validate port number
    if (config.port <= 0 || config.port > 65535)
    {
        Logger::getInstance()->error("Invalid port number: " + std::to_string(config.port));
        throw std::runtime_error("Invalid port number");
    }

    // validate static folder path
    if (!fs::exists(config.staticFolder))
    {
        Logger::getInstance()->error("Static folder does not exist: " + config.staticFolder);
        throw std::runtime_error("Invalid static folder path");
    }

    // validate thread count
    if (config.threadCount <= 0 || config.threadCount > 1000)
    {
        Logger::getInstance()->error("Invalid thread count: " + std::to_string(config.threadCount));
        throw std::runtime_error("Invalid thread count");
    }

    // validate maximum requests for rate limiting
    if (config.rateLimit.maxRequests <= 0)
    {
        Logger::getInstance()->error("Invalid max requests for rate limiting: " + std::to_string(config.rateLimit.maxRequests));
        throw std::runtime_error("Invalid max requests for rate limiting");
    }

    // validate time window for rate limiting
    if (config.rateLimit.timeWindow <= 0)
    {
        Logger::getInstance()->error("Invalid time window for rate limiting: " + std::to_string(config.rateLimit.timeWindow));
        throw std::runtime_error("Invalid time window for rate limiting");
    }

    // validate cache size
    if (config.cache.sizeMB <= 0 || config.cache.sizeMB > std::numeric_limits<size_t>::max() / (1024 * 1024))
    {
        Logger::getInstance()->error("Invalid cache size: " + std::to_string(config.cache.sizeMB) + " MB");
        throw std::runtime_error("Invalid cache size");
    }

    // validate cache max age
    if (config.cache.maxAgeSeconds <= 0)
    {
        Logger::getInstance()->error("Invalid cache max age: " + std::to_string(config.cache.maxAgeSeconds) + " seconds");
        throw std::runtime_error("Invalid cache max age");
    }

    // log successful configuration loading
    Logger::getInstance()->success("Configuration loaded successfully");

    // return populated and validated Config object
    return config;
}

class EpollWrapper
{
public:
    // constructor with minimal but essential error handling
    inline EpollWrapper() : epoll_fd(epoll_create1(EPOLL_CLOEXEC))
    {
        if (epoll_fd == -1)
        {
            throw std::system_error(errno, std::system_category(), "epoll_create1 failed");
        }
    }

    inline ~EpollWrapper() noexcept
    {
        if (epoll_fd != -1)
            close(epoll_fd);
    }

    // delete copy operations
    EpollWrapper(const EpollWrapper &) = delete;
    EpollWrapper &operator=(const EpollWrapper &) = delete;

    /*
    decide to use inline functions for performance
    but who knows, the compiler might ignore it
    */

    // optimized move operations
    inline EpollWrapper(EpollWrapper &&other) noexcept : epoll_fd(other.epoll_fd)
    {
        other.epoll_fd = -1;
    }

    inline EpollWrapper &operator=(EpollWrapper &&other) noexcept // move assignment operator
    {
        if (this != &other)
        {
            if (epoll_fd != -1)
                close(epoll_fd);
            epoll_fd = std::exchange(other.epoll_fd, -1);
        }
        return *this;
    }

    [[nodiscard]] inline int get() const noexcept // get epoll file descriptor
    {
        return epoll_fd;
    }

    [[nodiscard]] inline int wait(struct epoll_event *events, int maxEvents, int timeout) noexcept
    {
        return epoll_wait(epoll_fd, events, maxEvents, timeout);
    }

    inline bool add(int fd, uint32_t events) noexcept // add fd to epoll set with optimized error handling
    {

        struct epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;
        return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    inline bool modify(int fd, uint32_t events) noexcept // modify fd in epoll set with optimized error handling
    {
        struct epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;
        return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0;
    }

    inline bool remove(int fd) noexcept // remove fd from epoll set with optimized error handling
    {

        return epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == 0;
    }

    template <typename Iterator>
    inline size_t batch_operation(Iterator begin, Iterator end,
                                  int op, uint32_t events = 0) noexcept
    {
        size_t success_count = 0;
        struct epoll_event ev;
        ev.events = events;

        for (Iterator it = begin; it != end; ++it)
        {
            const int fd = *it; // get file descriptor
            ev.data.fd = fd;
            if (epoll_ctl(epoll_fd, op, fd, (op == EPOLL_CTL_DEL) ? nullptr : &ev) == 0)
            {
                ++success_count;
            }
        }
        return success_count;
    }

    [[nodiscard]] inline bool is_monitored(int fd) const noexcept // check if fd is monitored by epoll
    {
        struct epoll_event ev;
        return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) != -1 || errno != ENOENT;
    }

private:
    int epoll_fd; // epoll file descriptor

    // Static assertions for compile-time checks
    static_assert(sizeof(int) >= sizeof(void *) || sizeof(int64_t) >= sizeof(void *), // check if int or int64_t can store a pointer
                  "Platform must support storing pointers in epoll_data");
};

class Server
{
public:
    Server(int port, const std::string &staticFolder, int threadCount, int maxRequests, int timeWindow, int cacheSizeMB, int maxAgeSeconds);
    void start();
    void stop();

private:
    Socket socket;                             // server socket
    Router router;                             // server router instance
    ThreadPool pool;                           // server thread pool
    EpollWrapper epoll;                        // server epoll instance
    RateLimiter rateLimiter;                   // server rate limiter
    Cache cache;                               // server cache
    std::mutex connectionsMutex;               // mutex to protect connections map
    std::map<int, ConnectionInfo> connections; // map to store connection info
    std::atomic<bool> shouldStop{false};       // atomic flag to stop server

    void handleClient(int client_socket, const std::string &clientIp);
    void closeConnection(int client_socket);
    void logRequest(int client_socket, const std::string &message);
};

Server::Server(int port, const std::string &staticFolder, int threadCount, int maxRequests, int timeWindow, int cacheSizeMB, int maxAgeSeconds)
    : socket(port),
      router(staticFolder),
      pool(threadCount),
      epoll(),
      rateLimiter(maxRequests, std::chrono::seconds(timeWindow)),
      cache(cacheSizeMB, std::chrono::seconds(maxAgeSeconds))
{
    std::ostringstream oss;
    oss << "Creating dual-stack server on port: " << port
        << "\n   static folder: " << staticFolder
        << "\n   thread count: " << threadCount
        << ", rate limit: " << maxRequests << " requests per " << timeWindow << " seconds"
        << "\n   cache size: " << cacheSizeMB << "MB"
        << ", cache max age: " << maxAgeSeconds << " seconds";

    Logger::getInstance()->info(oss.str());
    socket.bind();
    socket.listen();
}

void Server::start()
{
    Logger::getInstance()->success("Server starting up...");

    try
    {
        // pre-allocate events array with optimal size
        static constexpr size_t MAX_EVENTS = 32;
        struct epoll_event events[MAX_EVENTS];

        // add server socket to epoll
        epoll.add(socket.getSocketFd(), EPOLLIN);
        Logger::getInstance()->success("Server is ready and waiting for connections...");

        while (!shouldStop)
        {
            // use shorter timeout for better responsiveness
            int nfds = epoll.wait(events, MAX_EVENTS, 50);

            if (nfds == -1)
            {
                if (errno == EINTR)
                    continue;
                Logger::getInstance()->error("Epoll wait failed: " + std::string(strerror(errno)));
                break;
            }

            for (int i = 0; i < nfds; ++i)
            {
                if (events[i].data.fd == socket.getSocketFd())
                {
                    // handle new connection
                    std::string clientIp;
                    int client_socket = socket.acceptConnection(clientIp);
                    if (client_socket < 0)
                        continue;

                    // add connection info under lock
                    {
                        std::lock_guard<std::mutex> lock(connectionsMutex);
                        connections.emplace(
                            client_socket,
                            ConnectionInfo{
                                std::chrono::steady_clock::now(),
                                clientIp,
                                false,
                                false,
                                0,
                                0}); // add connection info
                    }

                    try
                    {
                        // add to epoll with edge-triggered mode
                        epoll.add(client_socket, EPOLLIN | EPOLLET);
                    }
                    catch (const std::exception &e)
                    {
                        Logger::getInstance()->error("Failed to add client socket to epoll: " + std::string(e.what()));
                        closeConnection(client_socket);
                        continue;
                    }
                }
                else
                {
                    // handle existing connection
                    int client_socket = events[i].data.fd;
                    std::string clientIp;

                    // get client IP under lock
                    {
                        std::lock_guard<std::mutex> lock(connectionsMutex);
                        auto it = connections.find(client_socket);
                        if (it != connections.end())
                        {
                            clientIp = it->second.ip;
                        }
                    }

                    // enqueue client handling task
                    if (!clientIp.empty())
                    {
                        pool.enqueue([this, client_socket, clientIp]
                                     { handleClient(client_socket, clientIp); });
                    }
                }
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::getInstance()->error("Server error: " + std::string(e.what()));
    }

    Logger::getInstance()->info("Server is shutting down...");
}

void Server::stop()
{
    Logger::getInstance()->warning("Initiating server shutdown...");
    shouldStop = true;

    socket.closeSocket(); // stop accepting new connections

    pool.stop(); // stop worker threads

    // Close all existing connections
    std::vector<int> socketsToClose;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex); // lock scope
        for (const auto &[client_socket, _] : connections)
        {
            socketsToClose.push_back(client_socket);
        }
    }

    for (int socket : socketsToClose)
    {
        closeConnection(socket);
    }

    Logger::getInstance()->info("All connections closed");
}

void Server::handleClient(int client_socket, const std::string &clientIp)
{
    std::vector<char> buffer(1024); // initialize buffer for reading client data
    ssize_t valread;                // variable to store number of bytes read
    std::string request;            // string to accumulate complete client request
    bool connectionClosed = false;  // flag to track if connection has been closed

    // read data from client in a loop
    while ((valread = read(client_socket, buffer.data(), buffer.size())) > 0)
    {
        // check if server should stop and while reading data
        if (shouldStop)
        {
            closeConnection(client_socket);
            return;
        }

        // append read data to request string
        request.append(buffer.data(), valread);

        // update bytes received for this connection
        std::lock_guard<std::mutex> lock(connectionsMutex);
        auto it = connections.find(client_socket);
        if (it != connections.end())
        {
            it->second.bytesReceived += valread;
        }
    }

    // check if connection has been closed by client
    if (valread == 0 || (valread < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
    {
        closeConnection(client_socket);
        connectionClosed = true;
    }

    // process request if connection is still open and we have received data
    if (!connectionClosed && !request.empty())
    {
        std::string path = Http::getRequestPath(request); // extract request path
        bool isAsset = Http::isAssetRequest(path);        // check if it's an asset request

        // log non-asset requests
        if (!isAsset)
        {
            logRequest(client_socket, "Processing request: " + path);
        }

        // apply rate limiting to request
        std::string processedRequest = rateLimiter.process(request);

        // check if request was rate limited
        if (processedRequest == "HTTP/1.1 429 Too Many Requests\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: 19\r\n"
                                "\r\n"
                                "Too Many Requests")
        {
            // send rate limit response to client
            send(client_socket, processedRequest.c_str(), processedRequest.size(), 0);
        }
        else
        {
            // create a compression middleware instance
            Compression compressionMiddleware;
            // route request with compression middleware
            router.route(path, client_socket, clientIp, &compressionMiddleware, &cache);
        }

        // log completion of non-asset requests
        if (!isAsset)
        {
            logRequest(client_socket, "Request completed: " + path);
        }
    }
}

void Server::closeConnection(int client_socket)
{
    std::lock_guard<std::mutex> lock(connectionsMutex); // lock scope
    auto it = connections.find(client_socket);          // find client socket
    if (it != connections.end())
    {
        if (!it->second.isClosureLogged)
        {
            auto duration = std::chrono::steady_clock::now() - it->second.startTime;
            std::string durationStr = Socket::durationToString(duration);

            for (const auto &message : it->second.logBuffer) // log all buffered messages first
            {
                Logger::getInstance()->info(message, it->second.ip);
            }

            Logger::getInstance()->info(
                "Connection closed - Duration: " + durationStr +
                    ", Bytes received: " + std::to_string(it->second.bytesReceived) +
                    ", Bytes sent: " + std::to_string(it->second.bytesSent),
                it->second.ip); // always log connection closure

            it->second.isClosureLogged = true;
        }
        connections.erase(it); // erase connection info
    }

    try
    {
        epoll.remove(client_socket); // remove client socket from epoll
    }
    catch (const std::exception &e)
    {
        Logger::getInstance()->warning("Failed to remove client socket from epoll: " + std::string(e.what()));
    }

    close(client_socket);
}

void Server::logRequest(int client_socket, const std::string &message)
{
    std::lock_guard<std::mutex> lock(connectionsMutex);
    auto it = connections.find(client_socket); // find client socket
    if (it != connections.end())
        it->second.logBuffer.push_back(message);
}

std::unique_ptr<Server> server; // instance of Server

std::atomic<bool> running(true); // flag to control main loop

void signalHandler(int signum)
{
    running = false; // set running flag to false
}

int main(void)
{
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try
    {
        Config config = Parser::parseConfig("pgs_conf.json"); // parse configuration file
        server = std::make_unique<Server>(config.port, config.staticFolder, config.threadCount,
                                          config.rateLimit.maxRequests, config.rateLimit.timeWindow,
                                          config.cache.sizeMB, config.cache.maxAgeSeconds); // create server instance

        std::thread serverThread([&]()
                                 { server->start(); }); // start server in a separate thread

        while (running)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::thread shutdownThread([&]()
                                   { server->stop(); }); // initiate server shutdown in a separate thread

        if (shutdownThread.joinable())
        {
            shutdownThread.join();
        }
        else
        {
            Logger::getInstance()->error("Failed to join shutdown thread");
        }

        if (serverThread.joinable()) // join server thread
        {
            serverThread.join();
        }
        else
        {
            Logger::getInstance()->error("Failed to join server thread");
        }
    }
    catch (const std::exception &e)
    {
        Logger::getInstance()->error("Fatal error: " + std::string(e.what()));
        return EXIT_FAILURE;
    }

    Logger::getInstance()->info("Server shut down successfully");
    server.reset();            // explicitly destroy server object
    Logger::destroyInstance(); // destroy logger instance

    return EXIT_SUCCESS;
}
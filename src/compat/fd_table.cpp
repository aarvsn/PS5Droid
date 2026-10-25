// PS5Droid - Guest File Descriptor Table and Epoll implementation.
#include "compat/fd_table.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "platform/platform.h"

namespace ps5droid {
namespace compat {

namespace {

void set_table_err(int* err, int value) {
    if (err != nullptr)
        *err = value;
}

// ---------------------------------------------------------------------------
// Regular file backed by host stdio. Positioned I/O (offset >= 0) never
// moves the shared file offset, matching POSIX pread/pwrite.
// ---------------------------------------------------------------------------
class FileFdEntry : public FdEntry {
  public:
    FileFdEntry(const std::string& path, FILE* fp) : path_(path), fp_(fp) {}
    ~FileFdEntry() override { close(); }

    FdType type() const override { return FdType::kFile; }

    int64_t read(void* buf, size_t count, int64_t offset, int* err) override {
        if (!fp_) {
            set_err(err, linux_abi::kEbadf);
            return -1;
        }
        if (offset >= 0) {
            long cur = ftell(fp_);
            if (fseek(fp_, static_cast<long>(offset), SEEK_SET) != 0) {
                set_err(err, linux_abi::kEinval);
                return -1;
            }
            size_t r = fread(buf, 1, count, fp_);
            fseek(fp_, cur, SEEK_SET);
            return static_cast<int64_t>(r);
        }
        size_t r = fread(buf, 1, count, fp_);
        if (r < count && ferror(fp_)) {
            set_err(err, linux_abi::kEio);
            return -1;
        }
        return static_cast<int64_t>(r);
    }

    int64_t write(const void* buf, size_t count, int64_t offset, int* err) override {
        if (!fp_) {
            set_err(err, linux_abi::kEbadf);
            return -1;
        }
        if (offset >= 0) {
            long cur = ftell(fp_);
            if (fseek(fp_, static_cast<long>(offset), SEEK_SET) != 0) {
                set_err(err, linux_abi::kEinval);
                return -1;
            }
            size_t w = fwrite(buf, 1, count, fp_);
            fflush(fp_);
            fseek(fp_, cur, SEEK_SET);
            if (w < count && ferror(fp_)) {
                set_err(err, linux_abi::kEio);
                return -1;
            }
            return static_cast<int64_t>(w);
        }
        size_t w = fwrite(buf, 1, count, fp_);
        fflush(fp_);
        if (w < count && ferror(fp_)) {
            set_err(err, linux_abi::kEio);
            return -1;
        }
        return static_cast<int64_t>(w);
    }

    int64_t lseek(int64_t offset, int whence, int* err) override {
        if (!fp_) {
            set_err(err, linux_abi::kEbadf);
            return -1;
        }
        if (fseek(fp_, static_cast<long>(offset), whence) != 0) {
            set_err(err, linux_abi::kEinval);
            return -1;
        }
        return static_cast<int64_t>(ftell(fp_));
    }

    bool fstat(StatBuf* statbuf) override {
        if (!statbuf || !fp_)
            return false;
        memset(statbuf, 0, sizeof(*statbuf));
        long cur = ftell(fp_);
        fseek(fp_, 0, SEEK_END);
        long sz = ftell(fp_);
        fseek(fp_, cur, SEEK_SET);
        statbuf->st_size = sz;
        statbuf->st_mode = 0100644; // S_IFREG | 0644
        return true;
    }

    void close() override {
        if (fp_) {
            fclose(fp_);
            fp_ = nullptr;
        }
    }

  private:
    std::string path_;
    FILE* fp_;
};

// ---------------------------------------------------------------------------
// Process streams (fd 0/1/2). Write-only semantics.
// ---------------------------------------------------------------------------
class StreamFdEntry : public FdEntry {
  public:
    explicit StreamFdEntry(FILE* stream) : stream_(stream) {}
    FdType type() const override { return FdType::kStream; }

    int64_t read(void*, size_t, int64_t, int* err) override {
        set_err(err, linux_abi::kEbadf);
        return -1;
    }
    int64_t write(const void* buf, size_t count, int64_t, int* err) override {
        if (!stream_ || !buf) {
            set_err(err, linux_abi::kEbadf);
            return -1;
        }
        size_t w = fwrite(buf, 1, count, stream_);
        fflush(stream_);
        if (w < count && ferror(stream_)) {
            set_err(err, linux_abi::kEio);
            return -1;
        }
        return static_cast<int64_t>(w);
    }
    int64_t lseek(int64_t, int, int* err) override {
        set_err(err, linux_abi::kEspipe);
        return -1;
    }
    bool fstat(StatBuf* statbuf) override {
        if (!statbuf)
            return false;
        memset(statbuf, 0, sizeof(*statbuf));
        statbuf->st_mode = 0020666; // S_IFCHR
        return true;
    }
    void close() override { stream_ = nullptr; }

    uint32_t readiness() const override { return linux_abi::kEpollOut; }

  private:
    FILE* stream_;
};

// ---------------------------------------------------------------------------
// In-process epoll instance.
// ---------------------------------------------------------------------------
class EpollFdEntry : public FdEntry {
  public:
    explicit EpollFdEntry(CompatFdTable* table) : table_(table) {}
    FdType type() const override { return FdType::kEpoll; }

    int64_t read(void*, size_t, int64_t, int* err) override {
        set_err(err, linux_abi::kEinval);
        return -1;
    }
    int64_t write(const void*, size_t, int64_t, int* err) override {
        set_err(err, linux_abi::kEinval);
        return -1;
    }
    int64_t lseek(int64_t, int, int* err) override {
        set_err(err, linux_abi::kEinval);
        return -1;
    }
    bool fstat(StatBuf* statbuf) override {
        if (!statbuf)
            return false;
        memset(statbuf, 0, sizeof(*statbuf));
        return true;
    }
    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.clear();
    }

    uint32_t readiness() const override { return 0; }

    int ctl(int op, int fd, const EpollEvent* event) {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (op) {
        case linux_abi::kEpollCtlAdd:
            if (event == nullptr)
                return -linux_abi::kEfault;
            if (events_.count(fd) != 0)
                return -linux_abi::kEexist;
            events_[fd] = *event;
            return 0;
        case linux_abi::kEpollCtlDel:
            if (events_.erase(fd) == 0)
                return -linux_abi::kEnoent;
            return 0;
        case linux_abi::kEpollCtlMod:
            if (event == nullptr)
                return -linux_abi::kEfault;
            if (events_.count(fd) == 0)
                return -linux_abi::kEnoent;
            events_[fd] = *event;
            return 0;
        default:
            return -linux_abi::kEinval;
        }
    }

    int wait(EpollEvent* out_events, int maxevents, int timeout_ms);

  private:
    int collect_ready(EpollEvent* out_events, int maxevents) {
        int count = 0;
        for (auto it = events_.begin(); it != events_.end();) {
            auto entry = table_ ? table_->get(it->first) : nullptr;
            if (!entry) {
                it = events_.erase(it);
                continue;
            }
            uint32_t requested = it->second.events;
            uint32_t ready = entry->readiness();
            uint32_t reported = ready & requested;
            if (reported != 0 && count < maxevents) {
                EpollEvent ev = it->second;
                ev.events = reported;
                out_events[count++] = ev;
            }
            ++it;
        }
        return count;
    }

    CompatFdTable* table_;
    std::unordered_map<int, EpollEvent> events_;
    std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// eventfd counter.
// ---------------------------------------------------------------------------
class EventFdEntry : public FdEntry {
  public:
    explicit EventFdEntry(uint64_t initval) : counter_(initval) {}
    FdType type() const override { return FdType::kEventFd; }

    int64_t read(void* buf, size_t count, int64_t, int* err) override {
        if (count < sizeof(uint64_t) || !buf) {
            set_err(err, linux_abi::kEinval);
            return -1;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t val;
        if (semaphore_) {
            if (counter_ == 0) {
                set_err(err, linux_abi::kEagain);
                return -1;
            }
            val = 1;
            --counter_;
        } else {
            val = counter_;
            counter_ = 0;
        }
        std::memcpy(buf, &val, sizeof(uint64_t));
        return sizeof(uint64_t);
    }

    int64_t write(const void* buf, size_t count, int64_t, int* err) override {
        if (count < sizeof(uint64_t) || !buf) {
            set_err(err, linux_abi::kEinval);
            return -1;
        }
        uint64_t val = 0;
        std::memcpy(&val, buf, sizeof(uint64_t));
        std::lock_guard<std::mutex> lock(mutex_);
        if (val > linux_abi::kEventfdMax - counter_) {
            set_err(err, linux_abi::kEinval);
            return -1;
        }
        counter_ += val;
        return sizeof(uint64_t);
    }

    int64_t lseek(int64_t, int, int* err) override {
        set_err(err, linux_abi::kEspipe);
        return -1;
    }
    bool fstat(StatBuf* statbuf) override {
        if (!statbuf)
            return false;
        memset(statbuf, 0, sizeof(*statbuf));
        return true;
    }
    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        counter_ = 0;
    }

    uint32_t readiness() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t r = 0;
        if (counter_ > 0)
            r |= linux_abi::kEpollIn;
        if (counter_ < linux_abi::kEventfdMax)
            r |= linux_abi::kEpollOut;
        return r;
    }

    void set_semaphore(bool on) {
        std::lock_guard<std::mutex> lock(mutex_);
        semaphore_ = on;
    }

  private:
    mutable std::mutex mutex_;
    uint64_t counter_ = 0;
    bool semaphore_ = false;
};

// ---------------------------------------------------------------------------
// In-process unidirectional Pipe endpoint.
// ---------------------------------------------------------------------------
class PipeFdEntry : public FdEntry {
  public:
    struct PipeState {
        std::mutex mutex;
        std::vector<uint8_t> buffer;
        bool reader_closed = false;
        bool writer_closed = false;
    };

    PipeFdEntry(std::shared_ptr<PipeState> state, bool is_writer)
        : state_(state), is_writer_(is_writer) {}

    FdType type() const override { return FdType::kPipe; }

    int64_t read(void* buf, size_t count, int64_t, int* err) override {
        if (is_writer_) {
            set_err(err, linux_abi::kEbadf);
            return -1;
        }
        if (!buf || count == 0)
            return 0;

        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->buffer.empty()) {
            if (state_->writer_closed)
                return 0; // EOF
            set_err(err, linux_abi::kEagain);
            return -1;
        }

        size_t to_copy = std::min(count, state_->buffer.size());
        std::memcpy(buf, state_->buffer.data(), to_copy);
        state_->buffer.erase(state_->buffer.begin(), state_->buffer.begin() + to_copy);
        return static_cast<int64_t>(to_copy);
    }

    int64_t write(const void* buf, size_t count, int64_t, int* err) override {
        if (!is_writer_) {
            set_err(err, linux_abi::kEbadf);
            return -1;
        }
        if (!buf || count == 0)
            return 0;

        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->reader_closed) {
            set_err(err, linux_abi::kEpipe);
            return -1;
        }

        const uint8_t* u8 = static_cast<const uint8_t*>(buf);
        state_->buffer.insert(state_->buffer.end(), u8, u8 + count);
        return static_cast<int64_t>(count);
    }

    int64_t lseek(int64_t, int, int* err) override {
        set_err(err, linux_abi::kEspipe);
        return -1;
    }

    bool fstat(StatBuf* statbuf) override {
        if (!statbuf)
            return false;
        memset(statbuf, 0, sizeof(*statbuf));
        statbuf->st_mode = 0010666; // S_IFIFO
        return true;
    }

    void close() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (is_writer_) {
            state_->writer_closed = true;
        } else {
            state_->reader_closed = true;
            state_->buffer.clear();
        }
    }

    uint32_t readiness() const override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        uint32_t r = 0;
        if (!is_writer_) {
            if (!state_->buffer.empty() || state_->writer_closed)
                r |= linux_abi::kEpollIn;
        } else {
            if (!state_->reader_closed)
                r |= linux_abi::kEpollOut;
        }
        return r;
    }

  private:
    std::shared_ptr<PipeState> state_;
    bool is_writer_;
};

} // namespace

int EpollFdEntry::wait(EpollEvent* out_events, int maxevents, int timeout_ms) {
    if (out_events == nullptr || maxevents <= 0)
        return -1;

    if (timeout_ms == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        return collect_ready(out_events, maxevents);
    }

    int64_t deadline_ns = -1;
    if (timeout_ms > 0)
        deadline_ns = static_cast<int64_t>(platform::monotonic_ns()) +
                      static_cast<int64_t>(timeout_ms) * 1000000;

    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            int n = collect_ready(out_events, maxevents);
            if (n > 0)
                return n;
        }
        if (deadline_ns >= 0 && static_cast<int64_t>(platform::monotonic_ns()) >= deadline_ns) {
            return 0;
        }
        platform::sleep_ns(1000000);
    }
}

CompatFdTable::CompatFdTable() {
    table_[0] = std::make_shared<StreamFdEntry>(stdin);
    table_[1] = std::make_shared<StreamFdEntry>(stdout);
    table_[2] = std::make_shared<StreamFdEntry>(stderr);
}

CompatFdTable::~CompatFdTable() {
    std::lock_guard<std::mutex> lock(mutex_);
    table_.clear();
}

int CompatFdTable::allocate_fd_locked() {
    int fd = 0;
    while (table_.count(fd) != 0)
        ++fd;
    return fd;
}

int CompatFdTable::open_file(const std::string& path, int flags, int mode) {
    (void)mode;
    using namespace linux_abi;

    const bool cread = (flags & kOCreat) != 0;
    const bool excl = (flags & kOExcl) != 0;
    const bool trunc = (flags & kOTrunc) != 0;
    const bool append = (flags & kOAppend) != 0;
    const int access = flags & kOAccmode;
    if (access != kORdonly && access != kOWronly && access != kORdwr)
        return -kEinval;

    const bool exists = platform::file_exists(path);
    if (excl && cread && exists)
        return -kEexist;
    if (!cread && !exists)
        return -kEnoent;

    if (cread && !exists) {
        FILE* creator = fopen(path.c_str(), "ab");
        if (creator == nullptr)
            return -kEaccess;
        fclose(creator);
    }

    const char* fmode;
    if (append) {
        fmode = (access == kORdwr) ? "a+b" : "ab";
    } else if (trunc) {
        fmode = (access == kOWronly) ? "wb" : "w+b";
    } else {
        fmode = (access == kORdonly) ? "rb" : "r+b";
    }
    FILE* fp = fopen(path.c_str(), fmode);
    if (fp == nullptr)
        return -kEaccess;

    std::lock_guard<std::mutex> lock(mutex_);
    int fd = allocate_fd_locked();
    table_[fd] = std::make_shared<FileFdEntry>(path, fp);
    return fd;
}

int CompatFdTable::create_epoll() {
    std::lock_guard<std::mutex> lock(mutex_);
    int fd = allocate_fd_locked();
    table_[fd] = std::make_shared<EpollFdEntry>(this);
    return fd;
}

int CompatFdTable::create_eventfd(uint64_t initval, int flags) {
    using namespace linux_abi;
    if (initval > kEventfdMax)
        return -kEinval;

    std::lock_guard<std::mutex> lock(mutex_);
    int fd = allocate_fd_locked();
    auto entry = std::make_shared<EventFdEntry>(initval);
    entry->set_semaphore((flags & kEfdSemaphore) != 0);
    table_[fd] = entry;
    return fd;
}

int CompatFdTable::create_pipe(int pipefds[2], int flags) {
    (void)flags;
    if (!pipefds)
        return -linux_abi::kEfault;

    auto state = std::make_shared<PipeFdEntry::PipeState>();
    auto read_entry = std::make_shared<PipeFdEntry>(state, false);
    auto write_entry = std::make_shared<PipeFdEntry>(state, true);

    std::lock_guard<std::mutex> lock(mutex_);
    int rfd = allocate_fd_locked();
    table_[rfd] = read_entry;
    int wfd = allocate_fd_locked();
    table_[wfd] = write_entry;

    pipefds[0] = rfd;
    pipefds[1] = wfd;
    return 0;
}

int CompatFdTable::epoll_ctl(int epfd, int op, int fd, const EpollEvent* event) {
    auto entry = get(epfd);
    if (!entry || entry->type() != FdType::kEpoll)
        return -linux_abi::kEbadf;
    auto epoll = std::static_pointer_cast<EpollFdEntry>(entry);
    return epoll->ctl(op, fd, event);
}

int CompatFdTable::epoll_wait(int epfd, EpollEvent* events, int maxevents, int timeout_ms) {
    auto entry = get(epfd);
    if (!entry || entry->type() != FdType::kEpoll)
        return -1;
    auto epoll = std::static_pointer_cast<EpollFdEntry>(entry);
    return epoll->wait(events, maxevents, timeout_ms);
}

int64_t CompatFdTable::read(int fd, void* buf, size_t count, int* err) {
    auto entry = get(fd);
    if (!entry) {
        set_table_err(err, linux_abi::kEbadf);
        return -1;
    }
    return entry->read(buf, count, -1, err);
}

int64_t CompatFdTable::pread64(int fd, void* buf, size_t count, int64_t offset, int* err) {
    auto entry = get(fd);
    if (!entry) {
        set_table_err(err, linux_abi::kEbadf);
        return -1;
    }
    return entry->read(buf, count, offset, err);
}

int64_t CompatFdTable::write(int fd, const void* buf, size_t count, int* err) {
    auto entry = get(fd);
    if (!entry) {
        set_table_err(err, linux_abi::kEbadf);
        return -1;
    }
    return entry->write(buf, count, -1, err);
}

int64_t CompatFdTable::lseek(int fd, int64_t offset, int whence, int* err) {
    auto entry = get(fd);
    if (!entry) {
        set_table_err(err, linux_abi::kEbadf);
        return -1;
    }
    return entry->lseek(offset, whence, err);
}

bool CompatFdTable::fstat(int fd, StatBuf* statbuf) {
    auto entry = get(fd);
    if (!entry)
        return false;
    return entry->fstat(statbuf);
}

int CompatFdTable::dup(int oldfd) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find(oldfd);
    if (it == table_.end())
        return -1;
    int newfd = allocate_fd_locked();
    table_[newfd] = it->second;
    return newfd;
}

int CompatFdTable::dup2(int oldfd, int newfd) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find(oldfd);
    if (it == table_.end() || newfd < 0)
        return -1;
    if (oldfd == newfd)
        return newfd;
    table_[newfd] = it->second;
    return newfd;
}

int CompatFdTable::close(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find(fd);
    if (it == table_.end())
        return -1;
    it->second->close();
    table_.erase(it);
    return 0;
}

std::shared_ptr<FdEntry> CompatFdTable::get(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find(fd);
    if (it == table_.end())
        return nullptr;
    return it->second;
}

} // namespace compat
} // namespace ps5droid

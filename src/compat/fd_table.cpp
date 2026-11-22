// PS5Droid - Guest File Descriptor Table and Epoll implementation.
#include "compat/fd_table.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

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

    int fsync(int* err) override {
        if (!fp_) {
            set_err(err, linux_abi::kEbadf);
            return -1;
        }
        if (fflush(fp_) != 0) {
            set_err(err, linux_abi::kEio);
            return -1;
        }
        return 0;
    }

    int ioctl(unsigned long request, void* arg, int* err) override {
        if (request != linux_abi::kFionread)
            return FdEntry::ioctl(request, arg, err);
        if (!arg) {
            set_err(err, linux_abi::kEfault);
            return -1;
        }
        if (!fp_) {
            set_err(err, linux_abi::kEbadf);
            return -1;
        }
        long cur = ftell(fp_);
        if (cur < 0 || fseek(fp_, 0, SEEK_END) != 0) {
            set_err(err, linux_abi::kEio);
            return -1;
        }
        long end = ftell(fp_);
        fseek(fp_, cur, SEEK_SET);
        *static_cast<int*>(arg) = end > cur ? static_cast<int>(end - cur) : 0;
        return 0;
    }

    const std::string* lock_path() const override { return &path_; }

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
    int fsync(int* err) override {
        if (!stream_) {
            set_err(err, linux_abi::kEbadf);
            return -1;
        }
        if (fflush(stream_) != 0) {
            set_err(err, linux_abi::kEio);
            return -1;
        }
        return 0;
    }

    int ioctl(unsigned long request, void* arg, int* err) override {
        if (request != linux_abi::kFionread)
            return FdEntry::ioctl(request, arg, err);
        if (!arg) {
            set_err(err, linux_abi::kEfault);
            return -1;
        }
        *static_cast<int*>(arg) = 0;
        return 0;
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

    int ioctl(unsigned long request, void* arg, int* err) override {
        if (request != linux_abi::kFionread)
            return FdEntry::ioctl(request, arg, err);
        if (!arg) {
            set_err(err, linux_abi::kEfault);
            return -1;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        *static_cast<int*>(arg) = static_cast<int>(state_->buffer.size());
        return 0;
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

// ---------------------------------------------------------------------------
// Directory stream (getdents64). Names come from the portable platform
// directory listing; inode numbers are synthetic and d_type is DT_UNKNOWN
// because the platform layer does not expose either.
// ---------------------------------------------------------------------------
class DirFdEntry : public FdEntry {
  public:
    explicit DirFdEntry(const std::string& path) : path_(path) {
        names_.push_back(".");
        names_.push_back("..");
        for (const std::string& name : platform::list_dir(path))
            names_.push_back(name);
    }

    FdType type() const override { return FdType::kDirectory; }

    int64_t read(void*, size_t, int64_t, int* err) override {
        set_err(err, linux_abi::kEisdir);
        return -1;
    }
    int64_t write(const void*, size_t, int64_t, int* err) override {
        set_err(err, linux_abi::kEisdir);
        return -1;
    }
    int64_t lseek(int64_t offset, int whence, int* err) override {
        if (offset == 0 && whence == 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            index_ = 0;
            return 0;
        }
        set_err(err, linux_abi::kEinval);
        return -1;
    }
    bool fstat(StatBuf* statbuf) override {
        if (!statbuf)
            return false;
        memset(statbuf, 0, sizeof(*statbuf));
        statbuf->st_mode = 0040755; // S_IFDIR | 0755
        return true;
    }
    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        names_.clear();
        index_ = 0;
    }

    int64_t getdents64(void* buf, size_t count, int* err) override {
        if (!buf) {
            set_err(err, linux_abi::kEfault);
            return -1;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        size_t written = 0;
        while (index_ < names_.size()) {
            const std::string& name = names_[index_];
            size_t reclen = (19 + name.size() + 1 + 7) & ~static_cast<size_t>(7);
            if (reclen > count - written) {
                if (written == 0) {
                    set_err(err, linux_abi::kEinval);
                    return -1;
                }
                break;
            }
            uint8_t* rec = static_cast<uint8_t*>(buf) + written;
            memset(rec, 0, reclen);
            uint64_t ino = static_cast<uint64_t>(index_ + 1);
            int64_t off = static_cast<int64_t>(index_ + 1);
            memcpy(rec + 0, &ino, sizeof(ino));
            memcpy(rec + 8, &off, sizeof(off));
            uint16_t rl = static_cast<uint16_t>(reclen);
            memcpy(rec + 16, &rl, sizeof(rl));
            rec[18] = 0; // DT_UNKNOWN
            memcpy(rec + 19, name.c_str(), name.size() + 1);
            written += reclen;
            ++index_;
        }
        return static_cast<int64_t>(written);
    }

    const std::string* lock_path() const override { return &path_; }

    uint32_t readiness() const override { return linux_abi::kEpollIn; }

  private:
    std::string path_;
    std::vector<std::string> names_;
    size_t index_ = 0;
    mutable std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// In-process memory file (memfd_create).
// ---------------------------------------------------------------------------
class MemFdEntry : public FdEntry {
  public:
    explicit MemFdEntry(std::string name) : name_(std::move(name)) {}

    FdType type() const override { return FdType::kMemFd; }

    int64_t read(void* buf, size_t count, int64_t offset, int* err) override {
        if (!buf && count > 0) {
            set_err(err, linux_abi::kEfault);
            return -1;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        size_t pos = offset >= 0 ? static_cast<size_t>(offset) : offset_;
        if (pos >= data_.size())
            return 0;
        size_t n = std::min(count, data_.size() - pos);
        memcpy(buf, data_.data() + pos, n);
        if (offset < 0)
            offset_ = pos + n;
        return static_cast<int64_t>(n);
    }

    int64_t write(const void* buf, size_t count, int64_t offset, int* err) override {
        if (!buf && count > 0) {
            set_err(err, linux_abi::kEfault);
            return -1;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        size_t pos = offset >= 0 ? static_cast<size_t>(offset) : offset_;
        if (pos + count > data_.size())
            data_.resize(pos + count, 0);
        memcpy(data_.data() + pos, buf, count);
        if (offset < 0)
            offset_ = pos + count;
        return static_cast<int64_t>(count);
    }

    int64_t lseek(int64_t offset, int whence, int* err) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int64_t base;
        if (whence == 0) {
            base = 0;
        } else if (whence == 1) {
            base = static_cast<int64_t>(offset_);
        } else if (whence == 2) {
            base = static_cast<int64_t>(data_.size());
        } else {
            set_err(err, linux_abi::kEinval);
            return -1;
        }
        int64_t next = base + offset;
        if (next < 0) {
            set_err(err, linux_abi::kEinval);
            return -1;
        }
        offset_ = static_cast<size_t>(next);
        return next;
    }

    bool fstat(StatBuf* statbuf) override {
        if (!statbuf)
            return false;
        std::lock_guard<std::mutex> lock(mutex_);
        memset(statbuf, 0, sizeof(*statbuf));
        statbuf->st_mode = 0100666; // S_IFREG | 0666
        statbuf->st_size = static_cast<int64_t>(data_.size());
        statbuf->st_nlink = 1;
        return true;
    }

    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.clear();
        offset_ = 0;
    }

    int ioctl(unsigned long request, void* arg, int* err) override {
        if (request != linux_abi::kFionread)
            return FdEntry::ioctl(request, arg, err);
        if (!arg) {
            set_err(err, linux_abi::kEfault);
            return -1;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        size_t remaining = offset_ < data_.size() ? data_.size() - offset_ : 0;
        *static_cast<int*>(arg) = static_cast<int>(remaining);
        return 0;
    }

    uint32_t readiness() const override { return linux_abi::kEpollIn | linux_abi::kEpollOut; }

  private:
    std::string name_;
    std::vector<uint8_t> data_;
    size_t offset_ = 0;
    mutable std::mutex mutex_;
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
    table_[0] = TableSlot{std::make_shared<StreamFdEntry>(stdin), 0};
    table_[1] = TableSlot{std::make_shared<StreamFdEntry>(stdout), 0};
    table_[2] = TableSlot{std::make_shared<StreamFdEntry>(stderr), 0};
}

CompatFdTable::~CompatFdTable() {
    std::lock_guard<std::mutex> lock(mutex_);
    table_.clear();
}

int CompatFdTable::allocate_fd_locked(int minfd) {
    int fd = minfd < 0 ? 0 : minfd;
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
    const bool directory = (flags & kODirectory) != 0;
    const int access = flags & kOAccmode;
    if (access != kORdonly && access != kOWronly && access != kORdwr)
        return -kEinval;

    const bool exists = platform::file_exists(path);
    const bool is_dir = exists && platform::is_directory(path);

    if (directory) {
        if (!exists)
            return -kEnoent;
        if (!is_dir)
            return -kEnotdir;
        if (access != kORdonly)
            return -kEisdir;
    } else if (is_dir && access != kORdonly) {
        return -kEisdir;
    }
    if (is_dir && (cread || trunc))
        return -kEisdir;

    if (excl && cread && exists)
        return -kEexist;
    if (!cread && !exists)
        return -kEnoent;

    if (is_dir) {
        std::lock_guard<std::mutex> lock(mutex_);
        int fd = allocate_fd_locked();
        auto entry = std::make_shared<DirFdEntry>(path);
        entry->set_status_flags(flags);
        table_[fd] = TableSlot{entry, 0};
        return fd;
    }

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
    auto entry = std::make_shared<FileFdEntry>(path, fp);
    entry->set_status_flags(flags);
    table_[fd] = TableSlot{entry, 0};
    return fd;
}

int CompatFdTable::create_epoll() {
    std::lock_guard<std::mutex> lock(mutex_);
    int fd = allocate_fd_locked();
    table_[fd] = TableSlot{std::make_shared<EpollFdEntry>(this), 0};
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
    int fd_flags = (flags & kEfdCloexec) ? kFdCloexec : 0;
    table_[fd] = TableSlot{entry, fd_flags};
    return fd;
}

int CompatFdTable::create_memfd(const std::string& name, int flags) {
    using namespace linux_abi;
    if ((flags & ~(kMemfdCloexec | kMemfdAllowSealing)) != 0)
        return -kEinval;

    std::lock_guard<std::mutex> lock(mutex_);
    int fd = allocate_fd_locked();
    auto entry = std::make_shared<MemFdEntry>(name);
    int fd_flags = (flags & kMemfdCloexec) ? kFdCloexec : 0;
    table_[fd] = TableSlot{entry, fd_flags};
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
    table_[rfd] = TableSlot{read_entry, 0};
    int wfd = allocate_fd_locked();
    table_[wfd] = TableSlot{write_entry, 0};

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

int64_t CompatFdTable::getdents64(int fd, void* buf, size_t count, int* err) {
    auto entry = get(fd);
    if (!entry) {
        set_table_err(err, linux_abi::kEbadf);
        return -1;
    }
    return entry->getdents64(buf, count, err);
}

int CompatFdTable::ioctl(int fd, unsigned long request, void* arg, int* err) {
    auto entry = get(fd);
    if (!entry) {
        set_table_err(err, linux_abi::kEbadf);
        return -1;
    }
    return entry->ioctl(request, arg, err);
}

int CompatFdTable::poll(PollFd* fds, int nfds, int timeout_ms, int* err) {
    using namespace linux_abi;
    if (nfds < 0 || (nfds > 0 && fds == nullptr)) {
        set_table_err(err, nfds < 0 ? kEinval : kEfault);
        return -1;
    }

    int64_t deadline_ns = -1;
    if (timeout_ms > 0)
        deadline_ns = static_cast<int64_t>(platform::monotonic_ns()) +
                      static_cast<int64_t>(timeout_ms) * 1000000;

    while (true) {
        int ready = 0;
        for (int i = 0; i < nfds; ++i) {
            fds[i].revents = 0;
            auto entry = get(fds[i].fd);
            if (!entry) {
                fds[i].revents = static_cast<int16_t>(kPollNval);
                ++ready;
                continue;
            }
            uint32_t mask = entry->readiness();
            int16_t re = 0;
            if ((mask & kEpollIn) != 0 && (fds[i].events & kPollIn) != 0)
                re = static_cast<int16_t>(re | kPollIn);
            if ((mask & kEpollOut) != 0 && (fds[i].events & kPollOut) != 0)
                re = static_cast<int16_t>(re | kPollOut);
            fds[i].revents = re;
            if (re != 0)
                ++ready;
        }
        if (ready > 0)
            return ready;
        if (timeout_ms == 0)
            return 0;
        if (deadline_ns >= 0 && static_cast<int64_t>(platform::monotonic_ns()) >= deadline_ns)
            return 0;
        platform::sleep_ns(1000000);
    }
}

bool CompatFdTable::try_flock_locked(const std::string& key, FdEntry* owner, int op) {
    using namespace linux_abi;
    const int base = op & ~kFlockNb;
    auto it = flocks_.find(key);

    if (base == kFlockUn) {
        if (it == flocks_.end())
            return true;
        it->second.shared.erase(owner);
        if (it->second.exclusive == owner)
            it->second.exclusive = nullptr;
        if (it->second.exclusive == nullptr && it->second.shared.empty())
            flocks_.erase(it);
        return true;
    }
    if (base != kFlockSh && base != kFlockEx)
        return false;

    if (it == flocks_.end())
        it = flocks_.emplace(key, FlockState{}).first;
    FlockState& state = it->second;

    if (base == kFlockSh) {
        if (state.exclusive != nullptr && state.exclusive != owner)
            return false;
        state.shared[owner] = 1;
        return true;
    }

    if (state.exclusive != nullptr && state.exclusive != owner)
        return false;
    for (const auto& entry : state.shared) {
        if (entry.first != owner)
            return false;
    }
    state.shared.erase(owner);
    state.exclusive = owner;
    return true;
}

void CompatFdTable::release_locks_locked(FdEntry* entry) {
    for (auto it = flocks_.begin(); it != flocks_.end();) {
        it->second.shared.erase(entry);
        if (it->second.exclusive == entry)
            it->second.exclusive = nullptr;
        if (it->second.exclusive == nullptr && it->second.shared.empty()) {
            it = flocks_.erase(it);
        } else {
            ++it;
        }
    }
}

int CompatFdTable::flock(int fd, int op, int* err) {
    using namespace linux_abi;
    auto entry = get(fd);
    if (!entry) {
        set_table_err(err, kEbadf);
        return -1;
    }
    const std::string* path = entry->lock_path();
    std::string key =
        path ? *path : "entry:" + std::to_string(reinterpret_cast<uintptr_t>(entry.get()));
    const bool nonblock = (op & kFlockNb) != 0;
    const int base = op & ~kFlockNb;
    if (base != kFlockSh && base != kFlockEx && base != kFlockUn) {
        set_table_err(err, kEinval);
        return -1;
    }

    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (try_flock_locked(key, entry.get(), op))
                return 0;
        }
        if (nonblock) {
            set_table_err(err, kEagain);
            return -1;
        }
        platform::sleep_ns(1000000);
    }
}

int CompatFdTable::dup(int oldfd) {
    return dup_minfd(oldfd, 0, false);
}

int CompatFdTable::dup2(int oldfd, int newfd) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find(oldfd);
    if (it == table_.end() || newfd < 0)
        return -1;
    if (oldfd == newfd)
        return newfd;
    auto existing = table_.find(newfd);
    if (existing != table_.end()) {
        if (existing->second.entry.use_count() == 1)
            release_locks_locked(existing->second.entry.get());
        existing->second.entry->close();
    }
    auto entry = it->second.entry;
    table_[newfd] = TableSlot{entry, 0};
    return newfd;
}

int CompatFdTable::dup_minfd_locked(int oldfd, int minfd, bool cloexec) {
    auto it = table_.find(oldfd);
    if (it == table_.end() || minfd < 0)
        return -linux_abi::kEbadf;
    int newfd = allocate_fd_locked(minfd);
    int fd_flags = cloexec ? linux_abi::kFdCloexec : 0;
    table_[newfd] = TableSlot{it->second.entry, fd_flags};
    return newfd;
}

int CompatFdTable::dup_minfd(int oldfd, int minfd, bool cloexec) {
    std::lock_guard<std::mutex> lock(mutex_);
    return dup_minfd_locked(oldfd, minfd, cloexec);
}

int CompatFdTable::fsync(int fd, int* err) {
    auto entry = get(fd);
    if (!entry) {
        set_table_err(err, linux_abi::kEbadf);
        return -1;
    }
    return entry->fsync(err);
}

int CompatFdTable::fcntl(int fd, int cmd, uint64_t arg) {
    using namespace linux_abi;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find(fd);
    if (it == table_.end())
        return -kEbadf;

    switch (cmd) {
    case kFDupfd:
        return dup_minfd_locked(fd, static_cast<int>(arg), false);
    case kFDupfdCloexec:
        return dup_minfd_locked(fd, static_cast<int>(arg), true);
    case kFGetfd:
        return it->second.fd_flags;
    case kFSetfd:
        it->second.fd_flags = static_cast<int>(arg) & kFdCloexec;
        return 0;
    case kFGetfl:
        return it->second.entry->status_flags();
    case kFSetfl: {
        int old_fl = it->second.entry->status_flags();
        int modifiable = kOAppend | kONonblock;
        int new_fl = (old_fl & ~modifiable) | (static_cast<int>(arg) & modifiable);
        it->second.entry->set_status_flags(new_fl);
        return 0;
    }
    default:
        return -kEinval;
    }
}

int CompatFdTable::close(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find(fd);
    if (it == table_.end())
        return -1;
    // flock locks belong to the open file description: dup'ed fds share it,
    // so release only when the last descriptor referencing the entry closes.
    if (it->second.entry.use_count() == 1)
        release_locks_locked(it->second.entry.get());
    it->second.entry->close();
    table_.erase(it);
    return 0;
}

std::shared_ptr<FdEntry> CompatFdTable::get(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find(fd);
    if (it == table_.end())
        return nullptr;
    return it->second.entry;
}

} // namespace compat
} // namespace ps5droid

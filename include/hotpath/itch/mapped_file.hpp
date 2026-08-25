#pragma once
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hotpath::itch {

// Read-only mmap of the decompressed ITCH file.
//
// Why mmap and not read(): the parse is a single forward pass over ~12 GB. A
// read() loop would copy every byte into a userspace buffer first; mapping it
// lets the parser touch page-cache pages directly, so the message views point
// straight at the kernel's copy and the hot loop performs no I/O calls at all.
// That is what makes the "zero syscalls in steady state" invariant achievable.
class MappedFile {
public:
  MappedFile() = default;

  explicit MappedFile(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) throw std::runtime_error("open failed: " + path);
    struct ::stat st{};
    if (::fstat(fd_, &st) != 0) { ::close(fd_); throw std::runtime_error("fstat failed: " + path); }
    size_ = static_cast<std::size_t>(st.st_size);
    if (size_ == 0) { ::close(fd_); throw std::runtime_error("empty file: " + path); }
    void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (p == MAP_FAILED) { ::close(fd_); throw std::runtime_error("mmap failed: " + path); }
    data_ = static_cast<const std::uint8_t*>(p);
    // One-time hint, at startup, outside any measured region. Tells the kernel
    // to read ahead aggressively and drop pages behind us -- without it a 12 GB
    // sequential pass evicts everything else on the machine.
    ::madvise(const_cast<void*>(p), size_, MADV_SEQUENTIAL);
  }

  ~MappedFile() { release(); }

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&& o) noexcept
      : data_(o.data_), size_(o.size_), fd_(o.fd_) { o.data_ = nullptr; o.size_ = 0; o.fd_ = -1; }
  MappedFile& operator=(MappedFile&& o) noexcept {
    if (this != &o) {
      // release(), not an explicit destructor call: invoking ~MappedFile()
      // here ends the object's lifetime, and everything after it is touching a
      // dead object. It happens to work for a type this trivial, which is
      // exactly why it survives both review and the sanitizers.
      release();
      data_ = o.data_; size_ = o.size_; fd_ = o.fd_;
      o.data_ = nullptr; o.size_ = 0; o.fd_ = -1;
    }
    return *this;
  }

  [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }

private:
  void release() noexcept {
    if (data_) ::munmap(const_cast<std::uint8_t*>(data_), size_);
    if (fd_ >= 0) ::close(fd_);
    data_ = nullptr; size_ = 0; fd_ = -1;
  }

  const std::uint8_t* data_{nullptr};
  std::size_t size_{0};
  int fd_{-1};
};

} // namespace hotpath::itch

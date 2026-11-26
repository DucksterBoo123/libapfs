#include <algorithm>
#include <cassert>
#include <cerrno>
#include <limits>
#include <mutex>
#include <libapfs/BlockReader.hpp>
#include <libapfs/util.hpp>
#include <memory>
#ifdef _WIN32
#include <Windows.h>
#define apfs_fseek _fseeki64
#else
#define apfs_fseek fseeko
#endif

namespace apfs {
namespace {
// All readers can share the same CRT FILE*. Serialise the fallback seek/read
// pair so simultaneous operations cannot move the shared file position.
std::mutex fallback_io_mutex;

#ifdef _WIN32
std::shared_ptr<void> open_overlapped_handle(const std::string &path) {
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  if (h == INVALID_HANDLE_VALUE) return {};
  return std::shared_ptr<void>(h, [](void *p) { CloseHandle(static_cast<HANDLE>(p)); });
}

void read_at(HANDLE h, uint64_t pos, char *dst, size_t length) {
  // Per-request event: required for safe independent concurrent overlapped I/O.
  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ev) throw Error("CreateEventW failed: " + std::to_string(GetLastError()));
  try {
    size_t completed = 0;
    while (completed < length) {
      // ReadFile accepts DWORD lengths, and 4MB is an intentionally bounded
      // transfer size that avoids large driver allocations.
      const DWORD chunk = static_cast<DWORD>(std::min<size_t>(length - completed, 4u * 1024u * 1024u));
      const uint64_t at = pos + completed;
      OVERLAPPED ov{};
      ov.Offset = static_cast<DWORD>(at);
      ov.OffsetHigh = static_cast<DWORD>(at >> 32);
      ov.hEvent = ev;
      ResetEvent(ev);
      DWORD received = 0;
      BOOL ok = ReadFile(h, dst + completed, chunk, &received, &ov);
      if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(h, &ov, &received, TRUE);
      }
      if (!ok || received != chunk) {
        const DWORD err = ok ? ERROR_HANDLE_EOF : GetLastError();
        throw Error("ReadFile failed or returned short read at offset " + std::to_string(at) + ": " + std::to_string(err));
      }
      completed += received;
    }
  } catch (...) {
    CloseHandle(ev);
    throw;
  }
  CloseHandle(ev);
}
#endif
}

bytes_t BlockReader::read_block(uint64_t block_num) const {
  return read_blocks(block_num, 1);
}

bytes_t BlockReader::read_blocks(uint64_t block_num, uint64_t num_blocks) const {
  if (BLOCK_SIZE == 0 || num_blocks > (std::numeric_limits<size_t>::max)() / BLOCK_SIZE ||
      block_num > ((std::numeric_limits<uint64_t>::max)() - offset) / BLOCK_SIZE) {
    throw Error("invalid APFS block read bounds");
  }
  const size_t length = static_cast<size_t>(num_blocks * BLOCK_SIZE);
  const uint64_t pos = block_num * BLOCK_SIZE + offset;
  if (length > (std::numeric_limits<uint64_t>::max)() - pos) {
    throw Error("APFS read range overflows physical offset");
  }
  bytes_t data(length, 0);
  if (length == 0) return data;
#ifdef _WIN32
  if (native_handle) {
    read_at(static_cast<HANDLE>(native_handle.get()), pos, data.data(), data.size());
    return data;
  }
#endif
  std::lock_guard<std::mutex> guard(fallback_io_mutex);
  if (apfs_fseek(*f, pos, SEEK_SET) != 0 || fread(data.data(), 1, data.size(), *f) != data.size()) {
    throw Error("APFS device read failed at byte offset " + std::to_string(pos));
  }
  return data;
}

// Set `BLOCK_SIZE` to be the actual apfs block size
void BlockReader::set_apfs_block_size() {
  try {
    BLOCK_SIZE = NX_DEFAULT_BLOCK_SIZE;
    BLOCK_SIZE = read_object<nx_superblock_t>(0).nx_block_size;
  } catch (const apfs::Error &e) {
    throw Error("file is not the start of an APFS container (offset=" + std::to_string(offset) + "), with error " + e.what());
  }
}

BlockReader::BlockReader(std::string filename, bool apfs, uint64_t offset) : filename(filename), offset(offset) {
  f = std::make_shared<FILE *>(fopen(filename.data(), "rb"));
  if (*f == NULL) {
    throw Error("could not open file " + filename + ": " + std::system_category().message(errno));
  }

#ifdef _WIN32
  native_handle = open_overlapped_handle(filename);
  std::fprintf(stderr, "libapfs: offset I/O %s\n", native_handle ? "enabled" : "unavailable, using locked CRT fallback");
#endif
  if (apfs) {
    set_apfs_block_size();
  }
}

BlockReader::BlockReader(const BlockReader &reader, bool apfs, uint64_t offset)
    : f(reader.f), offset(offset)
#ifdef _WIN32
    , native_handle(reader.native_handle)
#endif
    , filename(reader.filename) {
  if (apfs) {
    set_apfs_block_size();
  }
}

BlockReader::BlockReader(FILE *fd) : offset(0) {
  f = std::make_shared<FILE *>(fd);
  if (*f == NULL) {
    throw Error("invalid file descriptor passed");
  }
}

BlockReader::~BlockReader() {
  if (f.use_count() == 1)
    fclose(*f);
}

// We need to overload the `read_object` function for some types that have variable sizes.
template <>
checkpoint_map_phys_t BlockReader::read_object(uint64_t block_num) const {
  bytes_t mem = read_block(block_num);
  if (!verify_object_checksum((void *)mem.data(), BLOCK_SIZE)) {
    throw Error("block number " + std::to_string(block_num) + ", checksum verification failed while reading object");
  }
  size_t off = sizeof(checkpoint_map_phys_t) - sizeof(std::vector<checkpoint_mapping_t>);
  checkpoint_map_phys_t obj;
  memcpy((void *)&obj, mem.data(), off);
  obj.cpm_map.resize(obj.cpm_count);
  memcpy(obj.cpm_map.data(), mem.data() + off, obj.cpm_count * sizeof(checkpoint_mapping_t));
  return obj;
}
template <>
btree_node_phys_t BlockReader::read_object(uint64_t block_num) const {
  bytes_t mem = read_block(block_num);
  if (!verify_object_checksum((void *)mem.data(), BLOCK_SIZE)) {
    throw Error("block number " + std::to_string(block_num) + ", checksum verification failed while reading object");
  }
  size_t off = sizeof(btree_node_phys_t) - sizeof(bytes_t);
  btree_node_phys_t obj;
  memcpy((void *)&obj, mem.data(), off);
  obj.btn_data.append(mem.data() + off, BTREE_NODE_SIZE_DEFAULT - off);
  return obj;
}

// Parse extended fields (xfields)
std::vector<x_field> BlockReader::parse_xfields(bytes_t &data) const {
  char *raw = data.data();
  xf_blob_t blob = *(xf_blob_t *)raw;

  auto advance = [&raw, &data](size_t cnt) {
    raw += cnt;
    int mod = ((raw - (data.data() + sizeof(xf_blob_t))) % 8);
    if (mod)
      raw += 8 - mod;
  };

  raw += sizeof(xf_blob_t);

  std::vector<x_field> fields(blob.xf_num_exts);
  for (x_field &field : fields) {
    x_field_t raw_field = *(x_field_t *)raw;
    field.type = raw_field.x_type;
    field.flags = raw_field.x_flags;
    field.data.resize(raw_field.x_size);
    raw += sizeof(x_field_t);
  }

  advance(0);
  for (x_field &field : fields) {
    memcpy(field.data.data(), raw, field.data.size());
    advance(field.data.size());
  }

  return fields;
}

} // namespace apfs


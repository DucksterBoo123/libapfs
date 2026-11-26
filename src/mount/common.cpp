#if defined(__linux__) || defined(_WIN32) || defined(_WIN64)

#include "mount.hpp"
#include <exception>
#include <cstdio>

namespace apfs {
namespace fuse {

// A bounded streambuf that writes directly to WinFsp's caller-provided buffer.
// It removes the ostringstream -> string -> memcpy copies from every read.
class read_output_buffer : public std::streambuf {
  char *destination;
  size_t capacity;
  size_t written = 0;

public:
  read_output_buffer(char *buf, size_t len) : destination(buf), capacity(len) {}
  size_t size() const { return written; }

protected:
  std::streamsize xsputn(const char *src, std::streamsize count) override {
    if (count <= 0) return 0;
    const size_t n = std::min(size_t(count), capacity - written);
    if (n) memcpy(destination + written, src, n);
    written += n;
    return std::streamsize(n);
  }

  int_type overflow(int_type ch) override {
    if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
    if (written == capacity) return traits_type::eof();
    destination[written++] = traits_type::to_char_type(ch);
    return ch;
  }
};

static void fill_timespec(apfs_timespec *spec, uint64_t ns) {
  spec->tv_sec = ns / int(1e9);
  spec->tv_nsec = ns % int(1e9);
}

static void fill_stat(apfs_stat *stbuf, const apfs::inode_t &inode) {
  memset(stbuf, 0, sizeof(apfs_stat));
  stbuf->st_ino = inode.num;
  stbuf->st_mode = inode.mode;
  stbuf->st_nlink = inode.nlink;
  stbuf->st_uid = inode.owner;
  stbuf->st_gid = inode.group;
  stbuf->st_size = inode.size;
  fill_timespec(&stbuf->st_atim, inode.access_time);
  fill_timespec(&stbuf->st_mtim, inode.mod_time);
#if defined(_WIN32) || defined(_WIN64)
  fill_timespec(&stbuf->st_birthtim, inode.create_time);
#endif
}

int apfs_fuse_open(const char *path, struct fuse_file_info *fi) {
  fuse_ctx *ctx = (fuse_ctx *)fuse_get_context()->private_data;
  try {
    apfs::directory_entry ent = ctx->vol->navigate_to(std::string(path));
    uint64_t fd = ctx->fd++;
    ctx->open_files->emplace(fd, std::move(ent));
    fi->fh = fd;
  } catch (const apfs::Error &e) {
    return -ENOENT;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "libapfs: open failed: %s\n", e.what());
    return -EIO;
  }

  return 0;
}

void apfs_fuse_destroy(void *private_data) {
  fuse_ctx *ctx = (fuse_ctx *)private_data;
  delete ctx->vol;
  delete ctx->part;
  delete ctx->disk;
  delete ctx->open_files;
}

int apfs_fuse_getattr(const char *path, apfs_stat *stbuf, struct fuse_file_info *fi) {
  fuse_ctx *ctx = (fuse_ctx *)fuse_get_context()->private_data;
  int res;
  try {
    auto dirent = (fi && fi->fh) ? ctx->open_files->at(fi->fh) : ctx->vol->navigate_to(std::string(path));
    auto inode = dirent.load_inode();
    fill_stat(stbuf, inode);
    res = 0;
  } catch (const apfs::Error &e) {
    res = -ENOENT;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "libapfs: filesystem operation failed: %s\n", e.what());
    res = -EIO;
  }

  return res;
}

int apfs_fuse_statfs(const char *, apfs_statvfs *stbuf) {
  fuse_ctx *ctx = (fuse_ctx *)fuse_get_context()->private_data;
  memset(stbuf, 0, sizeof(apfs_statvfs));

  stbuf->f_bsize = ctx->part->block_size;
  stbuf->f_frsize = ctx->part->block_size;

  stbuf->f_blocks = ctx->part->num_blocks;
  uint64_t tot_bytes = ctx->part->num_blocks * ctx->part->block_size;
  uint64_t free_bytes = tot_bytes - ctx->part->bytes_used;
  stbuf->f_bfree = free_bytes / ctx->part->block_size;
  stbuf->f_bavail = stbuf->f_bfree;

  // Just xor the two halves to collapse 128 bits => 64 bits
  stbuf->f_fsid = (*(uint64_t *)ctx->part->unique_guid.data()) ^ ((*(uint64_t *)(ctx->part->unique_guid.data() + 8)));
#ifndef ST_RDONLY
#define ST_RDONLY 1
#endif
  stbuf->f_flag = ST_RDONLY;

  stbuf->f_namemax = APFS_MAX_FILENAME_LENGTH;

  return 0;
}

int apfs_fuse_read(const char *path, char *buf, size_t size, apfs_off_t offset, struct fuse_file_info *fi) {
  fuse_ctx *ctx = (fuse_ctx *)fuse_get_context()->private_data;
  int res;

  try {
    read_output_buffer output(buf, size);
    std::ostream os(&output);
    if (fi && fi->fh) {
      ctx->open_files->at(fi->fh).read_file(os, offset, size);
    } else {
      ctx->vol->navigate_to(std::string(path)).read_file(os, offset, size);
    }
    res = int(output.size());
  } catch (const apfs::Error &e) {
    res = -ENOENT;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "libapfs: filesystem operation failed: %s\n", e.what());
    res = -EIO;
  }

  return res;
}

int apfs_fuse_release(const char *, struct fuse_file_info *fi) {
  fuse_ctx *ctx = (fuse_ctx *)fuse_get_context()->private_data;
  ctx->open_files->erase(fi->fh);
  return 0;
}

int apfs_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler, apfs_off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
  fuse_ctx *ctx = (fuse_ctx *)fuse_get_context()->private_data;
  try {
    apfs::directory_entry dirent = (fi && fi->fh) ? ctx->open_files->at(fi->fh) : ctx->vol->navigate_to(std::string(path));
    auto chs = dirent.list_children();
    for (size_t i = offset; i < chs.size(); ++i) {
      auto &ch = chs[i];
      auto inode = ch.load_inode();
      apfs_stat st;
      fill_stat(&st, inode);
      enum fuse_fill_dir_flags fill_flags = (flags & FUSE_READDIR_PLUS) ? FUSE_FILL_DIR_PLUS : fuse_fill_dir_flags(0);
      if (filler(buf, ch.name.c_str(), &st, i + 1, fill_flags)) {
        break;
      }
    }
  } catch (const apfs::Error &e) {
    return -ENOENT;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "libapfs: directory enumeration failed: %s\n", e.what());
    return -EIO;
  }

  return 0;
}

} // namespace fuse
} // namespace apfs

#endif

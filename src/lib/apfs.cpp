#include <GuidTable.hpp>
#include <algorithm>
#include <crc32c.hpp>
#include <iomanip>
#include <libapfs/Error.hpp>
#include <libapfs/Partition.hpp>
#include <libapfs/apfs.hpp>
#include <sstream>
#include <set>

static std::array<uint8_t, 16> _uuid_to_array(unsigned char uuid[]) {
  std::array<uint8_t, 16> ret;
  memcpy(ret.data(), uuid, 16);
  return ret;
}

static uint32_t drec_key_hash(const std::string &name) {
  std::string data;
  for (int i = 0; i < name.length(); ++i) {
    char c = name[i];
    c = tolower(c);
    if (c > 0x7F) {
      throw apfs::Error("name is not ASCII");
    }
    data.append(apfs::to_bytes(uint32_t(c)));
  }
  uint32_t hash_computed = apfs::crc32c.hash((const uint8_t *)data.data(), data.length());
  hash_computed ^= 0xFFFFFFFFu;
  hash_computed &= (1 << 22) - 1;
  return hash_computed;
}

static std::string guid_to_string(const apfs::guid_t &guid) {
  uint32_t data1;
  uint16_t data2;
  uint16_t data3;
  std::memcpy(&data1, guid.data(), sizeof(data1));
  std::memcpy(&data2, guid.data() + 4, sizeof(data2));
  std::memcpy(&data3, guid.data() + 6, sizeof(data3));

  std::ostringstream oss;
  oss << std::hex << std::uppercase << std::setfill('0');
  oss << std::setw(8) << data1 << '-'
      << std::setw(4) << data2 << '-'
      << std::setw(4) << data3 << '-'
      << std::setw(2) << uint32_t(guid[8])
      << std::setw(2) << uint32_t(guid[9]) << '-';

  for (int i = 10; i < 16; ++i) {
    oss << std::setw(2) << uint32_t(guid[i]);
  }

  std::string type = oss.str();
  if (type == "7C3457EF-0000-11AA-AA11-00306543ECAC") {
    type = "APFS";
  }
  if (type == "C12A7328-F81F-11D2-BA4B-00A0C93EC93B") {
    type = "EFI";
  }
  return type;
}

namespace apfs {

disk::disk(const BlockReader &reader_) : reader(reader_) {
  GuidTable gpt(reader);
  for (EFI_PARTITION_ENTRY &part : gpt.partitions) {
    std::string partition_name;
    for (char16_t &c : part.PartitionName) {
      if (c) {
        partition_name.push_back(char(c));
      }
    }
    partition_info_t p;
    p.name = partition_name;
    p.type_guid = _uuid_to_array(part.PartitionTypeGUID);
    p.unique_guid = _uuid_to_array(part.UniquePartitionGUID);
    p.addr = part.StartingLBA * gpt.reader.BLOCK_SIZE;
    partitions.push_back(p);
  }
}

disk::disk(const std::string &filename) : disk(BlockReader(filename, false)) {}

disk::disk(FILE *fd) : disk(BlockReader(fd)) {}

partition disk::load_partition(const partition_info_t &part) const {
  return partition(part, reader);
}

partition disk::get_partition(const std::string &guid) const {
  for (auto &partition : partitions) {
    if (guid_to_string(partition.unique_guid) == guid) {
      return load_partition(partition);
    }
  }
  throw Error("no partition with guid " + guid + " found");
}

void partition::_init() {
  num_blocks = part.superblock.nx_block_count;
  block_size = part.superblock.nx_block_size;

  bytes_used = 0;
  for (size_t i = 0; i < part.volumes.size(); ++i) {
    const auto &spblk = part.volumes[i];
    volumes.push_back({spblk, reader});
    bytes_used += volumes.back().size;
  }
}

partition::partition(const std::string &filename) : reader(filename, true), part(reader) {
  _init();
}

partition::partition(const partition_info_t &_part, const BlockReader &_reader) : partition_info_t(_part), reader(_reader, true, addr), part(_reader, addr) {
  _init();
}

/// @brief Search for a volume by name
volume &partition::get_volume(const std::string &volname) {
  for (volume &vol : volumes) {
    if (vol.name == volname) {
      return vol;
    }
  }
  throw Error("volume \"" + volname + "\" not found");
}

volume::volume(const apfs_superblock_t &spblk, const BlockReader &reader)
    : spblk(spblk), reader(reader),
      object_map(reader.read_btree<omap_key_t>(
          reader.read_object<omap_phys_t>(spblk.apfs_omap_oid).om_tree_oid)),
      filesystem(reader.read_btree<j_key_t>(get_paddr(spblk.apfs_root_tree_oid), compare_j_key_t)) {
  name = (char *)spblk.apfs_volname;
  size = spblk.apfs_fs_alloc_count * reader.BLOCK_SIZE;
}

paddr_t volume::identity(const oid_t &oid) { return oid; }
// Virtual object => physical address by reading the object_map BTree
paddr_t volume::get_paddr(const oid_t &oid) const {
  omap_key_t key = {oid, spblk.apfs_o.o_xid};
  auto kv = object_map.upper_bound(to_bytes(key), identity);
  kv = object_map.prev(kv.key, identity);
  if (cast<omap_key_t>(kv.key).ok_oid != oid) {
    throw Error("could not valid match for (" + std::to_string(oid) + ", " + std::to_string(key.ok_xid) + ") in the volume object map");
  }
  return cast<omap_val_t>(kv.val).ov_paddr;
}

directory_entry volume::root() const {
  j_drec_val_t drec_val;
  drec_val.file_id = ROOT_DIR_INO_NUM;
  drec_val.flags = DT_DIR;

  return {this, std::string("/"), drec_val, reader};
}

directory_entry volume::navigate_to(const std::string &path, bool follow_last) const {
  return root().resolve(path, follow_last);
}

directory_entry::directory_entry(const volume *vol, std::string name, const j_drec_val_t raw_drec, const BlockReader &reader)
    : vol(vol), name(name), inode_num(raw_drec.file_id), type(directory_entry_type(raw_drec.flags & DREC_TYPE_MASK)), reader(reader) {}

std::vector<directory_entry> directory_entry::list_children() const {
  if (type != DIRENT_DIR) {
    throw Error("\"" + name + "\" is not a directory");
  }

  j_drec_hashed_key_t key;
  key.obj_id_and_type = (uint64_t(APFS_TYPE_DIR_REC) << OBJ_TYPE_SHIFT) | inode_num;
  key.name_len_and_hash = 0;
  key.name = "";

  std::vector<directory_entry> children;

  auto kv = vol->filesystem.lower_bound(to_bytes(key), [&](const oid_t &oid) { return vol->get_paddr(oid); });
  while (kv != vol->filesystem.SENTINEL && ((cast<j_key_t>(kv.key).obj_id_and_type & OBJ_TYPE_MASK) >> OBJ_TYPE_SHIFT) == APFS_TYPE_DIR_REC) {
    std::string name = cast<j_drec_hashed_key_t>(kv.key).name;
    children.emplace_back(vol, name, cast<j_drec_val_t>(kv.val), reader);

    kv = vol->filesystem.upper_bound(kv.key, [&](const oid_t &oid) { return vol->get_paddr(oid); });
  }

  return children;
}

inode_t::inode_t(uint64_t inode_num, const _j_inode_val_t &raw, std::vector<x_field> &xfields) : num(inode_num), _j_inode_val_t(raw), xfields(xfields) {
  for (x_field &field : xfields) {
    if (field.type == INO_EXT_TYPE_DSTREAM) {
      size = cast<j_dstream_t>(field.data).size;
      break;
    }
  }
}

directory_entry directory_entry::resolve(const std::string &path, bool follow_last) const {
  // If path is relative to the root and we're not the root, get path relative to root
  if (!path.empty() && path[0] == '/' && inode_num != ROOT_DIR_INO_NUM) {
    return vol->navigate_to(path);
  }

  std::vector<std::string> dirs;
  std::string dir;
  for (const char &c : path) {
    if (c == '/') {
      if (!dir.empty()) {
        if (dir == "..") {
          if (!dirs.empty() && dirs.back() != "..") {
            dirs.pop_back();
          } else {
            dirs.push_back("..");
          }
        } else if (dir != ".") {
          dirs.push_back(dir);
        }
      }
      dir.clear();
      continue;
    }
    dir.push_back(c);
  }
  if (!dir.empty()) {
    dirs.push_back(dir);
  }

  // This works based on the assumption that:
  // the object id of a `drec_hashed_key_t` is equal to the inode's object id of the parent directory
  j_drec_val_t drec_val;
  drec_val.file_id = inode_num;
  drec_val.flags = type;

  for (size_t i = 0; i < dirs.size(); i++) {
    if (dirs[i] == "..") { // parent dir
      if (drec_val.file_id == ROOT_DIR_INO_NUM) { // skip ../ from root
        continue;
      }
      j_inode_key_t key;
      key.obj_id_and_type = (uint64_t(APFS_TYPE_INODE) << OBJ_TYPE_SHIFT) | drec_val.file_id;
      auto kv = vol->filesystem.lower_bound(to_bytes(key), [&](const oid_t &oid) { return vol->get_paddr(oid); });
      if (kv.key != to_bytes(key)) {
        throw Error("directory does not exist");
      }
      j_inode_val_t val = cast<j_inode_val_t>(kv.val);
      drec_val.file_id = val.parent_id;
      drec_val.flags = DT_DIR; // has to be a directory
      continue;
    }

    j_drec_hashed_key_t key;
    key.obj_id_and_type = (uint64_t(APFS_TYPE_DIR_REC) << OBJ_TYPE_SHIFT) | (drec_val.file_id);
    key.name = dirs[i];
    key.name_len_and_hash = (drec_key_hash(key.name) << J_DREC_HASH_SHIFT) | (key.name.length() + 1);
    auto kv = vol->filesystem.lower_bound(to_bytes(key), [&](const oid_t &oid) { return vol->get_paddr(oid); });
    if (kv.key != to_bytes(key)) {
      throw Error("file/directory does not exist");
    }
    drec_val = cast<j_drec_val_t>(kv.val);

    if (follow_last || i != dirs.size() - 1) {
      // Resolve symlink so our navigation actually works
      if (drec_val.flags & DT_LNK) {
        directory_entry entry(vol, key.name, drec_val, reader);
        entry = entry.follow_symlink();
        
        drec_val.file_id = entry.inode_num;
        drec_val.flags = entry.type;
      }
    }

    if (i != dirs.size() - 1) {
      if (!(drec_val.flags & DT_DIR)) {
        throw Error("not a directory");
      }
    }
  }

  return {vol, (dirs.empty() ? name : dirs.back()), drec_val, reader};
}

inode_t directory_entry::load_inode() const {
  j_inode_key_t inode_key;
  inode_key.obj_id_and_type = (uint64_t(APFS_TYPE_INODE) << OBJ_TYPE_SHIFT) | inode_num;
  const bytes_t expected_key = to_bytes(inode_key);
  auto inode_kv = vol->filesystem.lower_bound(expected_key, [&](const oid_t &oid) { return vol->get_paddr(oid); });
  if (inode_kv == vol->filesystem.SENTINEL || inode_kv.key != expected_key) {
    throw Error("inode not found: " + std::to_string(inode_num));
  }
  j_inode_val_t inode_val = cast<j_inode_val_t>(inode_kv.val);

  std::vector<x_field> xfields = reader.parse_xfields(inode_val.xfields);
  return {inode_num, inode_val, xfields};
}

void directory_entry::read_file(std::ostream &os, int64_t offset, apfs_ssize_t size) const {
  if (type != DIRENT_FILE && type != DIRENT_LINK) {
    throw Error("\"" + name + "\" is not a file");
  }
  if (size < -1) {
    throw Error("invalid size");
  }

  const inode_t inode_val = load_inode();
  if (offset < 0 || uint64_t(offset) > uint64_t(inode_val.size)) {
    throw Error("invalid offset");
  }
  if (uint64_t(offset) == uint64_t(inode_val.size)) return;

  const uint64_t private_id = inode_val.private_id;
  const uint64_t bytes_available = uint64_t(inode_val.size) - uint64_t(offset);
  uint64_t bytes_remaining = size == -1 ? bytes_available
                                        : std::min(bytes_available, uint64_t(size));
  uint64_t current_offset = uint64_t(offset);
  constexpr uint64_t max_read_bytes = 4 * 1024 * 1024;
  const uint64_t block_size = reader.BLOCK_SIZE;
  if (block_size == 0) throw Error("invalid APFS block size");

  while (bytes_remaining > 0) {
    uint64_t logical_start = 0, extent_length = 0, physical_block = 0;
    bool hit = false;
    {
      std::lock_guard<std::mutex> guard(extent_cache->mutex);
      if (extent_cache->valid && extent_cache->private_id == private_id &&
          current_offset >= extent_cache->logical_start &&
          current_offset - extent_cache->logical_start < extent_cache->length) {
        logical_start = extent_cache->logical_start;
        extent_length = extent_cache->length;
        physical_block = extent_cache->physical_block;
        hit = true;
      }
    }

    if (!hit) {
      j_file_extent_key_t key{};
      key.logical_addr = current_offset;
      key.obj_id_and_type = (uint64_t(APFS_TYPE_FILE_EXTENT) << OBJ_TYPE_SHIFT) | private_id;
      auto kv = vol->filesystem.upper_bound(to_bytes(key), [&](const oid_t &oid) { return vol->get_paddr(oid); });
      kv = vol->filesystem.prev(kv.key, [&](const oid_t &oid) { return vol->get_paddr(oid); });
      if (kv == vol->filesystem.SENTINEL) throw Error("file extent not found");

      const j_file_extent_key_t extent_key = cast<j_file_extent_key_t>(kv.key);
      if (extent_key.obj_id_and_type != key.obj_id_and_type) {
        throw Error("file extent belongs to another inode");
      }
      const j_file_extent_val_t extent_val = cast<j_file_extent_val_t>(kv.val);
      logical_start = extent_key.logical_addr;
      extent_length = extent_val.len_and_flags & J_FILE_EXTENT_LEN_MASK;
      physical_block = extent_val.phys_block_num;
      // Subtractions instead of start + length avoid 64-bit overflow.
      if (extent_length == 0 || current_offset < logical_start ||
          current_offset - logical_start >= extent_length) {
        throw Error("invalid or missing file extent");
      }
      {
        std::lock_guard<std::mutex> guard(extent_cache->mutex);
        extent_cache->private_id = private_id;
        extent_cache->logical_start = logical_start;
        extent_cache->length = extent_length;
        extent_cache->physical_block = physical_block;
        extent_cache->valid = true;
      }
    }

    const uint64_t offset_in_extent = current_offset - logical_start;
    const uint64_t extent_bytes_remaining = extent_length - offset_in_extent;
    uint64_t extent_output_remaining = std::min(bytes_remaining, extent_bytes_remaining);
    uint64_t device_block = physical_block + offset_in_extent / block_size;
    uint64_t start_in_block = offset_in_extent % block_size;

    while (extent_output_remaining > 0) {
      const uint64_t max_payload = max_read_bytes - start_in_block;
      const uint64_t payload = std::min(extent_output_remaining, max_payload);
      const uint64_t span = start_in_block + payload;
      const size_t block_count = size_t(span / block_size + (span % block_size != 0));
      if (block_count == 0) throw Error("empty block read");
      bytes_t raw = reader.read_blocks(device_block, block_count);
      if (raw.size() < span) throw Error("short APFS block read");
      os.write(raw.data() + size_t(start_in_block), std::streamsize(payload));
      if (!os) throw Error("failed to write file read data to output buffer");
      device_block += block_count;
      start_in_block = 0;
      current_offset += payload;
      extent_output_remaining -= payload;
      bytes_remaining -= payload;
    }
  }
}

std::string directory_entry::read_symlink() const {
  if (type != DIRENT_LINK) {
    throw Error("not a symbolic link");
  }

  j_xattr_key_t key;
  key.obj_id_and_type = inode_num | (uint64_t(APFS_TYPE_XATTR) << OBJ_TYPE_SHIFT);
  key.name_len = sizeof(SYMLINK_EA_NAME); // strlen(SYMLINK_EA_NAME) + 1 (for null terminator)
  key.name = SYMLINK_EA_NAME;

  auto kv = vol->filesystem.lower_bound(to_bytes(key), [&](const oid_t &oid) { return vol->get_paddr(oid); });
  assert(kv.key == to_bytes(key));

  std::string path = cast<j_xattr_val_t>(kv.val).xdata;
  path.pop_back(); // remove null byte

  return path;
}

directory_entry directory_entry::follow_symlink(int level) const {
  std::set<uint64_t> seen;
  auto resolved = *this;
  while (resolved.type == DIRENT_LINK && (level == -1 || --level >= 0)) {
    if (seen.contains(resolved.inode_num) && level == -1) {
      throw Error("symbolic links create a cycle");
    }
    seen.insert(resolved.inode_num);
    resolved = resolved.resolve("../" + resolved.read_symlink());
  }
  return resolved;
}

} // namespace apfs

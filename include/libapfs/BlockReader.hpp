#pragma once
#include "Error.hpp"
#include "checksum.hpp"
#include <cstdio>
#include <memory>
#include <string>
#include <system_error>
#include "types/types.hpp"

namespace apfs {

template <typename KeyType, typename Compare>
class BTree;

class BlockReader {
  std::shared_ptr<FILE *> f;
  uint64_t offset;
#ifdef _WIN32
  // Separate overlapped disk handle; inherited by readers of APFS partitions.
  std::shared_ptr<void> native_handle;
#endif
  void set_apfs_block_size();

public:
  size_t BLOCK_SIZE = NX_DEFAULT_BLOCK_SIZE;
  std::string filename;
  bytes_t read_block(uint64_t block_num) const;
  bytes_t read_blocks(uint64_t block_num, uint64_t num_blocks) const;
  template <typename T>
  T read_object(uint64_t block_num) const {
    bytes_t mem = read_block(block_num);
    if (!verify_object_checksum((void *)mem.data(), BLOCK_SIZE)) {
      throw Error("block number " + std::to_string(block_num) + ", checksum verification failed while reading object");
    }
    return *(T *)mem.data();
  }
  template <typename T>
  T read_struct(uint64_t block_num) const {
    return *(T *)read_block(block_num).data();
  };
  template <typename KeyType, typename Compare = std::less<KeyType>>
  BTree<KeyType, Compare> read_btree(uint64_t block_num) const;
  template <typename KeyType, typename Compare>
  BTree<KeyType, Compare> read_btree(uint64_t block_num, Compare lt) const;
  std::vector<x_field> parse_xfields(bytes_t &data) const;
  BlockReader(std::string filename, bool apfs = true, uint64_t offset = 0);
  BlockReader(const BlockReader &reader, bool apfs, uint64_t offset = 0);
  BlockReader(FILE *fd);
  ~BlockReader();
};
template <>
checkpoint_map_phys_t BlockReader::read_object(uint64_t block_num) const;
template <>
btree_node_phys_t BlockReader::read_object(uint64_t block_num) const;

} // namespace apfs

//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// extendible_hash_table.cpp
//
// Identification: src/container/hash/extendible_hash_table.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "common/logger.h"
#include "common/rid.h"
#include "container/hash/extendible_hash_table.h"

namespace bustub {

template <typename KeyType, typename ValueType, typename KeyComparator>
HASH_TABLE_TYPE::ExtendibleHashTable(const std::string &name, BufferPoolManager *buffer_pool_manager,
                                     const KeyComparator &comparator, HashFunction<KeyType> hash_fn)
    : buffer_pool_manager_(buffer_pool_manager), comparator_(comparator), hash_fn_(std::move(hash_fn)) {
  //  implement me!
  // fetch dir page
  Page *dir_pg_outside = buffer_pool_manager->NewPage(&directory_page_id_);
  HashTableDirectoryPage *dir_pg = reinterpret_cast<HashTableDirectoryPage *>(dir_pg_outside->GetData());

  // fetch bucket page, initially, global depth = local depth = 0
  page_id_t bucket_page_id;
  buffer_pool_manager->NewPage(&bucket_page_id);
  dir_pg->SetBucketPageId(0, bucket_page_id);
  dir_pg->SetLocalDepth(0, 0);
  dir_pg->SetPageId(directory_page_id_);

  // tag the pages evictable
  buffer_pool_manager->UnpinPage(directory_page_id_, false);
  buffer_pool_manager->UnpinPage(bucket_page_id, false);
}

/*****************************************************************************
 * HELPERS
 *****************************************************************************/
/**
 * Hash - simple helper to downcast MurmurHash's 64-bit hash to 32-bit
 * for extendible hashing.
 *
 * @param key the key to hash
 * @return the downcasted 32-bit hash
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
auto HASH_TABLE_TYPE::Hash(KeyType key) -> uint32_t {
  return static_cast<uint32_t>(hash_fn_.GetHash(key));
}

template <typename KeyType, typename ValueType, typename KeyComparator>
inline auto HASH_TABLE_TYPE::KeyToDirectoryIndex(KeyType key, HashTableDirectoryPage *dir_page) -> uint32_t {
  return Hash(key) & dir_page->GetGlobalDepthMask();
}

template <typename KeyType, typename ValueType, typename KeyComparator>
inline auto HASH_TABLE_TYPE::KeyToPageId(KeyType key, HashTableDirectoryPage *dir_page) -> uint32_t {
  return dir_page->GetBucketPageId(KeyToDirectoryIndex(key, dir_page));
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto HASH_TABLE_TYPE::FetchDirectoryPage() -> HashTableDirectoryPage * {
  Page *p = buffer_pool_manager_->FetchPage(directory_page_id_);
  return reinterpret_cast<HashTableDirectoryPage *>(p->GetData());
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto HASH_TABLE_TYPE::FetchBucketPage(page_id_t bucket_page_id) -> HASH_TABLE_BUCKET_TYPE * {
  Page *p = buffer_pool_manager_->FetchPage(bucket_page_id);
  return reinterpret_cast<HASH_TABLE_BUCKET_TYPE *>(p->GetData());
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
auto HASH_TABLE_TYPE::GetValue(Transaction *transaction, const KeyType &key, std::vector<ValueType> *result) -> bool {
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  page_id_t page_id = KeyToPageId(key, dir_page);
  HASH_TABLE_BUCKET_TYPE *bucket = FetchBucketPage(page_id);
  bool found = bucket->GetValue(key, comparator_, result);
  buffer_pool_manager_->UnpinPage(page_id, false);
  buffer_pool_manager_->UnpinPage(directory_page_id_, false);
  return found;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
auto HASH_TABLE_TYPE::Insert(Transaction *transaction, const KeyType &key, const ValueType &value) -> bool {
  // if not full, insert directly
  // if full splitInsert the key
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  page_id_t bucket_page_id = KeyToPageId(key, dir_page);
  HASH_TABLE_BUCKET_TYPE *bucket = FetchBucketPage(bucket_page_id);
  if (bucket->IsFull()) {
    // splitInsert
    buffer_pool_manager_->UnpinPage(directory_page_id_, false);
    buffer_pool_manager_->UnpinPage(bucket_page_id, false);
    return SplitInsert(transaction, key, value);
  }
  // direct Insert
  bool success = bucket->Insert(key, value, comparator_);
  buffer_pool_manager_->UnpinPage(bucket_page_id, success);
  buffer_pool_manager_->UnpinPage(directory_page_id_, false);
  return success;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto HASH_TABLE_TYPE::SplitInsert(Transaction *transaction, const KeyType &key, const ValueType &value) -> bool {
  bool global_changed = false;

  // when entering this while loop the first time, the page is definately full.
  // but when we are splitting until we have found a page will spare slot
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  page_id_t bkt1_page_id = KeyToPageId(key, dir_page);
  uint32_t bkt1_idx = KeyToDirectoryIndex(key, dir_page);
  HASH_TABLE_BUCKET_TYPE *bkt1 = FetchBucketPage(bkt1_page_id);

  // first check if the global depth should be updated
  uint32_t old_gd = dir_page->GetGlobalDepth();
  uint32_t now_gd = old_gd;
  if (dir_page->GetLocalDepth(bkt1_idx) == old_gd) {
    global_changed = true;
    ++now_gd;
    dir_page->IncrGlobalDepth();
  }
  dir_page->IncrLocalDepth(bkt1_idx);

  // get the split image (which is empty right now)
  uint32_t bkt2_idx = dir_page->GetSplitImageIndex(bkt1_idx);
  page_id_t bkt2_page_id;
  Page *pp = buffer_pool_manager_->NewPage(&bkt2_page_id);
  HASH_TABLE_BUCKET_TYPE *bkt2 = reinterpret_cast<HASH_TABLE_BUCKET_TYPE *>(pp->GetData());

  // setup the split image page
  dir_page->SetBucketPageId(bkt2_idx, bkt2_page_id);
  dir_page->SetLocalDepth(bkt2_idx, dir_page->GetLocalDepth(bkt1_idx));

  // redirect all new buckets
  if (global_changed) {
    uint32_t old_sz = 1 << old_gd;
    uint32_t new_sz = 1 << now_gd;
    for (uint32_t i = old_sz; i < new_sz; ++i) {
      if (i == bkt2_idx) {
        continue;
      }
      uint32_t bucket_idx = i - ((1 << now_gd) >> 1);
      dir_page->SetBucketPageId(i, dir_page->GetBucketPageId(bucket_idx));
      dir_page->SetLocalDepth(i, dir_page->GetLocalDepth(bucket_idx));
    }
  }

  // for all bucket pointing at bkt1, but should point bkt2 now, we change them
  // the old bucket shares the prefix of (localdepth - 1) of bkt2
  uint32_t ld = dir_page->GetLocalDepth(bkt2_idx);
  uint32_t shared_bits = bkt2_idx & ((1 << (ld - 1)) - 1);
  for (size_t i = shared_bits; i < dir_page->Size(); i += (1 << ld)) {
    if (i == bkt1_idx || i == bkt2_idx) {
      continue;
    }
    uint32_t judge_bit = 1 << (ld - 1);
    if ((i & judge_bit) != 0) {
      dir_page->SetBucketPageId(i, bkt2_page_id);
      dir_page->SetLocalDepth(i, dir_page->GetLocalDepth(bkt2_idx));
    }
  }

  // rehash the elements from the bucket
  for (size_t i = 0; i < BUCKET_ARRAY_SIZE; ++i) {
    if (!bkt1->IsReadable(i)) {
      if (!bkt1->IsOccupied(i)) {
        break;  // an empty slot
      }
      continue;  // an tombstone slot
    }
    // a value slot below, determine whether it should be redistributed
    KeyType k = bkt1->KeyAt(i);
    ValueType v = bkt1->ValueAt(i);
    uint32_t which_idx = Hash(k) & ((1 << ld) - 1);
    if (which_idx != bkt1_idx) {
      bkt1->RemoveAt(i);
      bkt2->Insert(k, v, comparator_);
    }
  }

  buffer_pool_manager_->UnpinPage(bkt2_page_id, true);
  buffer_pool_manager_->UnpinPage(bkt1_page_id, true);
  buffer_pool_manager_->UnpinPage(directory_page_id_, global_changed);

  return Insert(transaction, key, value);
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
auto HASH_TABLE_TYPE::Remove(Transaction *transaction, const KeyType &key, const ValueType &value) -> bool {
  // if after removal the bucket is empty, merge
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  page_id_t bucket_page_id = KeyToPageId(key, dir_page);
  HASH_TABLE_BUCKET_TYPE *bucket = FetchBucketPage(bucket_page_id);
  bool success = bucket->Remove(key, value, comparator_);
  if (success && bucket->IsEmpty()) {
    buffer_pool_manager_->UnpinPage(directory_page_id_, false);
    buffer_pool_manager_->UnpinPage(bucket_page_id, false);
    Merge(transaction, key, value);
    return true;
  }
  buffer_pool_manager_->UnpinPage(directory_page_id_, false);
  buffer_pool_manager_->UnpinPage(bucket_page_id, success);
  return success;
}

/*****************************************************************************
 * MERGE
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
void HASH_TABLE_TYPE::Merge(Transaction *transaction, const KeyType &key, const ValueType &value) {
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  uint32_t bucket_idx = KeyToDirectoryIndex(key, dir_page);
  bool shrinked = RealMerge(transaction, dir_page, bucket_idx);
  buffer_pool_manager_->UnpinPage(directory_page_id_, shrinked);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool HASH_TABLE_TYPE::RealMerge(Transaction *transaction, HashTableDirectoryPage *dir_page, uint32_t bucket_idx) {
  page_id_t bucket_page_id = dir_page->GetBucketPageId(bucket_idx);
  HASH_TABLE_BUCKET_TYPE *bucket = FetchBucketPage(bucket_page_id);

  uint32_t bucket_ld = dir_page->GetLocalDepth(bucket_idx);
  if (!bucket->IsEmpty() || !(bucket_ld > 0)) {
    buffer_pool_manager_->UnpinPage(bucket_page_id, false);
    return false;
  }

  uint32_t split_idx = dir_page->GetSplitImageIndex(bucket_idx);
  page_id_t split_page_id = dir_page->GetBucketPageId(split_idx);

  if (split_page_id == bucket_page_id) {
    buffer_pool_manager_->UnpinPage(bucket_page_id, false);
    return false;
  }

  // merge if all of the followings
  // 1. bucket is empty
  // 2. bucket and split image has the same local depth
  // 3. bucket's local depth > 0
  uint32_t split_ld = dir_page->GetLocalDepth(split_idx);
  if (bucket_ld != split_ld) {
    buffer_pool_manager_->UnpinPage(bucket_page_id, false);
    return false;
  }

  // merge by wiring all bucket which points at *bucket* to *split*
  dir_page->DecrLocalDepth(split_idx);
  split_ld--;
  for (size_t i = 0; i < dir_page->Size(); ++i) {
    if (dir_page->GetBucketPageId(i) == bucket_page_id) {
      dir_page->SetBucketPageId(i, split_page_id);
      dir_page->SetLocalDepth(i, split_ld);
    }
  }

  // remove the empty bucket
  buffer_pool_manager_->UnpinPage(bucket_page_id, false);
  buffer_pool_manager_->DeletePage(bucket_page_id);

  bool shrinked = false;
  if (dir_page->CanShrink()) {
    dir_page->DecrGlobalDepth();
    shrinked = true;
  }

  for (size_t i = 0; i < dir_page->Size(); ++i) {
    HASH_TABLE_BUCKET_TYPE *bucket = FetchBucketPage(dir_page->GetBucketPageId(i));
    page_id_t id = dir_page->GetBucketPageId(i);
    if (bucket->IsEmpty()) {
      buffer_pool_manager_->UnpinPage(id, false);
      bool shrinked_again = RealMerge(transaction, dir_page, i);
      return shrinked_again || shrinked;
    }
    buffer_pool_manager_->UnpinPage(id, false);
  }

  return shrinked;
}

/*****************************************************************************
 * GETGLOBALDEPTH - DO NOT TOUCH
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
auto HASH_TABLE_TYPE::GetGlobalDepth() -> uint32_t {
  table_latch_.RLock();
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  uint32_t global_depth = dir_page->GetGlobalDepth();
  assert(buffer_pool_manager_->UnpinPage(directory_page_id_, false, nullptr));
  table_latch_.RUnlock();
  return global_depth;
}

/*****************************************************************************
 * VERIFY INTEGRITY - DO NOT TOUCH
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
void HASH_TABLE_TYPE::VerifyIntegrity() {
  table_latch_.RLock();
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  dir_page->VerifyIntegrity();
  assert(buffer_pool_manager_->UnpinPage(directory_page_id_, false, nullptr));
  table_latch_.RUnlock();
}

/*****************************************************************************
 * TEMPLATE DEFINITIONS - DO NOT TOUCH
 *****************************************************************************/
template class ExtendibleHashTable<int, int, IntComparator>;

template class ExtendibleHashTable<GenericKey<4>, RID, GenericComparator<4>>;
template class ExtendibleHashTable<GenericKey<8>, RID, GenericComparator<8>>;
template class ExtendibleHashTable<GenericKey<16>, RID, GenericComparator<16>>;
template class ExtendibleHashTable<GenericKey<32>, RID, GenericComparator<32>>;
template class ExtendibleHashTable<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub

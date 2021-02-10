/*
 * Copyright 2018 OmniSci, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "QueryEngine/JoinHashTable/OverlapsJoinHashTable.h"

#include "QueryEngine/CodeGenerator.h"
#include "QueryEngine/Execute.h"
#include "QueryEngine/ExpressionRewrite.h"
#include "QueryEngine/JoinHashTable/Builders/BaselineHashTableBuilder.h"
#include "QueryEngine/JoinHashTable/HashJoin.h"
#include "QueryEngine/JoinHashTable/PerfectJoinHashTable.h"
#include "QueryEngine/JoinHashTable/Runtime/HashJoinKeyHandlers.h"
#include "QueryEngine/JoinHashTable/Runtime/JoinHashTableGpuUtils.h"

std::unique_ptr<OverlapsHashTableCache<OverlapsHashTableCacheKey,
                                       OverlapsJoinHashTable::HashTableCacheValue>>
    OverlapsJoinHashTable::hash_table_cache_ = std::make_unique<
        OverlapsHashTableCache<OverlapsHashTableCacheKey,
                               OverlapsJoinHashTable::HashTableCacheValue>>();

std::unique_ptr<HashTableCache<HashTableCacheKey,
                               std::pair<OverlapsJoinHashTable::BucketThreshold,
                                         OverlapsJoinHashTable::BucketSizes>>>
    OverlapsJoinHashTable::auto_tuner_cache_ =
        std::make_unique<HashTableCache<HashTableCacheKey,
                                        std::pair<OverlapsJoinHashTable::BucketThreshold,
                                                  OverlapsJoinHashTable::BucketSizes>>>();

//! Make hash table from an in-flight SQL query's parse tree etc.
std::shared_ptr<OverlapsJoinHashTable> OverlapsJoinHashTable::getInstance(
    const std::shared_ptr<Analyzer::BinOper> condition,
    const std::vector<InputTableInfo>& query_infos,
    const Data_Namespace::MemoryLevel memory_level,
    const int device_count,
    ColumnCacheMap& column_cache,
    Executor* executor,
    const QueryHint& query_hint) {
  decltype(std::chrono::steady_clock::now()) ts1, ts2;
  auto inner_outer_pairs = normalize_column_pairs(
      condition.get(), *executor->getCatalog(), executor->getTemporaryTables());

  const auto getHashTableType =
      [](const std::shared_ptr<Analyzer::BinOper> condition,
         const std::vector<InnerOuter>& inner_outer_pairs) -> HashType {
    HashType layout = HashType::OneToMany;
    if (condition->is_overlaps_oper()) {
      CHECK_EQ(inner_outer_pairs.size(), size_t(1));
      if (inner_outer_pairs[0].first->get_type_info().is_array() &&
          inner_outer_pairs[0].second->get_type_info().is_array()) {
        layout = HashType::ManyToMany;
      }
    }
    return layout;
  };

  auto layout = getHashTableType(condition, inner_outer_pairs);

  if (VLOGGING(1)) {
    VLOG(1) << "Building geo hash table " << getHashTypeString(layout)
            << " for qual: " << condition->toString();
    ts1 = std::chrono::steady_clock::now();
  }

  const auto qi_0 = query_infos[0].info.getNumTuplesUpperBound();
  const auto qi_1 = query_infos[1].info.getNumTuplesUpperBound();

  VLOG(1) << "table_id = " << query_infos[0].table_id << " has " << qi_0 << " tuples.";
  VLOG(1) << "table_id = " << query_infos[1].table_id << " has " << qi_1 << " tuples.";

  const auto& query_info =
      get_inner_query_info(HashJoin::getInnerTableId(inner_outer_pairs), query_infos)
          .info;
  const auto total_entries = 2 * query_info.getNumTuplesUpperBound();
  if (total_entries > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    throw TooManyHashEntries();
  }

  auto join_hash_table = std::make_shared<OverlapsJoinHashTable>(condition,
                                                                 query_infos,
                                                                 memory_level,
                                                                 column_cache,
                                                                 executor,
                                                                 inner_outer_pairs,
                                                                 device_count);
  if (query_hint.hint_delivered) {
    join_hash_table->registerQueryHint(query_hint);
  }
  try {
    join_hash_table->reify(layout);
  } catch (const HashJoinFail& e) {
    throw HashJoinFail(std::string("Could not build a 1-to-1 correspondence for columns "
                                   "involved in overlaps join | ") +
                       e.what());
  } catch (const ColumnarConversionNotSupported& e) {
    throw HashJoinFail(std::string("Could not build hash tables for overlaps join | "
                                   "Inner table too big. Attempt manual table reordering "
                                   "or create a single fragment inner table. | ") +
                       e.what());
  } catch (const std::exception& e) {
    throw HashJoinFail(std::string("Failed to build hash tables for overlaps join | ") +
                       e.what());
  }
  if (VLOGGING(1)) {
    ts2 = std::chrono::steady_clock::now();
    VLOG(1) << "Built geo hash table " << getHashTypeString(layout) << " in "
            << std::chrono::duration_cast<std::chrono::milliseconds>(ts2 - ts1).count()
            << " ms";
  }
  return join_hash_table;
}

namespace {

void compute_bucket_sizes(std::vector<double>& bucket_sizes_for_dimension,
                          const double bucket_threshold,
                          const Data_Namespace::MemoryLevel effective_memory_level,
                          const JoinColumn& join_column,
                          const JoinColumnTypeInfo& join_column_type,
                          const std::vector<InnerOuter>& inner_outer_pairs,
                          const Executor* executor) {
  // No coalesced keys for overlaps joins yet
  CHECK_EQ(inner_outer_pairs.size(), 1u);

  const auto col = inner_outer_pairs[0].first;
  CHECK(col);
  const auto col_ti = col->get_type_info();
  CHECK(col_ti.is_array());

  // TODO: Compute the number of dimensions for this overlaps key
  const int num_dims = 2;
  std::vector<double> local_bucket_sizes(num_dims, std::numeric_limits<double>::max());

  VLOG(1)
      << "Computing x and y bucket sizes for overlaps hash join with minimum bucket size "
      << std::to_string(bucket_threshold);

  if (effective_memory_level == Data_Namespace::MemoryLevel::CPU_LEVEL) {
    const int thread_count = cpu_threads();
    compute_bucket_sizes_on_cpu(local_bucket_sizes,
                                join_column,
                                join_column_type,
                                bucket_threshold,
                                thread_count);
  }
#ifdef HAVE_CUDA
  else {
    // Note that we compute the bucket sizes using only a single GPU
    const int device_id = 0;
    auto& data_mgr = executor->getCatalog()->getDataMgr();
    CudaAllocator allocator(&data_mgr, device_id);
    auto device_bucket_sizes_gpu =
        transfer_vector_of_flat_objects_to_gpu(local_bucket_sizes, allocator);
    auto join_column_gpu = transfer_flat_object_to_gpu(join_column, allocator);
    auto join_column_type_gpu = transfer_flat_object_to_gpu(join_column_type, allocator);

    compute_bucket_sizes_on_device(
        device_bucket_sizes_gpu, join_column_gpu, join_column_type_gpu, bucket_threshold);
    allocator.copyFromDevice(reinterpret_cast<int8_t*>(local_bucket_sizes.data()),
                             reinterpret_cast<int8_t*>(device_bucket_sizes_gpu),
                             local_bucket_sizes.size() * sizeof(double));
  }
#endif

  size_t ctr = 0;
  for (auto& bucket_sz : local_bucket_sizes) {
    VLOG(1) << "Computed bucket size for dim[" << ctr++ << "]: " << bucket_sz;
    bucket_sizes_for_dimension.push_back(1.0 / bucket_sz);
  }

  return;
}

struct BucketSizeTuner {
  BucketSizeTuner(const double bucket_threshold,
                  const double step,
                  const double min_threshold)
      : bucket_threshold(bucket_threshold * step)
      , step(step)
      , min_threshold(min_threshold) {}

  bool operator()(const std::vector<double> last_iteration_bucket_sizes) {
    bucket_threshold /= step;
    if (!last_iteration_bucket_sizes.empty() &&
        last_iteration_bucket_sizes == previous_bucket_sizes) {
      // abort the tuning if the bucket size does not change. note that this will always
      // run at least two steps, as the first step will have no computed bucket sizes
      VLOG(1) << "Aborting overlaps tuning as bucket size is no longer changing.";
      return false;
    }
    num_steps++;

    previous_bucket_sizes = last_iteration_bucket_sizes;

    return bucket_threshold >= min_threshold;
  }

  std::vector<double> computeBucketSizes(
      const Data_Namespace::MemoryLevel effective_memory_level,
      std::vector<ColumnsForDevice>& columns_per_device,
      const std::vector<InnerOuter>& inner_outer_pairs,
      const size_t device_count,
      const Executor* executor) {
    // compute bucket info for the current threshold value
    CHECK(!columns_per_device.empty());
    std::vector<double> bucket_sizes_for_dimension;
    compute_bucket_sizes(bucket_sizes_for_dimension,
                         bucket_threshold,
                         effective_memory_level,
                         columns_per_device.front().join_columns[0],
                         columns_per_device.front().join_column_types[0],
                         inner_outer_pairs,
                         executor);
    return bucket_sizes_for_dimension;
  }

  double bucket_threshold;
  size_t num_steps{0};
  const double step;
  const double min_threshold;

  std::vector<double> previous_bucket_sizes;
};

std::ostream& operator<<(std::ostream& os, const BucketSizeTuner& tuner) {
  os << "Step Num: " << tuner.num_steps << ", Threshold: " << std::fixed
     << tuner.bucket_threshold << ", Step Size: " << std::fixed << tuner.step
     << ", Min: " << std::fixed << tuner.min_threshold;
  return os;
}

}  // namespace

void OverlapsJoinHashTable::reifyWithLayout(const HashType layout) {
  auto timer = DEBUG_TIMER(__func__);
  CHECK(layoutRequiresAdditionalBuffers(layout));
  const auto& query_info =
      get_inner_query_info(HashJoin::getInnerTableId(inner_outer_pairs_), query_infos_)
          .info;
  VLOG(1) << "Reify with layout " << getHashTypeString(layout)
          << "for table_id: " << HashJoin::getInnerTableId(inner_outer_pairs_);
  if (query_info.fragments.empty()) {
    return;
  }

  const double default_overlaps_bucket_threshold = 0.1;
  auto overlaps_max_table_size_bytes = g_overlaps_max_table_size_bytes;
  std::optional<double> overlaps_threshold_override;
  auto query_hint = getRegisteredQueryHint();
  if (query_hint.hint_delivered) {
    if (query_hint.overlaps_bucket_threshold != default_overlaps_bucket_threshold) {
      VLOG(1) << "Setting overlaps bucket threshold "
                 "\'overlaps_hashjoin_bucket_threshold\' via "
                 "query hint: "
              << query_hint.overlaps_bucket_threshold;
      overlaps_threshold_override = query_hint.overlaps_bucket_threshold;
    }
    if (query_hint.overlaps_max_size != overlaps_max_table_size_bytes) {
      std::ostringstream oss;
      oss << "User requests to change a threshold \'overlaps_max_table_size_bytes\' via "
             "query hint: "
          << overlaps_max_table_size_bytes << " -> " << query_hint.overlaps_max_size;
      if (!overlaps_threshold_override.has_value()) {
        overlaps_max_table_size_bytes = query_hint.overlaps_max_size;
      } else {
        oss << ", but is skipped since the query hint also changes the threshold "
               "\'overlaps_hashjoin_bucket_threshold\'";
      }
      VLOG(1) << oss.str();
    }
  }

  std::vector<ColumnsForDevice> columns_per_device;
  const auto catalog = executor_->getCatalog();
  CHECK(catalog);
  auto& data_mgr = catalog->getDataMgr();
  std::vector<std::unique_ptr<CudaAllocator>> dev_buff_owners;
  if (memory_level_ == Data_Namespace::MemoryLevel::GPU_LEVEL) {
    for (int device_id = 0; device_id < device_count_; ++device_id) {
      dev_buff_owners.emplace_back(std::make_unique<CudaAllocator>(&data_mgr, device_id));
    }
  }
  const auto shard_count = shardCount();
  for (int device_id = 0; device_id < device_count_; ++device_id) {
    const auto fragments =
        shard_count
            ? only_shards_for_device(query_info.fragments, device_id, device_count_)
            : query_info.fragments;
    const auto columns_for_device =
        fetchColumnsForDevice(fragments,
                              device_id,
                              memory_level_ == Data_Namespace::MemoryLevel::GPU_LEVEL
                                  ? dev_buff_owners[device_id].get()
                                  : nullptr);
    columns_per_device.push_back(columns_for_device);
  }

  // Prepare to calculate the size of the hash table.
  const auto composite_key_info =
      HashJoin::getCompositeKeyInfo(inner_outer_pairs_, executor_);

  auto cache_key_contains_intermediate_table = [](const auto cache_key) {
    for (auto key : cache_key.chunk_keys) {
      CHECK_GE(key.size(), size_t(2));
      if (key[1] < 0) {
        return true;
      }
    }
    return false;
  };

  if (overlaps_threshold_override) {
    // compute bucket sizes based on the user provided threshold
    BucketSizeTuner tuner(
        *overlaps_threshold_override, /*step=*/1.0, /*min_threshold=*/0.0);
    auto bucket_sizes =
        tuner.computeBucketSizes(getEffectiveMemoryLevel(inner_outer_pairs_),
                                 columns_per_device,
                                 inner_outer_pairs_,
                                 device_count_,
                                 executor_);

    auto [entry_count, emitted_keys_count] =
        computeHashTableCounts(shard_count, bucket_sizes, columns_per_device);
    setBucketSizeInfo(bucket_sizes, columns_per_device, device_count_);
    // reifyImpl will check the hash table cache for an appropriate hash table w/ those
    // bucket sizes (or within tolerances) if a hash table exists use it, otherwise build
    // one
    reifyImpl(columns_per_device,
              query_info,
              layout,
              shard_count,
              entry_count,
              emitted_keys_count);
  } else {
    HashTableCacheKey cache_key{columns_per_device.front().join_columns.front().num_elems,
                                composite_key_info.cache_key_chunks,
                                condition_->get_optype()};
    double overlaps_bucket_threshold = -1;
    auto cached_bucket_threshold_opt = auto_tuner_cache_->get(cache_key);
    if (cached_bucket_threshold_opt) {
      overlaps_bucket_threshold = cached_bucket_threshold_opt->first;
      VLOG(1) << "Auto tuner using cached overlaps hash table size of: "
              << overlaps_bucket_threshold;
      auto bucket_sizes = cached_bucket_threshold_opt->second;

      OverlapsHashTableCacheKey hash_table_cache_key(cache_key, bucket_sizes);
      if (auto hash_table_cache_opt =
              hash_table_cache_->getWithKey(hash_table_cache_key)) {
        // if we already have a built hash table, we can skip the scans required for
        // computing bucket size and tuple count
        auto key = hash_table_cache_opt->first;
        // reset as the hash table sizes can vary a bit
        setBucketSizeInfo(key.bucket_sizes, columns_per_device, device_count_);
        auto hash_table = hash_table_cache_opt->second;
        CHECK(hash_table);

        VLOG(1) << "Using cached hash table bucket size";

        reifyImpl(columns_per_device,
                  query_info,
                  layout,
                  shard_count,
                  hash_table->getEntryCount(),
                  hash_table->getEmittedKeysCount());
      } else {
        VLOG(1) << "Computing bucket size for cached bucket threshold";
        // compute bucket size using our cached tuner value
        BucketSizeTuner tuner(
            overlaps_bucket_threshold, /*step=*/1, /*min_threshold=*/0.0);

        auto bucket_sizes =
            tuner.computeBucketSizes(getEffectiveMemoryLevel(inner_outer_pairs_),
                                     columns_per_device,
                                     inner_outer_pairs_,
                                     device_count_,
                                     executor_);

        auto [entry_count, emitted_keys_count] =
            computeHashTableCounts(shard_count, bucket_sizes, columns_per_device);
        setBucketSizeInfo(bucket_sizes, columns_per_device, device_count_);

        reifyImpl(columns_per_device,
                  query_info,
                  layout,
                  shard_count,
                  entry_count,
                  emitted_keys_count);
      }
    } else {
      // compute bucket size using the auto tuner

      // TODO(jclay): Currently, joining on large poly sets
      // will lead to lengthy construction times (and large hash tables)
      // tune this to account for the characteristics of the data being joined.
      size_t entry_count{0};
      size_t emitted_keys_count{0};
      double chosen_overlaps_threshold{-1};
      double last_keys_per_bin{-1};
      const size_t max_hash_table_size_for_uncapped_min_keys_per_bin{2097152};  // 2MB

      BucketSizeTuner tuner(
          /*initial_threshold=*/1.0, /*step=*/2.0, /*min_threshold=*/1e-7);
      VLOG(1) << "Running overlaps join size auto tune with parameters: " << tuner;

      std::vector<double> bucket_sizes;
      while (tuner(bucket_sizes)) {
        bucket_sizes =
            tuner.computeBucketSizes(getEffectiveMemoryLevel(inner_outer_pairs_),
                                     columns_per_device,
                                     inner_outer_pairs_,
                                     device_count_,
                                     executor_);
        const auto [crt_entry_count, crt_emitted_keys_count] =
            computeHashTableCounts(shard_count, bucket_sizes, columns_per_device);

        const size_t hash_table_size = calculateHashTableSize(
            bucket_sizes.size(), crt_emitted_keys_count, crt_entry_count);
        const double keys_per_bin = crt_emitted_keys_count / (crt_entry_count / 2.0);
        VLOG(1) << "Tuner output: " << tuner << " giving entry_count: " << crt_entry_count
                << ", emitted_keys " << crt_emitted_keys_count << ", hash table size "
                << hash_table_size << ", keys per bin " << keys_per_bin;

        // decision points for termination prior to reaching the minimum threshold
        const bool previous_iteration_valid =
            last_keys_per_bin > 0 && chosen_overlaps_threshold > 0;
        const bool hash_table_too_big = hash_table_size > overlaps_max_table_size_bytes;
        const bool keys_per_bin_increasing = keys_per_bin > last_keys_per_bin;
        if (previous_iteration_valid && (hash_table_too_big || keys_per_bin_increasing)) {
          if (hash_table_too_big) {
            VLOG(1) << "Reached hash table size limit: " << overlaps_max_table_size_bytes
                    << " with " << hash_table_size << " byte hash table, " << keys_per_bin
                    << " keys per bin.";
          } else if (keys_per_bin_increasing) {
            VLOG(1) << "Keys per bin increasing from " << last_keys_per_bin << " to "
                    << keys_per_bin;
          }
          VLOG(1) << "Using previous threshold value " << chosen_overlaps_threshold;
          // reset bucket size info, as it will get overwriten with calculate counts above
          setBucketSizeInfo(
              bucket_sizes_for_dimension_, columns_per_device, device_count_);
          break;
        }
        chosen_overlaps_threshold = tuner.bucket_threshold;
        last_keys_per_bin = keys_per_bin;
        setBucketSizeInfo(bucket_sizes, columns_per_device, device_count_);
        entry_count = crt_entry_count;
        emitted_keys_count = crt_emitted_keys_count;
        const bool keys_per_bin_under_threshold =
            hash_table_size > max_hash_table_size_for_uncapped_min_keys_per_bin &&
            keys_per_bin < g_overlaps_target_entries_per_bin;
        if (keys_per_bin_under_threshold) {
          VLOG(1) << "Hash table reached size " << hash_table_size << " over threshold "
                  << max_hash_table_size_for_uncapped_min_keys_per_bin
                  << " with keys per bin " << keys_per_bin << " under threshold "
                  << g_overlaps_target_entries_per_bin
                  << ". Terminating bucket size loop.";
          break;
        }
      }
      const size_t hash_table_size = calculateHashTableSize(
          bucket_sizes_for_dimension_.size(), emitted_keys_count, entry_count);
      const double keys_per_bin = emitted_keys_count / (entry_count / 2.0);
      VLOG(1) << "Final tuner output: " << tuner << " giving entry_count: " << entry_count
              << ", emitted_keys_count " << emitted_keys_count << " hash table size "
              << hash_table_size << ", keys per bin " << keys_per_bin;
      CHECK(!bucket_sizes_for_dimension_.empty());
      VLOG(1) << "Final bucket sizes: ";
      for (size_t dim = 0; dim < bucket_sizes_for_dimension_.size(); dim++) {
        VLOG(1) << "dim[" << dim << "]: " << 1.0 / bucket_sizes_for_dimension_[dim];
      }
      CHECK_GE(chosen_overlaps_threshold, double(0));
      if (!cache_key_contains_intermediate_table(cache_key)) {
        auto cache_value =
            std::make_pair(chosen_overlaps_threshold, bucket_sizes_for_dimension_);
        auto_tuner_cache_->insert(cache_key, cache_value);
      }

      reifyImpl(columns_per_device,
                query_info,
                layout,
                shard_count,
                entry_count,
                emitted_keys_count);
    }
  }
}

size_t OverlapsJoinHashTable::calculateHashTableSize(size_t number_of_dimensions,
                                                     size_t emitted_keys_count,
                                                     size_t entry_count) const {
  const auto key_component_width = getKeyComponentWidth();
  const auto key_component_count = number_of_dimensions;
  const auto entry_size = key_component_count * key_component_width;
  const auto keys_for_all_rows = emitted_keys_count;
  const size_t one_to_many_hash_entries = 2 * entry_count + keys_for_all_rows;
  const size_t hash_table_size =
      entry_size * entry_count + one_to_many_hash_entries * sizeof(int32_t);
  return hash_table_size;
}

ColumnsForDevice OverlapsJoinHashTable::fetchColumnsForDevice(
    const std::vector<Fragmenter_Namespace::FragmentInfo>& fragments,
    const int device_id,
    DeviceAllocator* dev_buff_owner) {
  const auto& catalog = *executor_->getCatalog();
  const auto effective_memory_level = getEffectiveMemoryLevel(inner_outer_pairs_);

  std::vector<JoinColumn> join_columns;
  std::vector<std::shared_ptr<Chunk_NS::Chunk>> chunks_owner;
  std::vector<JoinColumnTypeInfo> join_column_types;
  std::vector<std::shared_ptr<void>> malloc_owner;
  for (const auto& inner_outer_pair : inner_outer_pairs_) {
    const auto inner_col = inner_outer_pair.first;
    const auto inner_cd = get_column_descriptor_maybe(
        inner_col->get_column_id(), inner_col->get_table_id(), catalog);
    if (inner_cd && inner_cd->isVirtualCol) {
      throw FailedToJoinOnVirtualColumn();
    }
    join_columns.emplace_back(fetchJoinColumn(inner_col,
                                              fragments,
                                              effective_memory_level,
                                              device_id,
                                              chunks_owner,
                                              dev_buff_owner,
                                              malloc_owner,
                                              executor_,
                                              &column_cache_));
    const auto& ti = inner_col->get_type_info();
    join_column_types.emplace_back(JoinColumnTypeInfo{static_cast<size_t>(ti.get_size()),
                                                      0,
                                                      0,
                                                      inline_int_null_value<int64_t>(),
                                                      false,
                                                      0,
                                                      get_join_column_type_kind(ti)});
    CHECK(ti.is_array()) << "Overlaps join currently only supported for arrays.";
  }
  return {join_columns, join_column_types, chunks_owner, {}, malloc_owner};
}

std::pair<size_t, size_t> OverlapsJoinHashTable::computeHashTableCounts(
    const size_t shard_count,
    const std::vector<double>& bucket_sizes_for_dimension,
    std::vector<ColumnsForDevice>& columns_per_device) {
  CHECK(!bucket_sizes_for_dimension.empty());
  const auto [tuple_count, emitted_keys_count] =
      approximateTupleCount(bucket_sizes_for_dimension, columns_per_device);
  const auto entry_count = 2 * std::max(tuple_count, size_t(1));

  return std::make_pair(
      get_entries_per_device(entry_count, shard_count, device_count_, memory_level_),
      emitted_keys_count);
}

std::pair<size_t, size_t> OverlapsJoinHashTable::approximateTupleCount(
    const std::vector<double>& bucket_sizes_for_dimension,
    std::vector<ColumnsForDevice>& columns_per_device) {
  const auto effective_memory_level = getEffectiveMemoryLevel(inner_outer_pairs_);
  CountDistinctDescriptor count_distinct_desc{
      CountDistinctImplType::Bitmap,
      0,
      11,
      true,
      effective_memory_level == Data_Namespace::MemoryLevel::GPU_LEVEL
          ? ExecutorDeviceType::GPU
          : ExecutorDeviceType::CPU,
      1};
  const auto padded_size_bytes = count_distinct_desc.bitmapPaddedSizeBytes();

  CHECK(!columns_per_device.empty() && !columns_per_device.front().join_columns.empty());
  if (columns_per_device.front().join_columns.front().num_elems == 0) {
    return std::make_pair(0, 0);
  }

  // TODO: state management in here should be revisited, but this should be safe enough
  // for now
  // re-compute bucket counts per device based on global bucket size
  for (size_t device_id = 0; device_id < columns_per_device.size(); ++device_id) {
    auto& columns_for_device = columns_per_device[device_id];
    columns_for_device.setBucketInfo(bucket_sizes_for_dimension, inner_outer_pairs_);
  }

  // Number of keys must match dimension of buckets
  CHECK_EQ(columns_per_device.front().join_columns.size(),
           columns_per_device.front().join_buckets.size());
  if (effective_memory_level == Data_Namespace::MemoryLevel::CPU_LEVEL) {
    // Note that this path assumes each device has the same hash table (for GPU hash join
    // w/ hash table built on CPU)
    const auto composite_key_info =
        HashJoin::getCompositeKeyInfo(inner_outer_pairs_, executor_);
    OverlapsHashTableCacheKey cache_key{
        columns_per_device.front().join_columns.front().num_elems,
        composite_key_info.cache_key_chunks,
        condition_->get_optype(),
        bucket_sizes_for_dimension};
    const auto cached_count_info = getApproximateTupleCountFromCache(cache_key);
    if (cached_count_info) {
      VLOG(1) << "Using a cached tuple count: " << cached_count_info->first
              << ", emitted keys count: " << cached_count_info->second;
      return *cached_count_info;
    }
    int thread_count = cpu_threads();
    std::vector<uint8_t> hll_buffer_all_cpus(thread_count * padded_size_bytes);
    auto hll_result = &hll_buffer_all_cpus[0];

    std::vector<int32_t> num_keys_for_row;
    // TODO(adb): support multi-column overlaps join
    num_keys_for_row.resize(columns_per_device.front().join_columns[0].num_elems);

    approximate_distinct_tuples_overlaps(hll_result,
                                         num_keys_for_row,
                                         count_distinct_desc.bitmap_sz_bits,
                                         padded_size_bytes,
                                         columns_per_device.front().join_columns,
                                         columns_per_device.front().join_column_types,
                                         columns_per_device.front().join_buckets,
                                         thread_count);
    for (int i = 1; i < thread_count; ++i) {
      hll_unify(hll_result,
                hll_result + i * padded_size_bytes,
                1 << count_distinct_desc.bitmap_sz_bits);
    }
    return std::make_pair(
        hll_size(hll_result, count_distinct_desc.bitmap_sz_bits),
        static_cast<size_t>(num_keys_for_row.size() > 0 ? num_keys_for_row.back() : 0));
  }
#ifdef HAVE_CUDA
  auto& data_mgr = executor_->getCatalog()->getDataMgr();
  std::vector<std::vector<uint8_t>> host_hll_buffers(device_count_);
  for (auto& host_hll_buffer : host_hll_buffers) {
    host_hll_buffer.resize(count_distinct_desc.bitmapPaddedSizeBytes());
  }
  std::vector<size_t> emitted_keys_count_device_threads(device_count_, 0);
  std::vector<std::future<void>> approximate_distinct_device_threads;
  for (int device_id = 0; device_id < device_count_; ++device_id) {
    approximate_distinct_device_threads.emplace_back(std::async(
        std::launch::async,
        [device_id,
         &columns_per_device,
         &count_distinct_desc,
         &data_mgr,
         &host_hll_buffers,
         &emitted_keys_count_device_threads] {
          CudaAllocator allocator(&data_mgr, device_id);
          auto device_hll_buffer =
              allocator.alloc(count_distinct_desc.bitmapPaddedSizeBytes());
          data_mgr.getCudaMgr()->zeroDeviceMem(
              device_hll_buffer, count_distinct_desc.bitmapPaddedSizeBytes(), device_id);
          const auto& columns_for_device = columns_per_device[device_id];
          auto join_columns_gpu = transfer_vector_of_flat_objects_to_gpu(
              columns_for_device.join_columns, allocator);

          CHECK_GT(columns_for_device.join_buckets.size(), 0u);
          const auto& bucket_sizes_for_dimension =
              columns_for_device.join_buckets[0].bucket_sizes_for_dimension;
          auto bucket_sizes_gpu =
              allocator.alloc(bucket_sizes_for_dimension.size() * sizeof(double));
          copy_to_gpu(&data_mgr,
                      reinterpret_cast<CUdeviceptr>(bucket_sizes_gpu),
                      bucket_sizes_for_dimension.data(),
                      bucket_sizes_for_dimension.size() * sizeof(double),
                      device_id);
          const size_t row_counts_buffer_sz =
              columns_per_device.front().join_columns[0].num_elems * sizeof(int32_t);
          auto row_counts_buffer = allocator.alloc(row_counts_buffer_sz);
          data_mgr.getCudaMgr()->zeroDeviceMem(
              row_counts_buffer, row_counts_buffer_sz, device_id);
          const auto key_handler =
              OverlapsKeyHandler(bucket_sizes_for_dimension.size(),
                                 join_columns_gpu,
                                 reinterpret_cast<double*>(bucket_sizes_gpu));
          const auto key_handler_gpu =
              transfer_flat_object_to_gpu(key_handler, allocator);
          approximate_distinct_tuples_on_device_overlaps(
              reinterpret_cast<uint8_t*>(device_hll_buffer),
              count_distinct_desc.bitmap_sz_bits,
              reinterpret_cast<int32_t*>(row_counts_buffer),
              key_handler_gpu,
              columns_for_device.join_columns[0].num_elems);

          auto& host_emitted_keys_count = emitted_keys_count_device_threads[device_id];
          copy_from_gpu(&data_mgr,
                        &host_emitted_keys_count,
                        reinterpret_cast<CUdeviceptr>(
                            row_counts_buffer +
                            (columns_per_device.front().join_columns[0].num_elems - 1) *
                                sizeof(int32_t)),
                        sizeof(int32_t),
                        device_id);

          auto& host_hll_buffer = host_hll_buffers[device_id];
          copy_from_gpu(&data_mgr,
                        &host_hll_buffer[0],
                        reinterpret_cast<CUdeviceptr>(device_hll_buffer),
                        count_distinct_desc.bitmapPaddedSizeBytes(),
                        device_id);
        }));
  }
  for (auto& child : approximate_distinct_device_threads) {
    child.get();
  }
  CHECK_EQ(Data_Namespace::MemoryLevel::GPU_LEVEL, effective_memory_level);
  auto& result_hll_buffer = host_hll_buffers.front();
  auto hll_result = reinterpret_cast<int32_t*>(&result_hll_buffer[0]);
  for (int device_id = 1; device_id < device_count_; ++device_id) {
    auto& host_hll_buffer = host_hll_buffers[device_id];
    hll_unify(hll_result,
              reinterpret_cast<int32_t*>(&host_hll_buffer[0]),
              1 << count_distinct_desc.bitmap_sz_bits);
  }
  const size_t emitted_keys_count =
      std::accumulate(emitted_keys_count_device_threads.begin(),
                      emitted_keys_count_device_threads.end(),
                      0);
  return std::make_pair(hll_size(hll_result, count_distinct_desc.bitmap_sz_bits),
                        emitted_keys_count);
#else
  UNREACHABLE();
  return {0, 0};
#endif  // HAVE_CUDA
}

void OverlapsJoinHashTable::setBucketSizeInfo(
    const std::vector<double>& bucket_sizes,
    std::vector<ColumnsForDevice>& columns_per_device,
    const size_t device_count) {
  // set global bucket size
  bucket_sizes_for_dimension_ = bucket_sizes;

  // re-compute bucket counts per device based on global bucket size
  CHECK_EQ(columns_per_device.size(), size_t(device_count));
  for (size_t device_id = 0; device_id < device_count; ++device_id) {
    auto& columns_for_device = columns_per_device[device_id];
    columns_for_device.setBucketInfo(bucket_sizes_for_dimension_, inner_outer_pairs_);
  }
}

size_t OverlapsJoinHashTable::getKeyComponentWidth() const {
  return 8;
}

size_t OverlapsJoinHashTable::getKeyComponentCount() const {
  CHECK(!bucket_sizes_for_dimension_.empty());
  return bucket_sizes_for_dimension_.size();
}

void OverlapsJoinHashTable::reify(const HashType preferred_layout) {
  auto timer = DEBUG_TIMER(__func__);
  CHECK_LT(0, device_count_);
  const auto composite_key_info =
      HashJoin::getCompositeKeyInfo(inner_outer_pairs_, executor_);

  CHECK(condition_->is_overlaps_oper());
  CHECK_EQ(inner_outer_pairs_.size(), size_t(1));
  HashType layout;
  if (inner_outer_pairs_[0].second->get_type_info().is_array()) {
    layout = HashType::ManyToMany;
  } else {
    layout = HashType::OneToMany;
  }
  try {
    reifyWithLayout(layout);
    return;
  } catch (const std::exception& e) {
    VLOG(1) << "Caught exception while building overlaps baseline hash table: "
            << e.what();
    throw;
  }
}

void OverlapsJoinHashTable::reifyImpl(std::vector<ColumnsForDevice>& columns_per_device,
                                      const Fragmenter_Namespace::TableInfo& query_info,
                                      const HashType layout,
                                      const size_t shard_count,
                                      const size_t entry_count,
                                      const size_t emitted_keys_count) {
  std::vector<std::future<void>> init_threads;
  for (int device_id = 0; device_id < device_count_; ++device_id) {
    const auto fragments =
        shard_count
            ? only_shards_for_device(query_info.fragments, device_id, device_count_)
            : query_info.fragments;
    init_threads.push_back(std::async(std::launch::async,
                                      &OverlapsJoinHashTable::reifyForDevice,
                                      this,
                                      columns_per_device[device_id],
                                      layout,
                                      entry_count,
                                      emitted_keys_count,
                                      device_id,
                                      logger::thread_id()));
  }
  for (auto& init_thread : init_threads) {
    init_thread.wait();
  }
  for (auto& init_thread : init_threads) {
    init_thread.get();
  }
}

void OverlapsJoinHashTable::reifyForDevice(const ColumnsForDevice& columns_for_device,
                                           const HashType layout,
                                           const size_t entry_count,
                                           const size_t emitted_keys_count,
                                           const int device_id,
                                           const logger::ThreadId parent_thread_id) {
  DEBUG_TIMER_NEW_THREAD(parent_thread_id);
  CHECK_EQ(getKeyComponentWidth(), size_t(8));
  CHECK(layoutRequiresAdditionalBuffers(layout));
  const auto effective_memory_level = getEffectiveMemoryLevel(inner_outer_pairs_);

  if (effective_memory_level == Data_Namespace::MemoryLevel::CPU_LEVEL) {
    VLOG(1) << "Building overlaps join hash table on CPU.";
    auto hash_table = initHashTableOnCpu(columns_for_device.join_columns,
                                         columns_for_device.join_column_types,
                                         columns_for_device.join_buckets,
                                         layout,
                                         entry_count,
                                         emitted_keys_count);
    CHECK(hash_table);

#ifdef HAVE_CUDA
    if (memory_level_ == Data_Namespace::MemoryLevel::GPU_LEVEL) {
      auto gpu_hash_table = copyCpuHashTableToGpu(
          std::move(hash_table), layout, entry_count, emitted_keys_count, device_id);
      CHECK_LT(size_t(device_id), hash_tables_for_device_.size());
      hash_tables_for_device_[device_id] = std::move(gpu_hash_table);
    } else {
#else
    CHECK_EQ(Data_Namespace::CPU_LEVEL, effective_memory_level);
#endif
      CHECK_EQ(hash_tables_for_device_.size(), size_t(1));
      hash_tables_for_device_[0] = std::move(hash_table);
#ifdef HAVE_CUDA
    }
#endif
  } else {
#ifdef HAVE_CUDA
    auto hash_table = initHashTableOnGpu(columns_for_device.join_columns,
                                         columns_for_device.join_column_types,
                                         columns_for_device.join_buckets,
                                         layout,
                                         entry_count,
                                         emitted_keys_count,
                                         device_id);
    CHECK_LT(size_t(device_id), hash_tables_for_device_.size());
    hash_tables_for_device_[device_id] = std::move(hash_table);
#else
    UNREACHABLE();
#endif
  }
}

std::shared_ptr<BaselineHashTable> OverlapsJoinHashTable::initHashTableOnCpu(
    const std::vector<JoinColumn>& join_columns,
    const std::vector<JoinColumnTypeInfo>& join_column_types,
    const std::vector<JoinBucketInfo>& join_bucket_info,
    const HashType layout,
    const size_t entry_count,
    const size_t emitted_keys_count) {
  auto timer = DEBUG_TIMER(__func__);
  const auto composite_key_info =
      HashJoin::getCompositeKeyInfo(inner_outer_pairs_, executor_);
  CHECK(!join_columns.empty());
  CHECK(!join_bucket_info.empty());
  OverlapsHashTableCacheKey cache_key{join_columns.front().num_elems,
                                      composite_key_info.cache_key_chunks,
                                      condition_->get_optype(),
                                      bucket_sizes_for_dimension_};

  std::lock_guard<std::mutex> cpu_hash_table_buff_lock(cpu_hash_table_buff_mutex_);
  if (auto generic_hash_table = initHashTableOnCpuFromCache(cache_key)) {
    if (auto hash_table =
            std::dynamic_pointer_cast<BaselineHashTable>(generic_hash_table)) {
      VLOG(1) << "Using cached CPU hash table for initialization.";
      // See if a hash table of a different layout was returned.
      // If it was OneToMany, we can reuse it on ManyToMany.
      if (layout == HashType::ManyToMany &&
          hash_table->getLayout() == HashType::OneToMany) {
        // use the cached hash table
        layout_override_ = HashType::ManyToMany;
        return hash_table;
      }
    }
  }
  CHECK(layoutRequiresAdditionalBuffers(layout));
  const auto key_component_count = join_bucket_info[0].bucket_sizes_for_dimension.size();

  const auto key_handler =
      OverlapsKeyHandler(key_component_count,
                         &join_columns[0],
                         join_bucket_info[0].bucket_sizes_for_dimension.data());
  const auto catalog = executor_->getCatalog();
  BaselineJoinHashTableBuilder builder(catalog);
  const auto err = builder.initHashTableOnCpu(&key_handler,
                                              composite_key_info,
                                              join_columns,
                                              join_column_types,
                                              join_bucket_info,
                                              entry_count,
                                              emitted_keys_count,
                                              layout,
                                              getKeyComponentWidth(),
                                              getKeyComponentCount());
  if (err) {
    throw HashJoinFail(
        std::string("Unrecognized error when initializing CPU overlaps hash table (") +
        std::to_string(err) + std::string(")"));
  }
  std::shared_ptr<BaselineHashTable> hash_table = builder.getHashTable();
  if (HashJoin::getInnerTableId(inner_outer_pairs_) > 0) {
    putHashTableOnCpuToCache(cache_key, hash_table);
  }
  return hash_table;
}

#ifdef HAVE_CUDA

std::shared_ptr<BaselineHashTable> OverlapsJoinHashTable::initHashTableOnGpu(
    const std::vector<JoinColumn>& join_columns,
    const std::vector<JoinColumnTypeInfo>& join_column_types,
    const std::vector<JoinBucketInfo>& join_bucket_info,
    const HashType layout,
    const size_t entry_count,
    const size_t emitted_keys_count,
    const size_t device_id) {
  CHECK_EQ(memory_level_, Data_Namespace::MemoryLevel::GPU_LEVEL);

  VLOG(1) << "Building overlaps join hash table on GPU.";

  const auto catalog = executor_->getCatalog();
  CHECK(catalog);
  BaselineJoinHashTableBuilder builder(catalog);

  auto& data_mgr = catalog->getDataMgr();
  CudaAllocator allocator(&data_mgr, device_id);
  auto join_columns_gpu = transfer_vector_of_flat_objects_to_gpu(join_columns, allocator);
  CHECK_EQ(join_columns.size(), 1u);
  CHECK(!join_bucket_info.empty());
  auto& bucket_sizes_for_dimension = join_bucket_info[0].bucket_sizes_for_dimension;
  auto bucket_sizes_gpu =
      transfer_vector_of_flat_objects_to_gpu(bucket_sizes_for_dimension, allocator);
  const auto key_handler = OverlapsKeyHandler(
      bucket_sizes_for_dimension.size(), join_columns_gpu, bucket_sizes_gpu);

  const auto err = builder.initHashTableOnGpu(&key_handler,
                                              join_columns,
                                              layout,
                                              getKeyComponentWidth(),
                                              getKeyComponentCount(),
                                              entry_count,
                                              emitted_keys_count,
                                              device_id);
  if (err) {
    throw HashJoinFail(
        std::string("Unrecognized error when initializing GPU overlaps hash table (") +
        std::to_string(err) + std::string(")"));
  }
  return builder.getHashTable();
}

std::shared_ptr<BaselineHashTable> OverlapsJoinHashTable::copyCpuHashTableToGpu(
    std::shared_ptr<BaselineHashTable>&& cpu_hash_table,
    const HashType layout,
    const size_t entry_count,
    const size_t emitted_keys_count,
    const size_t device_id) {
  CHECK_EQ(memory_level_, Data_Namespace::MemoryLevel::GPU_LEVEL);

  auto catalog = executor_->getCatalog();
  CHECK(catalog);
  auto& data_mgr = catalog->getDataMgr();

  // copy hash table to GPU
  BaselineJoinHashTableBuilder gpu_builder(executor_->catalog_);
  gpu_builder.allocateDeviceMemory(layout,
                                   getKeyComponentWidth(),
                                   getKeyComponentCount(),
                                   entry_count,
                                   emitted_keys_count,
                                   device_id);
  std::shared_ptr<BaselineHashTable> gpu_hash_table = gpu_builder.getHashTable();
  CHECK(gpu_hash_table);
  auto gpu_buffer_ptr = gpu_hash_table->getGpuBuffer();
  CHECK(gpu_buffer_ptr);

  CHECK_LE(cpu_hash_table->getHashTableBufferSize(ExecutorDeviceType::CPU),
           gpu_hash_table->getHashTableBufferSize(ExecutorDeviceType::GPU));
  copy_to_gpu(&data_mgr,
              reinterpret_cast<CUdeviceptr>(gpu_buffer_ptr),
              cpu_hash_table->getCpuBuffer(),
              cpu_hash_table->getHashTableBufferSize(ExecutorDeviceType::CPU),
              device_id);
  return gpu_hash_table;
}

#endif  // HAVE_CUDA

#define LL_CONTEXT executor_->cgen_state_->context_
#define LL_BUILDER executor_->cgen_state_->ir_builder_
#define LL_INT(v) executor_->cgen_state_->llInt(v)
#define LL_FP(v) executor_->cgen_state_->llFp(v)
#define ROW_FUNC executor_->cgen_state_->row_func_

llvm::Value* OverlapsJoinHashTable::codegenKey(const CompilationOptions& co) {
  AUTOMATIC_IR_METADATA(executor_->cgen_state_.get());
  const auto key_component_width = getKeyComponentWidth();
  CHECK(key_component_width == 4 || key_component_width == 8);
  const auto key_size_lv = LL_INT(getKeyComponentCount() * key_component_width);
  llvm::Value* key_buff_lv{nullptr};
  switch (key_component_width) {
    case 4:
      key_buff_lv =
          LL_BUILDER.CreateAlloca(llvm::Type::getInt32Ty(LL_CONTEXT), key_size_lv);
      break;
    case 8:
      key_buff_lv =
          LL_BUILDER.CreateAlloca(llvm::Type::getInt64Ty(LL_CONTEXT), key_size_lv);
      break;
    default:
      CHECK(false);
  }

  const auto& inner_outer_pair = inner_outer_pairs_[0];
  const auto outer_col = inner_outer_pair.second;
  const auto outer_col_ti = outer_col->get_type_info();

  if (outer_col_ti.is_geometry()) {
    CodeGenerator code_generator(executor_);
    // TODO(adb): for points we will use the coords array, but for other geometries we
    // will need to use the bounding box. For now only support points.
    CHECK_EQ(outer_col_ti.get_type(), kPOINT);
    CHECK_EQ(bucket_sizes_for_dimension_.size(), static_cast<size_t>(2));

    const auto col_lvs = code_generator.codegen(outer_col, true, co);
    CHECK_EQ(col_lvs.size(), size_t(1));

    const auto outer_col_var = dynamic_cast<const Analyzer::ColumnVar*>(outer_col);
    CHECK(outer_col_var);
    const auto coords_cd = executor_->getCatalog()->getMetadataForColumn(
        outer_col_var->get_table_id(), outer_col_var->get_column_id() + 1);
    CHECK(coords_cd);

    const auto array_ptr = executor_->cgen_state_->emitExternalCall(
        "array_buff",
        llvm::Type::getInt8PtrTy(executor_->cgen_state_->context_),
        {col_lvs.front(), code_generator.posArg(outer_col)});
    CHECK(coords_cd->columnType.get_elem_type().get_type() == kTINYINT)
        << "Only TINYINT coordinates columns are supported in geo overlaps hash join.";
    const auto arr_ptr =
        code_generator.castArrayPointer(array_ptr, coords_cd->columnType.get_elem_type());

    for (size_t i = 0; i < 2; i++) {
      const auto key_comp_dest_lv = LL_BUILDER.CreateGEP(key_buff_lv, LL_INT(i));

      // Note that get_bucket_key_for_range_compressed will need to be specialized for
      // future compression schemes
      auto bucket_key =
          outer_col_ti.get_compression() == kENCODING_GEOINT
              ? executor_->cgen_state_->emitExternalCall(
                    "get_bucket_key_for_range_compressed",
                    get_int_type(64, LL_CONTEXT),
                    {arr_ptr, LL_INT(i), LL_FP(bucket_sizes_for_dimension_[i])})
              : executor_->cgen_state_->emitExternalCall(
                    "get_bucket_key_for_range_double",
                    get_int_type(64, LL_CONTEXT),
                    {arr_ptr, LL_INT(i), LL_FP(bucket_sizes_for_dimension_[i])});
      const auto col_lv = LL_BUILDER.CreateSExt(
          bucket_key, get_int_type(key_component_width * 8, LL_CONTEXT));
      LL_BUILDER.CreateStore(col_lv, key_comp_dest_lv);
    }
  } else {
    LOG(FATAL) << "Overlaps key currently only supported for geospatial types.";
  }
  return key_buff_lv;
}

std::vector<llvm::Value*> OverlapsJoinHashTable::codegenManyKey(
    const CompilationOptions& co) {
  AUTOMATIC_IR_METADATA(executor_->cgen_state_.get());
  const auto key_component_width = getKeyComponentWidth();
  CHECK(key_component_width == 4 || key_component_width == 8);
  auto hash_table = getHashTableForDevice(size_t(0));
  CHECK(hash_table);
  CHECK(getHashType() == HashType::ManyToMany);

  VLOG(1) << "Performing codgen for ManyToMany";
  const auto& inner_outer_pair = inner_outer_pairs_[0];
  const auto outer_col = inner_outer_pair.second;

  CodeGenerator code_generator(executor_);
  const auto col_lvs = code_generator.codegen(outer_col, true, co);
  CHECK_EQ(col_lvs.size(), size_t(1));

  const auto outer_col_var = dynamic_cast<const Analyzer::ColumnVar*>(outer_col);
  CHECK(outer_col_var);
  const auto coords_cd = executor_->getCatalog()->getMetadataForColumn(
      outer_col_var->get_table_id(), outer_col_var->get_column_id());
  CHECK(coords_cd);

  const auto array_ptr = executor_->cgen_state_->emitExternalCall(
      "array_buff",
      llvm::Type::getInt8PtrTy(executor_->cgen_state_->context_),
      {col_lvs.front(), code_generator.posArg(outer_col)});

  // TODO(jclay): this seems to cast to double, and causes the GPU build to fail.
  // const auto arr_ptr =
  //     code_generator.castArrayPointer(array_ptr,
  //     coords_cd->columnType.get_elem_type());
  array_ptr->setName("array_ptr");

  auto num_keys_lv =
      executor_->cgen_state_->emitExternalCall("get_num_buckets_for_bounds",
                                               get_int_type(32, LL_CONTEXT),
                                               {array_ptr,
                                                LL_INT(0),
                                                LL_FP(bucket_sizes_for_dimension_[0]),
                                                LL_FP(bucket_sizes_for_dimension_[1])});
  num_keys_lv->setName("num_keys_lv");

  return {num_keys_lv, array_ptr};
}

HashJoinMatchingSet OverlapsJoinHashTable::codegenMatchingSet(
    const CompilationOptions& co,
    const size_t index) {
  AUTOMATIC_IR_METADATA(executor_->cgen_state_.get());
  if (getHashType() == HashType::ManyToMany) {
    VLOG(1) << "Building codegenMatchingSet for ManyToMany";
    const auto key_component_width = getKeyComponentWidth();
    CHECK(key_component_width == 4 || key_component_width == 8);
    auto many_to_many_args = codegenManyKey(co);
    auto hash_ptr = HashJoin::codegenHashTableLoad(index, executor_);
    const auto composite_dict_ptr_type =
        llvm::Type::getIntNPtrTy(LL_CONTEXT, key_component_width * 8);
    const auto composite_key_dict =
        hash_ptr->getType()->isPointerTy()
            ? LL_BUILDER.CreatePointerCast(hash_ptr, composite_dict_ptr_type)
            : LL_BUILDER.CreateIntToPtr(hash_ptr, composite_dict_ptr_type);
    const auto key_component_count = getKeyComponentCount();

    auto one_to_many_ptr = hash_ptr;

    if (one_to_many_ptr->getType()->isPointerTy()) {
      one_to_many_ptr =
          LL_BUILDER.CreatePtrToInt(hash_ptr, llvm::Type::getInt64Ty(LL_CONTEXT));
    } else {
      CHECK(one_to_many_ptr->getType()->isIntegerTy(64));
    }

    const auto composite_key_dict_size = offsetBufferOff();
    one_to_many_ptr =
        LL_BUILDER.CreateAdd(one_to_many_ptr, LL_INT(composite_key_dict_size));

    // NOTE(jclay): A fixed array of size 200 is allocated on the stack.
    // this is likely the maximum value we can do that is safe to use across
    // all supported GPU architectures.
    const int max_array_size = 200;
    const auto arr_type = get_int_array_type(32, max_array_size, LL_CONTEXT);
    const auto out_arr_lv = LL_BUILDER.CreateAlloca(arr_type);
    out_arr_lv->setName("out_arr");

    const auto casted_out_arr_lv =
        LL_BUILDER.CreatePointerCast(out_arr_lv, arr_type->getPointerTo());

    const auto element_ptr = LL_BUILDER.CreateGEP(arr_type, casted_out_arr_lv, LL_INT(0));

    auto rowid_ptr_i32 =
        LL_BUILDER.CreatePointerCast(element_ptr, llvm::Type::getInt32PtrTy(LL_CONTEXT));

    const auto candidate_count_lv = executor_->cgen_state_->emitExternalCall(
        "get_candidate_rows",
        llvm::Type::getInt64Ty(LL_CONTEXT),
        {
            rowid_ptr_i32,
            LL_INT(max_array_size),
            many_to_many_args[1],
            LL_INT(0),
            LL_FP(bucket_sizes_for_dimension_[0]),
            LL_FP(bucket_sizes_for_dimension_[1]),
            many_to_many_args[0],
            LL_INT(key_component_count),               // key_component_count
            composite_key_dict,                        // ptr to hash table
            LL_INT(getEntryCount()),                   // entry_count
            LL_INT(composite_key_dict_size),           // offset_buffer_ptr_offset
            LL_INT(getEntryCount() * sizeof(int32_t))  // sub_buff_size
        });

    const auto slot_lv = LL_INT(int64_t(0));

    return {rowid_ptr_i32, candidate_count_lv, slot_lv};
  } else {
    VLOG(1) << "Building codegenMatchingSet for Baseline";
    // TODO: duplicated w/ BaselineJoinHashTable -- push into the hash table builder?
    const auto key_component_width = getKeyComponentWidth();
    CHECK(key_component_width == 4 || key_component_width == 8);
    auto key_buff_lv = codegenKey(co);
    CHECK(getHashType() == HashType::OneToMany);
    auto hash_ptr = HashJoin::codegenHashTableLoad(index, executor_);
    const auto composite_dict_ptr_type =
        llvm::Type::getIntNPtrTy(LL_CONTEXT, key_component_width * 8);
    const auto composite_key_dict =
        hash_ptr->getType()->isPointerTy()
            ? LL_BUILDER.CreatePointerCast(hash_ptr, composite_dict_ptr_type)
            : LL_BUILDER.CreateIntToPtr(hash_ptr, composite_dict_ptr_type);
    const auto key_component_count = getKeyComponentCount();
    const auto key = executor_->cgen_state_->emitExternalCall(
        "get_composite_key_index_" + std::to_string(key_component_width * 8),
        get_int_type(64, LL_CONTEXT),
        {key_buff_lv,
         LL_INT(key_component_count),
         composite_key_dict,
         LL_INT(getEntryCount())});
    auto one_to_many_ptr = hash_ptr;
    if (one_to_many_ptr->getType()->isPointerTy()) {
      one_to_many_ptr =
          LL_BUILDER.CreatePtrToInt(hash_ptr, llvm::Type::getInt64Ty(LL_CONTEXT));
    } else {
      CHECK(one_to_many_ptr->getType()->isIntegerTy(64));
    }
    const auto composite_key_dict_size = offsetBufferOff();
    one_to_many_ptr =
        LL_BUILDER.CreateAdd(one_to_many_ptr, LL_INT(composite_key_dict_size));
    return HashJoin::codegenMatchingSet(
        std::vector<llvm::Value*>{
            one_to_many_ptr, key, LL_INT(int64_t(0)), LL_INT(getEntryCount() - 1)},
        false,
        false,
        false,
        getComponentBufferSize(),
        executor_);
  }
  UNREACHABLE();
  return HashJoinMatchingSet{};
}

std::string OverlapsJoinHashTable::toString(const ExecutorDeviceType device_type,
                                            const int device_id,
                                            bool raw) const {
  auto buffer = getJoinHashBuffer(device_type, device_id);
  CHECK_LT(device_id, hash_tables_for_device_.size());
  auto hash_table = hash_tables_for_device_[device_id];
  CHECK(hash_table);
  auto buffer_size = hash_table->getHashTableBufferSize(device_type);
#ifdef HAVE_CUDA
  std::unique_ptr<int8_t[]> buffer_copy;
  if (device_type == ExecutorDeviceType::GPU) {
    buffer_copy = std::make_unique<int8_t[]>(buffer_size);
    CHECK(executor_);
    auto& data_mgr = executor_->getCatalog()->getDataMgr();

    copy_from_gpu(&data_mgr,
                  buffer_copy.get(),
                  reinterpret_cast<CUdeviceptr>(reinterpret_cast<int8_t*>(buffer)),
                  buffer_size,
                  device_id);
  }
  auto ptr1 = buffer_copy ? buffer_copy.get() : reinterpret_cast<const int8_t*>(buffer);
#else
  auto ptr1 = reinterpret_cast<const int8_t*>(buffer);
#endif  // HAVE_CUDA
  auto ptr2 = ptr1 + offsetBufferOff();
  auto ptr3 = ptr1 + countBufferOff();
  auto ptr4 = ptr1 + payloadBufferOff();
  CHECK(hash_table);
  const auto layout = getHashType();
  return HashTable::toString(
      "geo",
      getHashTypeString(layout),
      getKeyComponentCount() + (layout == HashType::OneToOne ? 1 : 0),
      getKeyComponentWidth(),
      hash_table->getEntryCount(),
      ptr1,
      ptr2,
      ptr3,
      ptr4,
      buffer_size,
      raw);
}

std::set<DecodedJoinHashBufferEntry> OverlapsJoinHashTable::toSet(
    const ExecutorDeviceType device_type,
    const int device_id) const {
  auto buffer = getJoinHashBuffer(device_type, device_id);
  auto hash_table = getHashTableForDevice(device_id);
  CHECK(hash_table);
  auto buffer_size = hash_table->getHashTableBufferSize(device_type);
#ifdef HAVE_CUDA
  std::unique_ptr<int8_t[]> buffer_copy;
  if (device_type == ExecutorDeviceType::GPU) {
    buffer_copy = std::make_unique<int8_t[]>(buffer_size);
    CHECK(executor_);
    auto& data_mgr = executor_->getCatalog()->getDataMgr();
    copy_from_gpu(&data_mgr,
                  buffer_copy.get(),
                  reinterpret_cast<CUdeviceptr>(reinterpret_cast<int8_t*>(buffer)),
                  buffer_size,
                  device_id);
  }
  auto ptr1 = buffer_copy ? buffer_copy.get() : reinterpret_cast<const int8_t*>(buffer);
#else
  auto ptr1 = reinterpret_cast<const int8_t*>(buffer);
#endif  // HAVE_CUDA
  auto ptr2 = ptr1 + offsetBufferOff();
  auto ptr3 = ptr1 + countBufferOff();
  auto ptr4 = ptr1 + payloadBufferOff();
  const auto layout = getHashType();
  return HashTable::toSet(getKeyComponentCount() + (layout == HashType::OneToOne ? 1 : 0),
                          getKeyComponentWidth(),
                          hash_table->getEntryCount(),
                          ptr1,
                          ptr2,
                          ptr3,
                          ptr4,
                          buffer_size);
}

Data_Namespace::MemoryLevel OverlapsJoinHashTable::getEffectiveMemoryLevel(
    const std::vector<InnerOuter>& inner_outer_pairs) const {
  // always build on CPU
  if (query_hint_.hint_delivered && query_hint_.overlaps_allow_gpu_build) {
    return memory_level_;
  }
  return Data_Namespace::MemoryLevel::CPU_LEVEL;
}

int OverlapsJoinHashTable::getInnerTableId() const noexcept {
  try {
    return HashJoin::getInnerTableId(inner_outer_pairs_);
  } catch (...) {
    CHECK(false);
  }
  return 0;
}

std::shared_ptr<HashTable> OverlapsJoinHashTable::initHashTableOnCpuFromCache(
    const OverlapsHashTableCacheKey& key) {
  auto timer = DEBUG_TIMER(__func__);
  VLOG(1) << "Checking CPU hash table cache.";
  CHECK(hash_table_cache_);
  auto hash_table_opt = hash_table_cache_->getWithKey(key);
  if (hash_table_opt) {
    CHECK(bucket_sizes_for_dimension_ == hash_table_opt->first.bucket_sizes);
    return hash_table_opt->second;
  }
  return nullptr;
}

std::optional<std::pair<size_t, size_t>>
OverlapsJoinHashTable::getApproximateTupleCountFromCache(
    const OverlapsHashTableCacheKey& key) {
  for (auto chunk_key : key.chunk_keys) {
    CHECK_GE(chunk_key.size(), size_t(2));
    if (chunk_key[1] < 0) {
      return std::nullopt;
      ;
    }
  }

  CHECK(hash_table_cache_);
  auto hash_table_opt = hash_table_cache_->getWithKey(key);
  if (hash_table_opt) {
    auto hash_table = hash_table_opt->second;
    return std::make_pair(hash_table->getEntryCount() / 2,
                          hash_table->getEmittedKeysCount());
  }
  return std::nullopt;
}

void OverlapsJoinHashTable::putHashTableOnCpuToCache(
    const OverlapsHashTableCacheKey& key,
    std::shared_ptr<HashTable> hash_table) {
  for (auto chunk_key : key.chunk_keys) {
    CHECK_GE(chunk_key.size(), size_t(2));
    if (chunk_key[1] < 0) {
      return;
    }
  }
  CHECK(hash_table_cache_);
  hash_table_cache_->insert(key, hash_table);
}

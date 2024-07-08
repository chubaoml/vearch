/**
 * Copyright 2019 The Gamma Authors.
 *
 * This source code is licensed under the Apache License, Version 2.0 license
 * found in the LICENSE file in the root directory of this source tree.
 */

#include "field_range_index.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <typeinfo>

#include "threadskv8.h"
#include "util/bitmap.h"
#include "util/log.h"
#include "util/utils.h"

namespace vearch {

class Node {
 public:
  Node() {
    size_ = 0;
    data_dense_ = nullptr;
    data_sparse_ = nullptr;
    min_ = std::numeric_limits<int>::max();
    min_aligned_ = std::numeric_limits<int>::max();
    max_ = -1;
    max_aligned_ = -1;
    capacity_ = 0;
    type_ = Sparse;
    n_extend_ = 0;
  }

  ~Node() {
    if (type_ == Dense) {
      if (data_dense_) {
        free(data_dense_);
        data_dense_ = nullptr;
      }
    } else {
      if (data_sparse_) {
        free(data_sparse_);
        data_sparse_ = nullptr;
      }
    }
  }

  typedef enum NodeType { Dense, Sparse } NodeType;

  int AddDense(int val) {
    int op_len = sizeof(BM_OPERATE_TYPE) * 8;

    if (size_ == 0) {
      min_ = val;
      max_ = val;
      min_aligned_ = (val / op_len) * op_len;
      max_aligned_ = (val / op_len + 1) * op_len - 1;
      int bytes_count = -1;

      if (bitmap::create(data_dense_, bytes_count,
                         max_aligned_ - min_aligned_ + 1) != 0) {
        LOG(ERROR) << "Cannot create bitmap!";
        return -1;
      }
      bitmap::set(data_dense_, val - min_aligned_);
      ++size_;
      return 0;
    }

    if (val < min_aligned_) {
      char *data = nullptr;
      int min_aligned = (val / op_len) * op_len;

      // LOG(INFO) << "Dense lower min_aligned_ [" << min_aligned_ << "],
      // min_aligned ["
      //           << min_aligned << "] max_aligned_ [" << max_aligned_ << "]";

      int bytes_count = -1;
      if (bitmap::create(data, bytes_count, max_aligned_ - min_aligned + 1) !=
          0) {
        LOG(ERROR) << "Cannot create bitmap!";
        return -1;
      }

      BM_OPERATE_TYPE *op_data_dst = (BM_OPERATE_TYPE *)data;
      BM_OPERATE_TYPE *op_data_ori = (BM_OPERATE_TYPE *)data_dense_;

      for (int i = 0; i < (max_aligned_ - min_aligned_ + 1) / op_len; ++i) {
        op_data_dst[i + (min_aligned_ - min_aligned) / op_len] = op_data_ori[i];
      }

      bitmap::set(data, val - min_aligned);
      free(data_dense_);
      data_dense_ = data;
      min_ = val;
      min_aligned_ = min_aligned;
    } else if (val > max_aligned_) {
      // double k = 1 + exp(-1 * n_extend_ + 1);
      char *data = nullptr;
      // 2X spare space to speed up insert
      int max_aligned = (val / op_len + 1) * op_len * 2 - 1;

      // LOG(INFO) << "Dense upper min_aligned_ [" << min_aligned_ << "],
      // max_aligned ["
      //           << max_aligned << "] max_aligned_ [" << max_aligned_ << "]
      //           size ["
      //           << size_ << "] val [" << val << "] min [" << min_ << "] max
      //           [" << max_ << "]";

      int bytes_count = -1;
      if (bitmap::create(data, bytes_count, max_aligned - min_aligned_ + 1) !=
          0) {
        LOG(ERROR) << "Cannot create bitmap!";
        return -1;
      }

      BM_OPERATE_TYPE *op_data_dst = (BM_OPERATE_TYPE *)data;
      BM_OPERATE_TYPE *op_data_ori = (BM_OPERATE_TYPE *)data_dense_;

      for (int i = 0; i < (max_aligned_ - min_aligned_ + 1) / op_len; ++i) {
        op_data_dst[i] = op_data_ori[i];
      }

      bitmap::set(data, val - min_aligned_);
      free(data_dense_);
      data_dense_ = data;
      max_ = val;
      max_aligned_ = max_aligned;
    } else {
      bitmap::set(data_dense_, val - min_aligned_);
      min_ = std::min(min_, val);
      max_ = std::max(max_, val);
    }

    ++size_;
    return 0;
  }

  int AddSparse(int val) {
    int op_len = sizeof(BM_OPERATE_TYPE) * 8;

    if (capacity_ == 0) {
      capacity_ = 1;
      data_sparse_ = (int *)malloc(capacity_ * sizeof(int));
    } else if (size_ >= capacity_) {
      capacity_ *= 2;
      // LOG(INFO) << "Sparse capacity [" << capacity_ << "]";
      int *data = (int *)malloc(capacity_ * sizeof(int));
      for (int i = 0; i < size_; ++i) {
        data[i] = data_sparse_[i];
      }

      free(data_sparse_);
      data_sparse_ = data;
    }
    data_sparse_[size_] = val;

    ++size_;
    min_ = std::min(min_, val);
    max_ = std::max(max_, val);
    if (val < min_aligned_) {
      min_aligned_ = (val / op_len) * op_len;
    }
    if (val > max_aligned_) {
      max_aligned_ = (val / op_len + 1) * op_len - 1;
    }
    return 0;
  }

  int Add(int val) {
    int offset = max_ - min_;
    double density = (size_ * 1.) / offset;

    if (type_ == Dense) {
      if (offset > 100000) {
        if (density < 0.08) {
          ConvertToSparse();
          return AddSparse(val);
        }
      }
      return AddDense(val);
    } else {
      if (offset > 100000) {
        if (density > 0.1) {
          ConvertToDense();
          return AddDense(val);
        }
      }
      return AddSparse(val);
    }
  }

  int ConvertToSparse() {
    data_sparse_ = (int *)malloc(size_ * sizeof(int));
    int offset = max_aligned_ - min_aligned_ + 1;
    int idx = 0;
    for (int i = 0; i < offset; ++i) {
      if (bitmap::test(data_dense_, i)) {
        if (idx >= size_) {
          LOG(WARNING) << "idx [" << idx << "] size [" << size_ << "] i [" << i
                       << "] offset [" << offset << "]";
          break;
        }
        data_sparse_[idx] = i + min_aligned_;
        ++idx;
      }
    }

    if (size_ != idx) {
      LOG(ERROR) << "size [" << size_ << "] idx [" << idx << "] max_aligned_ ["
                 << max_aligned_ << "] min_aligned_ [" << min_aligned_
                 << "] max [" << max_ << "] min [" << min_ << "]";
    }
    capacity_ = size_;
    type_ = Sparse;
    free(data_dense_);
    data_dense_ = nullptr;
    return 0;
  }

  int ConvertToDense() {
    int bytes_count = -1;
    if (bitmap::create(data_dense_, bytes_count,
                       max_aligned_ - min_aligned_ + 1) != 0) {
      LOG(ERROR) << "Cannot create bitmap!";
      return -1;
    }

    for (int i = 0; i < size_; ++i) {
      int val = data_sparse_[i];
      if (val < min_aligned_ || val > max_aligned_) {
        LOG(WARNING) << "val [" << val << "] size [" << size_ << "] i [" << i
                     << "]";
        continue;
      }
      bitmap::set(data_dense_, val - min_aligned_);
    }

    type_ = Dense;
    free(data_sparse_);
    data_sparse_ = nullptr;
    return 0;
  }

  int DeleteDense(int val) {
    int pos = val - min_aligned_;
    if (pos < 0 || val > max_aligned_) {
      LOG(DEBUG) << "Cannot find [" << val << "]";
      return -1;
    }
    --size_;
    bitmap::unset(data_dense_, pos);
    return 0;
  }

  int DeleteSparse(int val) {
    int i = 0;
    for (; i < size_; ++i) {
      if (data_sparse_[i] == val) {
        break;
      }
    }

    if (i == size_) {
      LOG(DEBUG) << "Cannot find [" << val << "]";
      return -1;
    }
    for (int j = i; j < size_ - 1; ++j) {
      data_sparse_[j] = data_sparse_[j + 1];
    }

    --size_;
    return 0;
  }

  int Delete(int val) {
    if (type_ == Dense) {
      return DeleteDense(val);
    } else {
      return DeleteSparse(val);
    }
  }

  int Min() { return min_; }
  int Max() { return max_; }

  int MinAligned() { return min_aligned_; }
  int MaxAligned() { return max_aligned_; }

  int Size() { return size_; }
  NodeType Type() { return type_; }

  char *DataDense() { return data_dense_; }
  int *DataSparse() { return data_sparse_; }

  // for debug
  void MemorySize(long &dense, long &sparse) {
    if (type_ == Dense) {
      dense += (max_aligned_ - min_aligned_) / 8;
    } else {
      sparse += capacity_ * sizeof(int);
    }
  }

 private:
  int min_;
  int max_;
  int min_aligned_;
  int max_aligned_;

  NodeType type_;
  int capacity_;  // for sparse node
  int size_;
  char *data_dense_;
  int *data_sparse_;

  int n_extend_;
};

typedef struct BTreeParameters {
  uint poolsize;
  uint mainbits;
  const char *kDelim;
} BTreeParameters;

class FieldRangeIndex {
 public:
  FieldRangeIndex(std::string &path, int field_idx, enum DataType field_type,
                  BTreeParameters &bt_param, std::string &name);
  ~FieldRangeIndex();

  int Add(std::string &key, int value);

  int Delete(std::string &key, int value);

  int Search(const std::string &low, const std::string &high,
             RangeQueryResult *result);

  int Search(const std::string &tags, RangeQueryResult *result);

  bool IsNumeric() { return is_numeric_; }

  enum DataType DataType() { return data_type_; }

  char *Delim() { return kDelim_; }

  // for debug
  long ScanMemory(long &dense, long &sparse);

 private:
  BtMgr *main_mgr_;
  bool is_numeric_;
  enum DataType data_type_;
  char *kDelim_;
  std::string path_;
  std::string name_;
  pthread_rwlock_t rw_lock_;
  long add_num_ = 0;
  long delete_num_ = 0;
};

FieldRangeIndex::FieldRangeIndex(std::string &path, int field_idx,
                                 enum DataType field_type,
                                 BTreeParameters &bt_param, std::string &name)
    : path_(path), name_(name) {
  std::string main_file =
      path + std::string("/main_") + std::to_string(field_idx) + ".dis";
  remove(main_file.c_str());

  main_mgr_ = bt_mgr(const_cast<char *>(main_file.c_str()), bt_param.mainbits,
                     bt_param.poolsize);

  if (field_type == DataType::STRING || field_type == DataType::STRINGARRAY) {
    is_numeric_ = false;
  } else {
    is_numeric_ = true;
  }
  data_type_ = field_type;
  kDelim_ = const_cast<char *>(bt_param.kDelim);

  int ret = pthread_rwlock_init(&rw_lock_, nullptr);
  if (ret != 0) {
    LOG(ERROR) << "init lock failed[";
  }
}

FieldRangeIndex::~FieldRangeIndex() {
  BtDb *bt = bt_open(main_mgr_);
  BtPageSet set[1];
  uid next, page_no = LEAF_page;  // start on first page of leaves

  do {
    if ((set->latch = bt_pinlatch(bt, page_no, 1))) {
      set->page = bt_mappage(bt, set->latch);
    } else {
      LOG(ERROR) << "unable to obtain latch";
      return;
    }
    bt_lockpage(bt, BtLockRead, set->latch);
    next = bt_getid(set->page->right);

    for (uint slot = 1; slot < set->page->cnt; slot++) {
      if (!slotptr(set->page, slot)->dead) {
        BtVal *val = valptr(set->page, slot);
        Node *p_node = nullptr;
        memcpy(&p_node, val->value, sizeof(Node *));
        delete p_node;
      }
    }

    bt_unlockpage(bt, BtLockRead, set->latch);
    bt_unpinlatch(set->latch);
    page_no = next;
  } while (page_no);

  bt_close(bt);

  if (main_mgr_) {
    bt_mgrclose(main_mgr_);
    main_mgr_ = nullptr;
  }
  pthread_rwlock_destroy(&rw_lock_);
}

static int ReverseEndian(const unsigned char *in, unsigned char *out,
                         uint len) {
  for (uint i = 0; i < len; ++i) {
    out[i] = in[len - i - 1];
  }

  unsigned char N = 0x80;
  out[0] += N;
  return 0;
}

int FieldRangeIndex::Add(std::string &key, int value) {
  BtDb *bt = bt_open(main_mgr_);
  size_t key_len = key.size();
  std::vector<unsigned char> key2(key_len);

  auto InsertToBt = [&](unsigned char *key_to_add, uint key_len) {
    Node *p_node = nullptr;
    int ret = bt_findkey(bt, key_to_add, key_len, (unsigned char *)&p_node,
                         sizeof(Node *));

    if (ret < 0) {
      auto new_node = std::make_unique<Node>();
      new_node->Add(value);
      Node *raw_ptr = new_node.get();
      BTERR bterr =
          bt_insertkey(bt, key_to_add, key_len, 0,
                       static_cast<void *>(&raw_ptr), sizeof(Node *), Update);
      if (bterr) {
        LOG(ERROR) << "Error " << bt->err;
        return;
      }
      new_node.release();  // successfully inserted, release ownership
    } else {
      pthread_rwlock_wrlock(&rw_lock_);
      p_node->Add(value);
      pthread_rwlock_unlock(&rw_lock_);
    }
  };

  if (is_numeric_) {
    ReverseEndian(reinterpret_cast<const unsigned char *>(key.c_str()),
                  key2.data(), key_len);
    InsertToBt(key2.data(), key_len);
  } else {
    std::vector<char> key_s(key_len + 1);
    memcpy(key_s.data(), key.c_str(), key_len);
    key_s[key_len] = '\0';

    char *p, *k;
    k = strtok_r(key_s.data(), kDelim_, &p);
    while (k != nullptr) {
      InsertToBt(reinterpret_cast<unsigned char *>(k), strlen(k));
      k = strtok_r(nullptr, kDelim_, &p);
    }
  }

  bt_close(bt);
#ifdef PERFORMANCE_TESTING
  add_num_ += 1;
  if (add_num_ % 10000 == 0) {
    LOG(INFO) << "field index [" << name_ << "] add count: " << add_num_;
  }
#endif  // PERFORMANCE_TESTING
  return 0;
}

int FieldRangeIndex::Delete(std::string &key, int value) {
  BtDb *bt = bt_open(main_mgr_);
  size_t key_len = key.size();
  std::vector<unsigned char> key2(key_len);

  auto DeleteFromBt = [&](unsigned char *key_to_add, uint key_len) {
    Node *p_node = nullptr;
    int ret =
        bt_findkey(bt, key_to_add, key_len,
                   reinterpret_cast<unsigned char *>(&p_node), sizeof(Node *));

    if (ret < 0) {
      LOG(WARNING) << "Cannot find docid [" << value << "] in range index";
      return;
    }

    pthread_rwlock_wrlock(&rw_lock_);
    p_node->Delete(value);
    if (p_node->Size() == 0) {
      bt_deletekey(bt, key_to_add, key_len, 0);
      delete p_node;
    }
    pthread_rwlock_unlock(&rw_lock_);
  };

  if (is_numeric_) {
    ReverseEndian(reinterpret_cast<const unsigned char *>(key.c_str()),
                  key2.data(), key_len);
    DeleteFromBt(key2.data(), key_len);
  } else {
    std::string key_copy = key;
    char *p = nullptr;
    char *k = strtok_r(&key_copy[0], kDelim_, &p);
    while (k != nullptr) {
      DeleteFromBt(reinterpret_cast<unsigned char *>(k), strlen(k));
      k = strtok_r(nullptr, kDelim_, &p);
    }
  }

  bt_close(bt);

#ifdef PERFORMANCE_TESTING
  delete_num_ += 1;
  if (delete_num_ % 10000 == 0) {
    LOG(INFO) << "field index [" << name_ << "] delete count: " << delete_num_;
  }
#endif  // PERFORMANCE_TESTING

  return 0;
}

int FieldRangeIndex::Search(const std::string &lower, const std::string &upper,
                            RangeQueryResult *result) {
  if (!is_numeric_) {
    return Search(lower, result);
  }

#ifdef DEBUG
  double start = utils::getmillisecs();
#endif
  BtDb *bt = bt_open(main_mgr_);
  size_t lower_len = lower.length();
  size_t upper_len = upper.length();
  std::vector<unsigned char> key_l(lower_len);
  std::vector<unsigned char> key_u(upper_len);
  ReverseEndian(reinterpret_cast<const unsigned char *>(lower.data()),
                key_l.data(), lower_len);
  ReverseEndian(reinterpret_cast<const unsigned char *>(upper.data()),
                key_u.data(), upper_len);

  std::vector<Node *> lists;

  int min_doc = std::numeric_limits<int>::max();
  int min_aligned = std::numeric_limits<int>::max();
  int max_doc = 0;
  int max_aligned = 0;
  pthread_rwlock_rdlock(&rw_lock_);
  uint slot = bt_startkey(bt, key_l.data(), lower_len);
  while (slot) {
    BtKey *key = bt_key(bt, slot);
    BtVal *val = bt_val(bt, slot);

    if (keycmp(key, key_u.data(), upper_len) > 0) {
      break;
    }
    Node *p_node = nullptr;
    memcpy(&p_node, val->value, sizeof(Node *));
    lists.push_back(p_node);

    min_doc = std::min(min_doc, p_node->Min());
    min_aligned = std::min(min_aligned, p_node->MinAligned());
    max_doc = std::max(max_doc, p_node->Max());
    max_aligned = std::max(max_aligned, p_node->MaxAligned());

    slot = bt_nextkey(bt, slot);
  }
  bt_close(bt);

#ifdef DEBUG
  double search_bt = utils::getmillisecs();
#endif
  if (max_doc - min_doc + 1 <= 0) {
    pthread_rwlock_unlock(&rw_lock_);
    return 0;
  }

  result->SetRange(min_aligned, max_aligned);
  result->Resize();
#ifdef DEBUG
  double end_resize = utils::getmillisecs();
#endif

  auto &bitmap = result->Ref();
  int list_size = lists.size();

  int total = 0;

  int op_len = sizeof(BM_OPERATE_TYPE) * 8;
  for (int i = 0; i < list_size; ++i) {
    Node *list = lists[i];
    Node::NodeType node_type = list->Type();
    if (node_type == Node::NodeType::Dense) {
      char *data = list->DataDense();
      int min = list->MinAligned();
      int max = list->MaxAligned();

      if (min < min_aligned || max > max_aligned) {
        continue;
      }

      total += list->Size();

      BM_OPERATE_TYPE *op_data_dst =
          reinterpret_cast<BM_OPERATE_TYPE *>(bitmap);
      BM_OPERATE_TYPE *op_data_ori = reinterpret_cast<BM_OPERATE_TYPE *>(data);
      int offset = (min - min_aligned) / op_len;
      for (int j = 0; j < (max - min + 1) / op_len; ++j) {
        op_data_dst[j + offset] |= op_data_ori[j];
      }
    } else {
      int *data = list->DataSparse();
      int min = list->Min();
      int max = list->Max();
      int size = list->Size();

      if (min < min_doc || max > max_doc) {
        continue;
      }

      total += list->Size();

      for (int j = 0; j < size; ++j) {
        bitmap::set(bitmap, data[j] - min_aligned);
      }
    }
  }
  pthread_rwlock_unlock(&rw_lock_);

  result->SetDocNum(total);

#ifdef DEBUG
  double end = utils::getmillisecs();
  LOG(INFO) << "bt cost [" << search_bt - start << "], resize cost ["
            << end_resize - search_bt << "], assemble result ["
            << end - end_resize << "], total [" << end - start << "]";
#endif
  return max_doc - min_doc + 1;
}

int FieldRangeIndex::Search(const std::string &tags, RangeQueryResult *result) {
  std::vector<std::string> items = utils::split(tags, kDelim_);
  std::vector<Node *> nodes(items.size());
  int op_len = sizeof(BM_OPERATE_TYPE) * 8;
#ifdef DEBUG
  double begin = utils::getmillisecs();
#endif

  for (size_t i = 0; i < items.size(); ++i) {
    nodes[i] = nullptr;
    const std::string &item = items[i];
    const unsigned char *key_tag =
        reinterpret_cast<const unsigned char *>(item.data());

    Node *p_node = nullptr;
    BtDb *bt = bt_open(main_mgr_);
    int ret =
        bt_findkey(bt, const_cast<unsigned char *>(key_tag), item.length(),
                   reinterpret_cast<unsigned char *>(&p_node), sizeof(Node *));
    bt_close(bt);

    if (ret < 0) {
      LOG(WARNING) << "find node failed, key=" << item;
      continue;
    }
    if (p_node == nullptr) {
      LOG(ERROR) << "node is nullptr, key=" << item;
      continue;
    }
    nodes[i] = p_node;
  }
#ifdef DEBUG
  double fend = utils::getmillisecs();
#endif

  pthread_rwlock_rdlock(&rw_lock_);
  int min_doc = std::numeric_limits<int>::max();
  int max_doc = 0;
  for (Node *node : nodes) {
    if (node == nullptr || node->Size() <= 0) continue;
    min_doc = std::min(min_doc, node->MinAligned());
    max_doc = std::max(max_doc, node->MaxAligned());
  }

  if (max_doc - min_doc + 1 <= 0) {
    pthread_rwlock_unlock(&rw_lock_);
    return 0;
  }

  int total = 0;
  result->SetRange(min_doc, max_doc);
  result->Resize();
  char *&bitmap = result->Ref();
#ifdef DEBUG
  double mbegin = utils::getmillisecs();
#endif
  for (Node *node : nodes) {
    if (node == nullptr) continue;
    int min_aligned = node->MinAligned();
    int max_aligned = node->MaxAligned();
    if (node->Type() == Node::NodeType::Dense) {
      char *data = node->DataDense();
      BM_OPERATE_TYPE *op_data_dst =
          reinterpret_cast<BM_OPERATE_TYPE *>(bitmap);
      BM_OPERATE_TYPE *op_data_ori = reinterpret_cast<BM_OPERATE_TYPE *>(data);

      int offset = (min_aligned - min_doc) / op_len;
      for (int j = 0; j < (max_aligned - min_aligned + 1) / op_len; ++j) {
        op_data_dst[j + offset] |= op_data_ori[j];
      }
    } else {
      int *data = node->DataSparse();
      int size = node->Size();

      for (int j = 0; j < size; ++j) {
        bitmap::set(bitmap, data[j] - min_doc);
      }
    }
    total += node->Size();
  }
  pthread_rwlock_unlock(&rw_lock_);

  result->SetDocNum(total);

#ifdef DEBUG
  double end = utils::getmillisecs();
  LOG(INFO) << "find node cost [" << fend - begin << "], merge cost ["
            << end - mbegin << "], total [" << end - begin << "]";
#endif
  return total;
}

long FieldRangeIndex::ScanMemory(long &dense, long &sparse) {
  dense = 0;
  sparse = 0;

  BtDb *bt = bt_open(main_mgr_);
  uint slot = bt_startkey(bt, nullptr, 0);
  while (slot) {
    BtVal *val = bt_val(bt, slot);
    Node *p_node = nullptr;
    memcpy(&p_node, val->value, sizeof(Node *));

    long node_dense = 0;
    long node_sparse = 0;
    p_node->MemorySize(node_dense, node_sparse);

    dense += node_dense;
    sparse += node_sparse;

    slot = bt_nextkey(bt, slot);
  }
  bt_close(bt);

  return dense + sparse;
}

MultiFieldsRangeIndex::MultiFieldsRangeIndex(std::string &path, Table *table)
    : path_(path),
      table_(table),
      fields_(table->FieldsNum()),
      field_operate_q_(std::make_unique<FieldOperateQueue>()),
      b_running_(true) {
  std::fill(fields_.begin(), fields_.end(), nullptr);

  worker_thread_ =
      std::thread(&MultiFieldsRangeIndex::FieldOperateWorker, this);
}

MultiFieldsRangeIndex::~MultiFieldsRangeIndex() {
  b_running_ = false;
  {
    std::unique_lock<std::mutex> lock(cv_mutex_);
    cv_.notify_all();
  }
  worker_thread_.join();
  for (size_t i = 0; i < fields_.size(); i++) {
    if (fields_[i]) {
      delete fields_[i];
      fields_[i] = nullptr;
    }
  }
}

void MultiFieldsRangeIndex::FieldOperateWorker() {
  bool ret = false;
  while (b_running_ || ret) {
    FieldOperate field_op;
    ret = field_operate_q_->try_pop(field_op);

    if (!ret) {
      std::unique_lock<std::mutex> lock(cv_mutex_);
      cv_.wait_for(lock, std::chrono::seconds(1));
      continue;
    }

    int doc_id = field_op.doc_id;
    int field_id = field_op.field_id;

    auto op = field_op.type;
    if (op == FieldOperate::ADD) {
      AddDoc(doc_id, field_id);
    } else {
      DeleteDoc(doc_id, field_id, field_op.value);
    }
  }
  LOG(INFO) << "FieldOperateWorker exited!";
}

int MultiFieldsRangeIndex::Add(int docid, int field) {
  FieldRangeIndex *index = fields_[field];
  if (index == nullptr) {
    return 0;
  }
  FieldOperate field_op(FieldOperate::ADD, docid, field);

  field_operate_q_->push(std::move(field_op));

  return 0;
}

int MultiFieldsRangeIndex::Delete(int docid, int field) {
  FieldRangeIndex *index = fields_[field];
  if (index == nullptr) {
    return 0;
  }
  FieldOperate field_op(FieldOperate::DELETE, docid, field);
  table_->GetFieldRawValue(docid, field, field_op.value);

  field_operate_q_->push(std::move(field_op));

  return 0;
}

int MultiFieldsRangeIndex::AddDoc(int docid, int field) {
  FieldRangeIndex *index = fields_[field];
  if (index == nullptr) {
    return 0;
  }

  std::string key;
  int ret = table_->GetFieldRawValue(docid, field, key);
  if (ret != 0) {
    LOG(ERROR) << "get doc " << docid << " failed";
    return ret;
  }
  index->Add(key, docid);

  return 0;
}

int MultiFieldsRangeIndex::DeleteDoc(int docid, int field, std::string &key) {
  FieldRangeIndex *index = fields_[field];
  if (index == nullptr) {
    return 0;
  }

  index->Delete(key, docid);

  return 0;
}

template <typename Type>
static void AdjustBoundary(std::string &boundary, int offset) {
  static_assert(std::is_fundamental<Type>::value, "Type must be fundamental.");

  if (boundary.size() >= sizeof(Type)) {
    Type b;
    std::vector<char> vec(sizeof(b));
    memcpy(&b, boundary.data(), sizeof(b));
    b += offset;
    memcpy(vec.data(), &b, sizeof(b));
    boundary = std::string(vec.begin(), vec.end());
  }
}

int MultiFieldsRangeIndex::Search(const std::vector<FilterInfo> &origin_filters,
                                  MultiRangeQueryResults *out) {
  out->Clear();

  std::vector<FilterInfo> filters;

  for (const auto &filter : origin_filters) {
    if (filter.field < 0) {
      return -1;
    }
    FieldRangeIndex *index = fields_[filter.field];
    if (index == nullptr) {
      return -1;
    }
    if (not index->IsNumeric() && (filter.is_union == FilterOperator::And)) {
      // type is string and operator is "and", split this filter
      std::vector<std::string> items =
          utils::split(filter.lower_value, index->Delim());
      for (std::string &item : items) {
        FilterInfo f = filter;
        f.lower_value = item;
        filters.push_back(f);
      }
      continue;
    }
    filters.push_back(filter);
  }

  int fsize = filters.size();

  if (fsize == 1) {
    auto &filter = filters[0];
    RangeQueryResult result;
    FieldRangeIndex *index = fields_[filter.field];

    if (not filter.include_lower) {
      if (index->DataType() == DataType::INT) {
        AdjustBoundary<int>(filter.lower_value, 1);
      } else if (index->DataType() == DataType::LONG) {
        AdjustBoundary<long>(filter.lower_value, 1);
      }
    }

    if (not filter.include_upper) {
      if (index->DataType() == DataType::INT) {
        AdjustBoundary<int>(filter.upper_value, -1);
      } else if (index->DataType() == DataType::LONG) {
        AdjustBoundary<long>(filter.upper_value, -1);
      }
    }
    int retval = index->Search(filter.lower_value, filter.upper_value, &result);
    if (retval > 0) {
      if (filter.is_union == FilterOperator::Not) {
        result.SetNotIn(true);
      }
      out->Add(std::move(result));
    } else if (filter.is_union == FilterOperator::Not) {
      retval = -1;
    }
    // result->Output();
    return retval;
  }

  std::vector<RangeQueryResult> results;
  results.reserve(fsize);

  // record the shortest docid list
  int shortest_idx = -1, shortest = std::numeric_limits<int>::max();

  for (int i = 0; i < fsize; ++i) {
    auto &filter = filters[i];

    FieldRangeIndex *index = fields_[filter.field];
    if (index == nullptr || filter.field < 0) {
      continue;
    }

    if (not filter.include_lower) {
      if (index->DataType() == DataType::INT) {
        AdjustBoundary<int>(filter.lower_value, 1);
      } else if (index->DataType() == DataType::LONG) {
        AdjustBoundary<long>(filter.lower_value, 1);
      }
    }

    if (not filter.include_upper) {
      if (index->DataType() == DataType::INT) {
        AdjustBoundary<int>(filter.upper_value, -1);
      } else if (index->DataType() == DataType::LONG) {
        AdjustBoundary<long>(filter.upper_value, -1);
      }
    }
    RangeQueryResult result;
    int num = index->Search(filter.lower_value, filter.upper_value, &result);
    if (num < 0) {
      ;
    } else if (num == 0) {
      if (filter.is_union == FilterOperator::Not) {
        continue;
      }
      return 0;  // no intersection
    } else {
      if (filter.is_union == FilterOperator::Not) {
        result.SetNotIn(true);
        out->Add(std::move(result));
        continue;
      }
      results.emplace_back(std::move(result));

      if (shortest > num) {
        shortest = num;
        shortest_idx = results.size() - 1;
      }
    }
  }

  if (results.size() == 0) {
    if (out->Size() > 0) {
      return 1;
    }
    return -1;  // universal set
  }

  RangeQueryResult tmp;
  int count = Intersect(results, shortest_idx, &tmp);
  if (count > 0) {
    out->Add(std::move(tmp));
  }

  return count;
}

int MultiFieldsRangeIndex::Intersect(std::vector<RangeQueryResult> &results,
                                     int shortest_idx, RangeQueryResult *out) {
  int min_doc = std::numeric_limits<int>::min();
  int max_doc = std::numeric_limits<int>::max();

  int total = results[0].Size();

  // results[0].Output();

  for (size_t i = 0; i < results.size(); i++) {
    RangeQueryResult &r = results[i];

    // the maximum of the minimum(s)
    if (r.MinAligned() > min_doc) {
      min_doc = r.MinAligned();
    }
    // the minimum of the maximum(s)
    if (r.MaxAligned() < max_doc) {
      max_doc = r.MaxAligned();
    }
  }

  if (max_doc - min_doc + 1 <= 0) {
    return 0;
  }
  out->SetRange(min_doc, max_doc);
  out->Resize();

  out->SetDocNum(total);

  char *&bitmap = out->Ref();

  int op_len = sizeof(BM_OPERATE_TYPE) * 8;

  BM_OPERATE_TYPE *op_data_dst = (BM_OPERATE_TYPE *)bitmap;

  // calculate the intersection with the shortest doc chain.
  {
    char *data = results[shortest_idx].Ref();
    int min_doc_shortest = results[shortest_idx].MinAligned();
    int offset = (min_doc - min_doc_shortest) / op_len;

    BM_OPERATE_TYPE *op_data_ori = (BM_OPERATE_TYPE *)data;
    for (int j = 0; j < (max_doc - min_doc + 1) / op_len; ++j) {
      op_data_dst[j] = op_data_ori[j + offset];
    }
  }

  for (size_t i = 0; i < results.size(); ++i) {
    if (i == (size_t)shortest_idx) {
      continue;
    }
    char *data = results[i].Ref();
    BM_OPERATE_TYPE *op_data_ori = (BM_OPERATE_TYPE *)data;
    int min = results[i].MinAligned();
    int max = results[i].MaxAligned();

    if (min < min_doc) {
      int offset = (min_doc - min) / op_len;
      if (max > max_doc) {
        for (int k = 0; k < (max_doc - min_doc + 1) / op_len; ++k) {
          op_data_dst[k] &= op_data_ori[k + offset];
        }
      } else {
        for (int k = 0; k < (max - min_doc + 1) / op_len; ++k) {
          op_data_dst[k] &= op_data_ori[k + offset];
        }
      }
    } else {
      int offset = (min - min_doc) / op_len;
      if (max > max_doc) {
        for (int k = 0; k < (max_doc - min_doc + 1) / op_len; ++k) {
          op_data_dst[k + offset] &= op_data_ori[k];
        }
      } else {
        for (int k = 0; k < (max - min_doc + 1) / op_len; ++k) {
          op_data_dst[k + offset] &= op_data_ori[k];
        }
      }
    }
  }

  return total;
}

int MultiFieldsRangeIndex::AddField(int field, enum DataType field_type,
                                    std::string &field_name) {
  BTreeParameters bt_param;
  bt_param.poolsize = 1024;
  bt_param.mainbits = 16;
  bt_param.kDelim = "\001";

  FieldRangeIndex *index =
      new FieldRangeIndex(path_, field, field_type, bt_param, field_name);
  fields_[field] = index;
  return 0;
}

long MultiFieldsRangeIndex::MemorySize(long &dense, long &sparse) {
  long total = 0;
  for (const auto &field : fields_) {
    if (field == nullptr) continue;

    total += field->ScanMemory(dense, sparse);
    total += sizeof(FieldRangeIndex);
  }
  return total;
}

}  // namespace vearch

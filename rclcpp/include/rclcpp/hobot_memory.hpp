// Copyright (c) 2021 Horizon Robotics.All Rights Reserved.
//
// The material in this file is confidential and contains trade secrets
// of Horizon Robotics Inc. This is proprietary information owned by
// Horizon Robotics Inc. No part of this work may be disclosed,
// reproduced, copied, transmitted, or used in any way for any purpose,
// without the express written permission of Horizon Robotics Inc.

#ifndef RCLCPP_INCLUDE_RCLCPP_HOBOT_MEMORY_HPP_
#define RCLCPP_INCLUDE_RCLCPP_HOBOT_MEMORY_HPP_

#include <atomic>
#include <mutex>
#include <queue>

#include "hb_mem_mgr.h"
#include "rcl_interfaces/msg/hobot_memory_common.hpp"

namespace rclcpp {

using MessageHbmem = rcl_interfaces::msg::HobotMemoryCommon;


class HbMemHeader {
 public:
  int32_t get_fd(void) { return fd_; }
  void set_fd(int32_t fd) { fd_ = fd; }
  uint64_t get_time_tamp(void) { return time_stamp_; }
  void set_time_stamp(uint64_t time_stamp) { time_stamp_ = time_stamp; }
  void set_counter(uint32_t cnt) { counter_ = cnt; }
  uint32_t get_counter(void) { return counter_; }
  void dec_counter(void) { counter_--; }
  uint64_t get_index(void) { return index_; }
  void set_index(uint64_t i) { index_ = i; }

 private:
  int32_t fd_;
  std::atomic<uint32_t> counter_;
  uint64_t index_;
  uint64_t time_stamp_;
};

class HbmemBulk {
 public:
  HbmemBulk() {}
  HbmemBulk(int i, HbMemHeader *h, void *m) : index(i), header(h), message(m) {}
  ~HbmemBulk() {
    index = 0;
    header = nullptr;
    message = nullptr;
  }

  int index = 0;
  HbMemHeader *header = nullptr;
  void *message = nullptr;
};

template <typename MessageT>
class HbmemManager {
 public:
  HbmemManager(int num, int keep_last, int max_mem = 128);

  ~HbmemManager();

  int get_message(int sub_cnt, void **message, MessageHbmem *hbmem_message);

 private:
  bool bulk_available(HbmemBulk bulk);

  int bulk_num_;
  int keep_last_;
  int max_memory_available_;

  hb_mem_common_buf_t com_buf_;
  bool common_buf_alloc_ = false;

  int one_bulk_size_ = 0;

  uint64_t time_stamp_last_ = 0;
  uint64_t interval_time_ = 0;

  std::queue<HbmemBulk> bulk_unused_;
  std::queue<HbmemBulk> bulk_keep_last_;
  std::queue<HbmemBulk> bulk_using_;
};

template <typename MessageT>
class HbmemBulkManager {
 public:
  explicit HbmemBulkManager(MessageHbmem *mh);

  ~HbmemBulkManager();

  int get_message(void **message);

  int free_message();

 private:
  hb_mem_common_buf_t com_buf_;
  HbmemBulk bulk_;
};

}  // namespace rclcpp

#endif  // RCLCPP_INCLUDE_RCLCPP_HOBOT_MEMORY_HPP_

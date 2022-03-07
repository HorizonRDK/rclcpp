// Copyright (c) 2021 Horizon Robotics.All Rights Reserved.
//
// The material in this file is confidential and contains trade secrets
// of Horizon Robotics Inc. This is proprietary information owned by
// Horizon Robotics Inc. No part of this work may be disclosed,
// reproduced, copied, transmitted, or used in any way for any purpose,
// without the express written permission of Horizon Robotics Inc.

#include "rclcpp/hobot_memory.hpp"

#include <cassert>
#include <queue>

#include "rclcpp/logging.hpp"

namespace rclcpp {

class HbmemModule {
 public:
  static std::shared_ptr<HbmemModule> &Instance() {
    static std::shared_ptr<HbmemModule> processor;
    static std::once_flag init_flag;
    std::call_once(init_flag, []() {
      processor = std::shared_ptr<HbmemModule>(new HbmemModule());
    });
    return processor;
  }

  ~HbmemModule();

  int alloc(uint64_t size, hb_mem_common_buf_t &buf);

  int import(hb_mem_common_buf_t &in_buf, hb_mem_common_buf_t &out_buf);

  int free(int32_t fd);

 private:
  HbmemModule();
};

HbmemModule::~HbmemModule() { hb_mem_module_close(); }

int HbmemModule::alloc(uint64_t size, hb_mem_common_buf_t &buf) {
  int64_t flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN |
                  HB_MEM_USAGE_CACHED | HB_MEM_USAGE_PRIV_HEAP_RESERVERD |
                  HB_MEM_USAGE_MAP_INITIALIZED;
  return hb_mem_alloc_com_buf(size, flags, &buf);
}

int HbmemModule::import(hb_mem_common_buf_t &in_buf,
                        hb_mem_common_buf_t &out_buf) {
  return hb_mem_import_com_buf(&in_buf, &out_buf);
}

int HbmemModule::free(int32_t fd) { return hb_mem_free_buf(fd); }

HbmemModule::HbmemModule() { hb_mem_module_open(); }

template <typename MessageT>
HbmemManager<MessageT>::HbmemManager(int num, int keep_last, int max_mem)
    : bulk_num_(num), keep_last_(keep_last), max_memory_available_(max_mem) {
  one_bulk_size_ = sizeof(HbMemHeader) + sizeof(MessageT);
  if (bulk_num_ * one_bulk_size_ > max_memory_available_ * 1024 * 1024) {
    bulk_num_ = max_memory_available_ * 1024 * 1024 / one_bulk_size_;
  }
  assert(bulk_num_ > keep_last_);

  auto ret =
      HbmemModule::Instance()->alloc(bulk_num_ * one_bulk_size_, com_buf_);
  if (!ret) {
    common_buf_alloc_ = true;
  } else {
    RCLCPP_ERROR(rclcpp::get_logger("HbmemManager"), "alloc error");
  }

  for (int i = 0; i < bulk_num_; ++i) {
    auto bulk_addr = com_buf_.virt_addr + i * one_bulk_size_;
    auto header = reinterpret_cast<HbMemHeader *>(bulk_addr);
    auto message = bulk_addr + sizeof(HbMemHeader);
    bulk_unused_.emplace(i, header, message);
  }
}

template <typename MessageT>
HbmemManager<MessageT>::~HbmemManager() {
  if (common_buf_alloc_) {
    auto ret = HbmemModule::Instance()->free(com_buf_.fd);
    if (ret) {
      RCLCPP_ERROR(rclcpp::get_logger("HbmemManager"), "free error");
    }
  }
}

template <typename MessageT>
int HbmemManager<MessageT>::get_message(int sub_cnt, void **message,
                                        MessageHbmem *hbmem_message) {
  int result = -1;
  if (!common_buf_alloc_) {
    RCLCPP_ERROR(rclcpp::get_logger("HbmemManager"), "get_message error!");
    return result;
  }

  auto time_now =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::high_resolution_clock::now().time_since_epoch())
          .count();
  if (time_stamp_last_) {
    interval_time_ = time_now - time_stamp_last_;
  }
  time_stamp_last_ = time_now;
  if (!bulk_using_.empty()) {
    auto bulk = bulk_using_.front();
    if (bulk_available(bulk)) {
      bulk_using_.pop();
      bulk_unused_.push(bulk);
    }
  }

  if (!bulk_unused_.empty()) {
    auto bulk = bulk_unused_.front();
    bulk_unused_.pop();

    bulk.header->set_fd(com_buf_.fd);
    bulk.header->set_counter(sub_cnt);
    bulk.header->set_time_stamp(time_now);

    *message = bulk.message;

    hbmem_message->fd = com_buf_.fd;
    hbmem_message->share_id = com_buf_.share_id;
    hbmem_message->flags = com_buf_.flags;
    hbmem_message->size = com_buf_.size;
    hbmem_message->virt_addr = reinterpret_cast<uint64_t>(com_buf_.virt_addr);
    hbmem_message->phys_addr = com_buf_.phys_addr;
    hbmem_message->offset = com_buf_.offset;
    hbmem_message->index = bulk.index;

    bulk_keep_last_.push(bulk);
    result = 0;
  }

  if (bulk_keep_last_.size() > keep_last_) {
    auto bulk = bulk_keep_last_.front();
    bulk_keep_last_.pop();

    if (bulk_available(bulk)) {
      bulk_unused_.push(bulk);
    } else {
      bulk_using_.push(bulk);
    }
  }

  return result;
}

template <typename MessageT>
bool HbmemManager<MessageT>::bulk_available(HbmemBulk bulk) {
  if (bulk.index >= bulk_num_) {
    return false;
  }

  if (bulk.header->get_counter() == 0) {
    return true;
  } else {
    auto time_now =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch())
            .count();
    if ((time_now - bulk.header->get_time_tamp()) >
        interval_time_ * bulk_num_) {
      bulk.header->set_counter(0);
      return true;
    }
  }

  return false;
}

template <typename MessageT>
HbmemBulkManager<MessageT>::HbmemBulkManager(MessageHbmem *mh) {
  hb_mem_common_buf_t com_buf_recv;
  com_buf_recv.fd = mh->fd;
  com_buf_recv.share_id = mh->share_id;
  com_buf_recv.flags = mh->flags;
  com_buf_recv.size = mh->size;
  com_buf_recv.virt_addr = reinterpret_cast<uint8_t *>(mh->virt_addr);
  com_buf_recv.phys_addr = mh->phys_addr;
  com_buf_recv.offset = mh->offset;

  HbmemModule::Instance()->import(com_buf_recv, com_buf_);

  auto bulk_index = mh->index;
  auto one_bulk_size = sizeof(HbMemHeader) + sizeof(MessageT);
  auto bulk_addr = com_buf_.virt_addr + bulk_index * one_bulk_size;

  bulk_.index = bulk_index;
  bulk_.header = reinterpret_cast<HbMemHeader *>(bulk_addr);
  bulk_.message = bulk_addr + sizeof(HbMemHeader);
}

template <typename MessageT>
HbmemBulkManager<MessageT>::~HbmemBulkManager() {
  HbmemModule::Instance()->free(com_buf_.fd);
}

template <typename MessageT>
int HbmemBulkManager<MessageT>::get_message(void **message) {
  *message = bulk_.message;
  return 0;
}

template <typename MessageT>
int HbmemBulkManager<MessageT>::free_message() {
  bulk_.header->dec_counter();
  return 0;
}

}  // namespace rclcpp

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
#include <chrono>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

#include "hb_mem_mgr.h"
#include "rcl_interfaces/msg/hobot_memory_common.hpp"

namespace rclcpp {

using MessageHbmem = rcl_interfaces::msg::HobotMemoryCommon;

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

  ~HbmemModule() { hb_mem_module_close(); }

  int alloc(uint64_t size, hb_mem_common_buf_t &buf) {
    int64_t flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN |
                    HB_MEM_USAGE_CACHED | HB_MEM_USAGE_PRIV_HEAP_RESERVERD |
                    HB_MEM_USAGE_MAP_INITIALIZED;
    return hb_mem_alloc_com_buf(size, flags, &buf);
  }

  int import(hb_mem_common_buf_t &in_buf, hb_mem_common_buf_t &out_buf) {
    return hb_mem_import_com_buf(&in_buf, &out_buf);
  }
  int free(int32_t fd) { return hb_mem_free_buf(fd); }

 private:
  HbmemModule() { hb_mem_module_open(); }
};

struct HbMemPoolHeader {
  std::atomic<bool> pub_import_;
  std::atomic<uint16_t> sub_import_counter_;
};

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
  void set_receive_counter(uint16_t cnt) { sub_receive_counter_ = cnt; }
  uint16_t get_receive_counter(void) { return sub_receive_counter_; }
  void dec_receive_counter(void) { sub_receive_counter_--; }
  void inc_receive_counter(void) { sub_receive_counter_++; }

 private:
  int32_t fd_;
  std::atomic<uint32_t> counter_;
  std::atomic<uint16_t> sub_receive_counter_;
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

struct ChunkInfo {
  MessageHbmem hbmsg;
  hb_mem_common_buf_t com_buf;
  uint64_t time_stamp;
};

template <typename MessageT>
class HbmemBulksManager {
 public:
  HbmemBulksManager() {
    watcher_runing_ = true;
    mem_chunks_watcher_ =
        std::make_shared<std::thread>(&HbmemBulksManager::chunks_watcher, this);
  }

  ~HbmemBulksManager() {
    {
      std::lock_guard<std::mutex> lg(mutex_);
      for (auto &chunk : mem_chunks_) {
        auto mempool_head =
            reinterpret_cast<HbMemPoolHeader *>(chunk.second.com_buf.virt_addr);
        mempool_head->sub_import_counter_--;
        HbmemModule::Instance()->free(chunk.second.com_buf.fd);
      }
    }
    watcher_runing_ = false;
    if (mem_chunks_watcher_) {
      mem_chunks_watcher_->join();
      mem_chunks_watcher_ = nullptr;
    }
  }

  int get_message(MessageHbmem *mh, void **message) {
    auto time_now =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch())
            .count();
    {
      std::lock_guard<std::mutex> lg(mutex_);

      auto chunk = mem_chunks_.find(mh->share_id);
      if (chunk != mem_chunks_.end()) {
        auto hbmsg = chunk->second.hbmsg;
        auto com_buf = chunk->second.com_buf;
        if (!is_same_hbmem(hbmsg, *mh)) {
          RCLCPP_ERROR(rclcpp::get_logger("HbmemBulksManager"),
                       "get_message error!");
          return -1;
        } else {
          auto bulk_index = mh->index;  // 一定要使用mh的
          auto one_bulk_size = sizeof(HbMemHeader) + sizeof(MessageT);
          auto bulk_addr = com_buf.virt_addr + sizeof(HbMemPoolHeader) +
                           bulk_index * one_bulk_size;
          *message = bulk_addr + sizeof(HbMemHeader);
        }
        chunk->second.time_stamp = time_now;
      } else {
        hb_mem_common_buf_t com_buf_recv;
        com_buf_recv.fd = mh->fd;
        com_buf_recv.share_id = mh->share_id;
        com_buf_recv.flags = mh->flags;
        com_buf_recv.size = mh->size;
        com_buf_recv.virt_addr = reinterpret_cast<uint8_t *>(mh->virt_addr);
        com_buf_recv.phys_addr = mh->phys_addr;
        com_buf_recv.offset = mh->offset;

        hb_mem_common_buf_t com_buf_import;
        HbmemModule::Instance()->import(com_buf_recv, com_buf_import);
        auto mempool_head =
            reinterpret_cast<HbMemPoolHeader *>(com_buf_import.virt_addr);
        mempool_head->sub_import_counter_++;

        ChunkInfo chunk_info;
        chunk_info.hbmsg = *mh;
        chunk_info.com_buf = com_buf_import;
        chunk_info.time_stamp = time_now;
        mem_chunks_.emplace(mh->share_id, chunk_info);

        auto bulk_index = mh->index;
        auto one_bulk_size = sizeof(HbMemHeader) + sizeof(MessageT);
        auto bulk_addr = com_buf_import.virt_addr + sizeof(HbMemPoolHeader) +
                         bulk_index * one_bulk_size;
        *message = bulk_addr + sizeof(HbMemHeader);
      }
    }

    return 0;
  }

  int free_message(MessageHbmem *mh) {
    std::lock_guard<std::mutex> lg(mutex_);

    auto chunk = mem_chunks_.find(mh->share_id);
    if (chunk != mem_chunks_.end()) {
      auto hbmsg = chunk->second.hbmsg;
      auto com_buf = chunk->second.com_buf;
      if (!is_same_hbmem(hbmsg, *mh)) {
        RCLCPP_ERROR(rclcpp::get_logger("HbmemBulksManager"),
                     "free_message error!");
        return -1;
      } else {
        auto bulk_index = mh->index;  // must use the mh index
        auto one_bulk_size = sizeof(HbMemHeader) + sizeof(MessageT);
        auto bulk_addr = com_buf.virt_addr + sizeof(HbMemPoolHeader) +
                         bulk_index * one_bulk_size;
        HbMemHeader *header = reinterpret_cast<HbMemHeader *>(bulk_addr);
        header->dec_counter();
        header->inc_receive_counter();
      }
    } else {
      RCLCPP_ERROR(rclcpp::get_logger("HbmemBulksManager"),
                   "free_message error!");
      return -1;
    }

    return 0;
  }

 private:
  bool is_same_hbmem(MessageHbmem &a, MessageHbmem &b) {
    return (a.fd == b.fd) && (a.share_id == b.share_id) &&
           (a.flags == b.flags) && (a.size == b.size) &&
           (a.virt_addr == b.virt_addr) && (a.phys_addr == b.phys_addr) &&
           (a.offset == b.offset);
  };

  void chunks_watcher() {
    while (watcher_runing_) {
      {
        std::lock_guard<std::mutex> lg(mutex_);

        for (auto chunk = mem_chunks_.begin(); chunk != mem_chunks_.end();) {
          auto mempool_head = reinterpret_cast<HbMemPoolHeader *>(
              chunk->second.com_buf.virt_addr);
          bool pub_import = mempool_head->pub_import_;
          if (!pub_import) {
            mempool_head->sub_import_counter_--;
            HbmemModule::Instance()->free(chunk->second.com_buf.fd);
            mem_chunks_.erase(chunk++);
          } else {
            ++chunk;
          }
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
  }
  std::unordered_map<int, ChunkInfo> mem_chunks_;

  std::mutex mutex_;

  bool watcher_runing_ = false;
  std::shared_ptr<std::thread> mem_chunks_watcher_;
};

}  // namespace rclcpp

#include "hobot_memory.inl"

#endif  // RCLCPP_INCLUDE_RCLCPP_HOBOT_MEMORY_HPP_

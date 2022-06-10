// Copyright 2014 Open Source Robotics Foundation, Inc.
// Copyright (c) 2022，Horizon Robotics.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef RCLCPP__PUBLISHER_HBMEM_HPP_
#define RCLCPP__PUBLISHER_HBMEM_HPP_

#include <rmw/error_handling.h>
#include <rmw/rmw.h>

#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "rcl/error_handling.h"
#include "rcl/publisher.h"
#include "rcl_interfaces/msg/hobot_memory_common.hpp"
#include "rclcpp/allocator/allocator_common.hpp"
#include "rclcpp/allocator/allocator_deleter.hpp"
#include "rclcpp/detail/resolve_use_intra_process.hpp"
#include "rclcpp/experimental/intra_process_manager.hpp"
#include "rclcpp/loaned_message_hbmem.hpp"
#include "rclcpp/macros.hpp"
#include "rclcpp/node_interfaces/node_base_interface.hpp"
#include "rclcpp/publisher_base.hpp"
#include "rclcpp/publisher_options.hpp"
#include "rclcpp/type_support_decl.hpp"
#include "rclcpp/visibility_control.hpp"

namespace rclcpp {

template <typename MessageT, typename AllocatorT>
class LoanedHbmemMessage;

using MessageHbmem = rcl_interfaces::msg::HobotMemoryCommon;

/// A publisher publishes messages of any type to a topic.
template <typename MessageT, typename AllocatorT = std::allocator<void>>
class PublisherHbmem : public PublisherBase {
 public:
  using MessageAllocatorTraits =
      allocator::AllocRebind<MessageHbmem, AllocatorT>;
  using MessageAllocator = typename MessageAllocatorTraits::allocator_type;
  using MessageDeleter = allocator::Deleter<MessageAllocator, MessageHbmem>;
  using MessageUniquePtr = std::unique_ptr<MessageHbmem, MessageDeleter>;
  using MessageSharedPtr = std::shared_ptr<const MessageHbmem>;

  RCLCPP_SMART_PTR_DEFINITIONS(PublisherHbmem<MessageT, AllocatorT>)

  /// Default constructor.
  /**
   * The constructor for a PublisherHbmem is almost never called directly.
   * Instead, subscriptions should be instantiated through the function
   * rclcpp::create_publisher().
   *
   * \param[in] node_base NodeBaseInterface pointer that is used in part of the
   * setup. \param[in] topic Name of the topic to publish to. \param[in] qos QoS
   * profile for Subcription. \param[in] options options for the subscription.
   */
  PublisherHbmem(
      rclcpp::node_interfaces::NodeBaseInterface* node_base,
      const std::string& topic, const rclcpp::QoS& qos,
      const rclcpp::PublisherOptionsWithAllocator<AllocatorT>& options)
      : PublisherBase(
            node_base, topic,
            *rosidl_typesupport_cpp::get_message_type_support_handle<
                MessageHbmem>(),
            options.template to_rcl_publisher_options<MessageHbmem>(qos)),
        options_(options),
        message_allocator_(
            new MessageAllocator(*options.get_allocator().get())) {
    allocator::set_allocator_for_deleter(&message_deleter_,
                                         message_allocator_.get());

    if (options_.event_callbacks.deadline_callback) {
      this->add_event_handler(options_.event_callbacks.deadline_callback,
                              RCL_PUBLISHER_OFFERED_DEADLINE_MISSED);
    }
    if (options_.event_callbacks.liveliness_callback) {
      this->add_event_handler(options_.event_callbacks.liveliness_callback,
                              RCL_PUBLISHER_LIVELINESS_LOST);
    }
    if (options_.event_callbacks.incompatible_qos_callback) {
      this->add_event_handler(
          options_.event_callbacks.incompatible_qos_callback,
          RCL_PUBLISHER_OFFERED_INCOMPATIBLE_QOS);
    } else if (options_.use_default_callbacks) {
      // Register default callback when not specified
      try {
        this->add_event_handler(
            [this](QOSOfferedIncompatibleQoSInfo& info) {
              this->default_incompatible_qos_callback(info);
            },
            RCL_PUBLISHER_OFFERED_INCOMPATIBLE_QOS);
      } catch (UnsupportedEventTypeException& /*exc*/) {
        // pass
      }
    }

    auto qos_profile = qos.get_rmw_qos_profile();
    if (qos_profile.history == RMW_QOS_POLICY_HISTORY_KEEP_ALL) {
      throw std::runtime_error(
          "publisher hbmem histoty keep all is not supported yet");
    }
    hbmem_manager_ = std::make_shared<HbmemManager<MessageT>>(
        qos_profile.depth * 3, qos_profile.depth);
    // Setup continues in the post construction method, post_init_setup().
  }

  /// Called post construction, so that construction may continue after
  /// shared_from_this() works.
  virtual void post_init_setup(
      rclcpp::node_interfaces::NodeBaseInterface* node_base,
      const std::string& topic, const rclcpp::QoS& qos,
      const rclcpp::PublisherOptionsWithAllocator<AllocatorT>& options) {
    // Topic is unused for now.
    (void)topic;
    (void)options;

    // If needed, setup intra process communication.
    if (rclcpp::detail::resolve_use_intra_process(options_, *node_base)) {
      throw std::runtime_error(
          "publisher hbmem intra process not supported yet");
    }
  }

  virtual ~PublisherHbmem() {}

  /// Borrow a loaned ROS message from the middleware.
  /**
   * If the middleware is capable of loaning memory for a ROS message instance,
   * the loaned message will be directly allocated in the middleware.
   * If not, the message allocator of this rclcpp::PublisherHbmem instance is
   * being used.
   *
   * With a call to \sa `publish` the LoanedMessage instance is being returned
   * to the middleware or free'd accordingly to the allocator. If the message is
   * not being published but processed differently, the destructor of this class
   * will either return the message to the middleware or deallocate it via the
   * internal allocator. \sa rclcpp::LoanedMessage for details of the
   * LoanedMessage class.
   *
   * \return LoanedMessage containing memory for a ROS message of type
   * MessageHbmem
   */
  rclcpp::LoanedHbmemMessage<MessageT, AllocatorT> borrow_loaned_message() {
    return rclcpp::LoanedHbmemMessage<MessageT, AllocatorT>(
        this, this->hbmem_manager_);
  }

  /// Publish an instance of a LoanedMessage.
  /**
   * When publishing a loaned message, the memory for this ROS message will be
   * deallocated after being published. The instance of the loaned message is no
   * longer valid after this call.
   *
   * \param loaned_msg The LoanedMessage instance to be published.
   */
  void publish(rclcpp::LoanedHbmemMessage<MessageT, AllocatorT>&& loaned_msg) {
    if (!loaned_msg.is_valid()) {
      throw std::runtime_error("loaned message is not valid");
    }
    if (intra_process_is_enabled_) {
      // TODO(Karsten1987): support loaned message passed by intraprocess
      throw std::runtime_error(
          "storing loaned messages in intra process is not supported yet");
    }

    this->do_loaned_message_publish(std::move(loaned_msg.release()));
  }

  std::shared_ptr<MessageAllocator> get_allocator() const {
    return message_allocator_;
  }

 protected:
  void do_loaned_message_publish(
      std::unique_ptr<MessageHbmem, std::function<void(MessageHbmem*)>> msg) {
    auto status = rcl_publish(publisher_handle_.get(), msg.get(), nullptr);
    if (RCL_RET_PUBLISHER_INVALID == status) {
      rcl_reset_error();  // next call will reset error message if not context
      if (rcl_publisher_is_valid_except_context(publisher_handle_.get())) {
        rcl_context_t* context =
            rcl_publisher_get_context(publisher_handle_.get());
        if (nullptr != context && !rcl_context_is_valid(context)) {
          // publisher is invalid due to context being shutdown
          return;
        }
      }
    }
    if (RCL_RET_OK != status) {
      rclcpp::exceptions::throw_from_rcl_error(status,
                                               "failed to publish message");
    }
  }

  const rclcpp::PublisherOptionsWithAllocator<AllocatorT> options_;
  std::shared_ptr<MessageAllocator> message_allocator_;
  std::shared_ptr<HbmemManager<MessageT>> hbmem_manager_;
  MessageDeleter message_deleter_;
};

}  // namespace rclcpp

#endif  // RCLCPP__PUBLISHER_HBMEM_HPP_

// Copyright 2020 Open Source Robotics Foundation, Inc.
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

#ifndef CONTROLLER_MANAGER__CONTROLLER_MANAGER_HPP_
#define CONTROLLER_MANAGER__CONTROLLER_MANAGER_HPP_

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "controller_interface/controller_interface.hpp"

#include "controller_manager/controller_spec.hpp"
#include "controller_manager/visibility_control.h"
#include "controller_manager_msgs/srv/list_controllers.hpp"
#include "controller_manager_msgs/srv/list_controller_types.hpp"
#include "controller_manager_msgs/srv/load_controller.hpp"
#include "controller_manager_msgs/srv/reload_controller_libraries.hpp"
#include "controller_manager_msgs/srv/switch_controller.hpp"
#include "controller_manager_msgs/srv/unload_controller.hpp"

#include "hardware_interface/robot_hardware.hpp"

#include "pluginlib/class_loader.hpp"

#include "rclcpp/executor.hpp"
#include "rclcpp/node.hpp"

namespace controller_manager
{

class ControllerManager : public rclcpp::Node
{
public:
  static constexpr bool WAIT_FOR_ALL_RESOURCES = false;
  static constexpr double INFINITE_TIMEOUT = 0.0;

  CONTROLLER_MANAGER_PUBLIC
  ControllerManager(
    std::shared_ptr<hardware_interface::RobotHardware> hw,
    std::shared_ptr<rclcpp::Executor> executor,
    const std::string & name = "controller_manager");

  CONTROLLER_MANAGER_PUBLIC
  virtual
  ~ControllerManager() = default;

  CONTROLLER_MANAGER_PUBLIC
  controller_interface::ControllerInterfaceSharedPtr
  load_controller(
    const std::string & controller_name,
    const std::string & controller_type);

  /**
   * @brief load_controller loads a controller by name, the type must be defined in the parameter server
   */
  CONTROLLER_MANAGER_PUBLIC
  controller_interface::ControllerInterfaceSharedPtr
  load_controller(
    const std::string & controller_name);

  CONTROLLER_MANAGER_PUBLIC
  controller_interface::return_type unload_controller(
    const std::string & controller_name);

  CONTROLLER_MANAGER_PUBLIC
  std::vector<ControllerSpec> get_loaded_controllers() const;

  template<
    typename T,
    typename std::enable_if<std::is_convertible<
      T *, controller_interface::ControllerInterface *>::value, T>::type * = nullptr>
  controller_interface::ControllerInterfaceSharedPtr
  add_controller(
    std::shared_ptr<T> controller, const std::string & controller_name,
    const std::string & controller_type)
  {
    ControllerSpec controller_spec;
    controller_spec.c = controller;
    controller_spec.info.name = controller_name;
    controller_spec.info.type = controller_type;
    return add_controller_impl(controller_spec);
  }

  /**
   * @brief switch_controller Stops some controllers and others.
   * @see Documentation in controller_manager_msgs/SwitchController.srv
   */
  CONTROLLER_MANAGER_PUBLIC
  controller_interface::return_type
  switch_controller(
    const std::vector<std::string> & start_controllers,
    const std::vector<std::string> & stop_controllers,
    int strictness,
    bool start_asap = WAIT_FOR_ALL_RESOURCES,
    const rclcpp::Duration & timeout = rclcpp::Duration(0, INFINITE_TIMEOUT));

  CONTROLLER_MANAGER_PUBLIC
  controller_interface::return_type
  update();

protected:
  CONTROLLER_MANAGER_PUBLIC
  controller_interface::ControllerInterfaceSharedPtr
  add_controller_impl(const ControllerSpec & controller);

  CONTROLLER_MANAGER_PUBLIC
  void manage_switch();

  CONTROLLER_MANAGER_PUBLIC
  void stop_controllers();

  CONTROLLER_MANAGER_PUBLIC
  void start_controllers();

  CONTROLLER_MANAGER_PUBLIC
  void start_controllers_asap();

  CONTROLLER_MANAGER_PUBLIC
  void list_controllers_srv_cb(
    const std::shared_ptr<controller_manager_msgs::srv::ListControllers::Request> request,
    std::shared_ptr<controller_manager_msgs::srv::ListControllers::Response> response);

  CONTROLLER_MANAGER_PUBLIC
  void list_controller_types_srv_cb(
    const std::shared_ptr<controller_manager_msgs::srv::ListControllerTypes::Request> request,
    std::shared_ptr<controller_manager_msgs::srv::ListControllerTypes::Response> response);

  CONTROLLER_MANAGER_PUBLIC
  void load_controller_service_cb(
    const std::shared_ptr<controller_manager_msgs::srv::LoadController::Request> request,
    std::shared_ptr<controller_manager_msgs::srv::LoadController::Response> response);

  CONTROLLER_MANAGER_PUBLIC
  void reload_controller_libraries_service_cb(
    const std::shared_ptr<controller_manager_msgs::srv::ReloadControllerLibraries::Request> request,
    std::shared_ptr<controller_manager_msgs::srv::ReloadControllerLibraries::Response> response);

  CONTROLLER_MANAGER_PUBLIC
  void switch_controller_service_cb(
    const std::shared_ptr<controller_manager_msgs::srv::SwitchController::Request> request,
    std::shared_ptr<controller_manager_msgs::srv::SwitchController::Response> response);

  CONTROLLER_MANAGER_PUBLIC
  void unload_controller_service_cb(
    const std::shared_ptr<controller_manager_msgs::srv::UnloadController::Request> request,
    std::shared_ptr<controller_manager_msgs::srv::UnloadController::Response> response);

private:
  std::vector<std::string> get_controller_names();

  std::shared_ptr<hardware_interface::RobotHardware> hw_;
  std::shared_ptr<rclcpp::Executor> executor_;
  std::shared_ptr<pluginlib::ClassLoader<controller_interface::ControllerInterface>> loader_;

  /**
   * @brief The RTControllerListWrapper class wraps a double-buffered list of controllers
   * to avoid needing to lock the real-time thread when switching controllers in
   * the non-real-time thread.
   *
   * There's always an "updated" list and an "outdated" one
   * There's always an "used by rt" list and an "unused by rt" list
   *
   * The updated state changes on the switch_updated_list()
   * The rt usage state changes on the update_and_get_used_by_rt_list()
   */
  class RTControllerListWrapper
  {
// *INDENT-OFF*
  public:
// *INDENT-ON*
    /**
     * @brief update_and_get_used_by_rt_list Makes the "updated" list the "used by rt" list
     * @warning Should only be called by the RT thread, no one should modify the
     * updated list while it's being used
     * @return reference to the updated list
     */
    std::vector<ControllerSpec> & update_and_get_used_by_rt_list();

    /**
     * @brief get_unused_list Waits until the "outdated" and "unused by rt"
     * lists match and returns a reference to it
     * This referenced list can be modified safely until switch_updated_controller_list()
     * is called, at this point the RT thread may start using it at any time
     * @param guard Guard needed to make sure the caller is the only one accessing the unused by rt list
     */
    std::vector<ControllerSpec> & get_unused_list(
      const std::lock_guard<std::recursive_mutex> & guard);

    /**
     * @brief get_updated_list Returns a const reference to the most updated list,
     * @warning May or may not being used by the realtime thread, read-only reference for safety
     * @param guard Guard needed to make sure the caller is the only one accessing the unused by rt list
     */
    const std::vector<ControllerSpec> & get_updated_list(
      const std::lock_guard<std::recursive_mutex> & guard) const;

    /**
     * @brief switch_updated_list Switches the "updated" and "outdated" lists, and waits
     *  until the RT thread is using the new "updated" list.
     * @param guard Guard needed to make sure the caller is the only one accessing the unused by rt list
     */
    void switch_updated_list(const std::lock_guard<std::recursive_mutex> & guard);

    // Mutex protecting the controllers list
    // must be acquired before using any list other than the "used by rt"
    mutable std::recursive_mutex controllers_lock_;

// *INDENT-OFF*
  private:
// *INDENT-ON*
    /**
     * @brief get_other_list get the list not pointed by index
     */
    int get_other_list(int index) const;

    void wait_until_rt_not_using(
      int index,
      std::chrono::microseconds sleep_delay = std::chrono::microseconds(200)) const;

    std::vector<ControllerSpec> controllers_lists_[2];
    /// The index of the controller list with the most updated information
    int updated_controllers_index_ = 0;
    /// The index of the controllers list being used in the real-time thread.
    int used_by_realtime_controllers_index_ = -1;
  };

  RTControllerListWrapper rt_controllers_wrapper_;
  /// mutex copied from ROS1 Control, protects service callbacks
  /// not needed if we're guaranteed that the callbacks don't come from multiple threads
  std::mutex services_lock_;
  rclcpp::Service<controller_manager_msgs::srv::ListControllers>::SharedPtr
    list_controllers_service_;
  rclcpp::Service<controller_manager_msgs::srv::ListControllerTypes>::SharedPtr
    list_controller_types_service_;
  rclcpp::Service<controller_manager_msgs::srv::LoadController>::SharedPtr
    load_controller_service_;
  rclcpp::Service<controller_manager_msgs::srv::ReloadControllerLibraries>::SharedPtr
    reload_controller_libraries_service_;
  rclcpp::Service<controller_manager_msgs::srv::SwitchController>::SharedPtr
    switch_controller_service_;
  rclcpp::Service<controller_manager_msgs::srv::UnloadController>::SharedPtr
    unload_controller_service_;

  std::vector<std::string> start_request_, stop_request_;
#ifdef TODO_IMPLEMENT_RESOURCE_CHECKING
//  std::list<hardware_interface::ControllerInfo> switch_start_list_, switch_stop_list_;
#endif

  struct SwitchParams
  {
    bool do_switch = {false};
    bool started = {false};
    rclcpp::Time init_time = {rclcpp::Time::max()};

    // Switch options
    int strictness = {0};
    bool start_asap = {false};
    rclcpp::Duration timeout = rclcpp::Duration{0, 0};
  };

  SwitchParams switch_params_;
};

}  // namespace controller_manager

#endif  // CONTROLLER_MANAGER__CONTROLLER_MANAGER_HPP_

// Copyright (c) 2021 PickNik, Inc.
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
//
// Author: Jafar Abdi, Denis Stogl

#include "fake_components/generic_system.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "hardware_interface/types/hardware_interface_type_values.hpp"
#include "rcutils/logging_macros.h"

namespace fake_components
{
return_type GenericSystem::configure(const hardware_interface::HardwareInfo & info)
{
  if (configure_default(info) != return_type::OK)
  {
    return return_type::ERROR;
  }

  // check if to create fake command interface for sensor
  auto it = info_.hardware_parameters.find("fake_sensor_commands");
  if (it != info_.hardware_parameters.end())
  {
    // TODO(anyone): change this to parse_bool() (see ros2_control#339)
    fake_sensor_command_interfaces_ = it->second == "true" || it->second == "True";
  }
  else
  {
    fake_sensor_command_interfaces_ = false;
  }

  // check if there is parameter that disables commands
  // this way we simulate disconnected driver
  it = info_.hardware_parameters.find("disable_commands");
  if (it != info.hardware_parameters.end())
  {
    command_propagation_disabled_ = it->second == "true" || it->second == "True";
  }
  else
  {
    command_propagation_disabled_ = false;
  }

  // process parameters about state following
  position_state_following_offset_ = 0.0;
  custom_interface_with_following_offset_ = "";

  it = info_.hardware_parameters.find("position_state_following_offset");
  if (it != info_.hardware_parameters.end())
  {
    position_state_following_offset_ = std::stod(it->second);
    it = info_.hardware_parameters.find("custom_interface_with_following_offset");
    if (it != info_.hardware_parameters.end())
    {
      custom_interface_with_following_offset_ = it->second;
    }
  }
  // its extremlly unprobably that std::distance results int this value - therefore default
  index_custom_interface_with_following_offset_ = std::numeric_limits<size_t>::max();

  // Initialize storage for standard interfaces
  initialize_storage_vectors(joint_commands_, joint_states_, standard_interfaces_);

  // set all values without initial values to 0
  for (auto i = 0u; i < info_.joints.size(); i++)
  {
    for (auto j = 0u; j < standard_interfaces_.size(); j++)
    {
      if (std::isnan(joint_states_[j][i]))
      {
        joint_states_[j][i] = 0.0;
      }
    }
  }

  // set memory position vector to initial value
  joint_pos_commands_old_.resize(joint_commands_[POSITION_INTERFACE_INDEX].size());
  joint_pos_commands_old_ = joint_commands_[POSITION_INTERFACE_INDEX];

  // joint velocity commands to zero
  for (auto i = 0u; i < joint_commands_[VELOCITY_INTERFACE_INDEX].size(); ++i)
  {
    joint_commands_[VELOCITY_INTERFACE_INDEX][i] = 0.0;
  }

  // Search for mimic joints
  for (auto i = 0u; i < info_.joints.size(); ++i)
  {
    const auto & joint = info_.joints.at(i);
    if (joint.parameters.find("mimic") != joint.parameters.cend())
    {
      const auto mimicked_joint_it = std::find_if(
        info_.joints.begin(), info_.joints.end(),
        [&mimicked_joint =
           joint.parameters.at("mimic")](const hardware_interface::ComponentInfo & joint_info) {
          return joint_info.name == mimicked_joint;
        });
      if (mimicked_joint_it == info_.joints.cend())
      {
        throw std::runtime_error(
          std::string("Mimicked joint '") + joint.parameters.at("mimic") + "' not found");
      }
      MimicJoint mimic_joint;
      mimic_joint.joint_index = i;
      mimic_joint.mimicked_joint_index = std::distance(info_.joints.begin(), mimicked_joint_it);
      auto param_it = joint.parameters.find("multiplier");
      if (param_it != joint.parameters.end())
      {
        mimic_joint.multiplier = std::stod(joint.parameters.at("multiplier"));
      }
      mimic_joints_.push_back(mimic_joint);
    }
  }
  // search for non-standard joint interfaces
  for (const auto & joint : info_.joints)
  {
    for (const auto & interface : joint.command_interfaces)
    {
      // add to list if non-standard interface
      if (
        std::find(standard_interfaces_.begin(), standard_interfaces_.end(), interface.name) ==
        standard_interfaces_.end())
      {
        if (
          std::find(other_interfaces_.begin(), other_interfaces_.end(), interface.name) ==
          other_interfaces_.end())
        {
          other_interfaces_.emplace_back(interface.name);
        }
      }
    }
    for (const auto & interface : joint.state_interfaces)
    {
      // add to list if non-standard interface
      if (
        std::find(standard_interfaces_.begin(), standard_interfaces_.end(), interface.name) ==
        standard_interfaces_.end())
      {
        if (
          std::find(other_interfaces_.begin(), other_interfaces_.end(), interface.name) ==
          other_interfaces_.end())
        {
          other_interfaces_.emplace_back(interface.name);
        }
      }
    }
  }
  // Initialize storage for non-standard interfaces
  initialize_storage_vectors(other_commands_, other_states_, other_interfaces_);

  // when following offset is used on custom interface then find its index
  if (!custom_interface_with_following_offset_.empty())
  {
    auto if_it = std::find(
      other_interfaces_.begin(), other_interfaces_.end(), custom_interface_with_following_offset_);
    if (if_it != other_interfaces_.end())
    {
      index_custom_interface_with_following_offset_ =
        std::distance(other_interfaces_.begin(), if_it);
      RCUTILS_LOG_INFO_NAMED(
        "fake_generic_system", "Custom interface with following offset '%s' found at index: %zu.",
        custom_interface_with_following_offset_.c_str(),
        index_custom_interface_with_following_offset_);
    }
    else
    {
      RCUTILS_LOG_WARN_NAMED(
        "fake_generic_system",
        "Custom interface with following offset '%s' does not exist. Offset will not be applied",
        custom_interface_with_following_offset_.c_str());
    }
  }

  for (const auto & sensor : info_.sensors)
  {
    for (const auto & interface : sensor.state_interfaces)
    {
      if (
        std::find(sensor_interfaces_.begin(), sensor_interfaces_.end(), interface.name) ==
        sensor_interfaces_.end())
      {
        sensor_interfaces_.emplace_back(interface.name);
      }
    }
  }
  initialize_storage_vectors(sensor_fake_commands_, sensor_states_, sensor_interfaces_);

  stop_modes_ = {StoppingInterface::NONE, StoppingInterface::NONE, StoppingInterface::NONE,
                 StoppingInterface::NONE, StoppingInterface::NONE, StoppingInterface::NONE};
  start_modes_ = {hardware_interface::HW_IF_POSITION, hardware_interface::HW_IF_POSITION,
                  hardware_interface::HW_IF_POSITION, hardware_interface::HW_IF_POSITION,
                  hardware_interface::HW_IF_POSITION, hardware_interface::HW_IF_POSITION};
  position_controller_running_ = false;
  velocity_controller_running_ = false;
  begin = std::chrono::system_clock::now();

  status_ = hardware_interface::status::CONFIGURED;
  return hardware_interface::return_type::OK;
}

std::vector<hardware_interface::StateInterface> GenericSystem::export_state_interfaces()
{
  std::vector<hardware_interface::StateInterface> state_interfaces;

  // Joints' state interfaces
  for (auto i = 0u; i < info_.joints.size(); i++)
  {
    const auto & joint = info_.joints[i];
    for (const auto & interface : joint.state_interfaces)
    {
      // Add interface: if not in the standard list than use "other" interface list
      if (!get_interface(
            joint.name, standard_interfaces_, interface.name, i, joint_states_, state_interfaces))
      {
        if (!get_interface(
              joint.name, other_interfaces_, interface.name, i, other_states_, state_interfaces))
        {
          throw std::runtime_error(
            "Interface is not found in the standard nor other list. "
            "This should never happen!");
        }
      }
    }
  }

  // Sensor state interfaces
  for (auto i = 0u; i < info_.sensors.size(); i++)
  {
    const auto & sensor = info_.sensors[i];
    for (const auto & interface : sensor.state_interfaces)
    {
      if (!get_interface(
            sensor.name, sensor_interfaces_, interface.name, i, sensor_states_, state_interfaces))
      {
        throw std::runtime_error(
          "Interface is not found in the standard nor other list. "
          "This should never happen!");
      }
    }
  }

  return state_interfaces;
}

std::vector<hardware_interface::CommandInterface> GenericSystem::export_command_interfaces()
{
  std::vector<hardware_interface::CommandInterface> command_interfaces;

  // Joints' state interfaces
  for (auto i = 0u; i < info_.joints.size(); i++)
  {
    const auto & joint = info_.joints[i];
    for (const auto & interface : joint.command_interfaces)
    {
      // Add interface: if not in the standard list than use "other" interface list
      if (!get_interface(
            joint.name, standard_interfaces_, interface.name, i, joint_commands_,
            command_interfaces))
      {
        if (!get_interface(
              joint.name, other_interfaces_, interface.name, i, other_commands_,
              command_interfaces))
        {
          throw std::runtime_error(
            "Interface is not found in the standard nor other list. "
            "This should never happen!");
        }
      }
    }
  }

  // Fake sensor command interfaces
  if (fake_sensor_command_interfaces_)
  {
    for (auto i = 0u; i < info_.sensors.size(); i++)
    {
      const auto & sensor = info_.sensors[i];
      for (const auto & interface : sensor.state_interfaces)
      {
        if (!get_interface(
              sensor.name, sensor_interfaces_, interface.name, i, sensor_fake_commands_,
              command_interfaces))
        {
          throw std::runtime_error(
            "Interface is not found in the standard nor other list. "
            "This should never happen!");
        }
      }
    }
  }

  return command_interfaces;
}

return_type GenericSystem::prepare_command_mode_switch(
  const std::vector<std::string> & start_interfaces,
  const std::vector<std::string> & stop_interfaces)
{
  hardware_interface::return_type ret_val = hardware_interface::return_type::OK;

  start_modes_.clear();
  stop_modes_.clear();

  // Starting interfaces
  // add start interface per joint in tmp var for later check
  for (const auto & key : start_interfaces)
  {
    for (auto i = 0u; i < info_.joints.size(); i++)
    {
      if (key == info_.joints[i].name + "/" + hardware_interface::HW_IF_POSITION)
      {
        start_modes_.push_back(hardware_interface::HW_IF_POSITION);
      }
      if (key == info_.joints[i].name + "/" + hardware_interface::HW_IF_VELOCITY)
      {
        start_modes_.push_back(hardware_interface::HW_IF_VELOCITY);
      }
    }
  }
  // set new mode to all interfaces at the same time
  if (start_modes_.size() != 0 && start_modes_.size() != info_.joints.size())
  {
    ret_val = hardware_interface::return_type::ERROR;
  }

  // all start interfaces must be the same - can't mix position and velocity control
  if (
    start_modes_.size() != 0 &&
    !std::equal(start_modes_.begin() + 1, start_modes_.end(), start_modes_.begin()))
  {
    ret_val = hardware_interface::return_type::ERROR;
  }

  // Stopping interfaces
  // add stop interface per joint in tmp var for later check
  for (const auto & key : stop_interfaces)
  {
    for (auto i = 0u; i < info_.joints.size(); i++)
    {
      if (key == info_.joints[i].name + "/" + hardware_interface::HW_IF_POSITION)
      {
        stop_modes_.push_back(StoppingInterface::STOP_POSITION);
      }
      if (key == info_.joints[i].name + "/" + hardware_interface::HW_IF_VELOCITY)
      {
        stop_modes_.push_back(StoppingInterface::STOP_VELOCITY);
      }
    }
  }
  // stop all interfaces at the same time
  if (
    stop_modes_.size() != 0 &&
    (stop_modes_.size() != info_.joints.size() ||
     !std::equal(stop_modes_.begin() + 1, stop_modes_.end(), stop_modes_.begin())))
  {
    ret_val = hardware_interface::return_type::ERROR;
  }

  return ret_val;
}

return_type GenericSystem::perform_command_mode_switch(
  const std::vector<std::string> & /*start_interfaces*/,
  const std::vector<std::string> & /*stop_interfaces*/)
{
  hardware_interface::return_type ret_val = hardware_interface::return_type::OK;

  position_controller_running_ = false;
  velocity_controller_running_ = false;

  if (
    start_modes_.size() != 0 &&
    std::find(start_modes_.begin(), start_modes_.end(), hardware_interface::HW_IF_POSITION) !=
      start_modes_.end())
  {
    for (size_t i = 0; i < joint_commands_[POSITION_INTERFACE_INDEX].size(); ++i)
    {
      joint_commands_[POSITION_INTERFACE_INDEX][i] = joint_states_[POSITION_INTERFACE_INDEX][i];
    }
    position_controller_running_ = true;
  }
  else if (
    start_modes_.size() != 0 &&
    std::find(start_modes_.begin(), start_modes_.end(), hardware_interface::HW_IF_VELOCITY) !=
      start_modes_.end())
  {
    for (size_t i = 0; i < joint_commands_[VELOCITY_INTERFACE_INDEX].size(); ++i)
    {
      joint_commands_[VELOCITY_INTERFACE_INDEX][i] = 0.0;
    }
    velocity_controller_running_ = true;
  }
  return ret_val;
}

return_type GenericSystem::read()
{
  std::chrono::system_clock::time_point begin_last = begin;
  begin = std::chrono::system_clock::now();
  period_ =
    std::chrono::duration_cast<std::chrono::milliseconds>(begin - begin_last).count() / 1000.0;

  // apply offset to positions only
  for (size_t j = 0; j < joint_states_[POSITION_INTERFACE_INDEX].size(); ++j)
  {
    if (
      !std::isnan(joint_commands_[POSITION_INTERFACE_INDEX][j]) && !command_propagation_disabled_ &&
      position_controller_running_)
    {
      joint_states_[POSITION_INTERFACE_INDEX][j] =
        joint_commands_[POSITION_INTERFACE_INDEX][j] +
        (custom_interface_with_following_offset_.empty() ? position_state_following_offset_ : 0.0);

      if (standard_interfaces_.size() > 1)
        joint_states_[VELOCITY_INTERFACE_INDEX][j] =
          (joint_commands_[POSITION_INTERFACE_INDEX][j] - joint_pos_commands_old_[j]) / period_;
    }
  }

  // velocity
  for (size_t j = 0; j < joint_commands_[VELOCITY_INTERFACE_INDEX].size(); ++j)
  {
    if (
      !std::isnan(joint_commands_[VELOCITY_INTERFACE_INDEX][j]) && !command_propagation_disabled_ &&
      velocity_controller_running_)
    {
      joint_states_[POSITION_INTERFACE_INDEX][j] +=
        joint_commands_[VELOCITY_INTERFACE_INDEX][j] * period_;

      joint_states_[VELOCITY_INTERFACE_INDEX][j] = joint_commands_[VELOCITY_INTERFACE_INDEX][j];

      joint_commands_[POSITION_INTERFACE_INDEX][j] = joint_states_[POSITION_INTERFACE_INDEX][j];
    }
  }

  // remember old value of position
  joint_pos_commands_old_ = joint_commands_[POSITION_INTERFACE_INDEX];

  // do loopback on all other interfaces - starts from 1 because 0 index is position interface
  for (size_t i = 2; i < joint_states_.size(); ++i)
  {
    for (size_t j = 0; j < joint_states_[i].size(); ++j)
    {
      if (!std::isnan(joint_commands_[i][j]))
      {
        joint_states_[i][j] = joint_commands_[i][j];
      }
    }
  }
  for (const auto & mimic_joint : mimic_joints_)
  {
    for (auto i = 0u; i < joint_states_.size(); ++i)
    {
      joint_states_[i][mimic_joint.joint_index] =
        mimic_joint.multiplier * joint_states_[i][mimic_joint.mimicked_joint_index];
    }
  }

  for (size_t i = 0; i < other_states_.size(); ++i)
  {
    for (size_t j = 0; j < other_states_[i].size(); ++j)
    {
      if (
        i == index_custom_interface_with_following_offset_ &&
        !std::isnan(joint_commands_[POSITION_INTERFACE_INDEX][j]))
      {
        other_states_[i][j] =
          joint_commands_[POSITION_INTERFACE_INDEX][j] + position_state_following_offset_;
      }
      else if (!std::isnan(other_commands_[i][j]))
      {
        other_states_[i][j] = other_commands_[i][j];
      }
    }
  }

  if (fake_sensor_command_interfaces_)
  {
    for (size_t i = 0; i < sensor_states_.size(); ++i)
    {
      for (size_t j = 0; j < sensor_states_[i].size(); ++j)
      {
        if (!std::isnan(sensor_fake_commands_[i][j]))
        {
          sensor_states_[i][j] = sensor_fake_commands_[i][j];
        }
      }
    }
  }
  return return_type::OK;
}

// Private methods
template <typename HandleType>
bool GenericSystem::get_interface(
  const std::string & name, const std::vector<std::string> & interface_list,
  const std::string & interface_name, const size_t vector_index,
  std::vector<std::vector<double>> & values, std::vector<HandleType> & interfaces)
{
  auto it = std::find(interface_list.begin(), interface_list.end(), interface_name);
  if (it != interface_list.end())
  {
    auto j = std::distance(interface_list.begin(), it);
    interfaces.emplace_back(name, *it, &values[j][vector_index]);
    return true;
  }
  return false;
}

void GenericSystem::initialize_storage_vectors(
  std::vector<std::vector<double>> & commands, std::vector<std::vector<double>> & states,
  const std::vector<std::string> & interfaces)
{
  // Initialize storage for all joints, regardless of their existence
  commands.resize(interfaces.size());
  states.resize(interfaces.size());
  for (auto i = 0u; i < interfaces.size(); i++)
  {
    commands[i].resize(info_.joints.size(), std::numeric_limits<double>::quiet_NaN());
    states[i].resize(info_.joints.size(), std::numeric_limits<double>::quiet_NaN());
  }

  // Initialize with values from URDF
  for (auto i = 0u; i < info_.joints.size(); i++)
  {
    const auto & joint = info_.joints[i];
    for (auto j = 0u; j < interfaces.size(); j++)
    {
      auto it = joint.parameters.find("initial_" + interfaces[j]);
      if (it != joint.parameters.end())
      {
        states[j][i] = std::stod(it->second);
      }
    }
  }
}

}  // namespace fake_components

#include "pluginlib/class_list_macros.hpp"
PLUGINLIB_EXPORT_CLASS(fake_components::GenericSystem, hardware_interface::SystemInterface)

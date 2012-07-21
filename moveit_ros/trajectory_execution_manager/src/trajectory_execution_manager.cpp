/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2012, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/* Author: Ioan Sucan */

#include "trajectory_execution_manager/trajectory_execution_manager.h"

namespace trajectory_execution_manager
{

TrajectoryExecutionManager::TrajectoryExecutionManager(const planning_models::KinematicModelConstPtr &kmodel) : 
  kinematic_model_(kmodel), node_handle_("~")
{
  if (!node_handle_.getParam("moveit_manage_controllers", manage_controllers_))
    manage_controllers_ = false;
  initialize();
}

TrajectoryExecutionManager::TrajectoryExecutionManager(const planning_models::KinematicModelConstPtr &kmodel, bool manage_controllers) :
  kinematic_model_(kmodel), node_handle_("~"), manage_controllers_(manage_controllers)
{
  initialize();
}

void TrajectoryExecutionManager::initialize(void)
{
  verbose_ = false;
  execution_complete_ = true;
  current_context_ = -1;
  last_execution_status_ = moveit_controller_manager::ExecutionStatus::SUCCEEDED;
  
  // load the controller manager plugin
  try
  {
    controller_manager_loader_.reset(new pluginlib::ClassLoader<moveit_controller_manager::MoveItControllerManager>("moveit_controller_manager", "moveit_controller_manager::MoveItControllerManager"));
  }
  catch(pluginlib::PluginlibException& ex)
  {
    ROS_FATAL_STREAM("Exception while creating controller manager plugin loader: " << ex.what());
    return;
  }

  std::string controller;
  if (!node_handle_.getParam("moveit_controller_manager", controller))
  {
    const std::vector<std::string> &classes = controller_manager_loader_->getDeclaredClasses();
    if (classes.size() == 1)
    {
      controller = classes[0];
      ROS_WARN("Parameter '~controller_manager' is not specified but only one matching plugin was found: '%s'. Using that one.", controller.c_str());
    }
    else
      ROS_FATAL("Parameter '~controller_manager' not specified. This is needed to identify the plugin to use for interacting with controllers. No paths can be executed.");
  }
  
  try
  {
    controller_manager_.reset(controller_manager_loader_->createUnmanagedInstance(controller));
  }
  catch(pluginlib::PluginlibException& ex)
  {
    ROS_FATAL_STREAM("Exception while loading controller manager '" << controller << "': " << ex.what());
  } 
  
  // other configuration steps
  reloadControllerInformation();
  
  event_topic_subscriber_ = root_node_handle_.subscribe("trajectory_execution_event", 100, &TrajectoryExecutionManager::receiveEvent, this);
  
  if (manage_controllers_)
    ROS_INFO("Trajectory execution is managing controllers");
  else
    ROS_INFO("Trajectory execution is not managing controllers");
}

bool TrajectoryExecutionManager::isManagingControllers(void) const
{
  return manage_controllers_;
}

const moveit_controller_manager::MoveItControllerManagerPtr& TrajectoryExecutionManager::getControllerManager(void) const
{
  return controller_manager_;
}

void TrajectoryExecutionManager::processEvent(const std::string &event)
{
  if (event == "stop")
    stopExecution(true);
  else
    ROS_WARN_STREAM("Unknown event type: '" << event << "'");
}

void TrajectoryExecutionManager::receiveEvent(const std_msgs::StringConstPtr &event)
{
  ROS_INFO_STREAM("Received event '" << event->data << "'");
  processEvent(event->data);
}

bool TrajectoryExecutionManager::push(const moveit_msgs::RobotTrajectory &trajectory, const std::string &controller)
{
  if (controller.empty())
    return push(trajectory, std::vector<std::string>());
  else    
    return push(trajectory, std::vector<std::string>(1, controller));    
}  

bool TrajectoryExecutionManager::push(const trajectory_msgs::JointTrajectory &trajectory, const std::string &controller)
{
  if (controller.empty())
    return push(trajectory, std::vector<std::string>());
  else    
    return push(trajectory, std::vector<std::string>(1, controller));
}

bool TrajectoryExecutionManager::push(const trajectory_msgs::JointTrajectory &trajectory, const std::vector<std::string> &controllers)
{
  moveit_msgs::RobotTrajectory traj;
  traj.joint_trajectory = trajectory;
  return push(traj, controllers);
}

bool TrajectoryExecutionManager::push(const moveit_msgs::RobotTrajectory &trajectory, const std::vector<std::string> &controllers)
{
  if (!execution_complete_)
  {
    ROS_ERROR("Cannot push a new trajectory while another is being executed");
    return false;
  }
  
  TrajectoryExecutionContext context;
  if (configure(context, trajectory, controllers))
  {
    if (verbose_)
    {
      std::stringstream ss;
      ss << "Pushed trajectory for execution using controllers [ ";
      for (std::size_t i = 0 ; i < context.controllers_.size() ; ++i)
        ss << context.controllers_[i] << " ";
      ss << "]:" << std::endl;
      for (std::size_t i = 0 ; i < context.trajectory_parts_.size() ; ++i)
        ss << context.trajectory_parts_[i] << std::endl;
      ROS_INFO("%s", ss.str().c_str());
    }
    trajectories_.push_back(context);
    return true;
  }
  return false;
}

void TrajectoryExecutionManager::reloadControllerInformation(void)
{
  known_controllers_.clear();
  if (controller_manager_)
  {
    std::vector<std::string> names;
    controller_manager_->getControllersList(names);
    for (std::size_t i = 0 ; i < names.size() ; ++i)
    {
      std::vector<std::string> joints;
      controller_manager_->getControllerJoints(names[i], joints);
      ControllerInformation ci;
      ci.name_ = names[i];
      ci.joints_.insert(joints.begin(), joints.end());
      known_controllers_[ci.name_] = ci;
    }

    for (std::map<std::string, ControllerInformation>::iterator it = known_controllers_.begin() ; it != known_controllers_.end() ; ++it)
      for (std::map<std::string, ControllerInformation>::iterator jt = known_controllers_.begin() ; jt != known_controllers_.end() ; ++jt)
        if (it != jt)
        {
          std::vector<std::string> intersect;
          std::set_intersection(it->second.joints_.begin(), it->second.joints_.end(),
                                jt->second.joints_.begin(), jt->second.joints_.end(),
                                std::back_inserter(intersect)); 
          if (!intersect.empty())
          {
            it->second.overlapping_controllers_.insert(jt->first);
            jt->second.overlapping_controllers_.insert(it->first);
          }
        }
  }
}

void TrajectoryExecutionManager::updateControllerState(const std::string &controller, const ros::Duration &age)
{
  std::map<std::string, ControllerInformation>::iterator it = known_controllers_.find(controller);
  if (it != known_controllers_.end())
    updateControllerState(it->second, age);
  else
    ROS_ERROR("Controller '%s' is not known.", controller.c_str());
}

void TrajectoryExecutionManager::updateControllerState(ControllerInformation &ci, const ros::Duration &age)
{
  if (ros::Time::now() - ci.last_update_ >= age)
    if (controller_manager_)
    {
      ci.state_ = controller_manager_->getControllerState(ci.name_);
      ci.last_update_ = ros::Time::now();
    }
}

void TrajectoryExecutionManager::updateControllersState(const ros::Duration &age)
{
  for (std::map<std::string, ControllerInformation>::iterator it = known_controllers_.begin() ; it != known_controllers_.end() ; ++it)
    updateControllerState(it->second, age);
}

bool TrajectoryExecutionManager::checkControllerCombination(std::vector<std::string> &selected, const std::set<std::string> &actuated_joints)
{
  std::set<std::string> combined_joints;
  for (std::size_t i = 0 ; i < selected.size() ; ++i)
  {
    const ControllerInformation &ci = known_controllers_[selected[i]];
    combined_joints.insert(ci.joints_.begin(), ci.joints_.end());
  }  

  if (verbose_)
  {
    std::stringstream ss, saj, sac;
    for (std::size_t i = 0 ; i < selected.size() ; ++i)
      ss << selected[i] << " ";
    for (std::set<std::string>::const_iterator it = actuated_joints.begin() ; it != actuated_joints.end() ; ++it)
      saj << *it << " ";
    for (std::set<std::string>::const_iterator it = combined_joints.begin() ; it != combined_joints.end() ; ++it)
      sac << *it << " ";
    ROS_INFO("Checking if controllers [ %s] operating on joints [ %s] cover joints [ %s]", ss.str().c_str(), sac.str().c_str(), saj.str().c_str());
  }
  
  return std::includes(combined_joints.begin(), combined_joints.end(),
                       actuated_joints.begin(), actuated_joints.end());
}

void TrajectoryExecutionManager::generateControllerCombination(std::size_t start_index, std::size_t controller_count,
                                                               const std::vector<std::string> &available_controllers, 
                                                               std::vector<std::string> &selected_controllers,
                                                               std::vector< std::vector<std::string> > &selected_options,
                                                               const std::set<std::string> &actuated_joints)
{
  if (selected_controllers.size() == controller_count)
  {
    if (checkControllerCombination(selected_controllers, actuated_joints))
      selected_options.push_back(selected_controllers);
    return;
  }
  
  for (std::size_t i = start_index ; i < available_controllers.size() ; ++i)
  {
    bool overlap = false;
    const ControllerInformation &ci = known_controllers_[available_controllers[i]];
    for (std::size_t j = 0 ; j < selected_controllers.size() && !overlap ; ++j)
    {
      if (ci.overlapping_controllers_.find(selected_controllers[j]) != ci.overlapping_controllers_.end())
        overlap = true;
    }
    if (overlap)
      continue;
    selected_controllers.push_back(available_controllers[i]);
    generateControllerCombination(i + 1, controller_count, available_controllers, selected_controllers, selected_options, actuated_joints);
    selected_controllers.pop_back();
  }
}

struct OrderPotentialControllerCombination
{
  bool operator()(const std::size_t a, const std::size_t b) const
  {  
    // preference is given to controllers marked as default
    if (nrdefault[a] > nrdefault[b])
      return true;
    if (nrdefault[a] < nrdefault[b])
      return false;
    
    // and then to ones that operate on fewer joints
    if (nrjoints[a] < nrjoints[b])
      return true;
    if (nrjoints[a] > nrjoints[b])
      return false;
    
    // and then to active ones
    if (nractive[a] < nractive[b])
      return true;
    if (nractive[a] > nractive[b])
      return false;
    
    return false;
  }
  
  std::vector< std::vector<std::string> > selected_options;
  std::vector<std::size_t> nrdefault;
  std::vector<std::size_t> nrjoints;
  std::vector<std::size_t> nractive;
};

bool TrajectoryExecutionManager::findControllers(const std::set<std::string> &actuated_joints, std::size_t controller_count, const std::vector<std::string> &available_controllers, std::vector<std::string> &selected_controllers)
{
  // generate all combinations of controller_count controllers that operate on disjoint sets of joints 
  std::vector<std::string> work_area;
  OrderPotentialControllerCombination order; 
  std::vector< std::vector<std::string> > &selected_options = order.selected_options;
  generateControllerCombination(0, controller_count, available_controllers, work_area, selected_options, actuated_joints);

  if (verbose_)
  {
    std::stringstream saj;
    std::stringstream sac;
    for (std::size_t i = 0 ; i < available_controllers.size() ; ++i)
      sac << available_controllers[i] << " ";
    for (std::set<std::string>::const_iterator it = actuated_joints.begin() ; it != actuated_joints.end() ; ++it)
      saj << *it << " ";
    ROS_INFO("Looking for %lu controllers among [ %s] that cover joints [ %s]. Found %ld options.", controller_count, sac.str().c_str(), saj.str().c_str(), selected_options.size());
  }
  
  // if none was found, this is a problem
  if (selected_options.empty())
    return false;
  
  // if only one was found, return it
  if (selected_options.size() == 1)
  {
    selected_controllers.swap(selected_options[0]);
    return true;
  }

  // if more options were found, evaluate them all and return the best one

  // count how many default controllers are used in each reported option, and how many joints are actuated in total by the selected controllers,
  // to use that in the ranking of the options
  order.nrdefault.resize(selected_options.size(), 0);
  order.nrjoints.resize(selected_options.size(), 0);
  order.nractive.resize(selected_options.size(), 0);
  for (std::size_t i = 0 ; i < selected_options.size() ; ++i)
  {
    for (std::size_t k = 0 ; k < selected_options[i].size() ; ++k)
    {
      const ControllerInformation &ci = known_controllers_[selected_options[i][k]];
      if (ci.state_.default_)
        order.nrdefault[i]++;
      if (ci.state_.active_)
        order.nractive[i]++;
      order.nrjoints[i] += ci.joints_.size();
    }
  } 

  // define a bijection to compute the raking of the found options
  std::vector<std::size_t> bijection(selected_options.size(), 0);
  for (std::size_t i = 0 ; i < selected_options.size() ; ++i)
    bijection[i] = i;

  // sort the options
  std::sort(bijection.begin(), bijection.end(), order);
  
  // depending on whether we are allowed to load & unload controllers, 
  // we have different preference on deciding between options
  if (!manage_controllers_)
  {
    // if we can't load different options at will, just choose one that is already loaded
    for (std::size_t i = 0 ; i < selected_options.size() ; ++i)
      if (areControllersActive(selected_options[bijection[i]]))
      {
        selected_controllers.swap(selected_options[bijection[i]]);
        return true;
      }
  }
  
  // otherwise, just use the first valid option
  selected_controllers.swap(selected_options[bijection[0]]);
  return true;
}


bool TrajectoryExecutionManager::areControllersActive(const std::vector<std::string> &controllers)
{
  static const ros::Duration default_age(1.0);
  for (std::size_t i = 0 ; i < controllers.size() ; ++i)
  {
    updateControllerState(controllers[i], default_age);
    std::map<std::string, ControllerInformation>::iterator it = known_controllers_.find(controllers[i]);
    if (it == known_controllers_.end() || !it->second.state_.active_)
      return false;
  }
  return true;
}

bool TrajectoryExecutionManager::selectControllers(const std::set<std::string> &actuated_joints, const std::vector<std::string> &available_controllers, std::vector<std::string> &selected_controllers)
{
  for (std::size_t i = 1 ; i <= available_controllers.size() ; ++i)
    if (findControllers(actuated_joints, i, available_controllers, selected_controllers))
    {
      // if we are not managing controllers, prefer to use active controllers even if there are more of them
      if (!manage_controllers_ && !areControllersActive(selected_controllers))
      {
        std::vector<std::string> other_option;
        for (std::size_t j = i + 1 ; j <= available_controllers.size() ; ++j)
          if (findControllers(actuated_joints, j, available_controllers, other_option))
          {
            if (areControllersActive(other_option))
            {
              selected_controllers = other_option;
              break;
            }
          }
      }
      return true;
    }
  return false;
}

bool TrajectoryExecutionManager::distributeTrajectory(const moveit_msgs::RobotTrajectory &trajectory, const std::vector<std::string> &controllers, std::vector<moveit_msgs::RobotTrajectory> &parts)
{
  parts.clear();
  parts.resize(controllers.size());
  
  std::set<std::string> actuated_joints_mdof;
  actuated_joints_mdof.insert(trajectory.multi_dof_joint_trajectory.joint_names.begin(),
                              trajectory.multi_dof_joint_trajectory.joint_names.end());
  std::set<std::string> actuated_joints_single;
  actuated_joints_single.insert(trajectory.joint_trajectory.joint_names.begin(),
                                trajectory.joint_trajectory.joint_names.end());
  
  for (std::size_t i = 0 ; i < controllers.size() ; ++i)
  {
    std::map<std::string, ControllerInformation>::iterator it = known_controllers_.find(controllers[i]);
    if (it == known_controllers_.end())
    {
      ROS_ERROR_STREAM("Controller " << controllers[i] << " not found.");
      return false;
    }
    std::vector<std::string> intersect_mdof;
    std::set_intersection(it->second.joints_.begin(), it->second.joints_.end(),
                          actuated_joints_mdof.begin(), actuated_joints_mdof.end(),
                          std::back_inserter(intersect_mdof));
    std::vector<std::string> intersect_single;
    std::set_intersection(it->second.joints_.begin(), it->second.joints_.end(),
                          actuated_joints_single.begin(), actuated_joints_single.end(),
                          std::back_inserter(intersect_single));
    if (intersect_mdof.empty() && intersect_single.empty())
      ROS_WARN_STREAM("No joints to be distributed for controller " << controllers[i]);
    {
      if (!intersect_mdof.empty())
      {
        std::vector<std::string> &jnames = parts[i].multi_dof_joint_trajectory.joint_names;
        jnames.insert(jnames.end(), intersect_mdof.begin(), intersect_mdof.end());
        parts[i].multi_dof_joint_trajectory.frame_ids.resize(jnames.size());
        parts[i].multi_dof_joint_trajectory.child_frame_ids.resize(jnames.size());
        std::map<std::string, std::size_t> index;
        for (std::size_t j = 0 ; j < trajectory.multi_dof_joint_trajectory.joint_names.size() ; ++j)
          index[trajectory.multi_dof_joint_trajectory.joint_names[j]] = j;
        std::vector<std::size_t> bijection(jnames.size());
        for (std::size_t j = 0 ; j < jnames.size() ; ++j)
        {
          bijection[j] = index[jnames[j]];
          if (trajectory.multi_dof_joint_trajectory.frame_ids.size() > bijection[j])
            parts[i].multi_dof_joint_trajectory.frame_ids[j] = trajectory.multi_dof_joint_trajectory.frame_ids[bijection[j]];
          if (trajectory.multi_dof_joint_trajectory.child_frame_ids.size() > bijection[j])
            parts[i].multi_dof_joint_trajectory.child_frame_ids[j] = trajectory.multi_dof_joint_trajectory.child_frame_ids[bijection[j]];
        }
        parts[i].multi_dof_joint_trajectory.points.resize(trajectory.multi_dof_joint_trajectory.points.size());
        for (std::size_t j = 0 ; j < trajectory.multi_dof_joint_trajectory.points.size() ; ++j)
        {
          parts[i].multi_dof_joint_trajectory.points[j].time_from_start = trajectory.multi_dof_joint_trajectory.points[j].time_from_start;
          parts[i].multi_dof_joint_trajectory.points[j].poses.resize(bijection.size());
          for (std::size_t k = 0 ; k < bijection.size() ; ++k)
            parts[i].multi_dof_joint_trajectory.points[j].poses[k] = trajectory.multi_dof_joint_trajectory.points[j].poses[bijection[k]];
        }        
      }
      if (!intersect_single.empty())
      {
        std::vector<std::string> &jnames = parts[i].joint_trajectory.joint_names;
        jnames.insert(jnames.end(), intersect_single.begin(), intersect_single.end());
        parts[i].joint_trajectory.header = trajectory.joint_trajectory.header;
        std::map<std::string, std::size_t> index;
        for (std::size_t j = 0 ; j < trajectory.joint_trajectory.joint_names.size() ; ++j)
          index[trajectory.joint_trajectory.joint_names[j]] = j;
        std::vector<std::size_t> bijection(jnames.size());
        for (std::size_t j = 0 ; j < jnames.size() ; ++j)
          bijection[j] = index[jnames[j]];
        parts[i].joint_trajectory.points.resize(trajectory.joint_trajectory.points.size());
        for (std::size_t j = 0 ; j < trajectory.joint_trajectory.points.size() ; ++j)
        {
          parts[i].joint_trajectory.points[j].time_from_start = trajectory.joint_trajectory.points[j].time_from_start;
          if (!trajectory.joint_trajectory.points[j].positions.empty())
          {
            parts[i].joint_trajectory.points[j].positions.resize(bijection.size());
            for (std::size_t k = 0 ; k < bijection.size() ; ++k)
              parts[i].joint_trajectory.points[j].positions[k] = trajectory.joint_trajectory.points[j].positions[bijection[k]];
          }
          if (!trajectory.joint_trajectory.points[j].velocities.empty())
          {
            parts[i].joint_trajectory.points[j].velocities.resize(bijection.size());
            for (std::size_t k = 0 ; k < bijection.size() ; ++k)
              parts[i].joint_trajectory.points[j].velocities[k] = trajectory.joint_trajectory.points[j].velocities[bijection[k]];
          }
          if (!trajectory.joint_trajectory.points[j].accelerations.empty())
          {
            parts[i].joint_trajectory.points[j].accelerations.resize(bijection.size());
            for (std::size_t k = 0 ; k < bijection.size() ; ++k)
              parts[i].joint_trajectory.points[j].accelerations[k] = trajectory.joint_trajectory.points[j].accelerations[bijection[k]];
          }
        } 
      }
    }
  }
  return true;
}

bool TrajectoryExecutionManager::configure(TrajectoryExecutionContext &context, const moveit_msgs::RobotTrajectory &trajectory, const std::vector<std::string> &controllers)
{
  if (trajectory.multi_dof_joint_trajectory.points.empty() &&  trajectory.joint_trajectory.points.empty())
  {
    ROS_WARN("The trajectory to execute is empty");
    return false;
  }
  std::set<std::string> actuated_joints;
  actuated_joints.insert(trajectory.multi_dof_joint_trajectory.joint_names.begin(),
                         trajectory.multi_dof_joint_trajectory.joint_names.end());
  actuated_joints.insert(trajectory.joint_trajectory.joint_names.begin(),
                         trajectory.joint_trajectory.joint_names.end());
  if (actuated_joints.empty())
  {
    ROS_WARN("The trajectory to execute specifies not joints");
    return false;
  }
  
  if (controllers.empty())
  {
    bool retry = true;
    bool reloaded = false;
    while (retry)
    {
      retry = false;
      std::vector<std::string> all_controller_names;    
      for (std::map<std::string, ControllerInformation>::const_iterator it = known_controllers_.begin() ; it != known_controllers_.end() ; ++it)
        all_controller_names.push_back(it->first);
      if (selectControllers(actuated_joints, all_controller_names, context.controllers_))
      {
        if (distributeTrajectory(trajectory, context.controllers_, context.trajectory_parts_))
          return true;
      }
      else
      {
        // maybe we failed because we did not have a complete list of controllers
        if (!reloaded)
        {
          reloadControllerInformation();
          reloaded = true;
          retry = true;
        }
      }
    }
  }
  else
  {
    // check if the specified controllers are valid names;
    // if they appear not to be, try to reload the controller information, just in case they are new in the system
    bool reloaded = false;
    for (std::size_t i = 0 ; i < controllers.size() ; ++i)
      if (known_controllers_.find(controllers[i]) == known_controllers_.end())
      {
        reloadControllerInformation();
        reloaded = true;
        break;
      }
    if (reloaded)
      for (std::size_t i = 0 ; i < controllers.size() ; ++i)
        if (known_controllers_.find(controllers[i]) == known_controllers_.end())
        {
          ROS_ERROR("Controller '%s' is not known", controllers[i].c_str());
          return false;
        }
    if (selectControllers(actuated_joints, controllers, context.controllers_))
    {
      if (distributeTrajectory(trajectory, context.controllers_, context.trajectory_parts_))
        return true;
    }
  }
  return false;
}

moveit_controller_manager::ExecutionStatus TrajectoryExecutionManager::executeAndWait(bool auto_clear)
{
  execute(ExecutionCompleteCallback(), auto_clear);
  return waitForExecution();
}

void TrajectoryExecutionManager::stopExecution(bool auto_clear)
{
  if (!execution_complete_)
  {
    execution_state_mutex_.lock();
    if (!execution_complete_)
    {
      // we call cancel for all active handles; we know these are not being modified as we loop through them because of the lock
      // we mark execution_complete_ as true ahead of time. Using this flag, executePart() will know that an external trigger to stop has been received
      execution_complete_ = true;
      for (std::size_t i = 0 ; i < active_handles_.size() ; ++i)
        try
        {
          active_handles_[i]->cancelExecution();
        }
        catch(...)
        {
          ROS_ERROR("Exception caught when canceling execution.");
        }
      // we set the status here; executePart() will not set status when execution_complete_ is true ahead of time
      last_execution_status_ = moveit_controller_manager::ExecutionStatus::PREEMPTED;
      execution_state_mutex_.unlock();
      ROS_INFO("Stopped trajectory execution.");
      
      // wait for the execution thread to finish
      execution_thread_->join();
      execution_thread_.reset();
      
      if (auto_clear)
        clear();
    }
    else
      execution_state_mutex_.unlock();
  }
  else
    if (execution_thread_) // just in case we have some thread waiting to be joined from some point in the past, we join it now
    {  
      execution_thread_->join();
      execution_thread_.reset();
    }
}

void TrajectoryExecutionManager::execute(const ExecutionCompleteCallback &callback, bool auto_clear)
{
  stopExecution(false);
  execution_complete_ = false;
  // start the execution thread
  execution_thread_.reset(new boost::thread(&TrajectoryExecutionManager::executeThread, this, callback, auto_clear));
}

moveit_controller_manager::ExecutionStatus TrajectoryExecutionManager::waitForExecution(void)
{
  boost::unique_lock<boost::mutex> ulock(execution_state_mutex_);
  while (!execution_complete_)
    execution_complete_condition_.wait(ulock);
  // this will join the thread
  stopExecution(false);
  return last_execution_status_;
}

void TrajectoryExecutionManager::clear(void)
{
  trajectories_.clear();
}

void TrajectoryExecutionManager::executeThread(const ExecutionCompleteCallback &callback, bool auto_clear)
{
  // if we already got a stop request before we even started anything, we abort
  if (execution_complete_)
  {
    last_execution_status_ = moveit_controller_manager::ExecutionStatus::ABORTED;
    return;
  }
    
  ROS_DEBUG("Starting trajectory execution ...");
  // assume everything will be OK
  last_execution_status_ = moveit_controller_manager::ExecutionStatus::SUCCEEDED;

  // execute each trajectory, one after the other (executePart() is blocking) or until one fails.
  // on failure, the status is set by executePart(). Otherwise, it will remain as set above (success)
  for (std::size_t i = 0 ; i < trajectories_.size() ; ++i)
    if (!executePart(i) || execution_complete_)
      break;
  
  // clear the paths just executed, if needed
  if (auto_clear)
    clear();
  
  ROS_DEBUG("Completed trajectory execution with status %s ...", last_execution_status_.asString().c_str());
  
  // notify whoever is waiting for the event of trajectory completion
  execution_state_mutex_.lock();
  execution_complete_ = true;
  execution_state_mutex_.unlock();
  execution_complete_condition_.notify_all();

  // call user-specified callback
  if (callback)
    callback(last_execution_status_);  
}

bool TrajectoryExecutionManager::executePart(std::size_t part_index)
{
  TrajectoryExecutionContext &context = trajectories_[part_index];

  // first make sure desired controllers are active
  if (ensureActiveControllers(context.controllers_))
  {
    // stop if we are already asked to do so
    if (execution_complete_)
      return false;

    std::vector<moveit_controller_manager::MoveItControllerHandlePtr> handles;
    {
      boost::mutex::scoped_lock slock(execution_state_mutex_);
      if (!execution_complete_)
      {    
        // time indexing uses this member too, so we lock this mutex as well
        time_index_mutex_.lock();
        current_context_ = part_index;
        time_index_mutex_.unlock();
        active_handles_.resize(context.controllers_.size());
        for (std::size_t i = 0 ; i < context.controllers_.size() ; ++i)
          active_handles_[i] = controller_manager_->getControllerHandle(context.controllers_[i]);
        handles = active_handles_; // keep a copy for later, to avoid thread safety issues
        for (std::size_t i = 0 ; i < context.trajectory_parts_.size() ; ++i)
        {
          bool ok = false;
          try
          {
            ok = active_handles_[i]->sendTrajectory(context.trajectory_parts_[i]);
          }
          catch(...)
          {
            ROS_ERROR("Exception caught when sending trajectory to controller");
          }
          if (!ok)
          {
            for (std::size_t j = 0 ; j < i ; ++j)
              try
              {
                active_handles_[j]->cancelExecution();
              }
              catch(...)
              {
                ROS_ERROR("Exception caught when canceling execution");
              }
            ROS_ERROR("Failed to send trajectory part %lu of %lu to controller %s", i + 1, context.trajectory_parts_.size(), active_handles_[i]->getName().c_str());
            if (i > 0)
              ROS_ERROR("Cancelling previously sent trajectory parts");  
            active_handles_.clear();
            current_context_ = -1;
            last_execution_status_ = moveit_controller_manager::ExecutionStatus::ABORTED;
            return false;
          }
        }
      }
    }
    
    // compute the expected duration of the trajectory and find the part of the trajectory that takes longest to execute
    ros::Time current_time = ros::Time::now();
    ros::Duration expected_trajectory_duration(0.0);
    int longest_part = -1;
    for (std::size_t i = 0 ; i < context.trajectory_parts_.size() ; ++i)
    {
      ros::Duration d(0.0);
      if (!context.trajectory_parts_[i].joint_trajectory.points.empty())
      {
        if (context.trajectory_parts_[i].joint_trajectory.header.stamp > current_time)
          d = context.trajectory_parts_[i].joint_trajectory.header.stamp - current_time;
        if (context.trajectory_parts_[i].multi_dof_joint_trajectory.header.stamp > current_time)
          d = std::max(d, context.trajectory_parts_[i].multi_dof_joint_trajectory.header.stamp - current_time);
        d += std::max(context.trajectory_parts_[i].joint_trajectory.points.empty() ? ros::Duration(0.0) : 
                      context.trajectory_parts_[i].joint_trajectory.points.back().time_from_start,
                      context.trajectory_parts_[i].multi_dof_joint_trajectory.points.empty() ? ros::Duration(0.0) : 
                      context.trajectory_parts_[i].multi_dof_joint_trajectory.points.back().time_from_start);

        if (longest_part < 0 || 
            std::max(context.trajectory_parts_[i].joint_trajectory.points.size(),
                     context.trajectory_parts_[i].multi_dof_joint_trajectory.points.size()) >
            std::max(context.trajectory_parts_[longest_part].joint_trajectory.points.size(),
                     context.trajectory_parts_[longest_part].multi_dof_joint_trajectory.points.size()))
          longest_part = i;
      }
      expected_trajectory_duration = std::max(d, expected_trajectory_duration);
    }
    // add 10% + 0.5s to the expected duration; this is just to allow things to finish propery
    expected_trajectory_duration = expected_trajectory_duration * 1.1 + ros::Duration(0.5);


    if (longest_part >= 0)
    {  
      boost::mutex::scoped_lock slock(time_index_mutex_);
      
      // construct a map from expected time to state index, for easy access to expected state location
      if (context.trajectory_parts_[longest_part].joint_trajectory.points.size() >= context.trajectory_parts_[longest_part].multi_dof_joint_trajectory.points.size())
      {
        ros::Duration d(0.0);
        if (context.trajectory_parts_[longest_part].joint_trajectory.header.stamp > current_time)
          d = context.trajectory_parts_[longest_part].joint_trajectory.header.stamp - current_time;
        for (std::size_t j = 0 ; j < context.trajectory_parts_[longest_part].joint_trajectory.points.size() ; ++j)
          time_index_.push_back(current_time + d + context.trajectory_parts_[longest_part].joint_trajectory.points[j].time_from_start);
      }
      else
      {
        ros::Duration d(0.0);
        if (context.trajectory_parts_[longest_part].multi_dof_joint_trajectory.header.stamp > current_time)
          d = context.trajectory_parts_[longest_part].multi_dof_joint_trajectory.header.stamp - current_time;
        for (std::size_t j = 0 ; j < context.trajectory_parts_[longest_part].multi_dof_joint_trajectory.points.size() ; ++j)
          time_index_.push_back(current_time + d + context.trajectory_parts_[longest_part].multi_dof_joint_trajectory.points[j].time_from_start);
      }
    }
    
    bool result = true;
    for (std::size_t i = 0 ; i < handles.size() ; ++i)
    {
      if (!handles[i]->waitForExecution(expected_trajectory_duration))
        if (!execution_complete_ && ros::Time::now() - current_time > expected_trajectory_duration)
        {
          ROS_ERROR("Controller is taking too long to execute trajectory (the expected upper bound for the trajectory execution was %lf seconds). Stopping trajectory.", expected_trajectory_duration.toSec());
          stopExecution(false);
          // we overwrite the PREEMPTED status set by stopExecution() here
          last_execution_status_ = moveit_controller_manager::ExecutionStatus::TIMED_OUT;
        }
      if (execution_complete_)
      {
        result = false;
        break;
      }
      else
        if (handles[i]->getLastExecutionStatus() != moveit_controller_manager::ExecutionStatus::SUCCEEDED)
        {
          ROS_WARN("Controller handle reports status %s", handles[i]->getLastExecutionStatus().asString().c_str());
          last_execution_status_ = handles[i]->getLastExecutionStatus();
          result = false;
        }
    }
    
    // clear the active handles
    execution_state_mutex_.lock();
    active_handles_.clear();

    // clear the time index
    time_index_mutex_.lock();
    time_index_.clear();
    current_context_ = -1;
    time_index_mutex_.unlock();
    
    execution_state_mutex_.unlock();
    return result;
  }
  else
    return false;
}

std::pair<int, int> TrajectoryExecutionManager::getCurrentExpectedTrajectoryIndex(void) const
{
  boost::mutex::scoped_lock slock(time_index_mutex_);
  if (current_context_ < 0)
    return std::make_pair(-1, -1);
  if (time_index_.empty())
    return std::make_pair((int)current_context_, -1);
  std::vector<ros::Time>::const_iterator it = std::lower_bound(time_index_.begin(), time_index_.end(), ros::Time::now());
  int pos = it - time_index_.begin();
  return std::make_pair((int)current_context_, pos);
}

const std::vector<TrajectoryExecutionManager::TrajectoryExecutionContext>& TrajectoryExecutionManager::getTrajectories(void) const
{
  return trajectories_;
}

moveit_controller_manager::ExecutionStatus TrajectoryExecutionManager::getLastExecutionStatus(void) const
{
  return last_execution_status_;
}

bool TrajectoryExecutionManager::ensureActiveControllersForGroup(const std::string &group)
{
  const planning_models::KinematicModel::JointModelGroup *joint_model_group = kinematic_model_->getJointModelGroup(group);
  if (joint_model_group)
    return ensureActiveControllersForJoints(joint_model_group->getJointModelNames());
  else
    return false;
}

bool TrajectoryExecutionManager::ensureActiveControllersForJoints(const std::vector<std::string> &joints)
{ 
  std::vector<std::string> all_controller_names;    
  for (std::map<std::string, ControllerInformation>::const_iterator it = known_controllers_.begin() ; it != known_controllers_.end() ; ++it)
    all_controller_names.push_back(it->first);
  std::vector<std::string> selected_controllers;
  std::set<std::string> jset;
  jset.insert(joints.begin(), joints.end());
  if (selectControllers(jset, all_controller_names, selected_controllers))
    return ensureActiveControllers(selected_controllers);
  else
    return false;
}

bool TrajectoryExecutionManager::ensureActiveController(const std::string &controller)
{
  return ensureActiveControllers(std::vector<std::string>(1, controller));
}

bool TrajectoryExecutionManager::ensureActiveControllers(const std::vector<std::string> &controllers)
{
  updateControllersState(ros::Duration(1.0));
  
  if (manage_controllers_)
  {
    std::vector<std::string> controllers_to_activate;
    std::vector<std::string> controllers_to_deactivate;
    std::set<std::string> joints_to_be_activated;
    std::set<std::string> joints_to_be_deactivated;
    for (std::size_t i = 0 ; i < controllers.size() ; ++i)
    {
      std::map<std::string, ControllerInformation>::const_iterator it = known_controllers_.find(controllers[i]);
      if (it == known_controllers_.end())
      {
        ROS_ERROR_STREAM("Controller " << controllers[i] << " is not known");
        return false;
      }
      if (!it->second.state_.active_)
      {
        ROS_DEBUG_STREAM("Need to activate " << controllers[i]);
        controllers_to_activate.push_back(controllers[i]);
        joints_to_be_activated.insert(it->second.joints_.begin(), it->second.joints_.end());
        for (std::set<std::string>::iterator kt = it->second.overlapping_controllers_.begin() ; 
             kt != it->second.overlapping_controllers_.end() ; ++kt)
        {
          const ControllerInformation &ci = known_controllers_[*kt];
          if (ci.state_.active_)
          {
            controllers_to_deactivate.push_back(*kt);
            joints_to_be_deactivated.insert(ci.joints_.begin(), ci.joints_.end());
          }
        }
      }
      else
        ROS_DEBUG_STREAM("Controller " << controllers[i] << " is already active");
    }
    std::set<std::string> diff;
    std::set_difference(joints_to_be_deactivated.begin(), joints_to_be_deactivated.end(),
                        joints_to_be_activated.begin(), joints_to_be_activated.end(),
                        std::inserter(diff, diff.end()));
    if (!diff.empty())
    {
      // find the set of controllers that do not overlap with the ones we want to activate so far
      std::vector<std::string> possible_additional_controllers;
      for (std::map<std::string, ControllerInformation>::const_iterator it = known_controllers_.begin() ; it != known_controllers_.end() ; ++it)
      {
        bool ok = true;
        for (std::size_t k = 0 ; k < controllers_to_activate.size() ; ++k)
          if (it->second.overlapping_controllers_.find(controllers_to_activate[k]) != it->second.overlapping_controllers_.end())
          {
            ok = false;
            break;
          }
        if (ok)
          possible_additional_controllers.push_back(it->first);
      }
      
      // out of the allowable controllers, try to find a subset of controllers that covers the joints to be actuated
      std::vector<std::string> additional_controllers;
      if (selectControllers(diff, possible_additional_controllers, additional_controllers))
        controllers_to_activate.insert(controllers_to_activate.end(), additional_controllers.begin(), additional_controllers.end());
      else
        return false;
    }
    if (!controllers_to_activate.empty() || !controllers_to_deactivate.empty())
    {
      if (controller_manager_)
      {   
        // load controllers to be activated, if needed, and reset the state update cache
        for (std::size_t a = 0 ; a < controllers_to_activate.size() ; ++a)
        {
          ControllerInformation &ci = known_controllers_[controllers_to_activate[a]];
          ci.last_update_ = ros::Time();
          if (!ci.state_.loaded_)
            if (!controller_manager_->loadController(controllers_to_activate[a]))
              return false;
        }
        // reset the state update cache
        for (std::size_t a = 0 ; a < controllers_to_deactivate.size() ; ++a)  
          known_controllers_[controllers_to_deactivate[a]].last_update_ = ros::Time();
        return controller_manager_->switchControllers(controllers_to_activate, controllers_to_deactivate);
      }
      else
        return false;
    }
    else
      return true;
  }
  else
  {   
    std::set<std::string> originally_active; 
    for (std::map<std::string, ControllerInformation>::const_iterator it = known_controllers_.begin() ; it != known_controllers_.end() ; ++it)
      if (it->second.state_.active_)
        originally_active.insert(it->first);
    return std::includes(originally_active.begin(), originally_active.end(), controllers.begin(), controllers.end());
  }
}


}

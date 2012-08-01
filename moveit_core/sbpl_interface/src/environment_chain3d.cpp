/*
 * Copyright (c) 2010, Maxim Likhachev
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the University of Pennsylvania nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/** \author Benjamin Cohen, E. Gil Jones */

#include <sbpl_interface/environment_chain3d.h>
#include <collision_detection/collision_common.h>
#include <planning_models/conversions.h>
#include <boost/timer.hpp>
#include <planning_models/angle_utils.h>

static const unsigned int DEBUG_OVER = 1;
static const unsigned int PRINT_HEURISTIC_UNDER = 1;

static const double LONG_RANGE_JOINT_DIFF=.1;
static const double JOINT_DIST_MULT=1000.0;

namespace sbpl_interface
{

EnvironmentChain3D::EnvironmentChain3D(const planning_scene::PlanningSceneConstPtr& planning_scene) : 
  planning_scene_(planning_scene),
  bfs_(NULL),
  state_(planning_scene->getCurrentState()),
  planning_data_(StateID2IndexMapping), 
  goal_constraint_set_(planning_scene->getKinematicModel(),
                       planning_scene->getTransforms()),
  closest_to_goal_(DBL_MAX)
{
}


EnvironmentChain3D::~EnvironmentChain3D()
{
  if(bfs_ != NULL) {
    delete bfs_;
  }
}

/////////////////////////////////////////////////////////////////////////////
//                      SBPL Planner Interface
/////////////////////////////////////////////////////////////////////////////

bool EnvironmentChain3D::InitializeMDPCfg(MDPConfig *MDPCfg)
{
  if(planning_data_.goal_hash_entry_ &&
     planning_data_.start_hash_entry_) {
    MDPCfg->goalstateid = planning_data_.goal_hash_entry_->stateID;
    MDPCfg->startstateid = planning_data_.start_hash_entry_->stateID;
    return true;
  }
  return false;
}

bool EnvironmentChain3D::InitializeEnv(const char* sEnvFile)
{
  ROS_INFO("[env] InitializeEnv is not implemented right now.");
  return true;
}

int EnvironmentChain3D::GetFromToHeuristic(int FromStateID, int ToStateID)
{
  //std::cerr << "Getting heuristic" << std::endl;
  return getEndEffectorHeuristic(FromStateID,ToStateID);
}

int EnvironmentChain3D::GetGoalHeuristic(int stateID)
{
  if(planning_data_.state_ID_to_coord_table_.size() < DEBUG_OVER) {
    std::cerr << "Getting heur distance from " << stateID << " to " 
              << planning_data_.goal_hash_entry_->stateID << " " 
              << GetFromToHeuristic(stateID, planning_data_.goal_hash_entry_->stateID) << std::endl;
  }
  return GetFromToHeuristic(stateID, planning_data_.goal_hash_entry_->stateID);
}

int EnvironmentChain3D::GetStartHeuristic(int stateID)
{
  return GetFromToHeuristic(stateID, planning_data_.start_hash_entry_->stateID);
}

int EnvironmentChain3D::SizeofCreatedEnv()
{
  return planning_data_.state_ID_to_coord_table_.size();
}

void EnvironmentChain3D::PrintState(int stateID, bool bVerbose, FILE* fOut /*=NULL*/)
{
  // if(fOut == NULL)
  //   fOut = stdout;

  // EnvChain3DHashEntry* HashEntry = EnvChain.StateID2CoordTable[stateID];

  // bool bGoal = false;
  // if(stateID == EnvChain.goalHashEntry->stateID)
  //   bGoal = true;

  // if(stateID == EnvChain.goalHashEntry->stateID && bVerbose)
  // {
  //   //ROS_DEBUG_NAMED(fOut, "the state is a goal state\n");
  //   bGoal = true;
  // }

  // printJointArray(fOut, HashEntry, bGoal, bVerbose);
}

void EnvironmentChain3D::PrintEnv_Config(FILE* fOut)
{
  std::cerr <<("ERROR in EnvChain... function: PrintEnv_Config is undefined\n");
  throw new SBPL_Exception();
}

void EnvironmentChain3D::GetSuccs(int source_state_ID, 
                                  std::vector<int>* succ_idv, 
                                  std::vector<int>* cost_v)
{
  //std::cerr << "Calling get succ state " << source_state_ID << std::endl;

  ros::WallTime expansion_start_time = ros::WallTime::now();

  succ_idv->clear();
  cost_v->clear();

  //From environment_robarm3d.cpp -- //goal state should be absorbing
  if(source_state_ID == planning_data_.goal_hash_entry_->stateID) {
    std::cerr << "Think we have goal" << std::endl;
    return;
  }

  if(source_state_ID > (int)planning_data_.state_ID_to_coord_table_.size()-1) {
    ROS_WARN_STREAM("source id too large");
    std::cerr << "Source id too large" << std::endl;
    return;
  }

  if(planning_data_.state_ID_to_coord_table_.size() < DEBUG_OVER) {
    std::cerr << "Expanding " << source_state_ID << std::endl;
  }

  EnvChain3DHashEntry* hash_entry = planning_data_.state_ID_to_coord_table_[source_state_ID];

  std::vector<double> source_joint_angles = hash_entry->angles;
  //convertCoordToJointAngles(hash_entry->coord, source_joint_angles);

  std::vector<int> succ_coord;
  std::vector<double> succ_joint_angles;

  // for(unsigned int i = 0; i < source_joint_angles.size(); i++) {
  //   std::cerr << "Source " << i << " " << source_joint_angles[i] << std::endl;
  // }

  planning_statistics_.total_expansions_++;

  for(unsigned int i = 0; i < possible_actions_.size(); i++) {
    if(!possible_actions_[i]->generateSuccessorState(source_joint_angles, succ_joint_angles)) {
      continue;
    }

    // for(unsigned int j = 0; j < planning_data_.goal_hash_entry_->angles.size(); j++) {
    //   if(joint_is_continuous_[j]) {
    //     if(source_joint_angles[j] < M_PI && succ_joint_angles[j] > M_PI) {
    //       succ_joint_angles[j] -= 2*M_PI;
    //     } else if(source_joint_angles[j] > -M_PI && succ_joint_angles[j] < -M_PI) {
    //       succ_joint_angles[j] += 2*M_PI;
    //     }
    //   } 
    // }
    // double dist = 0.0;
    // for(unsigned int j = 0; j < planning_data_.goal_hash_entry_->angles.size(); j++) {
    //   dist += fabs(planning_data_.goal_hash_entry_->angles[j]-succ_joint_angles[j]);
    //   //std::cerr << "Succ " << i << " " << j << " " << succ_joint_angles[j] << std::endl;
    // }
    // if(dist < closest_to_goal_) {
    //   //std::cerr << "Got dist " << dist << std::endl;
    //   closest_to_goal_ = dist;
    // }

    int max_dist = getJointDistanceIntegerMax(succ_joint_angles,
                                              planning_data_.goal_hash_entry_->angles);
    if(max_dist*1.0 < closest_to_goal_) {
      std::cerr << "Max integer distance is " << max_dist << std::endl;
      closest_to_goal_ = max_dist*1.0;
      // for(unsigned int j = 0; j < joint_motion_wrappers_.size(); j++) {
      //   if(joint_motion_wrappers_[j]->canGetCloser(succ_joint_angles[j], 
      //                                              planning_data_.goal_hash_entry_->angles[j],
      //                                              LONG_RANGE_JOINT_DIFF)) {
      //     std::cerr << "Joint " << j << " succ " << succ_joint_angles[j] << " "                                                  
      //               << planning_data_.goal_hash_entry_->angles[j] << " can get closer\n";
      //     break;
      //   } 
      // }
    }
    convertJointAnglesToCoord(succ_joint_angles, succ_coord);
    
    joint_state_group_->setStateValues(succ_joint_angles);

    // if(!joint_state_group_->satisfiesBounds()) {
    //   std::cerr << "ERROR - successor doesn't satisfy bounds" << std::endl;
    //   //std::cerr << "Successor doesn't satisfy bounds" << std::endl;
    //   continue;
    // }
    ros::WallTime before_coll = ros::WallTime::now();
    collision_detection::CollisionRequest req;
    collision_detection::CollisionResult res;
    req.group_name = planning_group_;
    hy_world_->checkCollisionDistanceField(req, 
                                           res, 
                                           *hy_robot_->getCollisionRobotDistanceField().get(),
                                           state_,
                                           gsr_);
    planning_statistics_.coll_checks_++;
    //std::cerr << "Elapsed " << t.elapsed() << std::endl;
    ros::WallDuration dur(ros::WallTime::now()-before_coll);
    //std::cerr << dur.toSec() << std::endl;
    //ROS_DEBUG_STREAM("Time " << ros::WallTime::now()-before_coll);
    planning_statistics_.total_coll_check_time_ += dur;
    if(res.collision) {
      //std::cerr << "Successor in collision" << std::endl;
      continue;
    }

    Eigen::Affine3d pose = tip_link_state_->getGlobalLinkTransform();
    
    int xyz[3];
    if(!getGridXYZInt(pose, xyz)) {
      std::cerr << "Can't get successor x y z" << std::endl;
      continue;
    }
    EnvChain3DHashEntry* succ_hash_entry;
    // bool can_get_closer = false;
    // for(unsigned int j = 0; j < joint_motion_wrappers_.size(); j++) {
    //   if(joint_motion_wrappers_[j]->canGetCloser(succ_joint_angles[j], 
    //                                              planning_data_.goal_hash_entry_->angles[j],
    //                                              LONG_RANGE_JOINT_DIFF)) {
    //     can_get_closer = true;
    //     // std::cerr << "Joint " << j << " succ " << succ_joint_angles[j] << " "                                                  
    //     //           << planning_data_.goal_hash_entry_->angles[j] << " can get closer\n";
    //     break;
    //   } 
    // }
    //if(!can_get_closer) {
    if(max_dist == 1) {
      succ_hash_entry = planning_data_.goal_hash_entry_;
    } else {
      succ_hash_entry = planning_data_.getHashEntry(succ_coord, i);
    }
    // double max_dist = getJointDistanceMax(planning_data_.goal_hash_entry_->angles, succ_joint_angles);
    // if(max_dist < closest_to_goal_) {
    //   std::cerr << "State " << source_state_ID << " sum dist " << getJointDistanceSum(planning_data_.goal_hash_entry_->angles, succ_joint_angles) << " max "
    //             << " " << getJointDistanceMax(planning_data_.goal_hash_entry_->angles, succ_joint_angles) << std::endl;
    //   for(unsigned int i = 0; i < planning_data_.goal_hash_entry_->angles.size(); i++) {
    //     std::cerr << "Joint " << i << " " << planning_data_.goal_hash_entry_->angles[i] << " " << succ_joint_angles[i] << std::endl;
    //   }
    //   closest_to_goal_ = max_dist;
    // }
    // std::cerr << "Dist " << dist << " source state id " << source_state_ID << std::endl;
    // if(max_dist <= LONG_RANGE_JOINT_DIFF/2.0+.001) {
    //   succ_hash_entry = planning_data_.goal_hash_entry_;
    // } else {
    //   succ_hash_entry = planning_data_.getHashEntry(succ_coord, i);
    // }
    
    // kinematic_constraints::ConstraintEvaluationResult con_res = goal_constraint_set_.decide(state_);
    // if(con_res.satisfied) {
    //   std::cerr << "Constraints satisfied, dist is " << dist << std::endl;
    //   succ_hash_entry = planning_data_.goal_hash_entry_;
    // } else {
    //   succ_hash_entry = planning_data_.getHashEntry(succ_coord, i);
    // }
    if(!succ_hash_entry) {
      succ_hash_entry = planning_data_.addHashEntry(succ_coord, succ_joint_angles, xyz, i);
    } else {
      if(planning_data_.state_ID_to_coord_table_.size() < DEBUG_OVER) {
        std::cerr << "Already have hash entry " << succ_hash_entry->stateID << std::endl;
      }
    }

    //std::cerr << "Adding hash entry" << std::endl;
    if(planning_data_.state_ID_to_coord_table_.size() < DEBUG_OVER) {
      std::cerr << std::endl;
      std::cerr << "Adding " << succ_hash_entry->stateID << std::endl;
      for(unsigned int j = 0; j < planning_data_.goal_hash_entry_->angles.size(); j++) {
        std::cerr << "Succ " << j << " " << succ_joint_angles[j] << std::endl;
      }
    }
    succ_idv->push_back(succ_hash_entry->stateID);
    cost_v->push_back(calculateCost(hash_entry, succ_hash_entry));
  }
  planning_statistics_.total_expansion_time_ += ros::WallTime::now()-expansion_start_time;
}

void EnvironmentChain3D::GetPreds(int TargetStateID, vector<int>* PredIDV, vector<int>* cost_v)
{
  std::cerr <<("ERROR in EnvChain... function: GetPreds is undefined\n");
  throw new SBPL_Exception();
}

bool EnvironmentChain3D::AreEquivalent(int StateID1, int StateID2)
{
  std::cerr <<("ERROR in EnvChain... function: AreEquivalent is undefined\n");
  throw new SBPL_Exception();
}

void EnvironmentChain3D::SetAllActionsandAllOutcomes(CMDPSTATE* state)
{
  std::cerr <<("ERROR in EnvChain..function: SetAllActionsandOutcomes is undefined\n");
  throw new SBPL_Exception();
}

void EnvironmentChain3D::SetAllPreds(CMDPSTATE* state)
{
  std::cerr <<("ERROR in EnvChain... function: SetAllPreds is undefined\n");
  throw new SBPL_Exception();
}

/////////////////////////////////////////////////////////////////////////////
//                      End of SBPL Planner Interface
/////////////////////////////////////////////////////////////////////////////

bool EnvironmentChain3D::setupForMotionPlan(const planning_scene::PlanningSceneConstPtr& planning_scene,
                                            const moveit_msgs::GetMotionPlan::Request &mreq,
                                            moveit_msgs::GetMotionPlan::Response &mres)
{
  planning_scene_ = planning_scene;
  
  planning_group_ = mreq.motion_plan_request.group_name;
  
  hy_world_ = dynamic_cast<const collision_detection::CollisionWorldHybrid*>(planning_scene->getCollisionWorld().get());
  if(!hy_world_) {
    ROS_WARN_STREAM("Could not initialize hybrid collision world from planning scene");
    mres.error_code.val = moveit_msgs::MoveItErrorCodes::COLLISION_CHECKING_UNAVAILABLE;
    return false;
  }
  
  hy_robot_ = dynamic_cast<const collision_detection::CollisionRobotHybrid*>(planning_scene->getCollisionRobot().get());
  if(!hy_robot_) {
    ROS_WARN_STREAM("Could not initialize hybrid collision robot from planning scene");
    mres.error_code.val = moveit_msgs::MoveItErrorCodes::COLLISION_CHECKING_UNAVAILABLE;
    return false;
  }

  state_ = planning_scene->getCurrentState();
  planning_models::robotStateToKinematicState(*planning_scene->getTransforms(), mreq.motion_plan_request.start_state, state_);
  joint_state_group_ = state_.getJointStateGroup(planning_group_);
  tip_link_state_ = state_.getLinkState(joint_state_group_->getJointModelGroup()->getLinkModelNames().back());
  setMotionPrimitives(planning_group_);
  
  collision_detection::CollisionRequest req;
  collision_detection::CollisionResult res;
  req.group_name = planning_group_;
  hy_world_->checkCollisionDistanceField(req, 
                                         res, 
                                         *hy_robot_->getCollisionRobotDistanceField().get(),
                                         state_,
                                         planning_scene_->getAllowedCollisionMatrix(),
                                         gsr_);
  if(res.collision) {
    ROS_WARN_STREAM("Start state is in collision.  Can't plan");
    mres.error_code.val = moveit_msgs::MoveItErrorCodes::START_STATE_IN_COLLISION;
    return false;
  }
  angle_discretization_ = gsr_->dfce_->distance_field_->getResolution();
  bfs_ = new BFS_3D(gsr_->dfce_->distance_field_->getXNumCells(),
                    gsr_->dfce_->distance_field_->getYNumCells(),
                    gsr_->dfce_->distance_field_->getZNumCells());

  boost::shared_ptr<const distance_field::DistanceField> world_distance_field = hy_world_->getCollisionWorldDistanceField()->getDistanceField();
  if(world_distance_field->getXNumCells() != gsr_->dfce_->distance_field_->getXNumCells() ||
     world_distance_field->getYNumCells() != gsr_->dfce_->distance_field_->getYNumCells() ||
     world_distance_field->getZNumCells() != gsr_->dfce_->distance_field_->getZNumCells()) {
    ROS_WARN_STREAM("Size mismatch between world and self distance fields");
    std::cerr << "Size mismatch between world and self distance fields" << std::endl;
    mres.error_code.val = moveit_msgs::MoveItErrorCodes::COLLISION_CHECKING_UNAVAILABLE;
    return false;
  }
  std::cerr << "BFS dimensions are "
            << world_distance_field->getXNumCells() << " " 
            << world_distance_field->getYNumCells() << " "
            << world_distance_field->getZNumCells() << std::endl;
  unsigned int wall_count = 0;
  for(int i = 0; i < gsr_->dfce_->distance_field_->getXNumCells()-2; i++) {
    for(int j = 0; j < gsr_->dfce_->distance_field_->getYNumCells()-2; j++) {
      for(int k = 0; k < gsr_->dfce_->distance_field_->getZNumCells()-2; k++) {
        if(gsr_->dfce_->distance_field_->getDistanceFromCell(i+1, j+1, k+1) == 0.0 ||
           world_distance_field->getDistanceFromCell(i+1, j+1, k+1) == 0.0) {
          bfs_->setWall(i+1, j+1, k+1);
          wall_count++;
        } 
      }
    }
  }
  std::cerr << "Wall cells are " << wall_count << " of " << 
    world_distance_field->getXNumCells()*world_distance_field->getYNumCells()*world_distance_field->getZNumCells() << std::endl;

  //setting start position
  std::vector<double> start_joint_values;
  joint_state_group_->getGroupStateValues(start_joint_values);
  std::vector<int> start_coords;
  convertJointAnglesToCoord(start_joint_values, start_coords);
  Eigen::Affine3d start_pose = tip_link_state_->getGlobalLinkTransform();

  int start_xyz[3];
  if(!getGridXYZInt(start_pose, start_xyz)) {
    std::cerr << "Bad start pose" << std::endl;
    mres.error_code.val = moveit_msgs::MoveItErrorCodes::INVALID_ROBOT_STATE;
    return false;
  }
  planning_data_.start_hash_entry_ = planning_data_.addHashEntry(start_coords,
                                                                 start_joint_values,
                                                                 start_xyz,
                                                                 0);

  //setting goal position
  planning_models::KinematicState goal_state(state_);
  std::map<std::string, double> goal_vals;
  for(unsigned int i = 0; i < mreq.motion_plan_request.goal_constraints[0].joint_constraints.size(); i++) {
    goal_vals[mreq.motion_plan_request.goal_constraints[0].joint_constraints[i].joint_name] = mreq.motion_plan_request.goal_constraints[0].joint_constraints[i].position;
  }
  goal_state.setStateValues(goal_vals);
  hy_world_->checkCollisionDistanceField(req, 
                                         res, 
                                         *hy_robot_->getCollisionRobotDistanceField().get(),
                                         goal_state,
                                         planning_scene_->getAllowedCollisionMatrix(),
                                         gsr_);
  if(res.collision) {
    ROS_WARN_STREAM("Goal state is in collision.  Can't plan");
    mres.error_code.val = moveit_msgs::MoveItErrorCodes::GOAL_IN_COLLISION;
    return false;
  }
  std::vector<double> goal_joint_values;
  planning_models::KinematicState::JointStateGroup* goal_joint_state_group = goal_state.getJointStateGroup(planning_group_);
  goal_joint_state_group->getGroupStateValues(goal_joint_values);
  std::vector<int> goal_coords;
  convertJointAnglesToCoord(goal_joint_values, goal_coords);
  goal_pose_ = goal_state.getLinkState(tip_link_state_->getName())->getGlobalLinkTransform();
  int goal_xyz[3];
  if(!getGridXYZInt(goal_pose_, goal_xyz)) {
    std::cerr << "Bad goal pose" << std::endl;
    mres.error_code.val = moveit_msgs::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS;
    return false;
  }
  std::vector<std::string> goal_dofs = goal_joint_state_group->getJointModelGroup()->getActiveDOFNames();
  for(unsigned int i = 0; i < goal_dofs.size(); i++) {
    ROS_INFO_STREAM("Start " << goal_dofs[i] << " pos " << start_joint_values[i]);
  }
  for(unsigned int i = 0; i < goal_dofs.size(); i++) {
    ROS_INFO_STREAM("Goal " << goal_dofs[i] << " pos " << goal_joint_values[i]);
  }
  //std::cerr << "Running bfs with goal " << goal_xyz[0] << " " <<  goal_xyz[1] << " " << goal_xyz[2] << std::endl;
                                
  bfs_->run(goal_xyz[0], goal_xyz[1], goal_xyz[2]);
  // std::cerr << "Got start " << start_xyz[0] << " " <<  start_xyz[1] << " " << start_xyz[2] << " cost " 
  //           << getBFSCostToGoal(start_xyz[0], start_xyz[1], start_xyz[2]) << std::endl;
  goal_constraint_set_.clear();
  goal_constraint_set_.add(mreq.motion_plan_request.goal_constraints[0]);
  planning_data_.goal_hash_entry_ = planning_data_.addHashEntry(goal_coords,
                                                                goal_joint_values,
                                                                goal_xyz,
                                                                0);
  return true;
}

void EnvironmentChain3D::setMotionPrimitives(const std::string& group_name) {
  possible_actions_.clear();
  if(!joint_state_group_) {
    ROS_ERROR_STREAM("Can't set motion primitives as joint state group not set");
  }
  const planning_models::KinematicModel::JointModelGroup* jmg = joint_state_group_->getJointModelGroup();
  for(unsigned int i = 0; i < jmg->getActiveDOFNames().size(); i++) {
    const planning_models::KinematicModel::JointModel* joint = jmg->getJointModel(jmg->getActiveDOFNames()[i]);
    boost::shared_ptr<JointMotionWrapper> jmw(new JointMotionWrapper(joint));
    joint_motion_wrappers_.push_back(jmw);
    boost::shared_ptr<SingleJointMotionPrimitive> sing_pos(new SingleJointMotionPrimitive(jmw, i, LONG_RANGE_JOINT_DIFF));
    boost::shared_ptr<SingleJointMotionPrimitive> sing_neg(new SingleJointMotionPrimitive(jmw, i, -LONG_RANGE_JOINT_DIFF));
    possible_actions_.push_back(sing_pos);
    possible_actions_.push_back(sing_neg);
  }
}

int EnvironmentChain3D::calculateCost(EnvChain3DHashEntry* HashEntry1, EnvChain3DHashEntry* HashEntry2)
{
  //if(prms_.use_uniform_cost_)
  return 1000.0;
  //return .05;//prms_.cost_multiplier_;
  //  else
  //   {
  //     // Max's suggestion is to just put a high cost on being close to
  //     // obstacles but don't provide some sort of gradient 
  //     if(int(HashEntry2->dist) < 7) // in cells
  //       return prms_.cost_multiplier_ * prms_.range1_cost_;
  //     else if(int(HashEntry2->dist) < 12)
  //       return prms_.cost_multiplier_ * prms_.range2_cost_;
  //     else if(int(HashEntry2->dist) < 17)
  //       return prms_.cost_multiplier_ * prms_.range3_cost_;
  //     else
  //       return prms_.cost_multiplier_;
  //   }
  // }
}

// double EnvironmentChain3D::getEpsilon()
// {
//   return 10.0;
//   //printf("%0.3f\n",prms_.epsilon_);fflush(stdout);
//   //return prms_.epsilon_;
// }

// int EnvironmentChain3D::getEdgeCost(int FromStateID, int ToStateID)
// {
// #if DEBUG
//   if(FromStateID >= (int)EnvChain.StateID2CoordTable.size() 
//       || ToStateID >= (int)EnvChain.StateID2CoordTable.size())
//   {
//     std::cerr <<("ERROR in EnvChain... function: stateID illegal\n");
//     throw new SBPL_Exception();
//   }
// #endif

//   //get X, Y for the state
//   EnvChain3DHashEntry* FromHashEntry = EnvChain.StateID2CoordTable[FromStateID];
//   EnvChain3DHashEntry* ToHashEntry = EnvChain.StateID2CoordTable[ToStateID];

//   return cost(FromHashEntry, ToHashEntry, false);
// }

int EnvironmentChain3D::getBFSCostToGoal(int x, int y, int z) const
{
  //std::cerr << "Getting cost for " << x << " " << y << " " << z << std::endl;
  //TODO - deal with cost_per_cell
  return  bfs_->getDistance(x,y,z)*100;
  //* .05;//prms_.cost_per_cell_;
}

int EnvironmentChain3D::getJointDistanceIntegerSum(const std::vector<double>& angles1,
                                                   const std::vector<double>& angles2) 
{
  if(angles1.size() != angles2.size()) {
    std::cerr << "Angles aren't the same size!!!" << std::endl;
    return INT_MAX;
  }
  int dist = 0;
  for(unsigned int i = 0; i < angles1.size(); i++) {
    dist += joint_motion_wrappers_[i]->getIntegerDistance(angles1[i], angles2[i], LONG_RANGE_JOINT_DIFF);
  }
  return dist;
}

int EnvironmentChain3D::getJointDistanceIntegerMax(const std::vector<double>& angles1,
                                                   const std::vector<double>& angles2) 
{
  if(angles1.size() != angles2.size()) {
    std::cerr << "Angles aren't the same size!!!" << std::endl;
    return INT_MAX;
  }
  int max_dist = 0;
  for(unsigned int i = 0; i < angles1.size(); i++) {
    int dist = joint_motion_wrappers_[i]->getIntegerDistance(angles1[i], angles2[i], LONG_RANGE_JOINT_DIFF);
    if(dist > max_dist) {
      max_dist = dist;
    }
  }
  return max_dist;
}

double EnvironmentChain3D::getJointDistanceDoubleSum(const std::vector<double>& angles1,
                                                     const std::vector<double>& angles2) 
{
  if(angles1.size() != angles2.size()) {
    return DBL_MAX;
  }
  double dist = 0.0;
  for(unsigned int i = 0; i < angles1.size(); i++) {
    dist += joint_motion_wrappers_[i]->getDoubleDistance(angles1[i], angles2[i]);
  }
  return dist;
}

// double EnvironmentChain3D::getJointDistanceMax(const std::vector<double>& angles1,
//                                                const std::vector<double>& angles2) 
// {
//   if(angles1.size() != angles2.size()) {
//     return DBL_MAX;
//   }
//   double max_dist = 0.0;
//   for(unsigned int i = 0; i < angles1.size(); i++) {
//     double dist;
//     if(joint_is_continuous_[i]) {
//       if(planning_data_.state_ID_to_coord_table_.size() < PRINT_HEURISTIC_UNDER) {
//         ROS_INFO_STREAM("Dist for cont joint " << i << " from " << angles1[i] << " to " << angles2[i] << " is " << 
//                         planning_models::shortestAngularDistance(angles1[i],angles2[i]));
//       }
//       dist = fabs(planning_models::shortestAngularDistance(angles1[i],angles2[i]));
//     } else {
//       if(planning_data_.state_ID_to_coord_table_.size() < PRINT_HEURISTIC_UNDER) {
//         ROS_INFO_STREAM("Dist for reg joint " << i << " from " << angles1[i] << " to " << angles2[i] << " is " << 
//                         fabs(angles1[i]-angles2[i]));
//       }
//       dist = fabs(angles1[i]-angles2[i]);
//     }
//     if(max_dist < dist) {
//       max_dist = dist;
//     }
//   }
//   return max_dist;
// }

int EnvironmentChain3D::getEndEffectorHeuristic(int from_stateID, int to_stateID)
{
  EnvChain3DHashEntry* from_hash_entry = planning_data_.state_ID_to_coord_table_[from_stateID];
  EnvChain3DHashEntry* to_hash_entry = planning_data_.state_ID_to_coord_table_[to_stateID];
  //if(planning_data_.state_ID_to_coord_table_.size() < PRINT_HEURISTIC_UNDER) {
  //std::cerr << " Dist " << dist << " heur " << getBFSCostToGoal(from_hash_entry->xyz[0], from_hash_entry->xyz[1], from_hash_entry->xyz[2]) << std::endl;
  return getJointDistanceIntegerSum(from_hash_entry->angles,
                                    to_hash_entry->angles)*JOINT_DIST_MULT;
  //return getBFSCostToGoal(from_hash_entry->xyz[0], from_hash_entry->xyz[1], from_hash_entry->xyz[2]);
  //else
  //{
  // double x, y, z;
  // if(!gsr_->dfce_->distance_field_->gridToWorld(from_hash_entry->xyz[0],
  //                                               from_hash_entry->xyz[1],
  //                                               from_hash_entry->xyz[2],
  //                                               x,y,z)) {
  //   std::cerr << "problem" << std::endl;
  //   return 1000000;
  // }
  // return getEuclideanDistance(x, y, z, goal_pose_.translation().x(), goal_pose_.translation().y(), goal_pose_.translation().z())*1000.0;
  // heur =  getEuclideanDistance(x, y, z, env_chain_config_.goal.xyz[0],env_chain_config_.goal.xyz[1], env_chain_config_.goal.xyz[2]) * prms_.cost_per_meter_;
  //}
}

bool EnvironmentChain3D::getGridXYZInt(const Eigen::Affine3d& pose,
                                       int(&xyz)[3]) const
{
  if(!gsr_ || !gsr_->dfce_->distance_field_) {
    ROS_WARN_STREAM("No distance field cache entry available");
    return false;
  }
  if(!gsr_->dfce_->distance_field_->worldToGrid(pose.translation().x(), 
                                                pose.translation().y(),
                                                pose.translation().z(),
                                                xyz[0],
                                                xyz[1],
                                                xyz[2])) {
    ROS_WARN_STREAM("Pose out of bounds");
    return false;
  }
  return true;
}

bool EnvironmentChain3D::populateTrajectoryFromStateIDSequence(const std::vector<int>& state_ids,
                                                               trajectory_msgs::JointTrajectory& traj) const
{
  traj.joint_names = joint_state_group_->getJointModelGroup()->getActiveDOFNames();
  std::vector<std::vector<double> > angle_vector;
  if(!planning_data_.convertFromStateIDsToAngles(state_ids,
                                                 angle_vector)) {
    return false;
  }
  traj.points.resize(angle_vector.size());
  for(unsigned int i = 0; i < angle_vector.size(); i++) {
    traj.points[i].positions = angle_vector[i];
  }
  for(unsigned int i = 0; i < traj.points.back().positions.size(); i++) {
    ROS_INFO_STREAM("Last " << i << " " << traj.points.back().positions[i]);
  }
  return true;
}

bool EnvironmentChain3D::getPlaneBFSMarker(visualization_msgs::Marker& plane_marker,
                                           double z_val)
{
  if(!gsr_ || !gsr_->dfce_ || !gsr_->dfce_->distance_field_) {
    return false;
  }
  plane_marker.header.frame_id = planning_scene_->getPlanningFrame();
  plane_marker.header.stamp = ros::Time::now();
  plane_marker.ns = "bfs_plane";
  plane_marker.id = 0;
  plane_marker.type = visualization_msgs::Marker::CUBE_LIST;
  plane_marker.action = visualization_msgs::Marker::ADD;
  plane_marker.points.resize(1);
  std_msgs::ColorRGBA wall_color;
  wall_color.r = wall_color.a = 1.0;
  plane_marker.colors.resize(1);
  plane_marker.colors[0] = wall_color;
  plane_marker.points[0].x = 1.0;
  plane_marker.pose.orientation.w = 1.0;
  plane_marker.color = wall_color;
  plane_marker.scale.x = plane_marker.scale.y = plane_marker.scale.z = gsr_->dfce_->distance_field_->getResolution();
  plane_marker.points.resize((gsr_->dfce_->distance_field_->getXNumCells()-2)*(gsr_->dfce_->distance_field_->getYNumCells()-2));
  plane_marker.colors.resize((gsr_->dfce_->distance_field_->getXNumCells()-2)*(gsr_->dfce_->distance_field_->getYNumCells()-2));
  //TODO - deal if 0,0 is not valid for x and y
  int x_val_temp, y_val_temp;
  int z_val_int;
  gsr_->dfce_->distance_field_->worldToGrid(0.0, 0.0, z_val, x_val_temp, y_val_temp, z_val_int); 
  std_msgs::ColorRGBA dist_color;
  dist_color.g = dist_color.a = 1.0;
  unsigned int count = 0;
  for(int i = 0; i < gsr_->dfce_->distance_field_->getXNumCells()-2; i++) {
    for(int j = 0; j < gsr_->dfce_->distance_field_->getYNumCells()-2; j++, count++) {
      gsr_->dfce_->distance_field_->gridToWorld(i, j, z_val_int, 
                                                plane_marker.points[count].x,
                                                plane_marker.points[count].y,
                                                plane_marker.points[count].z);
      //ROS_INFO_STREAM("Point " << count << " point " << plane_marker.points[count]);
      if(bfs_->isWall(i, j, z_val_int)) {
        plane_marker.colors[count] = wall_color;
        } else {
        int dist = bfs_->getDistance(i,j, z_val_int);
        if(dist < 40) {
          plane_marker.colors[count] = dist_color;
          plane_marker.colors[count].g = (40-dist)/40.0;
        }
      }
    }
  }      
  return true;
}
                        

}


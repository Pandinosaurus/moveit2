/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2018 Pilz GmbH & Co. KG
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
 *   * Neither the name of Pilz GmbH & Co. KG nor the names of its
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

#include <pilz_industrial_motion_planner/command_list_manager.h>

#include <rclcpp/logger.hpp>
#include <rclcpp/logging.hpp>
#include <cassert>
#include <functional>
#include <sstream>

#include <moveit/planning_pipeline/planning_pipeline.h>
#include <moveit/robot_state/conversions.h>
#include <moveit/utils/logger.hpp>

#include "cartesian_limits_parameters.hpp"
#include <pilz_industrial_motion_planner/joint_limits_aggregator.h>
#include <pilz_industrial_motion_planner/tip_frame_getter.h>
#include <pilz_industrial_motion_planner/trajectory_blend_request.h>
#include <pilz_industrial_motion_planner/trajectory_blender_transition_window.h>

namespace pilz_industrial_motion_planner
{
namespace
{
const std::string PARAM_NAMESPACE_LIMITS = "robot_description_planning";
rclcpp::Logger getLogger()
{
  return moveit::getLogger("moveit.planners.pilz.command_list_manager");
}
}  // namespace

CommandListManager::CommandListManager(const rclcpp::Node::SharedPtr& node,
                                       const moveit::core::RobotModelConstPtr& model)
  : node_(node), model_(model)
{
  // Obtain the aggregated joint limits
  pilz_industrial_motion_planner::JointLimitsContainer aggregated_limit_active_joints;

  aggregated_limit_active_joints = pilz_industrial_motion_planner::JointLimitsAggregator::getAggregatedLimits(
      node_, PARAM_NAMESPACE_LIMITS, model_->getActiveJointModels());

  param_listener_ =
      std::make_shared<cartesian_limits::ParamListener>(node, PARAM_NAMESPACE_LIMITS + ".cartesian_limits");
  params_ = param_listener_->get_params();

  pilz_industrial_motion_planner::LimitsContainer limits;
  limits.setJointLimits(aggregated_limit_active_joints);
  limits.setCartesianLimits(params_);

  plan_comp_builder_.setModel(model);
  plan_comp_builder_.setBlender(std::unique_ptr<pilz_industrial_motion_planner::TrajectoryBlender>(
      new pilz_industrial_motion_planner::TrajectoryBlenderTransitionWindow(limits)));
}

RobotTrajCont CommandListManager::solve(const planning_scene::PlanningSceneConstPtr& planning_scene,
                                        const planning_pipeline::PlanningPipelinePtr& planning_pipeline,
                                        const moveit_msgs::msg::MotionSequenceRequest& req_list)
{
  if (req_list.items.empty())
  {
    return RobotTrajCont();
  }

  checkForNegativeRadii(req_list);
  checkLastBlendRadiusZero(req_list);
  checkStartStates(req_list);

  MotionResponseCont resp_cont{ solveSequenceItems(planning_scene, planning_pipeline, req_list) };

  assert(model_);
  RadiiCont radii{ extractBlendRadii(*model_, req_list) };
  checkForOverlappingRadii(resp_cont, radii);

  plan_comp_builder_.reset();
  for (MotionResponseCont::size_type i = 0; i < resp_cont.size(); ++i)
  {
    plan_comp_builder_.append(planning_scene, resp_cont.at(i).trajectory,
                              // The blend radii has to be "attached" to
                              // the second part of a blend trajectory,
                              // therefore: "i-1".
                              (i > 0 ? radii.at(i - 1) : 0.));
  }

  const auto res_vec = plan_comp_builder_.build();

  // De-duplicate trajectory points with the same time value.
  // This is necessary since some controllers do not allow times that are not monotonically increasing.
  // TODO: Ideally, we would not need this code if the trajectory segments were created without
  // duplicate time points in the first place. Leaving this note to revisit this with a more principled fix.
  for (const auto& traj : res_vec)
  {
    for (size_t i = 0; i < traj->size() - 1; ++i)
    {
      if (traj->getWayPointDurationFromStart(i) == traj->getWayPointDurationFromStart(i + 1))
      {
        RCLCPP_WARN(getLogger(), "Removed duplicate point at time=%f", traj->getWayPointDurationFromStart(i));
        traj->removeWayPoint(i + 1);
      }
    }
  }

  return res_vec;
}

bool CommandListManager::checkRadiiForOverlap(const robot_trajectory::RobotTrajectory& traj_A, const double radii_A,
                                              const robot_trajectory::RobotTrajectory& traj_B,
                                              const double radii_B) const
{
  // No blending between trajectories from different groups
  if (traj_A.getGroupName() != traj_B.getGroupName())
  {
    return false;
  }

  auto sum_radii{ radii_A + radii_B };
  if (sum_radii == 0.)
  {
    return false;
  }

  const std::string& blend_frame{ getSolverTipFrame(model_->getJointModelGroup(traj_A.getGroupName())) };
  auto distance_endpoints = (traj_A.getLastWayPoint().getFrameTransform(blend_frame).translation() -
                             traj_B.getLastWayPoint().getFrameTransform(blend_frame).translation())
                                .norm();
  return distance_endpoints <= sum_radii;
}

void CommandListManager::checkForOverlappingRadii(const MotionResponseCont& resp_cont, const RadiiCont& radii) const
{
  if (resp_cont.empty())
  {
    return;
  }
  if (resp_cont.size() < 3)
  {
    return;
  }

  for (MotionResponseCont::size_type i = 0; i < resp_cont.size() - 2; ++i)
  {
    if (checkRadiiForOverlap(*(resp_cont.at(i).trajectory), radii.at(i), *(resp_cont.at(i + 1).trajectory),
                             radii.at(i + 1)))
    {
      std::ostringstream os;
      os << "Overlapping blend radii between command [" << i << "] and [" << i + 1 << "].";
      throw OverlappingBlendRadiiException(os.str());
    }
  }
}

CommandListManager::RobotState_OptRef
CommandListManager::getPreviousEndState(const MotionResponseCont& motion_plan_responses, const std::string& group_name)
{
  for (MotionResponseCont::const_reverse_iterator it = motion_plan_responses.crbegin();
       it != motion_plan_responses.crend(); ++it)
  {
    if (it->trajectory->getGroupName() == group_name)
    {
      return std::reference_wrapper(it->trajectory->getLastWayPoint());
    }
  }
  return {};
}

void CommandListManager::setStartState(const MotionResponseCont& motion_plan_responses, const std::string& group_name,
                                       moveit_msgs::msg::RobotState& start_state)
{
  RobotState_OptRef rob_state_op{ getPreviousEndState(motion_plan_responses, group_name) };
  if (rob_state_op)
  {
    moveit::core::robotStateToRobotStateMsg(rob_state_op.value(), start_state);
  }
}

bool CommandListManager::isInvalidBlendRadii(const moveit::core::RobotModel& model,
                                             const moveit_msgs::msg::MotionSequenceItem& item_A,
                                             const moveit_msgs::msg::MotionSequenceItem& item_B)
{
  // Zero blend radius is always valid
  if (item_A.blend_radius == 0.)
  {
    return false;
  }

  // No blending between different groups
  if (item_A.req.group_name != item_B.req.group_name)
  {
    RCLCPP_WARN_STREAM(getLogger(), "Blending between different groups (in this case: \""
                                        << item_A.req.group_name << "\" and \"" << item_B.req.group_name
                                        << "\") not allowed");
    return true;
  }

  // No blending for groups without solver
  if (!hasSolver(model.getJointModelGroup(item_A.req.group_name)))
  {
    RCLCPP_WARN_STREAM(getLogger(), "Blending for groups without solver not allowed");
    return true;
  }

  return false;
}

CommandListManager::RadiiCont
CommandListManager::extractBlendRadii(const moveit::core::RobotModel& model,
                                      const moveit_msgs::msg::MotionSequenceRequest& req_list)
{
  RadiiCont radii(req_list.items.size(), 0.);
  for (RadiiCont::size_type i = 0; i < (radii.size() - 1); ++i)
  {
    if (isInvalidBlendRadii(model, req_list.items.at(i), req_list.items.at(i + 1)))
    {
      RCLCPP_WARN_STREAM(getLogger(), "Invalid blend radii between commands: [" << i << "] and [" << i + 1
                                                                                << "] => Blend radii set to zero");
      continue;
    }
    radii.at(i) = req_list.items.at(i).blend_radius;
  }
  return radii;
}

CommandListManager::MotionResponseCont
CommandListManager::solveSequenceItems(const planning_scene::PlanningSceneConstPtr& planning_scene,
                                       const planning_pipeline::PlanningPipelinePtr& planning_pipeline,
                                       const moveit_msgs::msg::MotionSequenceRequest& req_list) const
{
  MotionResponseCont motion_plan_responses;
  size_t curr_req_index{ 0 };
  const size_t num_req{ req_list.items.size() };
  for (const auto& seq_item : req_list.items)
  {
    planning_interface::MotionPlanRequest req{ seq_item.req };
    setStartState(motion_plan_responses, req.group_name, req.start_state);

    planning_interface::MotionPlanResponse res;
    if (!planning_pipeline->generatePlan(planning_scene, req, res))
    {
      RCLCPP_ERROR(getLogger(), "Generating a plan with planning pipeline failed.");
      res.error_code.val = moveit_msgs::msg::MoveItErrorCodes::FAILURE;
    }
    if (res.error_code.val != res.error_code.SUCCESS)
    {
      std::ostringstream os;
      os << "Could not solve request\n";  // TODO(henning): re-enable "---\n" << req << "\n---\n";
      throw PlanningPipelineException(os.str(), res.error_code.val);
    }
    motion_plan_responses.emplace_back(res);
    RCLCPP_DEBUG_STREAM(getLogger(), "Solved [" << ++curr_req_index << '/' << num_req << ']');
  }
  return motion_plan_responses;
}

void CommandListManager::checkForNegativeRadii(const moveit_msgs::msg::MotionSequenceRequest& req_list)
{
  if (!std::all_of(req_list.items.begin(), req_list.items.end(),
                   [](const moveit_msgs::msg::MotionSequenceItem& req) { return (req.blend_radius >= 0.); }))
  {
    throw NegativeBlendRadiusException("All blending radii MUST be non negative");
  }
}

void CommandListManager::checkStartStatesOfGroup(const moveit_msgs::msg::MotionSequenceRequest& req_list,
                                                 const std::string& group_name)
{
  bool first_elem{ true };
  for (const moveit_msgs::msg::MotionSequenceItem& item : req_list.items)
  {
    if (item.req.group_name != group_name)
    {
      continue;
    }

    if (first_elem)
    {
      first_elem = false;
      continue;
    }

    if (!(item.req.start_state.joint_state.position.empty() && item.req.start_state.joint_state.velocity.empty() &&
          item.req.start_state.joint_state.effort.empty() && item.req.start_state.joint_state.name.empty()))
    {
      std::ostringstream os;
      os << "Only the first request is allowed to have a start state, but"
         << " the requests for group: \"" << group_name << "\" violate the rule";
      throw StartStateSetException(os.str());
    }
  }
}

void CommandListManager::checkStartStates(const moveit_msgs::msg::MotionSequenceRequest& req_list)
{
  if (req_list.items.size() <= 1)
  {
    return;
  }

  GroupNamesCont group_names{ getGroupNames(req_list) };
  for (const auto& curr_group_name : group_names)
  {
    checkStartStatesOfGroup(req_list, curr_group_name);
  }
}

CommandListManager::GroupNamesCont
CommandListManager::getGroupNames(const moveit_msgs::msg::MotionSequenceRequest& req_list)
{
  GroupNamesCont group_names;
  std::for_each(req_list.items.cbegin(), req_list.items.cend(),
                [&group_names](const moveit_msgs::msg::MotionSequenceItem& item) {
                  if (std::find(group_names.cbegin(), group_names.cend(), item.req.group_name) == group_names.cend())
                  {
                    group_names.emplace_back(item.req.group_name);
                  }
                });
  return group_names;
}

}  // namespace pilz_industrial_motion_planner

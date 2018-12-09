#include <eband_local_planner/eband_local_planner_ros.h>

#include <string>
#include <vector>

#include <pluginlib/class_list_macros.h>

#include <nav_core/base_local_planner.h>

PLUGINLIB_DECLARE_CLASS(eband_local_planner, EBandPlannerROS, eband_local_planner::EBandPlannerROS,
                        nav_core::BaseLocalPlanner)

namespace eband_local_planner
{

EBandPlannerROS::EBandPlannerROS() : costmap_ros_(nullptr), tf_buffer_(nullptr), goal_reached_(false)
{
}

EBandPlannerROS::~EBandPlannerROS()
{
}

void EBandPlannerROS::initialize(std::string name, tf2_ros::Buffer* tf_buffer, costmap_2d::Costmap2DROS* costmap_ros)
{
    costmap_ros_ = costmap_ros;
    tf_buffer_ = tf_buffer;

    ros::NodeHandle gn;
    ros::NodeHandle pn("~/" + name);

    plan_pub_ = pn.advertise<nav_msgs::Path>("plan", 1);

    const int num_optim_iterations = pn.param("num_iterations_eband_optimization", 3);
    const double internal_force_gain = pn.param("eband_internal_force_gain", 1.0);
    const double external_force_gain = pn.param("eband_external_force_gain", 2.0);
    const double tiny_bubble_distance = pn.param("eband_tiny_bubble_distance", 0.01);
    const double tiny_bubble_expansion = pn.param("eband_tiny_bubble_expansion", 0.01);
    const double min_bubble_overlap = pn.param("eband_min_relative_bubble_overlap", 0.7);
    const int equilibrium_max_recursion_depth = pn.param("eband_equilibrium_approx_max_recursion_depth", 4);
    const double equilibrium_relative_overshoot = pn.param("eband_equilibrium_relative_overshoot", 0.75);
    const double significant_force = pn.param("eband_significant_force_lower_bound", 0.15);
    const double costmap_weight = pn.param("costmap_weight", 10.0);

    ROS_INFO_STREAM("tiny_bubble_distance: " << tiny_bubble_distance);
    ROS_INFO_STREAM("tiny_bubble_expansion: " << tiny_bubble_expansion);

    eband_ = std::shared_ptr<EBandPlanner>(new EBandPlanner(
        costmap_ros_, num_optim_iterations, internal_force_gain, external_force_gain, tiny_bubble_distance,
        tiny_bubble_expansion, min_bubble_overlap, equilibrium_max_recursion_depth, equilibrium_relative_overshoot,
        significant_force, costmap_weight));

    const double max_vel_lin = pn.param("max_vel_lin", 0.75);
    const double max_vel_th = pn.param("max_vel_th", 1.0);
    const double min_vel_lin = pn.param("min_vel_lin", 0.1);
    const double min_vel_th = pn.param("min_vel_th", 0.0);
    const double min_in_place_vel_th = pn.param("min_in_place_vel_th", 0.0);
    const double in_place_trans_vel = pn.param("in_place_trans_vel", 0.0);
    const double xy_goal_tolerance = pn.param("xy_goal_tolerance", 0.1);
    const double yaw_goal_tolerance = pn.param("yaw_goal_tolerance", 0.05);
    const double k_prop = pn.param("k_prop", 4.0);
    const double k_damp = pn.param("k_damp", 3.5);
    const double ctrl_rate = pn.param("ctrl_rate", 10.0);
    const double max_acceleration = pn.param("max_acceleration", 0.5);
    const double virtual_mass = pn.param("virtual_mass", 0.75);
    const double max_translational_acceleration = pn.param("max_translational_acceleration", 0.5);
    const double max_rotational_acceleration = pn.param("max_rotational_acceleration", 1.5);
    const double rotation_correction_threshold = pn.param("rotation_correction_threshold", 0.5);

    eband_trj_ctrl_ = std::shared_ptr<EBandTrajectoryCtrl>(new EBandTrajectoryCtrl(
        costmap_ros_, max_vel_lin, max_vel_th, min_vel_lin, min_vel_th, min_in_place_vel_th, in_place_trans_vel,
        xy_goal_tolerance, yaw_goal_tolerance, k_prop, k_damp, ctrl_rate, max_acceleration, virtual_mass,
        max_translational_acceleration, max_rotational_acceleration, rotation_correction_threshold));

    eband_visual_ = std::shared_ptr<EBandVisualization>(new EBandVisualization(pn, costmap_ros));
    eband_->setVisualization(eband_visual_);
    eband_trj_ctrl_->setVisualization(eband_visual_);

    ROS_DEBUG("Elastic Band plugin initialized");
}

nav_core::Control EBandPlannerROS::computeControl(const ros::SteadyTime& steady_time, const ros::Time& ros_time,
                                                  const nav_msgs::Odometry& odom)
{
    // instantiate local variables
    geometry_msgs::PoseStamped global_pose_msg;
    std::vector<geometry_msgs::PoseStamped> tmp_plan;

    nav_core::Control result;

    // get current robot position
    ROS_DEBUG("Reading current robot Position from costmap and appending it to elastic band.");
    if (!costmap_ros_->getRobotPose(global_pose_msg))
    {
        ROS_WARN("Could not retrieve up to date robot pose from costmap for local planning.");
        result.state = nav_core::ControlState::FAILED;
        return result;
    }

    // convert robot pose to frame in plan and set position in band at which to append
    tmp_plan.assign(1, global_pose_msg);
    eband_local_planner::AddAtPosition add_frames_at = add_front;

    // set it to elastic band and let eband connect it
    if (!eband_->addFrames(tmp_plan, add_frames_at))
    {
        ROS_WARN("Could not connect robot pose to existing elastic band.");
        result.state = nav_core::ControlState::FAILED;
        return result;
    }

    // get additional path-frames which are now in moving window
    ROS_DEBUG("Checking for new path frames in moving window");
    std::vector<int> plan_start_end_counter = plan_start_end_counter_;
    std::vector<geometry_msgs::PoseStamped> append_transformed_plan;

    // transform global plan to the map frame we are working in - careful this also cuts the plan off (reduces it to
    // local window)
    if (!eband_local_planner::transformGlobalPlan(*tf_buffer_, global_plan_, *costmap_ros_,
                                                  costmap_ros_->getGlobalFrameID(), transformed_plan_,
                                                  plan_start_end_counter))
    {
        // if plan could not be transformed abort control and local planning
        ROS_WARN("Could not transform the global plan to the frame of the controller");

        ROS_INFO_STREAM("global_plan_.size(): " << global_plan_.size());
        ROS_INFO_STREAM("transformed_plan_.size(): " << transformed_plan_.size());
        result.state = nav_core::ControlState::FAILED;
        return result;
    }

    // also check if there really is a plan
    if (transformed_plan_.empty())
    {
        // if global plan passed in is empty... we won't do anything
        ROS_WARN("Transformed plan is empty. Aborting local planner!");
        ROS_INFO_STREAM("global_plan_.size(): " << global_plan_.size());
        ROS_INFO_STREAM("transformed_plan_.size(): " << transformed_plan_.size());
        result.state = nav_core::ControlState::FAILED;
        return result;
    }

    ROS_DEBUG("Retrieved start-end-counts are: (%d, %d)", plan_start_end_counter.at(0), plan_start_end_counter.at(1));
    ROS_DEBUG("Current start-end-counts are: (%d, %d)", plan_start_end_counter_.at(0), plan_start_end_counter_.at(1));

    // identify new frames - if there are any
    append_transformed_plan.clear();

    // did last transformed plan end further away from end of complete plan than this transformed plan?
    if (plan_start_end_counter_.at(1) >
        plan_start_end_counter.at(1))  // counting from the back (as start might be pruned)
    {
        // new frames in moving window
        if (plan_start_end_counter_.at(1) >
            plan_start_end_counter.at(0))  // counting from the back (as start might be pruned)
        {
            // append everything
            append_transformed_plan = transformed_plan_;
        }
        else
        {
            // append only the new portion of the plan
            int discarded_frames = plan_start_end_counter.at(0) - plan_start_end_counter_.at(1);
            ROS_ASSERT(transformed_plan_.begin() + discarded_frames + 1 >= transformed_plan_.begin());
            ROS_ASSERT(transformed_plan_.begin() + discarded_frames + 1 < transformed_plan_.end());
            append_transformed_plan.assign(transformed_plan_.begin() + discarded_frames + 1, transformed_plan_.end());
        }

        // set it to elastic band and let eband connect it
        ROS_DEBUG("Adding %d new frames to current band", (int)append_transformed_plan.size());
        add_frames_at = add_back;
        if (eband_->addFrames(append_transformed_plan, add_back))
        {
            // appended frames succesfully to global plan - set new start-end counts
            ROS_DEBUG("Sucessfully added frames to band");
            plan_start_end_counter_ = plan_start_end_counter;
        }
        else
        {
            ROS_WARN("Failed to add frames to existing band");
            result.state = nav_core::ControlState::FAILED;
            return result;
        }
    }
    else
        ROS_DEBUG("Nothing to add");

    // update Elastic Band (react on obstacle from costmap, ...)
    ROS_DEBUG("Calling optimization method for elastic band");
    std::vector<eband_local_planner::Bubble> current_band;
    if (!eband_->optimizeBand())
    {
        ROS_WARN("Optimization failed - Band invalid - No controls available");
        // display current band
        if (eband_->getBand(current_band))
            eband_visual_->publishBand("bubbles", current_band);
        result.state = nav_core::ControlState::FAILED;
        return result;
    }

    // get current Elastic Band and
    eband_->getBand(current_band);

    // set it to the controller
    if (!eband_trj_ctrl_->setBand(current_band))
    {
        ROS_DEBUG("Failed to to set current band to Trajectory Controller");
        result.state = nav_core::ControlState::FAILED;
        return result;
    }

    // set Odometry to controller
    if (!eband_trj_ctrl_->setOdometry(odom))
    {
        ROS_DEBUG("Failed to to set current odometry to Trajectory Controller");
        result.state = nav_core::ControlState::FAILED;
        return result;
    }

    // get resulting commands from the controller
    geometry_msgs::Twist cmd_twist;
    if (!eband_trj_ctrl_->getTwist(cmd_twist, goal_reached_))
    {
        ROS_DEBUG("Failed to calculate Twist from band in Trajectory Controller");
        result.state = nav_core::ControlState::FAILED;
        return result;
    }

    // set retrieved commands to reference variable
    ROS_DEBUG("Retrieving velocity command: (%f, %f, %f)", cmd_twist.linear.x, cmd_twist.linear.y, cmd_twist.angular.z);
    result.cmd_vel = cmd_twist;

    // publish plan
    std::vector<geometry_msgs::PoseStamped> refined_plan;
    if (eband_->getPlan(refined_plan))
    {
        nav_msgs::Path gui_path;
        gui_path.header.frame_id = refined_plan[0].header.frame_id;
        gui_path.header.stamp = refined_plan[0].header.stamp;
        gui_path.poses = refined_plan;
        plan_pub_.publish(gui_path);
    }

    // display current band
    if (eband_->getBand(current_band))
    {
        eband_visual_->publishBand("bubbles", current_band);
    }

    result.state = nav_core::ControlState::RUNNING;
    return result;
}

bool EBandPlannerROS::setPlan(const std::vector<geometry_msgs::PoseStamped>& orig_global_plan)
{
    // Reset the global plan
    global_plan_ = orig_global_plan;

    // transform global plan to the map frame we are working in this also cuts the plan off (reduces it to local window)
    std::vector<int> start_end_counts(2, static_cast<int>(global_plan_.size()));  // counts from the end() of the plan
    if (!eband_local_planner::transformGlobalPlan(*tf_buffer_, global_plan_, *costmap_ros_,
                                                  costmap_ros_->getGlobalFrameID(), transformed_plan_,
                                                  start_end_counts))
    {
        // if plan could not be tranformed abort control and local planning
        ROS_WARN("Could not transform the global plan to the frame of the controller");
        return false;
    }

    // also check if there really is a plan
    if (transformed_plan_.empty())
    {
        // if global plan passed in is empty... we won't do anything
        ROS_WARN("Transformed plan is empty. Aborting local planner!");
        ROS_INFO_STREAM("global_plan_.size(): " << global_plan_.size());
        ROS_INFO_STREAM("transformed_plan_.size(): " << transformed_plan_.size());
        return false;
    }

    // set plan - as this is fresh from the global planner robot pose should be identical to start frame
    if (!eband_->setPlan(transformed_plan_))
    {
        ROS_WARN("Eband local planner detected collision");
        return false;
    }

    // plan transformed and set to elastic band successfully - set counters to global variable
    plan_start_end_counter_ = start_end_counts;

    // display result
    std::vector<eband_local_planner::Bubble> current_band;
    if (eband_->getBand(current_band))
    {
        eband_visual_->publishBand("bubbles", current_band);
    }

    // let eband refine the plan before starting continuous operation (to smooth sampling based plans)
    eband_->optimizeBand();

    // display result
    if (eband_->getBand(current_band))
    {
        eband_visual_->publishBand("bubbles", current_band);
    }

    // set goal as not reached
    goal_reached_ = false;

    return true;
}

bool EBandPlannerROS::clearPlan()
{
    return true;
}

}  // namespace eband_local_planner

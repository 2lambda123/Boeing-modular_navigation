#ifndef AUTONOMY_H
#define AUTONOMY_H

#include <actionlib/server/simple_action_server.h>
#include <autonomy/DriveAction.h>
#include <geometry_msgs/PoseStamped.h>
#include <gridmap/layered_map.h>
#include <nav_msgs/Odometry.h>
#include <navigation_interface/controller.h>
#include <navigation_interface/path_planner.h>
#include <navigation_interface/trajectory_planner.h>
#include <pluginlib/class_loader.h>
#include <ros/ros.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace autonomy
{

template <typename T> T get_param_with_default_warn(const std::string& param_name, const T& default_val)
{
    T param_val;
    if (ros::param::has(param_name))
    {
        if (ros::param::get(param_name, param_val))
        {
            return param_val;
        }
    }
    ROS_WARN_STREAM("Using default value for " << param_name << "=" << default_val);
    return default_val;
}

template <typename T> T get_param_or_throw(const std::string& param_name)
{
    T param_val;
    if (ros::param::has(param_name))
    {
        if (ros::param::get(param_name, param_val))
        {
            return param_val;
        }
    }
    throw std::runtime_error("Must specify: " + param_name);
}

struct RobotState
{
    ros::SteadyTime time;
    navigation_interface::KinodynamicState robot_state;

    bool localised;  // true if map_to_odom is valid
    Eigen::Isometry2d map_to_odom;
};

struct ControlTrajectory
{
    bool goal_trajectory;
    navigation_interface::Trajectory trajectory;
};

struct TrackingPath
{
    // transformed goal in map frame
    Eigen::Isometry2d goal;

    ros::SteadyTime start_time;
    double start_cost;

    // re-calculation of cost
    ros::SteadyTime last_successful_time;
    double last_successful_cost;

    navigation_interface::Path path;
};

class Autonomy
{
  public:
    typedef actionlib::ServerGoalHandle<autonomy::DriveAction> GoalHandle;

    Autonomy();
    virtual ~Autonomy();

  private:
    void activeMapCallback(const hd_map::MapInfo::ConstPtr& map);

    void executionThread();
    void executeGoal(GoalHandle& goal);

    void goalCallback(GoalHandle goal);
    void cancelCallback(GoalHandle goal);

    std::unique_ptr<Eigen::Isometry2d> transformGoal(const Eigen::Isometry2d& goal, const std::string& frame_id);

    void pathPlannerThread(const Eigen::Isometry2d& goal, const std::string& frame_id);
    void trajectoryPlannerThread();
    void controllerThread();

    ros::NodeHandle nh_;

    std::shared_ptr<tf2_ros::Buffer> tf_buffer_;
    tf2_ros::TransformListener tf_listener_;

    std::mutex goal_mutex_;
    std::unique_ptr<GoalHandle> goal_;
    std::condition_variable execution_condition_;
    std::thread execution_thread_;
    actionlib::ActionServer<autonomy::DriveAction> as_;

    pluginlib::ClassLoader<gridmap::Layer> layer_loader_;
    pluginlib::ClassLoader<navigation_interface::PathPlanner> pp_loader_;
    pluginlib::ClassLoader<navigation_interface::TrajectoryPlanner> tp_loader_;
    pluginlib::ClassLoader<navigation_interface::Controller> c_loader_;

    std::shared_ptr<navigation_interface::PathPlanner> path_planner_;
    std::shared_ptr<navigation_interface::TrajectoryPlanner> trajectory_planner_;
    std::shared_ptr<navigation_interface::Controller> controller_;

    std::shared_ptr<gridmap::LayeredMap> layered_map_;

    ros::Subscriber active_map_sub_;

    ros::Publisher costmap_publisher_;
    ros::Publisher costmap_updates_publisher_;

    ros::Publisher current_goal_pub_;
    ros::Publisher vel_pub_;

    ros::Publisher path_pub_;
    ros::Publisher trajectory_pub_;

    std::atomic<bool> running_;
    std::atomic<bool> execution_thread_running_;

    std::unique_ptr<std::thread> path_planner_thread_;
    std::unique_ptr<std::thread> trajectory_planner_thread_;
    std::unique_ptr<std::thread> controller_thread_;

    std::atomic<bool> controller_done_;

    std::unique_ptr<TrackingPath> current_path_;
    std::unique_ptr<ControlTrajectory> current_trajectory_;

    std::mutex path_mutex_;
    std::mutex trajectory_mutex_;

    // Configuration
    const double map_publish_frequency_;

    const std::string global_frame_;

    const double clear_radius_;

    const double path_planner_frequency_;
    const double trajectory_planner_frequency_;
    const double controller_frequency_;
    const double path_swap_fraction_;
    const double localisation_timeout_;
    const double path_persistence_time_ = 6.0;

    std::mutex robot_state_mutex_;
    std::condition_variable robot_state_conditional_;
    RobotState robot_state_;
    ros::Subscriber odom_sub_;
    void odomCallback(const nav_msgs::Odometry::ConstPtr& msg);
};

}  // namespace autonomy

#endif
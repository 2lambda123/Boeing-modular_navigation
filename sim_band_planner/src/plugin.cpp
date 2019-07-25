#include <sim_band_planner/plugin.h>

#include <navigation_interface/params.h>

#include <pluginlib/class_list_macros.h>

PLUGINLIB_EXPORT_CLASS(sim_band_planner::SimBandPlanner, navigation_interface::TrajectoryPlanner)

namespace sim_band_planner
{

SimBandPlanner::SimBandPlanner() : viz_(nullptr)
{
}

SimBandPlanner::~SimBandPlanner()
{
}

bool SimBandPlanner::setPath(const navigation_interface::Path& path)
{
    if (path.nodes.empty())
        return false;

    moving_window_.reset(new MovingWindow(path));

    return true;
}

void SimBandPlanner::clearPath()
{
    moving_window_.reset();
}

boost::optional<std::string> SimBandPlanner::pathId() const
{
    if (moving_window_)
        return boost::optional<std::string>(moving_window_->nominal_path.id);
    else
        return {};
}

boost::optional<navigation_interface::Path> SimBandPlanner::path() const
{
    if (moving_window_)
        return boost::optional<navigation_interface::Path>(moving_window_->nominal_path);
    else
        return {};
}

navigation_interface::TrajectoryPlanner::Result
    SimBandPlanner::plan(const gridmap::AABB& local_region, const navigation_interface::KinodynamicState& robot_state,
                         const Eigen::Isometry2d& map_to_odom)
{
    navigation_interface::TrajectoryPlanner::Result result;

    if (!moving_window_)
    {
        result.outcome = navigation_interface::TrajectoryPlanner::Outcome::FAILED;
        return result;
    }

    try
    {
        // Take a window of the planning scene at the robot pose
        const Eigen::Isometry2d robot_pose = map_to_odom * robot_state.pose;

        moving_window_->updateWindow(robot_pose, max_window_length_);

        gridmap::Grid2D<uint8_t> local_grid(map_data_->grid, local_region);

        // Construct and cv::Mat from the costmap data
        const cv::Mat cv_im = cv::Mat(local_grid.dimensions().size().y(), local_grid.dimensions().size().x(), CV_8U,
                                      reinterpret_cast<void*>(local_grid.cells().data()));

        DistanceField distance_field(cv_im, local_grid.dimensions().origin().x(), local_grid.dimensions().origin().y(),
                                     local_grid.dimensions().resolution(), robot_radius_);

        Band sim_band;

        // Add the robot position to the front of the band
        sim_band.nodes.push_back(Node(robot_pose));

        // The first moving window node is the previous segment (exclude from optimization)
        if (moving_window_->window.nodes.size() > 1)
            sim_band.nodes.insert(sim_band.nodes.end(), moving_window_->window.nodes.begin() + 1,
                                  moving_window_->window.nodes.end());
        else
            sim_band.nodes.push_back(moving_window_->window.nodes.front());

        bool long_path = true;
        bool reverse_direction = false;
        const double path_length = moving_window_->nominal_path.length();
        const auto goal_wrt_robot = robot_pose.inverse() * moving_window_->nominal_path.nodes.back();
        const double rotation = std::abs(Eigen::Rotation2Dd(goal_wrt_robot.linear()).smallestAngle());

        if (path_length < max_holonomic_distance_)
        {
            long_path = false;
        }
        else if (goal_wrt_robot.translation().x() < 0 && path_length < max_reverse_distance_ && rotation < M_PI / 2.0)
        {
            reverse_direction = true;
        }

        auto t0 = std::chrono::steady_clock::now();

        simulate(sim_band, distance_field, num_iterations_, min_overlap_, min_distance_, internal_force_gain_,
                 external_force_gain_, (long_path ? rotation_factor_ : 0.0), reverse_direction, velocity_decay_, 1.0,
                 alpha_decay_, max_distance_);

        std::cout
            << "simulate took: "
            << std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - t0).count()
            << std::endl;


        // debug viz
        if (viz_)
        {
            viz_->deleteAllMarkers();
            for (const auto& node : sim_band.nodes)
            {
                const Eigen::Affine3d p =
                    Eigen::Translation3d(node.pose.translation().x(), node.pose.translation().y(), 0.0) *
                    Eigen::AngleAxisd(Eigen::Rotation2Dd(node.pose.rotation()).angle(), Eigen::Vector3d::UnitZ());
                viz_->publishAxis(p, 0.1, 0.02);

                geometry_msgs::Point start;
                start.x = node.pose.translation().x() + node.control_points[node.closest_point].gradient.x() * node.control_points[node.closest_point].distance;
                start.y = node.pose.translation().y() + node.control_points[node.closest_point].gradient.y() * node.control_points[node.closest_point].distance;

                geometry_msgs::Point end;
                end.x = node.pose.translation().x();
                end.y = node.pose.translation().y();

                viz_->publishArrow(start, end, rviz_visual_tools::colors::RED);
            }

            for (size_t c = 0; c < sim_band.nodes.front().control_points.size(); c++)
            {
                const ControlPoint& cp = sim_band.nodes.front().control_points[c];
                const Eigen::Vector2d position = sim_band.nodes.front().pose.translation() + sim_band.nodes.front().pose.linear() * cp.offset;

                std_msgs::ColorRGBA ros_color;
                ros_color.a = 0.25f;
                if (cp.distance < 0)
                {
                    ros_color.r = 1.0f;
                }
                else if (cp.distance < 0.1)
                {
                    ros_color.r = 1.0f;
                    ros_color.g = 0.5f;
                }
                else if (cp.distance < 0.2)
                {
                    ros_color.r = 1.0f;
                    ros_color.g = 1.0f;
                }
                else
                {
                    ros_color.g = 1.0f;
                }

                geometry_msgs::Pose ros_pose;
                ros_pose.position.x = position.x();
                ros_pose.position.y = position.y();
                ros_pose.orientation.w = 1;

                viz_->publishCylinder(ros_pose, ros_color, 0.1, robot_radius_ * 2.0);
            }

            viz_->trigger();
        }

        // copy the nodes back to the moving window
        moving_window_->window.nodes.clear();
        moving_window_->window.nodes.insert(moving_window_->window.nodes.end(), sim_band.nodes.begin(),
                                            sim_band.nodes.end());

        // trim to valid
        result.outcome = navigation_interface::TrajectoryPlanner::Outcome::SUCCESSFUL;
        result.path_start_i = 0;
        result.path_end_i = moving_window_->end_i;
        for (std::size_t i = 0; i < sim_band.nodes.size(); ++i)
        {
            if (sim_band.nodes[i].control_points[sim_band.nodes[i].closest_point].distance < 0)
            {
                sim_band.nodes.erase(sim_band.nodes.begin() + i, sim_band.nodes.end());
                result.outcome = navigation_interface::TrajectoryPlanner::Outcome::PARTIAL;
                ROS_WARN_STREAM("Point: " << i << " of trajectory is in collision");

                // if the band is broken we might as well try again from nominal
                // on the next iteration the band will reset
                moving_window_->window.nodes.clear();
                result.path_end_i = moving_window_->end_i - (sim_band.nodes.size() - 1 - i);
                break;
            }
        }

        if (sim_band.nodes.empty())
        {
            result.outcome = navigation_interface::TrajectoryPlanner::Outcome::FAILED;
            return result;
        }

        Band splined;
        if (spline_ && sim_band.nodes.size() > 1)
        {
            Eigen::MatrixXd points(2, sim_band.nodes.size());
            for (std::size_t i = 0; i < sim_band.nodes.size(); ++i)
            {
                points(0, i) = sim_band.nodes[i].pose.translation().x();
                points(1, i) = sim_band.nodes[i].pose.translation().y();
            }

            Eigen::Spline<double, 2>::KnotVectorType chord_lengths;
            Eigen::ChordLengths(points, chord_lengths);

            const int degree = std::min(3, static_cast<int>(sim_band.nodes.size()) - 1);
            const Eigen::Spline<double, 2> spline =
                Eigen::SplineFitting<Eigen::Spline<double, 2>>::Interpolate(points, degree, chord_lengths);

            const int total_steps = sim_band.length() / (4 * map_data_->grid.dimensions().resolution());
            for (Eigen::DenseIndex i = 0; i < points.cols() - 1; ++i)
            {
                const double cl = chord_lengths(i);
                const double ncl = chord_lengths(i + 1);

                auto rot = Eigen::Rotation2Dd(sim_band.nodes[i].pose.rotation());
                auto next_rot = Eigen::Rotation2Dd(sim_band.nodes[i + 1].pose.rotation());

                const double diff = ncl - cl;
                const int steps = diff * total_steps;

                for (int j = 0; j < steps; ++j)
                {
                    const double fraction = static_cast<double>(j) / static_cast<double>(steps);
                    auto spline_p = spline(cl + fraction * diff);
                    const Eigen::Isometry2d p =
                        Eigen::Translation2d(spline_p.x(), spline_p.y()) * rot.slerp(fraction, next_rot);
                    auto node = Node(p);
                    splined.nodes.push_back(node);
                }
                splined.nodes.push_back(sim_band.nodes[i + 1]);
            }

            updateDistances(splined, distance_field, max_distance_);

            // check the splined path
            for (std::size_t i = 0; i < splined.nodes.size(); ++i)
            {
                if (splined.nodes[i].control_points[splined.nodes[i].closest_point].distance < 0)
                {
                    ROS_WARN("Splining failed");
                    result.outcome = navigation_interface::TrajectoryPlanner::Outcome::FAILED;
                    return result;
                }
            }
        }
        else
        {
            splined = sim_band;
        }

        // Transform to odom frame
        result.trajectory.header.frame_id = "odom";
        result.cost = 0;
        const Eigen::Isometry2d odom_to_map = map_to_odom.inverse();
        for (const auto& node : splined.nodes)
        {
            const double min_distance = node.control_points[node.closest_point].distance;
            const double velocity =
                desired_speed_ * std::max(0.2, min_distance >= max_distance_ ? 1.0 : 4.0 * min_distance);
            result.trajectory.states.push_back({odom_to_map * node.pose, Eigen::Vector3d(velocity, 0, 0)});
        }
    }
    catch (const std::exception& e)
    {
        ROS_ERROR_STREAM("Optimization failed: " << e.what());
        result.outcome = navigation_interface::TrajectoryPlanner::Outcome::FAILED;
        return result;
    }

    return result;
}

bool SimBandPlanner::valid(const navigation_interface::Trajectory&) const
{
    return true;
}

double SimBandPlanner::cost(const navigation_interface::Trajectory&) const
{
    return 0.0;
}

void SimBandPlanner::onInitialize(const XmlRpc::XmlRpcValue& parameters)
{
    debug_viz_ = navigation_interface::get_config_with_default_warn<bool>(parameters, "debug_viz", debug_viz_,
                                                                          XmlRpc::XmlRpcValue::TypeBoolean);
    num_iterations_ = navigation_interface::get_config_with_default_warn<int>(
        parameters, "num_iterations", num_iterations_, XmlRpc::XmlRpcValue::TypeInt);
    internal_force_gain_ = navigation_interface::get_config_with_default_warn<double>(
        parameters, "internal_force_gain", internal_force_gain_, XmlRpc::XmlRpcValue::TypeDouble);
    external_force_gain_ = navigation_interface::get_config_with_default_warn<double>(
        parameters, "external_force_gain", external_force_gain_, XmlRpc::XmlRpcValue::TypeDouble);
    min_distance_ = navigation_interface::get_config_with_default_warn<double>(
        parameters, "min_distance", min_distance_, XmlRpc::XmlRpcValue::TypeDouble);
    max_distance_ = navigation_interface::get_config_with_default_warn<double>(
        parameters, "max_distance", max_distance_, XmlRpc::XmlRpcValue::TypeDouble);
    min_overlap_ = navigation_interface::get_config_with_default_warn<double>(parameters, "min_overlap", min_overlap_,
                                                                              XmlRpc::XmlRpcValue::TypeDouble);
    max_window_length_ = navigation_interface::get_config_with_default_warn<double>(
        parameters, "max_window_length", max_window_length_, XmlRpc::XmlRpcValue::TypeDouble);
    max_holonomic_distance_ = navigation_interface::get_config_with_default_warn<double>(
        parameters, "max_holonomic_distance", max_holonomic_distance_, XmlRpc::XmlRpcValue::TypeDouble);
    max_reverse_distance_ = navigation_interface::get_config_with_default_warn<double>(
        parameters, "max_reverse_distance", max_reverse_distance_, XmlRpc::XmlRpcValue::TypeDouble);
    robot_radius_ = navigation_interface::get_config_with_default_warn<double>(
        parameters, "robot_radius", robot_radius_, XmlRpc::XmlRpcValue::TypeDouble);
    rotation_factor_ = navigation_interface::get_config_with_default_warn<double>(
        parameters, "rotation_factor", rotation_factor_, XmlRpc::XmlRpcValue::TypeDouble);
    velocity_decay_ = navigation_interface::get_config_with_default_warn<double>(
        parameters, "velocity_decay", velocity_decay_, XmlRpc::XmlRpcValue::TypeDouble);
    alpha_decay_ = navigation_interface::get_config_with_default_warn<double>(parameters, "alpha_decay", alpha_decay_,
                                                                              XmlRpc::XmlRpcValue::TypeDouble);
    desired_speed_ = navigation_interface::get_config_with_default_warn<double>(
        parameters, "desired_speed", desired_speed_, XmlRpc::XmlRpcValue::TypeDouble);
    spline_ = navigation_interface::get_config_with_default_warn<bool>(parameters, "spline", spline_,
                                                                       XmlRpc::XmlRpcValue::TypeBoolean);

    if (debug_viz_)
        viz_.reset(new rviz_visual_tools::RvizVisualTools("map", "debug"));
}

void SimBandPlanner::onMapDataChanged()
{
}
}

#include <omni_rrt_planner/omni_rrt_planner.h>
#include <navigation_interface/params.h>

#include <algorithm>

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <pluginlib/class_list_macros.h>

#include <ompl/geometric/PathSimplifier.h>

#include <ompl/control/planners/rrt/RRT.h>
#include <ompl/geometric/planners/rrt/RRTstar.h>
#include <ompl/geometric/planners/rrt/RRTConnect.h>
#include <ompl/geometric/planners/rrt/InformedRRTstar.h>
#include <ompl/geometric/planners/rrt/BiTRRT.h>

PLUGINLIB_DECLARE_CLASS(omni_rrt_planner, OmniRRTPlanner, omni_rrt_planner::OmniRRTPlanner, navigation_interface::PathPlanner)

namespace omni_rrt_planner
{

namespace
{

std::shared_ptr<Costmap> buildCostmap(const gridmap::MapData& map_data, const double robot_radius, const double exponential_weight)
{
    auto grid = std::make_shared<Costmap>();

    std::chrono::steady_clock::time_point t0;

    // downsample
    cv::Mat dilated;
    {
        cv::Mat obstacle_map;
        {
            auto lock = map_data.grid.getLock();

            grid->resolution = map_data.grid.dimensions().resolution();

            const int size_x = map_data.grid.dimensions().size().x();
            const int size_y = map_data.grid.dimensions().size().y();

            grid->origin_x = map_data.grid.dimensions().origin().x();
            grid->origin_y = map_data.grid.dimensions().origin().y();

            t0 = std::chrono::steady_clock::now();
            const cv::Mat raw(size_y, size_x, CV_8U, reinterpret_cast<void*>(const_cast<uint8_t*>(map_data.grid.cells().data())));
            obstacle_map = raw.clone();
        }

        // dilate robot radius
        t0 = std::chrono::steady_clock::now();
        const int cell_inflation_radius = static_cast<int>(2.0 * robot_radius / grid->resolution);
        auto ellipse =
            cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(cell_inflation_radius, cell_inflation_radius));
        cv::dilate(obstacle_map, dilated, ellipse);
        ROS_DEBUG_STREAM("dilation took " << std::chrono::duration_cast<std::chrono::duration<double>>(
                                                 std::chrono::steady_clock::now() - t0)
                                                 .count());
    }

    t0 = std::chrono::steady_clock::now();

    // flip
    cv::bitwise_not(dilated, dilated);

    // allocate
    grid->distance_to_collision = cv::Mat(dilated.size(), CV_32F);

    // find obstacle distances
    cv::distanceTransform(dilated, grid->distance_to_collision, cv::DIST_L2, cv::DIST_MASK_PRECISE, CV_32F);

    // inflate
    grid->cost = -exponential_weight * grid->distance_to_collision * map_data.grid.dimensions().resolution();

    // negative exponent maps values to [1,0)
    cv::exp(grid->cost, grid->cost);

    ROS_DEBUG_STREAM(
        "inflation took "
        << std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - t0).count());

    return grid;
}

}

OmniRRTPlanner::OmniRRTPlanner()
{
}

OmniRRTPlanner::~OmniRRTPlanner()
{

}

navigation_interface::PathPlanner::Result OmniRRTPlanner::plan(const Eigen::Isometry2d& start,
                                                               const Eigen::Isometry2d& goal)
{
    navigation_interface::PathPlanner::Result result;

    const auto costmap = buildCostmap(*map_data_, robot_radius_, exponential_weight_);

    //
    // Setup OMPL
    //
    se2_space_ = ompl::base::StateSpacePtr(new ompl::base::SE2StateSpace());
    se2_space_->setLongestValidSegmentFraction(0.006);
    si_ = ompl::base::SpaceInformationPtr(new ompl::base::SpaceInformation(se2_space_));
    si_->setStateValidityChecker(std::make_shared<ValidityChecker>(si_, costmap));

    //
    // Update XY sample bounds
    //
    const double search_window = std::max(map_data_->grid.dimensions().size().x() * map_data_->grid.dimensions().resolution(),
                                          map_data_->grid.dimensions().size().y() * map_data_->grid.dimensions().resolution()) / 2.0;

    ompl::base::RealVectorBounds bounds(2);
    bounds.setLow(0, -search_window);
    bounds.setHigh(0, +search_window);
    bounds.setLow(1, -search_window);
    bounds.setHigh(1, +search_window);
    se2_space_->as<ompl::base::SE2StateSpace>()->setBounds(bounds);

    // Optimize criteria
    ompl::base::OptimizationObjectivePtr cost_objective(new CostMapObjective(si_, costmap));
    ompl::base::OptimizationObjectivePtr length_objective(new ompl::base::PathLengthOptimizationObjective(si_));
    ompl::base::MultiOptimizationObjective* objective = new ompl::base::MultiOptimizationObjective(si_);
    // Highest cost is 1 so you would have 1 per m
//    objective->addObjective(cost_objective, 1.0);
    // Highest cost would be 1 per m
    objective->addObjective(length_objective, 1.0);
    objective_ = ompl::base::OptimizationObjectivePtr(objective);
//    const double distance = (goal.translation() - start.translation()).norm();
//    objective_->setCostThreshold(ompl::base::Cost(1.2 * distance));

//    auto cost_to_go = [](const ompl::base::State* state, const ompl::base::Goal* goal)
//    {
//        const auto* se2state = state->as<ompl::base::SE2StateSpace::StateType>();
//        const auto* se2goal = goal->as<ompl::base::SE2StateSpace::StateType>();
//        return ompl::base::Cost((Eigen::Vector2d(se2state->getX(), se2state->getY()) - Eigen::Vector2d(se2goal->getX(), se2goal->getY())).norm());
//    };

//    objective_->setCostToGoHeuristic(cost_to_go);

    // Define problem
    ompl::base::ScopedState<> ompl_start(se2_space_);
    ompl_start[0] = start.translation().x();
    ompl_start[1] = start.translation().y();
    ompl_start[2] = Eigen::Rotation2Dd(start.rotation()).smallestAngle();

    ompl::base::ScopedState<> ompl_goal(se2_space_);
    ompl_goal[0] = goal.translation().x();
    ompl_goal[1] = goal.translation().y();
    ompl_goal[2] = Eigen::Rotation2Dd(goal.rotation()).smallestAngle();

    ompl::base::ProblemDefinitionPtr pdef(new ompl::base::ProblemDefinition(si_));
    pdef->setOptimizationObjective(objective_);

    // A copy of the start and goal is made
    pdef->setStartAndGoalStates(ompl_start, ompl_goal, 0.01);

    ROS_INFO("Problem defined, running planner");
    auto rrt = new ompl::geometric::BiTRRT(si_);
//    rrt->setGoalBias(0.6);
    rrt->setRange(0.00);

    auto planner = ompl::base::PlannerPtr(rrt);
    planner->setProblemDefinition(pdef);
    planner->setup();

    ompl::base::PlannerStatus solved;

    solved = planner->solve(0.2);
    auto tc = ompl::base::timedPlannerTerminationCondition(2.0);
    solved = planner->solve(tc);

    auto pd = std::make_shared<ompl::base::PlannerData>(si_);
    planner->getPlannerData(*pd);

    if (rrt_viz_)
        visualisePlannerData(*pd);

    if (ompl::base::PlannerStatus::StatusType(solved) == ompl::base::PlannerStatus::EXACT_SOLUTION
            || ompl::base::PlannerStatus::StatusType(solved) == ompl::base::PlannerStatus::APPROXIMATE_SOLUTION)
    {
        result.outcome = navigation_interface::PathPlanner::Outcome::SUCCESSFUL;
        const double length = pdef->getSolutionPath()->length();

        ROS_INFO_STREAM(planner->getName()
                     << " found a solution of length " << length
                     << " with an optimization objective value of " << result.cost);

        ompl::base::PathPtr path_ptr = pdef->getSolutionPath();
        ompl::geometric::PathGeometric result_path = static_cast<ompl::geometric::PathGeometric&>(*path_ptr);

        ompl::geometric::PathSimplifier simplifier(si_);
        simplifier.simplify(result_path, 0.05);
        //simplifier.smoothBSpline(result_path, 5, 0.005);
        result_path.interpolate();
        const auto check_ret = result_path.checkAndRepair(1000);

        if (check_ret.second)
        {
            if (trajectory_viz_)
                visualisePathGeometric(result_path);

            result.cost = result_path.cost(pdef->getOptimizationObjective()).value();

            for (unsigned int i=0; i<result_path.getStateCount(); ++i)
            {
                const ompl::base::State* state = result_path.getState(i);
                const auto* se2state = state->as<ompl::base::SE2StateSpace::StateType>();

                const double x = se2state->getX();
                const double y = se2state->getY();
                const double theta = se2state->getYaw();

                const Eigen::Isometry2d p = Eigen::Translation2d(x, y) * Eigen::Rotation2Dd(theta);
                result.path.nodes.push_back(p);
            }
        }
        else
        {
            result.outcome = navigation_interface::PathPlanner::Outcome::FAILED;
            result.cost = 0;
        }
    }
    else
    {
        result.outcome = navigation_interface::PathPlanner::Outcome::FAILED;
        result.cost = 0;
    }

    return result;
}

bool OmniRRTPlanner::valid(const navigation_interface::Path& path) const
{
    // assume this is called immediatelty after plan to re-use the data structures
    ROS_ASSERT(si_);
    ROS_ASSERT(objective_);

    ompl::geometric::PathGeometric trajectory(si_);

    for (const auto& p : path.nodes)
    {
        ompl::base::SE2StateSpace::StateType state;
        state.setX(p.translation().x());
        state.setY(p.translation().y());
        state.setYaw(Eigen::Rotation2Dd(p.linear()).smallestAngle());
        trajectory.append(&state);
    }

    return trajectory.check();
}

double OmniRRTPlanner::cost(const navigation_interface::Path& path) const
{
    // assume this is called immediatelty after plan to re-use the data structures
    ROS_ASSERT(si_);
    ROS_ASSERT(objective_);

    ompl::geometric::PathGeometric trajectory(si_);

    for (const auto& p : path.nodes)
    {
        ompl::base::ScopedState<ompl::base::SE2StateSpace> state(si_);
        state->setX(p.translation().x());
        state->setY(p.translation().y());
        state->setYaw(Eigen::Rotation2Dd(p.linear()).smallestAngle());
        trajectory.append(state.get());
    }

    if (!trajectory.check())
    {
        ROS_INFO_STREAM("path is not valid: length: " << trajectory.length() << " cost: " << trajectory.cost(objective_) << " smoothness" << trajectory.smoothness());

//        for (size_t i = 1; i < trajectory.getStateCount(); ++i)
//        {
//            ROS_INFO_STREAM("state motion: " << si_->checkMotion(trajectory.getState(i-1), trajectory.getState(i)));
//        }

        const auto check_ret = trajectory.checkAndRepair(1000);

        ROS_INFO_STREAM("repaired: " << check_ret.second);

        if (!check_ret.second)
            return std::numeric_limits<double>::max();
    }

    return trajectory.cost(objective_).value();
}

void OmniRRTPlanner::onInitialize(const XmlRpc::XmlRpcValue& parameters)
{
    debug_viz_ = navigation_interface::get_config_with_default_warn<bool>(parameters, "debug_viz", debug_viz_, XmlRpc::XmlRpcValue::TypeBoolean);

    robot_radius_ = navigation_interface::get_config_with_default_warn<double>(parameters, "robot_radius", robot_radius_, XmlRpc::XmlRpcValue::TypeDouble);

    if (debug_viz_)
    {
        rrt_viz_.reset(new rviz_visual_tools::RvizVisualTools("map", "/rrt"));
        trajectory_viz_.reset(new rviz_visual_tools::RvizVisualTools("map", "/trajectory"));
    }
}

void OmniRRTPlanner::onMapDataChanged()
{
}

void OmniRRTPlanner::visualisePlannerData(const ompl::base::PlannerData& pd)
{
    rrt_viz_->deleteAllMarkers();

    const rviz_visual_tools::colors color = rviz_visual_tools::BLUE;
    const rviz_visual_tools::scales scale = rviz_visual_tools::XXSMALL;

    auto get_pose = [](const ompl::base::PlannerData& pd, unsigned int vertex_id)
    {
        const ompl::base::PlannerDataVertex& v = pd.getVertex(vertex_id);
        const ompl::base::State* state = v.getState();
        const auto* se2state = state->as<ompl::base::SE2StateSpace::StateType>();

        const double x = se2state->getX();
        const double y = se2state->getY();
        const double theta = se2state->getYaw();

        const Eigen::Quaterniond qt(Eigen::AngleAxisd(theta, Eigen::Vector3d::UnitZ()));

        geometry_msgs::Pose pose;
        pose.position.x = x;
        pose.position.y = y;
        pose.orientation.w = qt.w();
        pose.orientation.x = qt.x();
        pose.orientation.y = qt.y();
        pose.orientation.z = qt.z();

        return pose;
    };

    for (unsigned int i=0; i<pd.numVertices(); ++i)
    {
        const geometry_msgs::Pose pose = get_pose(pd, i);

        rrt_viz_->publishAxis(pose, scale);

        // Draw edges
        {
            std::vector<unsigned int> edge_list;
            pd.getEdges(i, edge_list);
            for (unsigned int e : edge_list)
            {
                const geometry_msgs::Pose e_pose = get_pose(pd, e);
                rrt_viz_->publishLine(pose.position, e_pose.position, color, scale);
            }
        }
    }

    rrt_viz_->trigger();
}

void OmniRRTPlanner::visualisePathGeometric(const ompl::geometric::PathGeometric& path)
{
    std::vector<Eigen::Isometry3d> poses;

    for (unsigned int i=0; i<path.getStateCount(); ++i)
    {
        const ompl::base::State* state = path.getState(i);
        const auto* se2state = state->as<ompl::base::SE2StateSpace::StateType>();

        const double x = se2state->getX();
        const double y = se2state->getY();
        const double theta = se2state->getYaw();

        const Eigen::Quaterniond qt(Eigen::AngleAxisd(theta, Eigen::Vector3d::UnitZ()));
        const Eigen::Translation3d tr(x, y, 0);
        const Eigen::Isometry3d pose = tr * qt;

        poses.push_back(pose);
    }

    trajectory_viz_->deleteAllMarkers();

    const rviz_visual_tools::colors color = rviz_visual_tools::RED;
    const rviz_visual_tools::scales scale = rviz_visual_tools::SMALL;

    for (unsigned int i=0; i<poses.size() - 1; ++i)
    {
        trajectory_viz_->publishAxis(poses[i], scale);
        trajectory_viz_->publishLine(poses[i].translation(), poses[i+1].translation(), color, scale);
    }
    if (!poses.empty())
        trajectory_viz_->publishAxis(poses.back(), scale);

    trajectory_viz_->trigger();
}

}

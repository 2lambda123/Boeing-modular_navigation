#ifndef OMNI_RRT_PLANNER_H
#define OMNI_RRT_PLANNER_H

#include <navigation_interface/path_planner.h>

#include <gridmap/operations/raytrace.h>

#include <Eigen/Geometry>

#include <atomic>
#include <mutex>
#include <memory>

#include <opencv2/core.hpp>

#include <ros/ros.h>

#include <rviz_visual_tools/rviz_visual_tools.h>

#include <ompl/base/objectives/MechanicalWorkOptimizationObjective.h>
#include <ompl/base/objectives/PathLengthOptimizationObjective.h>
#include <ompl/base/objectives/StateCostIntegralObjective.h>
#include <ompl/base/spaces/SE2StateSpace.h>
#include <ompl/control/SimpleSetup.h>

namespace omni_rrt_planner
{

struct Costmap
{
    cv::Mat distance_to_collision;
    cv::Mat cost;

    double resolution;
    double origin_x;
    double origin_y;
};

class OmniRRTPlanner : public navigation_interface::PathPlanner
{
  public:
    OmniRRTPlanner();
    virtual ~OmniRRTPlanner() override;

    virtual Result plan(const Eigen::Isometry2d& start, const Eigen::Isometry2d& goal) override;

    virtual bool valid(const navigation_interface::Path& path) const override;
    virtual double cost(const navigation_interface::Path& path) const override;

    virtual void onInitialize(const XmlRpc::XmlRpcValue& parameters) override;
    virtual void onMapDataChanged() override;

    // Visualisation
    void visualisePlannerData(const ompl::base::PlannerData& pd);
    void visualisePathGeometric(const ompl::geometric::PathGeometric& path);

  private:
    std::unique_ptr<rviz_visual_tools::RvizVisualTools> rrt_viz_;
    std::unique_ptr<rviz_visual_tools::RvizVisualTools> trajectory_viz_;

    bool debug_viz_ = true;
    double robot_radius_ = 0.5;
    double exponential_weight_ = 2.0;

    // OMPL data structures
    ompl::base::StateSpacePtr se2_space_;
    ompl::base::StateSpacePtr velocity_space_;
    ompl::base::StateSpacePtr space_;
    ompl::control::ControlSpacePtr cspace_;
    ompl::base::SpaceInformationPtr si_;
    ompl::base::OptimizationObjectivePtr objective_;
};

class ValidityChecker : public ompl::base::StateValidityChecker
 {
 public:
     ValidityChecker(const ompl::base::SpaceInformationPtr& si, const std::shared_ptr<const Costmap>& costmap) :
         ompl::base::StateValidityChecker(si),
         costmap_(costmap),
         offsets_({
                  {-0.268, 0.000},
                  { 0.268, 0.000},
                  { 0.265,-0.185},
                  { 0.077,-0.185},
                  {-0.077,-0.185},
                  {-0.265,-0.185},
                  { 0.265, 0.185},
                  {-0.265, 0.185},
                  {-0.077, 0.185},
                  { 0.077, 0.185}
                  })
     {
     }
     virtual ~ValidityChecker() override = default;

     bool isValid(const ompl::base::State* state) const override
     {
         return clearance(state) > 0.0;
     }

     double clearance(const ompl::base::State* state) const override
     {
         const auto* se2state = state->as<ompl::base::SE2StateSpace::StateType>();
         const double x = se2state->getX();
         const double y = se2state->getY();
         const double theta = se2state->getYaw();

         double min_distance = std::numeric_limits<double>::max();
         for (const auto& offset : offsets_)
         {
             const auto offset_xy = Eigen::Vector2d(x, y) + Eigen::Rotation2Dd(theta) * offset;

             const int mx = static_cast<int>((offset_xy.x() - costmap_->origin_x) / costmap_->resolution - 0.5);
             const int my = static_cast<int>((offset_xy.y() - costmap_->origin_y) / costmap_->resolution - 0.5);

             double distance = 0;
             if (mx >=0 && mx < costmap_->distance_to_collision.cols
                     && my >=0 && my < costmap_->distance_to_collision.rows)
             {
                 distance = static_cast<double>(costmap_->distance_to_collision.at<float>(my, mx));
             }
             min_distance = std::min(min_distance, distance);
         }
         return min_distance;
     }

  private:
     const std::shared_ptr<const Costmap> costmap_;
     const std::vector<Eigen::Vector2d> offsets_;
 };

/*
class MotionChecker : public ompl::base::MotionValidator
 {
 public:
     MotionChecker(const ompl::base::SpaceInformationPtr& si, const std::shared_ptr<const Costmap>& costmap) :
         ompl::base::MotionValidator(si),
         costmap_(costmap),
         offsets_({
                  {-0.268, 0.000},
                  { 0.268, 0.000},
                  { 0.265,-0.185},
                  { 0.077,-0.185},
                  {-0.077,-0.185},
                  {-0.265,-0.185},
                  { 0.265, 0.185},
                  {-0.265, 0.185},
                  {-0.077, 0.185},
                  { 0.077, 0.185}
                  })
     {
     }
     virtual ~MotionChecker() override = default;

     bool checkMotion(const ompl::base::State* s1, const ompl::base::State* s2) const override
     {
         const auto* s1_state = s1->as<ompl::base::SE2StateSpace::StateType>();
         const auto* s2_state = s2->as<ompl::base::SE2StateSpace::StateType>();

         const int s1_mx = static_cast<int>((s1_state->getX() - costmap_->origin_x) / costmap_->resolution - 0.5);
         const int s1_my = static_cast<int>((s1_state->getY() - costmap_->origin_y) / costmap_->resolution - 0.5);

         const int s2_mx = static_cast<int>((s2_state->getX() - costmap_->origin_x) / costmap_->resolution - 0.5);
         const int s2_my = static_cast<int>((s2_state->getY() - costmap_->origin_y) / costmap_->resolution - 0.5);

         const auto line = gridmap::drawLine(s1_mx, s1_my, s2_mx, s2_my);

         const Eigen::Rotation2Dd rot_1(s1_state->getYaw());
         const Eigen::Rotation2Dd rot_2(s2_state->getYaw());

         for (size_t t=0; t < line.size(); ++t)
         {
             const Eigen::Rotation2Dd r = rot_1.slerp(fraction, rot_2);
         }

     }

     double clearance(const ompl::base::State* state) const
     {
         const auto* se2state = state->as<ompl::base::SE2StateSpace::StateType>();
         const double x = se2state->getX();
         const double y = se2state->getY();
         const double theta = se2state->getYaw();

         double min_distance = std::numeric_limits<double>::max();
         for (const auto& offset : offsets_)
         {
             const auto offset_xy = Eigen::Vector2d(x, y) + Eigen::Rotation2Dd(theta) * offset;

             const int mx = static_cast<int>((offset_xy.x() - costmap_->origin_x) / costmap_->resolution - 0.5);
             const int my = static_cast<int>((offset_xy.y() - costmap_->origin_y) / costmap_->resolution - 0.5);

             double distance = 0;
             if (mx >=0 && mx < costmap_->distance_to_collision.cols
                     && my >=0 && my < costmap_->distance_to_collision.rows)
             {
                 distance = static_cast<double>(costmap_->distance_to_collision.at<float>(my, mx));
             }
             min_distance = std::min(min_distance, distance);
         }
         return min_distance;
     }

  private:
     const std::shared_ptr<const Costmap> costmap_;
     const std::vector<Eigen::Vector2d> offsets_;
 };
*/

class CostMapObjective : public ompl::base::StateCostIntegralObjective
{
  public:
    CostMapObjective(const ompl::base::SpaceInformationPtr& si, const std::shared_ptr<const Costmap>& costmap)
        : ompl::base::StateCostIntegralObjective(si, true),
          costmap_(costmap),
          offsets_({
                   {-0.268, 0.000},
                   { 0.268, 0.000},
                   { 0.265,-0.185},
                   { 0.077,-0.185},
                   {-0.077,-0.185},
                   {-0.265,-0.185},
                   { 0.265, 0.185},
                   {-0.265, 0.185},
                   {-0.077, 0.185},
                   { 0.077, 0.185}
                   })
    {
    }
    virtual ~CostMapObjective() override = default;

    virtual ompl::base::Cost stateCost(const ompl::base::State* s) const override
    {
        const auto* se2state = s->as<ompl::base::SE2StateSpace::StateType>();
        const double x = se2state->getX();
        const double y = se2state->getY();
        const double theta = se2state->getYaw();

        double max_cost = 0;
        for (const auto& offset : offsets_)
        {
            const auto offset_xy = Eigen::Vector2d(x, y) + Eigen::Rotation2Dd(theta) * offset;

            const int mx = static_cast<int>((offset_xy.x() - costmap_->origin_x) / costmap_->resolution - 0.5);
            const int my = static_cast<int>((offset_xy.y() - costmap_->origin_y) / costmap_->resolution - 0.5);

            double cost = 1.0;
            if (mx >=0 && mx < costmap_->cost.cols
                    && my >=0 && my < costmap_->cost.rows)
            {
                cost = static_cast<double>(costmap_->cost.at<float>(my, mx));
            }
            max_cost = std::max(max_cost, cost);
        }
        return ompl::base::Cost(max_cost);
    }

  private:
    const std::shared_ptr<const Costmap> costmap_;
    const std::vector<Eigen::Vector2d> offsets_;
};

}

#endif

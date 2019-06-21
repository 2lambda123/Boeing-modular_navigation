#ifndef NAVIGATION_INTERFACE_TRAJECTORY_H
#define NAVIGATION_INTERFACE_TRAJECTORY_H

#include <Eigen/Geometry>
#include <std_msgs/Header.h>
#include <vector>

namespace navigation_interface
{

struct KinodynamicState
{
    Eigen::Isometry2d pose;
    Eigen::Vector3d velocity;
};

struct Trajectory
{
    double cost;
    std::string id;
    std_msgs::Header header;
    std::vector<KinodynamicState> states;
};
};

#endif

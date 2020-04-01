#include <gridmap/layers/obstacle_data/laser_data.h>
#include <gridmap/operations/clip_line.h>
#include <gridmap/params.h>
#include <pluginlib/class_list_macros.h>

#include <chrono>

PLUGINLIB_EXPORT_CLASS(gridmap::LaserData, gridmap::DataSource)

namespace gridmap
{

LaserData::LaserData()
    : TopicDataSource<sensor_msgs::LaserScan>("scan"), hit_probability_log_(0), miss_probability_log_(0),
      min_obstacle_height_(0), max_obstacle_height_(0), obstacle_range_(0), raytrace_range_(0)
{
}

void LaserData::onInitialize(const XmlRpc::XmlRpcValue& parameters)
{
    hit_probability_log_ = logodds(
        get_config_with_default_warn<double>(parameters, "hit_probability", 0.8, XmlRpc::XmlRpcValue::TypeDouble));
    miss_probability_log_ = logodds(
        get_config_with_default_warn<double>(parameters, "miss_probability", 0.4, XmlRpc::XmlRpcValue::TypeDouble));
    min_obstacle_height_ =
        get_config_with_default_warn<double>(parameters, "min_obstacle_height", 0.0, XmlRpc::XmlRpcValue::TypeDouble);
    max_obstacle_height_ =
        get_config_with_default_warn<double>(parameters, "max_obstacle_height", 2.0, XmlRpc::XmlRpcValue::TypeDouble);
    obstacle_range_ =
        get_config_with_default_warn<double>(parameters, "obstacle_range", 3.5, XmlRpc::XmlRpcValue::TypeDouble);
    raytrace_range_ =
        get_config_with_default_warn<double>(parameters, "raytrace_range", 4.0, XmlRpc::XmlRpcValue::TypeDouble);
}

void LaserData::onMapDataChanged()
{
}

bool LaserData::processData(const sensor_msgs::LaserScan::ConstPtr& msg, const Eigen::Isometry3d& sensor_transform)
{
    const Eigen::Vector3d sensor_pt = sensor_transform.translation();
    const Eigen::Vector2d sensor_pt_2d(sensor_pt.x(), sensor_pt.y());
    const Eigen::Vector2i sensor_pt_map = map_data_->dimensions().getCellIndex(sensor_pt_2d);

    // Check sensor is on map
    if (sensor_pt_map.x() < 0 || sensor_pt_map.x() >= map_data_->dimensions().size().x() || sensor_pt_map.y() < 0 ||
        sensor_pt_map.y() >= map_data_->dimensions().size().y())
    {
        ROS_WARN("Laser sensor is not on gridmap");
        return false;
    }

    if (laser_directions_.size() != msg->ranges.size())
    {
        laser_directions_.resize(msg->ranges.size());
        double angle = static_cast<double>(msg->angle_min);
        for (size_t i = 0; i < msg->ranges.size(); i++)
        {
            laser_directions_[i] = Eigen::AngleAxisd(angle, Eigen::Vector3d::UnitZ()) * Eigen::Vector3d::UnitX();
            angle += static_cast<double>(msg->angle_increment);
        }
    }

    const auto robot_tr = tf_buffer_->lookupTransform(global_frame_, "base_link", msg->header.stamp);
    const Eigen::Isometry2d robot_t = convert(robot_tr.transform);
    const auto footprint = buildFootprintSet(map_data_->dimensions(), robot_t, robot_footprint_, 1.00);

    const unsigned int cell_raytrace_range =
        static_cast<unsigned int>(raytrace_range_ / map_data_->dimensions().resolution());

    {
        auto _lock = map_data_->getLock();
        AddLogCost marker(map_data_->cells().data(), miss_probability_log_, map_data_->clampingThresMinLog(),
                          map_data_->clampingThresMaxLog());
        for (size_t i = 0; i < msg->ranges.size(); i++)
        {
            double range = static_cast<double>(msg->ranges[i]);
            if (!std::isfinite(range) && range > 0)
            {
                range = static_cast<double>(msg->range_max);
            }

            const Eigen::Vector3d pt = sensor_transform * (range * laser_directions_[i]);
            if (pt.z() < min_obstacle_height_ || pt.z() > max_obstacle_height_)
            {
                continue;
            }

            const Eigen::Vector2d pt_2d(pt.x(), pt.y());
            Eigen::Array2i ray_end = map_data_->dimensions().getCellIndex(pt_2d);
            cohenSutherlandLineClipEnd(sensor_pt_map.x(), sensor_pt_map.y(), ray_end.x(), ray_end.y(),
                                       map_data_->dimensions().size().x() - 1, map_data_->dimensions().size().y() - 1);
            raytraceLine(marker, sensor_pt_map.x(), sensor_pt_map.y(), ray_end.x(), ray_end.y(),
                         map_data_->dimensions().size().x(), cell_raytrace_range);
            if (range < static_cast<double>(msg->range_max) && range < obstacle_range_)
            {
                map_data_->update(ray_end, -miss_probability_log_);
                map_data_->update(ray_end, hit_probability_log_);
            }

            for (auto elem : footprint)
            {
                const Eigen::Array2i index = KeyToIndex(elem);
                if (map_data_->dimensions().contains(index))
                    map_data_->setMinThres(index);
            }
        }
    }
    return true;
}

}  // namespace gridmap
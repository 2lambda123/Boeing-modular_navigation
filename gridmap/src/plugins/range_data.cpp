#include <gridmap/params.h>
#include <gridmap/plugins/range_data.h>

#include <set>
#include <chrono>

#include <tf2_ros/message_filter.h>
#include <tf2_sensor_msgs/tf2_sensor_msgs.h>

#include <pluginlib/class_list_macros.h>

#include <sensor_msgs/point_cloud2_iterator.h>
#include <pcl.h>
#include <tf2_sensor_msgs/tf2_sensor_msgs.h>

#include <opencv2/highgui.hpp>

#include <chrono>

PLUGINLIB_EXPORT_CLASS(gridmap::RangeData, gridmap::DataSource)

namespace gridmap
{

RangeData::RangeData()
{
}

void RangeData::onInitialize(const XmlRpc::XmlRpcValue& parameters)
{
    ros::NodeHandle nh("~/" + name_);
    ros::NodeHandle g_nh;

    const std::string topic =
        get_config_with_default_warn<std::string>(parameters, "topic", name_ + "/range", XmlRpc::XmlRpcValue::TypeString);
    hit_probability_log_ = logodds(
        get_config_with_default_warn<double>(parameters, "hit_probability", 0.65, XmlRpc::XmlRpcValue::TypeDouble));
    miss_probability_log_ = logodds(
        get_config_with_default_warn<double>(parameters, "miss_probability", 0.10, XmlRpc::XmlRpcValue::TypeDouble));
    min_obstacle_height_ =
        get_config_with_default_warn<double>(parameters, "min_obstacle_height", 0.0, XmlRpc::XmlRpcValue::TypeDouble);
    max_obstacle_height_ =
        get_config_with_default_warn<double>(parameters, "max_obstacle_height", 2.0, XmlRpc::XmlRpcValue::TypeDouble);
    obstacle_range_ =
        get_config_with_default_warn<double>(parameters, "obstacle_range", 2.5, XmlRpc::XmlRpcValue::TypeDouble);
    raytrace_range_ =
        get_config_with_default_warn<double>(parameters, "raytrace_range", 3.0, XmlRpc::XmlRpcValue::TypeDouble);
    sub_sample_ = get_config_with_default_warn<int>(parameters, "sub_sample", 1, XmlRpc::XmlRpcValue::TypeInt);

    ROS_INFO_STREAM("Subscribing to range sensor: " << topic);

    subscriber_.reset(new message_filters::Subscriber<sensor_msgs::Range>(g_nh, topic, 50));
    message_filter_.reset(
        new tf2_ros::MessageFilter<sensor_msgs::Range>(*subscriber_, *tf_buffer_, global_frame_, 50, g_nh));
    message_filter_->registerCallback(boost::bind(&RangeData::rangeCallback, this, _1));
}

RangeData::~RangeData()
{
}

void RangeData::rangeCallback(const sensor_msgs::RangeConstPtr& message)
{
    if (sub_sample_ == 0 || (sub_sample_ > 0 && sub_sample_count_ > sub_sample_))
    {
        sub_sample_count_ = 0;

        const auto tr = tf_buffer_->lookupTransform(global_frame_, message->header.frame_id, message->header.stamp);

        const Eigen::Isometry3d t = Eigen::Translation3d(tr.transform.translation.x, tr.transform.translation.y,
                                                   tr.transform.translation.z) * Eigen::Quaterniond(
                                                   tr.transform.rotation.w, tr.transform.rotation.x,
                                                   tr.transform.rotation.y, tr.transform.rotation.z);

        const Eigen::Vector3d sensor_pt = t.translation();
        const Eigen::Vector2d sensor_pt_2d(sensor_pt.x(), sensor_pt.y());
        const Eigen::Vector2i sensor_pt_map = map_data_->worldToMapNoBounds(sensor_pt_2d);

        // Check sensor is on map
        if (sensor_pt_map.x() < 0 || sensor_pt_map.x() >= map_data_->sizeX()
                || sensor_pt_map.y() < 0 || sensor_pt_map.y() >= map_data_->sizeY())
        {
            ROS_WARN("Range sensor is not on gridmap");
            return;
        }

        const double half_fov = static_cast<double>(message->field_of_view / 2.0f);

        const Eigen::Vector3d left_pt = t * Eigen::Vector3d(static_cast<double>(message->range) * std::cos(half_fov),
                                                            static_cast<double>(message->range) * std::sin(half_fov),
                                                            0.0);
        const Eigen::Vector2d left_pt_2d(left_pt.x(), left_pt.y());
        const Eigen::Vector2i left_pt_map = map_data_->worldToMapNoBounds(left_pt_2d);

        const Eigen::Vector3d right_pt = t * Eigen::Vector3d(static_cast<double>(message->range) * std::cos(half_fov),
                                                             -static_cast<double>(message->range) * std::sin(half_fov),
                                                             0.0);
        const Eigen::Vector2d right_pt_2d(right_pt.x(), right_pt.y());
        const Eigen::Vector2i right_pt_map = map_data_->worldToMapNoBounds(right_pt_2d);

        const std::vector<Eigen::Vector2i> line = drawLine(left_pt_map, right_pt_map);

        {
            auto lock = map_data_->getLock();
            AddLogCost marker(map_data_->data(), miss_probability_log_, map_data_->clampingThresMin(), map_data_->clampingThresMax());
            for (std::size_t i=0; i<line.size(); ++i)
            {
                auto ray_end = line[i];
                clipRayEnd(sensor_pt_map, ray_end, map_data_->size());
                raytraceLine(marker, sensor_pt_map.x(), sensor_pt_map.y(), ray_end.x(), ray_end.y(), map_data_->sizeX());
                if (message->range < message->max_range)
                {
                    const double fraction = 1.0 - std::abs(static_cast<double>(i) / line.size() - 0.5);
                    map_data_->update(ray_end.x(), ray_end.y(), fraction * hit_probability_log_);
                }
            }
        }
    }
    else
        ++sub_sample_count_;
}

void RangeData::matchSize()
{
}

}

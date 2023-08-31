include "map_builder.lua"
include "trajectory_builder.lua"

options = {
    map_builder = MAP_BUILDER,
    trajectory_builder = TRAJECTORY_BUILDER,
    map_frame = "map",
    tracking_frame = "base_link",
    odom_frame = "odom",
    use_odometry = true,
    use_nav_sat = false,
    use_landmarks = false,
    num_laser_scans = 2,
    num_multi_echo_laser_scans = 0,
    num_subdivisions_per_laser_scan = 1,
    num_point_clouds = 0,
    lookup_transform_timeout_sec = 0.1,
    submap_publish_period_sec = 0.05,
    pose_publish_period_sec = 0.1,
    trajectory_publish_period_sec = 0.1,
    rangefinder_sampling_ratio = 1.0,
    odometry_sampling_ratio = 0.5,
    fixed_frame_pose_sampling_ratio = 1.,
    landmarks_sampling_ratio = 1.,
}

return options


#ifndef RODAQ_SUPPORT_ROS2_ROS2_H
#define RODAQ_SUPPORT_ROS2_ROS2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ROS 2 duration representation (16 bytes). sec is seconds; nsec is normalized nanoseconds
 * (< 1000000000). Preserves unspecified {0,0} and infinite {9223372036,854775807}.
 */
typedef struct rodaq_ros2_duration {
    uint64_t sec;
    uint64_t nsec;
} rodaq_ros2_duration_t;

typedef uint32_t rodaq_ros2_qos_history_t;
enum {
    RODAQ_ROS2_QOS_HISTORY_SYSTEM_DEFAULT = 0,
    RODAQ_ROS2_QOS_HISTORY_KEEP_LAST = 1,
    RODAQ_ROS2_QOS_HISTORY_KEEP_ALL = 2,
    RODAQ_ROS2_QOS_HISTORY_UNKNOWN = 3
};

typedef uint32_t rodaq_ros2_qos_reliability_t;
enum {
    RODAQ_ROS2_QOS_RELIABILITY_SYSTEM_DEFAULT = 0,
    RODAQ_ROS2_QOS_RELIABILITY_RELIABLE = 1,
    RODAQ_ROS2_QOS_RELIABILITY_BEST_EFFORT = 2,
    RODAQ_ROS2_QOS_RELIABILITY_UNKNOWN = 3
};

typedef uint32_t rodaq_ros2_qos_durability_t;
enum {
    RODAQ_ROS2_QOS_DURABILITY_SYSTEM_DEFAULT = 0,
    RODAQ_ROS2_QOS_DURABILITY_TRANSIENT_LOCAL = 1,
    RODAQ_ROS2_QOS_DURABILITY_VOLATILE = 2,
    RODAQ_ROS2_QOS_DURABILITY_UNKNOWN = 3
};

typedef uint32_t rodaq_ros2_qos_liveliness_t;
enum {
    RODAQ_ROS2_QOS_LIVELINESS_SYSTEM_DEFAULT = 0,
    RODAQ_ROS2_QOS_LIVELINESS_AUTOMATIC = 1,
    RODAQ_ROS2_QOS_LIVELINESS_MANUAL_BY_NODE = 2,
    RODAQ_ROS2_QOS_LIVELINESS_MANUAL_BY_TOPIC = 3,
    RODAQ_ROS2_QOS_LIVELINESS_UNKNOWN = 4
};

/**
 * ROS 2 plugin-side QoS profile (Humble/Jazzy metadata-v8 vocabulary; no RMW headers).
 * Serialized into Descriptor metadata under format "ros2/1.0", never part of Core C ABI.
 */
typedef struct rodaq_ros2_qos_profile {
    rodaq_ros2_qos_history_t history;
    rodaq_ros2_qos_reliability_t reliability;
    rodaq_ros2_qos_durability_t durability;
    rodaq_ros2_qos_liveliness_t liveliness;
    uint64_t depth;
    rodaq_ros2_duration_t deadline;
    rodaq_ros2_duration_t lifespan;
    rodaq_ros2_duration_t liveliness_lease_duration;
    uint8_t avoid_ros_namespace_conventions;
    uint8_t reserved[7];
} rodaq_ros2_qos_profile_t;

#ifdef __cplusplus
}
#endif

#endif

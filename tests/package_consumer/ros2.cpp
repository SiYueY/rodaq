#include <ros2/schema.hpp>

int main() {
    return rodaq::ros2::valid_type_name("std_msgs/msg/Int32") &&
                   !rodaq::ros2::valid_type_name("../invalid")
               ? 0
               : 1;
}

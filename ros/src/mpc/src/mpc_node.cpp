#include <math.h> /* floor, abs */
#include <cmath> /* atan2, M_PI */
#include <string>
#include <sstream>

#include <ros/ros.h>
#include <ros/console.h>
#include <std_msgs/Float64.h>
#include <visualization_msgs/Marker.h>
#include <nav_msgs/Odometry.h>

#include "mpc_node.h"
#include "MPC.h"


MPCControllerNode::MPCControllerNode(const ros::NodeHandle & nodehandle, const Params & params)
        : m_controller(params)
{

    m_nodehandle = nodehandle;
    m_old_time = ros::Time::now();
    m_last_stop_msg_ts = ros::Time::now().toSec();

    m_pts_OK = false;
    m_pos_OK = false;
    m_speed_OK = false;
    m_psi_OK = false;

    m_ref_v = params.ref_v;
    m_ref_v_alpha = params.ref_v_alpha;
    m_poly_degree = params.poly_degree;
    m_num_steps_poly = params.num_steps_poly;

    m_debug = params.debug;
    m_go_flag = false;

    ///* Actuators
    m_steer = CENTER_IN_DZIK;
    m_rpm = 0;

    ///* Publishers
    m_pub_commands_servo_position = m_nodehandle.advertise<std_msgs::Float64>(
            "/commands/servo/position",
            1,
            true
    );
    m_pub_commands_motor_speed = m_nodehandle.advertise<std_msgs::Float64>(
            "/commands/motor/speed",
            1,
            true
    );
    if (m_debug) {
        m_pub_closest = m_nodehandle.advertise<visualization_msgs::Marker>(
                "/mpc/closest_cpp",
                1
        );

        m_pub_next_pos = m_nodehandle.advertise<visualization_msgs::Marker>(
                "/mpc/next_pos_cpp",
                1
        );

        m_pub_poly = m_nodehandle.advertise<visualization_msgs::Marker>(
                "/mpc/poly_cpp",
                1
        );

    }

    ///* Subscribers
    m_sub_centerline = m_nodehandle.subscribe(
            "/centerline",
            1,
            &MPCControllerNode::centerline_cb,
            this
    );
    m_sub_odom = m_nodehandle.subscribe(
            "/odom",
            1,
            &MPCControllerNode::odom_cb,
            this
    );
    m_sub_pf_pose_odom = m_nodehandle.subscribe(
            "/pf/pose/odom",
            1,
            &MPCControllerNode::pf_pose_odom_cb,
            this
    );
    m_sub_signal_go = m_nodehandle.subscribe(
            "/signal/go",
            10,
            &MPCControllerNode::signal_go_cb,
            this
    );
}


void MPCControllerNode::centerline_cb(const visualization_msgs::Marker & data) {
    int num_points = data.points.size();

    m_pts_x = std::vector<double>();
    m_pts_x.reserve(num_points);

    m_pts_y = std::vector<double>();
    m_pts_y.reserve(num_points);

    for (auto & p : data.points) {
        m_pts_x.push_back(p.x);
        m_pts_y.push_back(p.y);
    }
    m_pts_OK = true;
}


void MPCControllerNode::signal_go_cb(const std_msgs::UInt16 & data) {
    if (data.data == 0) {
        ROS_WARN("Emergency stop!");
        m_go_flag = false;
    } else if (data.data == 2309){
        ROS_WARN("GO!");
        m_go_flag = true;
    }
}


visualization_msgs::Marker MPCControllerNode::get_marker(
        const std::vector<double> & vars,
        double pos_x_lat, double pos_y_lat,
        double sin_psi_lat, double cos_psi_lat,
        float red, float green, float blue

    ) {
    visualization_msgs::Marker marker;
    marker.header.frame_id = "/map";
    marker.header.stamp = ros::Time::now();
    marker.type = visualization_msgs::Marker::LINE_STRIP;
    marker.action = visualization_msgs::Marker::ADD;

    marker.scale.x = 0.1;
    marker.scale.y = 0.1;
    marker.scale.z = 0.1;

    marker.pose.position.x = 0;
    marker.pose.position.y = 0;
    marker.pose.position.z = 0;

    marker.pose.orientation.x = 0.0;
    marker.pose.orientation.y = 0.0;
    marker.pose.orientation.z = 0.0;
    marker.pose.orientation.w = 1.0;

    marker.color.a = 0.5;
    marker.color.r = red;
    marker.color.g = green;
    marker.color.b = blue;

    marker.lifetime = ros::Duration();

    // The first two values are the actuators
    marker.points.reserve((vars.size() - 2) / 2);

    for (size_t i=2; i < vars.size(); i+=2) {
        geometry_msgs::Point p;
        double x = vars[i];
        double y = vars[i+1];
        double x_rot = x * cos_psi_lat - y * sin_psi_lat;
        double y_rot = x * sin_psi_lat + y * cos_psi_lat;
        p.x = x_rot + pos_x_lat;
        p.y = y_rot + pos_y_lat;
        p.z = 0.0f;
        marker.points.push_back(p);
    }

    return marker;
}


void MPCControllerNode::odom_cb(const nav_msgs::Odometry & data) {
    m_speed = data.twist.twist.linear.x;
    m_speed_OK = true;
}


void MPCControllerNode::pf_pose_odom_cb(const nav_msgs::Odometry & data) {
    m_pos_x = data.pose.pose.position.x;
    m_pos_y = data.pose.pose.position.y;
    m_pos_OK = true;

    // Calculate the psi Euler angle
    // (source: https://en.wikipedia.org/wiki/Conversion_between_quaternions_and_Euler_angles)
    auto o = data.pose.pose.orientation;
    double siny_cosp = 2.0 * (o.w * o.z + o.x * o.y);
    double cosy_cosp = 1.0 - 2.0 * (o.y * o.y + o.z * o.z);
    m_psi = atan2(siny_cosp, cosy_cosp);
    m_psi_OK = true;
}


void MPCControllerNode::loop() {
    while (m_nodehandle.ok()) {
        m_time = ros::Time::now();

        if (m_pts_OK and m_speed_OK and m_pos_OK and m_psi_OK) {
            double v_lat = m_speed;// + m_latency * m_throttle; TODO: you can collect m_throttle from /odom
            double psi_lat = m_psi - m_latency * (v_lat * m_steer / Lf());
            double pos_x_lat = m_pos_x + m_latency * (v_lat * cos(psi_lat));
            double pos_y_lat = m_pos_y + m_latency * (v_lat * sin(psi_lat));

            int closest_idx = find_closest(m_pts_x, m_pts_y, pos_x_lat, pos_y_lat);

            // It pays to use `NUM_STEPS_BACK` points for fitting the polynomial
            // (stabilizes the polynomial)
            closest_idx -= NUM_STEPS_BACK;

            std::vector<double> closest_pts_x;
            closest_pts_x.reserve(m_num_steps_poly);
            std::vector<double> closest_pts_y;
            closest_pts_y.reserve(m_num_steps_poly);
            for (size_t i=0; i < m_num_steps_poly; i++) {
                int idx = (closest_idx + i*STEP_POLY) % m_pts_x.size();
                closest_pts_x.push_back(m_pts_x[idx]);
                closest_pts_y.push_back(m_pts_y[idx]);
            }

            // Before we get the actuators, we need to calculate points in car's
            // coordinate system; these will be passed later on to polyfit
            std::vector<double> xvals_vec;
            std::vector<double> yvals_vec;
            xvals_vec.reserve(m_num_steps_poly);
            yvals_vec.reserve(m_num_steps_poly);

            double sin_psi_lat = sin(psi_lat);
            double cos_psi_lat = cos(psi_lat);
            double fraction_steps_OK = 1.0;

            for (size_t i=0; i<m_num_steps_poly; i++) {
                double dx = closest_pts_x[i] - pos_x_lat;
                double dy = closest_pts_y[i] - pos_y_lat;

                // Rotation around the origin
                double x_rot = dx * cos_psi_lat + dy * sin_psi_lat;
                double y_rot = -dx * sin_psi_lat + dy * cos_psi_lat;

                if (i > m_poly_degree) {
                    bool x_delta_too_low = (x_rot - xvals_vec[i-1] < X_DELTA_MIN_VALUE);
                    if (x_delta_too_low) {
                        size_t num_steps_remaining = m_num_steps_poly-i+1;
                        fraction_steps_OK = 1.0 * (i+1) / m_num_steps_poly;
                        ROS_WARN("X delta too low, breaking at %lu, num_steps_remaining: %lu", i, num_steps_remaining);

                        // Fill out the rest of the points with fake waypoints
                        double delta_x = xvals_vec[i-1] - xvals_vec[i-2];
                        double delta_y = yvals_vec[i-1] - yvals_vec[i-2];
                        delta_x = delta_x / num_steps_remaining;
                        delta_y = delta_y / num_steps_remaining;

                        for (size_t sub_i=1; sub_i<num_steps_remaining; sub_i++) {
                            xvals_vec.push_back(xvals_vec[i-1] + sub_i * delta_x);
                            yvals_vec.push_back(yvals_vec[i-1] + sub_i * delta_y);
                        }

                        break;
                    }
                }

                xvals_vec.push_back(x_rot);
                yvals_vec.push_back(y_rot);
            }

            double new_ref_v = m_ref_v_alpha * m_ref_v + (1-m_ref_v_alpha) * (fraction_steps_OK * m_ref_v);

            Eigen::VectorXd xvals(m_num_steps_poly);
            Eigen::VectorXd yvals(m_num_steps_poly);
            for (size_t i=0; i < m_num_steps_poly; i++) {
                xvals[i] = xvals_vec[i];
                yvals[i] = yvals_vec[i];
            }

            // Here we calculate the fit to the points in *car's coordinate system*
            Eigen::VectorXd coeffs = polyfit(xvals, yvals, m_poly_degree);
            std::stringstream ss;
            ss << "coeffs: ";
            for (size_t c=0; c<m_poly_degree+1; c++)
                ss << std::setprecision(3) << coeffs[c] << " ";
            ROS_WARN("%s", ss.str().c_str());

            // Now, we can calculate the cross track error
            double cte = polyeval(coeffs, 0);

            // ... and psi's error
            double epsi = -atan(coeffs[1]);

            ROS_WARN("CTE: %.2f, ePsi: %.2f, psi: %.2f", cte, epsi, m_psi);

            // And now we're ready to calculate the actuators using the MPC
            Eigen::VectorXd state(5);
            state << 0, 0, 0, cte, epsi;
            auto vars = m_controller.Solve(state, coeffs, new_ref_v);

            // Extract the actuator values
            double steering_angle_in_radians = vars[0];
            double speed_in_meters_by_second = vars[1];

            ROS_WARN("steer: %.2f [rad], speed: %.2f [m/s]", steering_angle_in_radians, speed_in_meters_by_second);

            // Map the angle to the values used in Dzik
            m_steer = CENTER_IN_DZIK - steering_angle_in_radians;

            if (m_steer < 0.0) {
                ROS_WARN("steer angle %.2f is below 0 -- clipping it to 0", m_steer);
                m_steer = 0.0;   
            } else if (m_steer > 1.0) {
                ROS_WARN("steer angle %.2f is greater than 1 -- clipping it to 1", m_steer);
                m_steer = 1.0;
            }

            // Map the speed to the values used in Dzik
            m_rpm = speed_in_meters_by_second / 2 / M_PI / WHEEL_RADIUS_IN_DZIK; // [1/s]
            m_rpm *= 60; // [1/min = RPM]
            m_rpm *= 10; // TODO: I had to multiply by 10 to get values as they really are in Dzik
            ROS_WARN("speed_in_Dzik: %.2f [RPM]", m_rpm);

            if (!m_go_flag) {
                m_steer = CENTER_IN_DZIK;
                m_rpm = 0;
            } else {
                ROS_WARN("GO flag is 'true'");
            }

            // Publish the transformed angle
            std_msgs::Float64 msg_placeholder;
            msg_placeholder.data = m_steer;
            m_pub_commands_servo_position.publish(msg_placeholder);
            msg_placeholder.data = m_rpm;
            m_pub_commands_motor_speed.publish(msg_placeholder);

            if (m_debug) {
                // First, publish the next points as predicted from MPC
                auto next_pos_marker = get_marker(vars, pos_x_lat, pos_y_lat, sin_psi_lat, cos_psi_lat, 0.0, 0.0, 1.0);
                m_pub_next_pos.publish(next_pos_marker);

                // Now, publish the closest waypoints (those used for polyfit)
                std::vector<double> closest_waypoints;
                closest_waypoints.reserve(m_num_steps_poly);
                for (size_t i=0; i<m_num_steps_poly; i++) {
                    closest_waypoints.push_back(xvals_vec[i]);
                    closest_waypoints.push_back(yvals_vec[i]);
                }
                auto closest_marker = get_marker(closest_waypoints, pos_x_lat, pos_y_lat, sin_psi_lat, cos_psi_lat, 1.0, 1.0, 1.0);
                m_pub_closest.publish(closest_marker);

                // Now, publish markers that show the polynomial that was fit to the waypoints
                std::vector<double> for_poly_marker;
                for_poly_marker.push_back(0);
                for_poly_marker.push_back(0);
                for (double x=0; x < 2.1; x+=0.2) {
                    for_poly_marker.push_back(x);
                    for_poly_marker.push_back(polyeval(coeffs, x));
                }
                auto poly_marker = get_marker(for_poly_marker, pos_x_lat, pos_y_lat, sin_psi_lat, cos_psi_lat, 0.7, 0.2, 0.1);
                m_pub_poly.publish(poly_marker);
            }

            // Print out calculation times
            double delta_between_callbacks = (m_time.toSec() - m_old_time.toSec());
            double delta_within_callback = (ros::Time::now().toSec() - m_time.toSec());
            ROS_WARN(
                    "dt_bet_cb: %.3f[s] dt_in_cb: %.3f[s]",
                    delta_between_callbacks, delta_within_callback
            );

            // Print out relevant attributes
            ROS_WARN("m_speed: %.3f [m/s]", m_speed);

        } else {
            ROS_WARN(
                    "No optimization, m_pts_OK: %d, m_speed_OK: %d, m_pos_OK: %d, m_psi_OK: %d",
                    m_pts_OK, m_speed_OK, m_pos_OK, m_psi_OK
            );
        }

        m_old_time = m_time;
        ros::spinOnce();
    }
}


int MPCControllerNode::find_closest(const std::vector<double> & pts_x, const std::vector<double> & pts_y, double pos_x, double pos_y) {
    int closest_idx = -1;
    double closest_dist = 1.0e19;
    for (size_t i=0; i < pts_x.size(); i++) {
        double diff_x = (pts_x[i] - pos_x);
        double diff_y = (pts_y[i] - pos_y);
        double dist = diff_x*diff_x + diff_y*diff_y;
        if (dist < closest_dist) {
            closest_idx = i;
            closest_dist = dist;
        }
    }
    return closest_idx;
}



int main(int argc, char **argv) {

    ros::init(argc, argv, "mpc_node_cpp");

    Params params;

    int num_expected_args = 15;

    if (argc == num_expected_args) {
        params.steps_ahead = atoi(argv[1]);
        params.dt = atof(argv[2]);
        params.ref_v = atof(argv[3]);
        params.ref_v_alpha = atof(argv[4]);
        if (params.ref_v_alpha > 1.0 or params.ref_v_alpha < 0.0) {
            std::cout << "The ref_v_alpha argument should be a float"
                      << " between 0.0 and 1.0 (inclusive) and you"
                      << " passed "
                      << params.ref_v_alpha
                      << "\n";
            return 1;
        }

        params.latency = atof(argv[5]);

        params.cte_coeff = atof(argv[6]);
        params.epsi_coeff = atof(argv[7]);
        params.speed_coeff = atof(argv[8]);
        params.steer_coeff = atof(argv[9]);

        params.consec_steer_coeff = atof(argv[10]);
        params.consec_speed_coeff = atof(argv[11]);

        params.poly_degree = atoi(argv[12]);
        params.num_steps_poly = atoi(argv[13]);

        std::string debug_msg(argv[14]);

        if (debug_msg == "true") {
            params.debug = true;
        } else if (debug_msg == "false") {
            params.debug = false;
        } else {
            std::cout << "The debug argument should either be \"true\" or \"false\""
                      << " and you passed "
                      << argv[14]
                      << "\n";
            return 1;
        }

    } else if (argc > num_expected_args ) {
        std::cout << "Too many arguments passed to main\n";
        return 1;
    } else {
        std::cout << "Too few arguments passed to main\n";
        return 1;
    }

    std::cout << "steps_ahead: " << params.steps_ahead
              << " dt: " << params.dt
              << " ref_v: " << params.ref_v
              << " ref_v_alpha: " << params.ref_v_alpha
              << " latency: " << params.latency << "[s]"
              << " cte_coeff: " << params.cte_coeff
              << " epsi_coeff: " << params.epsi_coeff
              << " speed_coeff: " << params.speed_coeff
              << " steer_coeff: " << params.steer_coeff
              << " consec_steer_coeff: " << params.consec_steer_coeff
              << " consec_speed_coeff: " << params.consec_speed_coeff
              << " poly degree: " << params.poly_degree
              << " num_steps_poly: " << params.num_steps_poly
              << " debug: " << params.debug
              << "\n";

    if (params.latency > 1)
        std::cout << "Latency passed to main is > 1."
                  << " However, it should be in seconds, isn't "
                  << params.latency
                  << " too high?\n";

    ros::NodeHandle nodehandle;
    MPCControllerNode mpc_node(nodehandle, params);

    ros::Rate loop_rate(100);

    mpc_node.loop();

    return 0;
}

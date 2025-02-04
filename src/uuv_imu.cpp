/*
 * MIT License (MIT)
 *
 * Copyright (c) 2018 Dereck Wonnacott <dereck@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <iostream>
#include <cmath>
#include <eigen3/Eigen/Dense>
// No need to define PI twice if we already have it included...
//#define M_PI 3.14159265358979323846  /* M_PI */

// ROS Libraries
#include "ros/ros.h"
#include "geometry_msgs/Pose2D.h"
#include "geometry_msgs/Vector3.h"
#include "sensor_msgs/Imu.h"
#include "sensor_msgs/MagneticField.h"
#include "sensor_msgs/NavSatFix.h"
#include "nav_msgs/Odometry.h"
#include "sensor_msgs/Temperature.h"
#include "sensor_msgs/FluidPressure.h"
#include "std_srvs/Empty.h"
#include <tf2/LinearMath/Transform.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <vectornav/Ins.h>

using namespace Eigen;


ros::Publisher pubIMU, pubMag, pubGPS, pubOdom, pubTemp, pubPres, pubIns, ins_pos_pub, local_vel_pub, NED_pose_pub, ECEF_pose_pub, ins_ref_pub, ecef_ref_pub, ang_rate_pub, accel_pub, att_rpy_pub;
ros::ServiceServer resetOdomSrv;

//Unused covariances initilized to zero's
boost::array<double, 9ul> linear_accel_covariance = { };  //covariances
boost::array<double, 9ul> angular_vel_covariance = { };
boost::array<double, 9ul> orientation_covariance = { };
XmlRpc::XmlRpcValue rpc_temp;

// Custom user data to pass to packet callback function
struct UserData {
    int device_family;
};

// Include this header file to get access to VectorNav sensors.
#include "vn/sensors.h"
#include "vn/compositedata.h"
#include "vn/util.h"

using namespace std;
using namespace vn::math;
using namespace vn::sensors;
using namespace vn::protocol::uart;
using namespace vn::xplat;

// Method declarations for future use.
void BinaryAsyncMessageReceived(void* userData, Packet& p, size_t index);

// frame id used only for Odom header.frame_id
std::string map_frame_id;
// frame id used for header.frame_id of other messages and for Odom child_frame_id
std::string frame_id;
// Boolean to use ned or enu frame. Defaults to enu which is data format from sensor.
bool tf_ned_to_enu;
bool frame_based_enu;

// Initial position after getting a GPS fix.
//vec3d initial_position;
bool initial_position_set = false;

Vector3d Pe_ref;
Matrix3d Rne;
Vector3d Pe;
Vector3d NED;
double re = 6378137; //equatorial radius
double rp = 6356752; //polar axis radius
double ecc = 0.0818; //eccentricity
//Alternatively use
//float ecc = pow(re*re - rp*rp,0.5)/re;
double Ne; //prime vertical radius of curvature
double lat_radians; //latitude in radians
double lon_radians; //longitude in radians

//Custom topics
geometry_msgs::Pose2D ins_ref;
geometry_msgs::Vector3 ecef_ref;
geometry_msgs::Pose2D ins_pose; //inertial navigation system pose (latitude, longitude, yaw)
geometry_msgs::Vector3 local_vel; //veocity/speed in a local reference frame
geometry_msgs::Pose2D NED_pose; //pose in a local reference frame (N, E, yaw)
geometry_msgs::Vector3 ECEF_pose; //pose in ECEF frame (X, Y, Z)

geometry_msgs::Vector3 ang_rate; //angular rate
geometry_msgs::Vector3 accel;   //acceleration
geometry_msgs::Vector3 att_rpy;//RPY



// Basic loop so we can initilize our covariance parameters above
boost::array<double, 9ul> setCov(XmlRpc::XmlRpcValue rpc){
    // Output covariance vector
    boost::array<double, 9ul> output = {0.0};

    // Convert the RPC message to array
    ROS_ASSERT(rpc.getType() == XmlRpc::XmlRpcValue::TypeArray);

    for(int i = 0; i < 9; i++){
        ROS_ASSERT(rpc[i].getType() == XmlRpc::XmlRpcValue::TypeDouble);
        output[i] = (double)rpc[i];
    }
    return output;
}

// Reset initial position to current position
bool resetOdom(std_srvs::Empty::Request &req, std_srvs::Empty::Response &resp)
{
    initial_position_set = false;
    return true;
}

int main(int argc, char *argv[])
{

    // ROS node init
    ros::init(argc, argv, "vectornav");
    ros::NodeHandle n;
    ros::NodeHandle pn("~");

    pubIMU = n.advertise<sensor_msgs::Imu>("vectornav/IMU", 1000);
    pubMag = n.advertise<sensor_msgs::MagneticField>("vectornav/Mag", 1000);
    pubGPS = n.advertise<sensor_msgs::NavSatFix>("vectornav/GPS", 1000);
    pubOdom = n.advertise<nav_msgs::Odometry>("vectornav/Odom", 1000);
    pubTemp = n.advertise<sensor_msgs::Temperature>("vectornav/Temp", 1000);
    pubPres = n.advertise<sensor_msgs::FluidPressure>("vectornav/Pres", 1000);
    pubIns = n.advertise<vectornav::Ins>("vectornav/INS", 1000);
    ins_pos_pub = n.advertise<geometry_msgs::Pose2D>("/vectornav/ins_2d/ins_pose", 1000);
    local_vel_pub = n.advertise<geometry_msgs::Vector3>("/vectornav/ins_2d/local_vel", 1000);
    NED_pose_pub = n.advertise<geometry_msgs::Pose2D>("/vectornav/ins_2d/NED_pose", 1000);
    ECEF_pose_pub = n.advertise<geometry_msgs::Vector3>("/vectornav/ins_2d/ECEF_pose", 1000);
    ins_ref_pub = n.advertise<geometry_msgs::Pose2D>("/vectornav/ins_2d/ins_ref", 1000);
    ecef_ref_pub = n.advertise<geometry_msgs::Vector3>("/vectornav/ins_2d/ecef_ref", 1000);
    //Para el Filtro de Kalman
    ang_rate_pub = n.advertise<geometry_msgs::Vector3>("/vectornav/ins_2d/ang_rate", 1000);
    accel_pub = n.advertise<geometry_msgs::Vector3>("/vectornav/ins_2d/accel", 1000);
    att_rpy_pub = n.advertise<geometry_msgs::Vector3>("/vectornav/ins_2d/att_rpy", 1000);


    resetOdomSrv = n.advertiseService("reset_odom", resetOdom);

    // Serial Port Settings
    string SensorPort;
    int SensorBaudrate;
    int async_output_rate;

    // Sensor IMURATE (800Hz by default, used to configure device)
    int SensorImuRate;

    // Load all params
    pn.param<std::string>("map_frame_id", map_frame_id, "map");
    pn.param<std::string>("frame_id", frame_id, "vectornav");
    pn.param<bool>("tf_ned_to_enu", tf_ned_to_enu, false);
    pn.param<bool>("frame_based_enu", frame_based_enu, false);
    pn.param<int>("async_output_rate", async_output_rate, 200);
    pn.param<std::string>("serial_port", SensorPort, "/dev/ttyUSB0");
    pn.param<int>("serial_baud", SensorBaudrate, 921600);
    pn.param<int>("fixed_imu_rate", SensorImuRate, 800);

    //Call to set covariances
    if(pn.getParam("linear_accel_covariance",rpc_temp))
    {
        linear_accel_covariance = setCov(rpc_temp);
    }
    if(pn.getParam("angular_vel_covariance",rpc_temp))
    {
        angular_vel_covariance = setCov(rpc_temp);
    }
    if(pn.getParam("orientation_covariance",rpc_temp))
    {
        orientation_covariance = setCov(rpc_temp);
    }

    ROS_INFO("Connecting to : %s @ %d Baud", SensorPort.c_str(), SensorBaudrate);

    // Create a VnSensor object and connect to sensor
    VnSensor vs;

    // Default baudrate variable
    int defaultBaudrate;
    // Run through all of the acceptable baud rates until we are connected
    // Looping in case someone has changed the default
    bool baudSet = false;
    while(!baudSet){
        // Make this variable only accessible in the while loop
        static int i = 0;
        defaultBaudrate = vs.supportedBaudrates()[i];
        ROS_INFO("Connecting with default at %d", defaultBaudrate);
        // Default response was too low and retransmit time was too long by default.
        // They would cause errors
        vs.setResponseTimeoutMs(1000); // Wait for up to 1000 ms for response
        vs.setRetransmitDelayMs(50);  // Retransmit every 50 ms

        // Acceptable baud rates 9600, 19200, 38400, 57600, 128000, 115200, 230400, 460800, 921600
        // Data sheet says 128000 is a valid baud rate. It doesn't work with the VN100 so it is excluded.
        // All other values seem to work fine.
        try{
            // Connect to sensor at it's default rate
            if(defaultBaudrate != 128000 && SensorBaudrate != 128000)
            {
                vs.connect(SensorPort, defaultBaudrate);
                // Issues a change baudrate to the VectorNav sensor and then
                // reconnects the attached serial port at the new baudrate.
                vs.changeBaudRate(SensorBaudrate);
                // Only makes it here once we have the default correct
                ROS_INFO("Connected baud rate is %d",vs.baudrate());
                baudSet = true;
            }
        }
        // Catch all oddities
        catch(...){
            // Disconnect if we had the wrong default and we were connected
            vs.disconnect();
            ros::Duration(0.2).sleep();
        }
        // Increment the default iterator
        i++;
        // There are only 9 available data rates, if no connection
        // made yet possibly a hardware malfunction?
        if(i > 8)
        {
            break;
        }
    }

    // Now we verify connection (Should be good if we made it this far)
    if(vs.verifySensorConnectivity())
    {
        ROS_INFO("Device connection established");
    }else{
        ROS_ERROR("No device communication");
        ROS_WARN("Please input a valid baud rate. Valid are:");
        ROS_WARN("9600, 19200, 38400, 57600, 115200, 128000, 230400, 460800, 921600");
        ROS_WARN("With the test IMU 128000 did not work, all others worked fine.");
    }
    // Query the sensor's model number.
    string mn = vs.readModelNumber();
    string fv = vs.readFirmwareVersion();
    uint32_t hv = vs.readHardwareRevision();
    uint32_t sn = vs.readSerialNumber();
    ROS_INFO("Model Number: %s, Firmware Version: %s", mn.c_str(), fv.c_str());
    ROS_INFO("Hardware Revision : %d, Serial Number : %d", hv, sn);

    // Set the device info for passing to the packet callback function
    UserData user_data;
    user_data.device_family = vs.determineDeviceFamily();

    // Set Data output Freq [Hz]
    vs.writeAsyncDataOutputFrequency(async_output_rate);

    // Configure binary output message
    BinaryOutputRegister bor(
            ASYNCMODE_PORT1,
            SensorImuRate / async_output_rate,  // update rate [ms]
            COMMONGROUP_QUATERNION
            | COMMONGROUP_YAWPITCHROLL
            | COMMONGROUP_ANGULARRATE
            | COMMONGROUP_POSITION
            | COMMONGROUP_ACCEL
            | COMMONGROUP_MAGPRES,
            TIMEGROUP_NONE
            | TIMEGROUP_GPSTOW
            | TIMEGROUP_GPSWEEK
            | TIMEGROUP_TIMEUTC,
            IMUGROUP_NONE,
            GPSGROUP_NONE,
            ATTITUDEGROUP_YPRU, //<-- returning yaw pitch roll uncertainties
            INSGROUP_INSSTATUS
            | INSGROUP_POSLLA
            | INSGROUP_POSECEF
            | INSGROUP_VELBODY
            | INSGROUP_ACCELECEF
            | INSGROUP_VELNED
            | INSGROUP_POSU
            | INSGROUP_VELU,
            GPSGROUP_NONE);

    vs.writeBinaryOutput1(bor);

    // Set Data output Freq [Hz]
    vs.writeAsyncDataOutputFrequency(async_output_rate);
    vs.registerAsyncPacketReceivedHandler(&user_data, BinaryAsyncMessageReceived);

    // You spin me right round, baby
    // Right round like a record, baby
    // Right round round round
    while (ros::ok())
    {
        ros::spin(); // Need to make sure we disconnect properly. Check if all ok.
    }

    // Node has been terminated
    vs.unregisterAsyncPacketReceivedHandler();
    ros::Duration(0.5).sleep();
    ROS_INFO ("Unregisted the Packet Received Handler");
    vs.disconnect();
    ros::Duration(0.5).sleep();
    ROS_INFO ("%s is disconnected successfully", mn.c_str());
    return 0;
}

//
// Callback function to process data packet from sensor
//
void BinaryAsyncMessageReceived(void* userData, Packet& p, size_t index)
{
    vn::sensors::CompositeData cd = vn::sensors::CompositeData::parse(p);
    UserData user_data = *static_cast<UserData*>(userData);

    // IMU
    /*sensor_msgs::Imu msgIMU;
    msgIMU.header.stamp = ros::Time::now();
    msgIMU.header.frame_id = frame_id;

    if (cd.hasQuaternion() && cd.hasAngularRate() && cd.hasAcceleration())
    {

        vec4f q = cd.quaternion();
        vec3f ar = cd.angularRate();
        vec3f al = cd.acceleration();

        local_vel.z = ar[2]; //yaw rate

        if (cd.hasAttitudeUncertainty())
        {
            vec3f orientationStdDev = cd.attitudeUncertainty();
            msgIMU.orientation_covariance[0] = pow(orientationStdDev[2]*M_PI/180, 2); // Convert to radians pitch
            msgIMU.orientation_covariance[4] = pow(orientationStdDev[1]*M_PI/180, 2); // Convert to radians Roll
            msgIMU.orientation_covariance[8] = pow(orientationStdDev[0]*M_PI/180, 2); // Convert to radians Yaw
        }

        //Quaternion message comes in as a Yaw (z) pitch (y) Roll (x) format
        if (tf_ned_to_enu)
        {
            // If we want the orientation to be based on the reference label on the imu
            tf2::Quaternion tf2_quat(q[0],q[1],q[2],q[3]);
            geometry_msgs::Quaternion quat_msg;

            if(frame_based_enu)
            {
                // Create a rotation from NED -> ENU
                tf2::Quaternion q_rotate;
                q_rotate.setRPY (M_PI, 0.0, M_PI/2);
                // Apply the NED to ENU rotation such that the coordinate frame matches
                tf2_quat = q_rotate*tf2_quat;
                quat_msg = tf2::toMsg(tf2_quat);

                // Since everything is in the normal frame, no flipping required
                msgIMU.angular_velocity.x = ar[0];
                msgIMU.angular_velocity.y = ar[1];
                msgIMU.angular_velocity.z = ar[2];

                msgIMU.linear_acceleration.x = al[0];
                msgIMU.linear_acceleration.y = al[1];
                msgIMU.linear_acceleration.z = al[2];
            }
            else
            {
                // put into ENU - swap X/Y, invert Z
                quat_msg.x = q[1];
                quat_msg.y = q[0];
                quat_msg.z = -q[2];
                quat_msg.w = q[3];

                // Flip x and y then invert z
                msgIMU.angular_velocity.x = ar[1];
                msgIMU.angular_velocity.y = ar[0];
                msgIMU.angular_velocity.z = -ar[2];
                // Flip x and y then invert z
                msgIMU.linear_acceleration.x = al[1];
                msgIMU.linear_acceleration.y = al[0];
                msgIMU.linear_acceleration.z = -al[2];

                if (cd.hasAttitudeUncertainty())
                {
                    vec3f orientationStdDev = cd.attitudeUncertainty();
                    msgIMU.orientation_covariance[0] = pow(orientationStdDev[1]*M_PI/180, 2); // Convert to radians pitch
                    msgIMU.orientation_covariance[4] = pow(orientationStdDev[0]*M_PI/180, 2); // Convert to radians Roll
                    msgIMU.orientation_covariance[8] = pow(orientationStdDev[2]*M_PI/180, 2); // Convert to radians Yaw
                }
            }

          msgIMU.orientation = quat_msg;
        }
        else
        {
            msgIMU.orientation.x = q[0];
            msgIMU.orientation.y = q[1];
            msgIMU.orientation.z = q[2];
            msgIMU.orientation.w = q[3];

            msgIMU.angular_velocity.x = ar[0];
            msgIMU.angular_velocity.y = ar[1];
            msgIMU.angular_velocity.z = ar[2];
            msgIMU.linear_acceleration.x = al[0];
            msgIMU.linear_acceleration.y = al[1];
            msgIMU.linear_acceleration.z = al[2];
        }
        // Covariances pulled from parameters
        msgIMU.angular_velocity_covariance = angular_vel_covariance;
        msgIMU.linear_acceleration_covariance = linear_accel_covariance;
        pubIMU.publish(msgIMU);
    }*/

    // Magnetic Field
    /*if (cd.hasMagnetic())
    {
        vec3f mag = cd.magnetic();
        sensor_msgs::MagneticField msgMag;
        msgMag.header.stamp = msgIMU.header.stamp;
        msgMag.header.frame_id = msgIMU.header.frame_id;
        msgMag.magnetic_field.x = mag[0];
        msgMag.magnetic_field.y = mag[1];
        msgMag.magnetic_field.z = mag[2];
        pubMag.publish(msgMag);
    }*/

    // GPS
    /*if (user_data.device_family != VnSensor::Family::VnSensor_Family_Vn100)
    {
        vec3d lla = cd.positionEstimatedLla();

        sensor_msgs::NavSatFix msgGPS;
        msgGPS.header.stamp = msgIMU.header.stamp;
        msgGPS.header.frame_id = msgIMU.header.frame_id;
        msgGPS.latitude = lla[0];
        msgGPS.longitude = lla[1];
        msgGPS.altitude = lla[2];

        // Read the estimation uncertainty (1 Sigma) from the sensor and write it to the covariance matrix.
        if(cd.hasPositionUncertaintyEstimated())
        {
            double posVariance = pow(cd.positionUncertaintyEstimated(), 2);
            msgGPS.position_covariance[0] = posVariance;    // East position variance
            msgGPS.position_covariance[4] = posVariance;    // North position vaciance
            msgGPS.position_covariance[8] = posVariance;    // Up position variance

            // mark gps fix as not available if the outputted standard deviation is 0
            if(cd.positionUncertaintyEstimated() != 0.0)
            {
                // Position available
                msgGPS.status.status = sensor_msgs::NavSatStatus::STATUS_FIX;
            } else {
                // position not detected
                msgGPS.status.status = sensor_msgs::NavSatStatus::STATUS_NO_FIX;
            }

            // add the type of covariance to the gps message
            msgGPS.position_covariance_type = sensor_msgs::NavSatFix::COVARIANCE_TYPE_DIAGONAL_KNOWN;
        } else {
            msgGPS.position_covariance_type = sensor_msgs::NavSatFix::COVARIANCE_TYPE_UNKNOWN;
        }

        pubGPS.publish(msgGPS);

        // Odometry
        if (pubOdom.getNumSubscribers() > 0)
        {
            nav_msgs::Odometry msgOdom;
            msgOdom.header.stamp = msgIMU.header.stamp;
            msgOdom.child_frame_id = frame_id;
            msgOdom.header.frame_id = map_frame_id;
            vec3d pos = cd.positionEstimatedEcef();

            if (!initial_position_set)
            {
                initial_position_set = true;
                initial_position.x = pos[0];
                initial_position.y = pos[1];
                initial_position.z = pos[2];
                vec3d lla = cd.positionEstimatedLla();
                float refx = (M_PI / 180)*(lla[0]);
                float refy = (M_PI / 180)*(lla[1]);
                Pe_ref << pos[0],
            			  pos[1],
			              pos[2];
                Rne << -sin(refx) * cos(refy), -sin(refx) * sin(refy), cos(refx),
                       -sin(refy), cos(refy), 0,
                       -cos(refx) * cos(refy), -cos(refx) * sin(refy), -sin(refx);
                ins_ref.x = lla[0];
                ins_ref.y = lla[1];
                ecef_ref.x = pos[0];
                ecef_ref.y = pos[1];
                ecef_ref.z = pos[2];
            }

            msgOdom.pose.pose.position.x = pos[0] - initial_position[0];
            msgOdom.pose.pose.position.y = pos[1] - initial_position[1];
            msgOdom.pose.pose.position.z = pos[2] - initial_position[2];

            // Read the estimation uncertainty (1 Sigma) from the sensor and write it to the covariance matrix.
            if(cd.hasPositionUncertaintyEstimated())
            {
                double posVariance = pow(cd.positionUncertaintyEstimated(), 2);
                msgOdom.pose.covariance[0] = posVariance;   // x-axis position variance
                msgOdom.pose.covariance[7] = posVariance;   // y-axis position vaciance
                msgOdom.pose.covariance[14] = posVariance;  // z-axis position variance
            }

            if (cd.hasQuaternion())
            {
                vec4f q = cd.quaternion();

                msgOdom.pose.pose.orientation.x = q[0];
                msgOdom.pose.pose.orientation.y = q[1];
                msgOdom.pose.pose.orientation.z = q[2];
                msgOdom.pose.pose.orientation.w = q[3];

                // Read the estimation uncertainty (1 Sigma) from the sensor and write it to the covariance matrix.
                if(cd.hasAttitudeUncertainty())
                {
                    vec3f orientationStdDev = cd.attitudeUncertainty();
                    // convert the standard deviation values from all three axis from degrees to radiant and calculate the variances from these (squared), which are assigned to the covariance matrix.
                    msgOdom.pose.covariance[21] = pow(orientationStdDev[0] * M_PI / 180, 2);    // yaw variance
                    msgOdom.pose.covariance[28] = pow(orientationStdDev[1] * M_PI / 180, 2);    // pitch variance
                    msgOdom.pose.covariance[35] = pow(orientationStdDev[2] * M_PI / 180, 2);    // roll variance
                }
            }

            // Add the velocity in the body frame (frame_id) to the message
            if (cd.hasVelocityEstimatedBody())
            {
                vec3f vel = cd.velocityEstimatedBody();

                msgOdom.twist.twist.linear.x = vel[0];
                msgOdom.twist.twist.linear.y = vel[1];
                msgOdom.twist.twist.linear.z = vel[2];

                // Read the estimation uncertainty (1 Sigma) from the sensor and write it to the covariance matrix.
                if(cd.hasVelocityUncertaintyEstimated())
                {
                    double velVariance = pow(cd.velocityUncertaintyEstimated(), 2);
                    msgOdom.twist.covariance[0] = velVariance;  // x-axis velocity variance
                    msgOdom.twist.covariance[7] = velVariance;  // y-axis velocity vaciance
                    msgOdom.twist.covariance[14] = velVariance; // z-axis velocity variance
                }
            }
            if (cd.hasAngularRate())
            {
                vec3f ar = cd.angularRate();

                msgOdom.twist.twist.angular.x = ar[0];
                msgOdom.twist.twist.angular.y = ar[1];
                msgOdom.twist.twist.angular.z = ar[2];

                // add covariance matrix of the measured angular rate to odom message.
                // go through matrix rows
                for(int row = 0; row < 3; row++) {
                    // go through matrix columns
                    for(int col = 0; col < 3; col++) {
                        // Target matrix has 6 rows and 6 columns, source matrix has 3 rows and 3 columns. The covariance values are put into the fields (3, 3) to (5, 5) of the destination matrix.
                        msgOdom.twist.covariance[(row + 3) * 6 + (col + 3)] = angular_vel_covariance[row * 3 + col];
                    }
                }
            }
            pubOdom.publish(msgOdom);
        }
    }*/

    // Temperature
    /*if (cd.hasTemperature())
    {
        float temp = cd.temperature();

        sensor_msgs::Temperature msgTemp;
        msgTemp.header.stamp = msgIMU.header.stamp;
        msgTemp.header.frame_id = msgIMU.header.frame_id;
        msgTemp.temperature = temp;
        pubTemp.publish(msgTemp);
    }*/

    // Barometer
    /*if (cd.hasPressure())
    {
        float pres = cd.pressure();

        sensor_msgs::FluidPressure msgPres;
        msgPres.header.stamp = msgIMU.header.stamp;
        msgPres.header.frame_id = msgIMU.header.frame_id;
        msgPres.fluid_pressure = pres;
        pubPres.publish(msgPres);
    }*/

    // INS
    /*vectornav::Ins msgINS;
    msgINS.header.stamp = ros::Time::now();
    msgINS.header.frame_id = frame_id;

    if (cd.hasInsStatus())
    {
        InsStatus insStatus = cd.insStatus();
        msgINS.insStatus = static_cast<uint16_t>(insStatus);
    }

    if (cd.hasTow()){
        msgINS.time = cd.tow();
    }

    if (cd.hasWeek()){
        msgINS.week = cd.week();
    }*/

    /*if (cd.hasTimeUtc()){
        TimeUtc utcTime = cd.timeUtc();
        char* utcTimeBytes = reinterpret_cast<char*>(&utcTime);
        //msgINS.utcTime bytes are in Little Endian Byte Order
        std::memcpy(&msgINS.utcTime, utcTimeBytes, 8);
    }*/

    //if (cd.hasYawPitchRoll()) {
        //vec3f rpy = cd.yawPitchRoll();
        /*msgINS.yaw = rpy[0];
        msgINS.pitch = rpy[1];
        msgINS.roll = rpy[2];*/
       // ins_pose.theta = (M_PI / 180)*(rpy[0]);
   // }

    //if (cd.hasPositionEstimatedLla()) {
       // vec3d lla = cd.positionEstimatedLla();
        /*msgINS.latitude = lla[0];
        msgINS.longitude = lla[1];
        msgINS.altitude = lla[2];*/
       // ins_pose.x = lla[0];
       // ins_pose.y = lla[1];
    //}

    /*if (cd.hasPositionEstimatedEcef()) {
        vec3d pos = cd.positionEstimatedEcef();
            if (!initial_position_set)
            {
                initial_position_set = true;
                initial_position.x = pos[0];
                initial_position.y = pos[1];
                initial_position.z = pos[2];
                vec3d lla = cd.positionEstimatedLla();
                float refx = (M_PI / 180)*(lla[0]);
                float refy = (M_PI / 180)*(lla[1]);
                Pe_ref << pos[0],
            			  pos[1],
			              pos[2];
                Rne << -sin(refx) * cos(refy), -sin(refx) * sin(refy), cos(refx),
                       -sin(refy), cos(refy), 0,
                       -cos(refx) * cos(refy), -cos(refx) * sin(refy), -sin(refx);
                ins_ref.x = lla[0];
                ins_ref.y = lla[1];
                ecef_ref.x = pos[0];
                ecef_ref.y = pos[1];
                ecef_ref.z = pos[2];
            }
        Pe << pos[0],
			  pos[1],
              pos[2];
        ECEF_pose.x = Pe(0);
        ECEF_pose.y = Pe(1);
        ECEF_pose.z = Pe(2);
        NED = Rne * (Pe - Pe_ref);
        NED_pose.x = NED(0);
        NED_pose.y = NED(1);
        NED_pose.theta = ins_pose.theta;
    }*/

    //if (cd.hasPositionEstimatedLla() & cd.hasYawPitchRoll() & cd.hasPositionEstimatedEcef()) {
  /*  if (cd.hasPositionEstimatedLla() == true && cd.hasYawPitchRoll() == true) {
        //vec3d pos = cd.positionEstimatedEcef();
        vec3f  = cd.yawPitchRoll();
        vec3d lla = cd.positionEstimatedLla();
            if (initial_position_set == false && lla[0] != 0.0)
            {
                initial_position_set = true;
                ROS_WARN("in");
                double refx = (M_PI / 180)*(lla[0]); //starting latitude in radians
                double refy = (M_PI / 180)*(lla[1]); //starting longitude in radians
                Ne = (re) / (pow(1 - (ecc*ecc * sin(refx)*sin(refx)),0.5));
                Pe_ref << Ne * cos(refx)*cos(refy),
            			  Ne * cos(refx)*sin(refy),
			              (Ne*(1 - ecc*ecc)) * sin(refx);
                Rne << -sin(refx) * cos(refy), -sin(refx) * sin(refy), cos(refx),
                       -sin(refy), cos(refy), 0,
                       -cos(refx) * cos(refy), -cos(refx) * sin(refy), -sin(refx);
                ins_ref.x = lla[0];
                ins_ref.y = lla[1];
                ins_ref.theta = (M_PI / 180)*(rpy[0]);
                ecef_ref.x = Pe_ref(0);
                ecef_ref.y = Pe_ref(1);
                ecef_ref.z = Pe_ref(2);
            }
        lat_radians = (M_PI / 180)*(lla[0]);
        lon_radians = (M_PI / 180)*(lla[1]);
        Ne = (re) / (pow(1 - (ecc*ecc * sin(lat_radians)*sin(lat_radians)),0.5));
        Pe << Ne * cos(lat_radians)*cos(lon_radians),
			  Ne * cos(lat_radians)*sin(lon_radians),
              (Ne*(1 - ecc*ecc)) * sin(lat_radians);
        ECEF_pose.x = Pe(0);
        ECEF_pose.y = Pe(1);
        ECEF_pose.z = Pe(2);
        NED = Rne * (Pe - Pe_ref);
        NED_pose.x = NED(0);
        NED_pose.y = NED(1);
        ins_pose.x = lla[0];
        ins_pose.y = lla[1];
        ins_pose.theta = (M_PI / 180)*(rpy[0]);
        NED_pose.theta = ins_pose.theta;
        ins_pos_pub.publish(ins_pose);
        NED_pose_pub.publish(NED_pose);
        ECEF_pose_pub.publish(ECEF_pose);
        ins_ref_pub.publish(ins_ref);
        ecef_ref_pub.publish(ecef_ref);
        } */

    /*if (cd.hasVelocityEstimatedNed()) {
        vec3f nedVel = cd.velocityEstimatedNed();
        msgINS.nedVelX = nedVel[0];
        msgINS.nedVelY = nedVel[1];
        msgINS.nedVelZ = nedVel[2];
    }*/

   /* if (cd.hasVelocityEstimatedBody() & cd.hasAngularRate()) {
        vec3f bodyVel = cd.velocityEstimatedBody();
        vec3f ar = cd.angularRate();
        local_vel.z = ar[2]; //yaw rate
        local_vel.x = bodyVel[0]; //surge velocity
	    local_vel.y = bodyVel[1]; //sway velocity
        local_vel_pub.publish(local_vel);
    }
    */
   if (cd.hasYawPitchRoll()){

       vec3f at = cd.yawPitchRoll();

       att_rpy.x = (M_PI / 180)*(at[2]);
       att_rpy.y = (M_PI / 180)*(at[1]);
       att_rpy.z = (M_PI / 180)*(at[0]);
       att_rpy_pub.publish(att_rpy); 

   }

   if (cd.hasAngularRate() & cd.hasAcceleration() ){
       vec3f ar= cd.angularRate();
        vec3f al = cd.acceleration();

        ang_rate.x = ar[0];
        ang_rate.y = ar[1];
        ang_rate.z = ar[2];
        ang_rate_pub.publish(ang_rate);

        accel.x = al[0];
        accel.y = al[1];
        accel.z = al[2];
        accel_pub.publish(accel);
   }

  /*  if (cd.hasQuaternion() && cd.has AngularRate() && cd.hasAcceleration()) //no se ocupa quaternion
    {

        vec4f q = cd.quaternion();
        vec3f ar = cd.angularRate();
        vec3f al = cd.acceleration(); //que aceleraciones me da?

        local_vel.x = ar[0]; //roll rate
        local_vel.y = ar[1]; //pitch rate
        local_vel.z = ar[2]; //yaw rate


        if (cd.hasAttitudeUncertainty()) //esto me da en radianes o considera algo mas?
        {
            vec3f orientationStdDev = cd.attitudeUncertainty();
            msgIMU.orientation_covariance[0] = pow(orientationStdDev[2]*M_PI/180, 2); // Convert to radians pitch
            msgIMU.orientation_covariance[4] = pow(orientationStdDev[1]*M_PI/180, 2); // Convert to radians Roll
            msgIMU.orientation_covariance[8] = pow(orientationStdDev[0]*M_PI/180, 2); // Convert to radians Yaw
        }
        
    }*/


    /*if (cd.hasAngularRate()) {
        vec3f ar = cd.angularRate();
        local_vel.z = ar[2]; //yaw rate
    }*/

    /*if (cd.hasAttitudeUncertainty())
    {
        vec3f attUncertainty = cd.attitudeUncertainty();
        msgINS.attUncertainty[0] = attUncertainty[0];
        msgINS.attUncertainty[1] = attUncertainty[1];
        msgINS.attUncertainty[2] = attUncertainty[2];
    }*/

    /*if (cd.hasPositionUncertaintyEstimated()){
        msgINS.posUncertainty = cd.positionUncertaintyEstimated();
    }*/

    /*if (cd.hasVelocityUncertaintyEstimated()){
        msgINS.velUncertainty = cd.velocityUncertaintyEstimated();
    }*/

    //if (msgINS.insStatus && msgINS.utcTime) {
        //pubIns.publish(msgINS);
    /*ins_pos_pub.publish(ins_pose);
    local_vel_pub.publish(local_vel);
    NED_pose_pub.publish(NED_pose);
    ECEF_pose_pub.publish(ECEF_pose);
    ins_ref_pub.publish(ins_ref);
    ecef_ref_pub.publish(ecef_ref);*/
    //}
//velocidad angular RPY (body), aceleracion SSH(body), angulo RPY(inertial), covarianzas
}

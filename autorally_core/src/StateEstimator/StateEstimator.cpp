/*
* Software License Agreement (BSD License)
* Copyright (c) 2013, Georgia Institute of Technology
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice, this
* list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright notice,
* this list of conditions and the following disclaimer in the documentation
* and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/**********************************************
 * @file StateEstimator.cpp
 * @author Paul Drews <pdrews3@gatech.edu>
 * @date May 1, 2017
 * @copyright 2017 Georgia Institute of Technology
 * @brief ROS node to fuse information sources and create an accurate state estimation
 *
 * @details Subscribes to the GPS, IMU, and wheel odometry topics, claculates
 * an estimate of the car's current state using GTSAM, and publishes that data.
 ***********************************************/



#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <fstream>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>

#include <boost/algorithm/string.hpp>
#include <boost/thread/thread.hpp>
#include <vector>
#include "StateEstimator.h"

#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/slam/PriorFactor.h>
#include <gtsam/navigation/ImuFactor.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/navigation/ImuBias.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/nonlinear/GaussNewtonOptimizer.h>

// Convenience for named keys
using symbol_shorthand::X;
using symbol_shorthand::V;
using symbol_shorthand::B;
// Gps pose
using symbol_shorthand::G;

namespace autorally_core
{

  StateEstimator::StateEstimator() :
    Diagnostics("StateEstimator", "", ""),
    m_nh("~"),
    m_biasKey(0),
    m_poseVelKey(0),
    m_lastImuT(0.0),
    m_lastImuTgps(0.0),
    m_imuQPrevTime(0),
    m_gpsCounter(0),
    m_maxQSize(0),
    m_gpsOptQ(40),
    m_ImuOptQ(400),
    m_odomOptQ(100),
    m_gotFirstFix(false)
  {
    double accSigma, gyroSigma;
    m_nh.param<double>("InitialYaw", m_initialYaw, 5);
    m_nh.param<double>("InitialRotationNoise", m_initialRotationNoise, 1.0);
    m_nh.param<double>("InitialVelocityNoise", m_initialVelNoise, 0.1);
    m_nh.param<double>("InitialBiasNoiseAcc", m_initialBiasNoiseAcc, 1e-1);
    m_nh.param<double>("InitialBiasNoiseGyro", m_initialBiasNoiseGyro, 1e-2);
    m_nh.param<double>("AccelerometerSigma", accSigma, 6.0e-2);
    m_nh.param<double>("GyroSigma", gyroSigma, 2.0e-2);
    m_nh.param<double>("AccelBiasSigma", m_AccelBiasSigma, 2.0e-4);
    m_nh.param<double>("GyroBiasSigma", m_GyroBiasSigma, 3.0e-5);
    m_nh.param<double>("GPSSigma", m_gpsSigma, 0.07);
    m_nh.param<double>("SensorTransformX", m_sensorX, 0.0);
    m_nh.param<double>("SensorTransformY", m_sensorY, 0.0);
    m_nh.param<double>("SensorTransformZ", m_sensorZ, 0.0);
    m_nh.param<double>("SensorXAngle",  m_sensorXAngle, 0);
    m_nh.param<double>("SensorYAngle", m_sensorYAngle, 0);
    m_nh.param<double>("SensorZAngle",   m_sensorZAngle, 0);
    m_nh.param<double>("CarXAngle",  m_carXAngle, 0);
    m_nh.param<double>("CarYAngle",  m_carYAngle, 0);
    m_nh.param<double>("CarZAngle",  m_carZAngle, 0);
    m_nh.param<int>("GpsSkip",   m_gpsSkip, 5);
    m_nh.param<double>("Gravity",   m_gravityMagnitude, 9.8);
    //Limit to about 0.9g
//    m_nh.param<double>("MaxGPSAccel", m_accelLimit, 9.0);
    m_nh.param<bool>("InvertX", m_invertx, false);
    m_nh.param<bool>("InvertY", m_inverty, false);
    m_nh.param<bool>("InvertZ", m_invertz, false);
    m_nh.param<double>("Imudt", m_imuDt, 1.0/200.0);

    double gpsx, gpsy, gpsz;
    m_nh.param<double>("GPSX",  gpsx, 0);
    m_nh.param<double>("GPSY",  gpsy, 0);
    m_nh.param<double>("GPSZ",  gpsz, 0);
    m_imuPgps = Pose3(Rot3(), Point3(gpsx, gpsy, gpsz));
    m_imuPgps.print("IMU->GPS");

    bool fixedInitialPose;
    double initialRoll, intialPitch, initialYaw;

    m_nh.param<bool>("FixedInitialPose", fixedInitialPose, false);
    m_nh.param<double>("initialRoll", initialRoll, 0);
    m_nh.param<double>("intialPitch", intialPitch, 0);
    m_nh.param<double>("initialYaw", initialYaw, 0);

    double latOrigin, lonOrigin, altOrigin;
    m_nh.param<bool>("FixedOrigin", m_fixedOrigin, false);
    m_nh.param<double>("latOrigin", latOrigin, 0);
    m_nh.param<double>("lonOrigin", lonOrigin, 0);
    m_nh.param<double>("altOrigin", altOrigin, 0);

    if (m_fixedOrigin)
      m_enu.Reset(latOrigin, lonOrigin, altOrigin);


    std::cout << "InitialYaw " << m_initialYaw << "\n"
    << "InitialRotationNoise " << m_initialRotationNoise << "\n"
    << "InitialVelocityNoise " << m_initialVelNoise << "\n"
    << "InitialBiasNoiseAcc " << m_initialBiasNoiseAcc << "\n"
    << "InitialBiasNoiseGyro " << m_initialBiasNoiseGyro << "\n"
    << "AccelerometerSigma " << accSigma << "\n"
    << "GyroSigma " << gyroSigma << "\n"
    << "AccelBiasSigma " << m_AccelBiasSigma << "\n"
    << "GyroBiasSigma " << m_GyroBiasSigma << "\n"
    << "GPSSigma " << m_gpsSigma << "\n"
    << "SensorTransformX " << m_sensorX << "\n"
    << "SensorTransformY " << m_sensorY << "\n"
    << "SensorTransformZ " << m_sensorZ << "\n"
    << "SensorXAngle " <<  m_sensorXAngle << "\n"
    << "SensorYAngle " << m_sensorYAngle << "\n"
    << "SensorZAngle " <<   m_sensorZAngle << "\n"
    << "CarXAngle " <<  m_carXAngle << "\n"
    << "CarYAngle " <<  m_carYAngle << "\n"
    << "CarZAngle " <<  m_carZAngle << "\n"
    << "GpsSkip " <<   m_gpsSkip << "\n"
    << "Gravity " <<   m_gravityMagnitude << "\n";

    // Use an ENU frame
    m_preintegrationParams =  PreintegrationParams::MakeSharedU(m_gravityMagnitude);
    m_preintegrationParams->accelerometerCovariance = accSigma * I_3x3;
    m_preintegrationParams->gyroscopeCovariance = gyroSigma * I_3x3;
    m_preintegrationParams->integrationCovariance = 1e-5 * I_3x3;

    Vector biases((Vector(6) << 0, 0, 0, 0, 0, 0).finished());
    m_optimizedBias = imuBias::ConstantBias(biases);
    m_previousBias = imuBias::ConstantBias(biases);
    m_imuPredictor = boost::make_shared<PreintegratedImuMeasurements>(m_preintegrationParams,m_optimizedBias);

    m_prevTime = ros::TIME_MIN;
    m_optimizedTime = 0;

    imu_3dm_gx4::FilterOutputConstPtr ip;
    if (!fixedInitialPose) {
      while (!ip)
      {
        ROS_WARN("Waiting for valid initial pose");
        ip = ros::topic::waitForMessage<imu_3dm_gx4::FilterOutput>("/imu/filter", m_nh, ros::Duration(15));
      }
      m_initialPose = *ip;
    }
    else {
      ROS_WARN("Using fixed initial pose");
      Rot3 initialRotation = Rot3::Ypr(initialYaw, intialPitch, initialRoll);
      m_initialPose.orientation.w = initialRotation.quaternion()[0];
      m_initialPose.orientation.x = initialRotation.quaternion()[1];
      m_initialPose.orientation.y = initialRotation.quaternion()[2];
      m_initialPose.orientation.z = initialRotation.quaternion()[3];
      m_initialPose.bias.x = 0;
      m_initialPose.bias.y = 0;
      m_initialPose.bias.z = 0;

    }

    Rot3 initRot(Quaternion(m_initialPose.orientation.w, m_initialPose.orientation.x, m_initialPose.orientation.y, m_initialPose.orientation.z));

    m_bodyPSensor = Pose3(Rot3::RzRyRx(m_sensorXAngle, m_sensorYAngle, m_sensorZAngle),
        Point3(m_sensorX, m_sensorY, m_sensorZ));
    m_carENUPcarNED = Pose3(Rot3::RzRyRx(m_carXAngle, m_carYAngle, m_carZAngle), Point3());

    m_bodyPSensor.print("Body pose\n");
    m_carENUPcarNED.print("CarBodyPose\n");

    m_posePub = m_nh.advertise<nav_msgs::Odometry>("pose", 1);
    m_biasAccPub = m_nh.advertise<geometry_msgs::Point>("bias_acc", 1);
    m_biasGyroPub = m_nh.advertise<geometry_msgs::Point>("bias_gyro", 1);
    m_timePub = m_nh.advertise<geometry_msgs::Point>("time_delays", 1);


    m_gravity << 0, 0, m_gravityMagnitude; // Define gravity
    m_omegaCoriolis << 0, 0, 0;

    ISAM2Params params;
    params.factorization = ISAM2Params::QR;
    m_isam = new ISAM2(params);

    m_prevVel = (Vector(3) << 0.0,0.0,0.0).finished();

    // prior on the first pose
    priorNoisePose = noiseModel::Diagonal::Sigmas(
         (Vector(6) << m_initialRotationNoise, m_initialRotationNoise, 3*m_initialRotationNoise,
             m_gpsSigma, m_gpsSigma, m_gpsSigma).finished());
     // add to solver and values

     // Add velocity prior
     priorNoiseVel = noiseModel::Diagonal::Sigmas(
         (Vector(3) << m_initialVelNoise, m_initialVelNoise, m_initialVelNoise).finished());

     // Add bias prior
     priorNoiseBias = noiseModel::Diagonal::Sigmas(
         (Vector(6) << m_initialBiasNoiseAcc,
             m_initialBiasNoiseAcc,
             m_initialBiasNoiseAcc,
             m_initialBiasNoiseGyro,
             m_initialBiasNoiseGyro,
             m_initialBiasNoiseGyro).finished());

      sigma_acc_bias_c << m_AccelBiasSigma,  m_AccelBiasSigma,  m_AccelBiasSigma;
      sigma_gyro_bias_c << m_GyroBiasSigma, m_GyroBiasSigma, m_GyroBiasSigma;

     noiseModelBetweenbias_sigma = (Vector(6) << sigma_acc_bias_c, sigma_gyro_bias_c).finished();
     noiseModelBetweenbias = noiseModel::Diagonal::Sigmas((noiseModelBetweenbias_sigma));

     m_gpsSub = m_nh.subscribe("/gpsRoverStatus", 300, &StateEstimator::GpsCallback, this);
     m_imuSub = m_nh.subscribe("/imu/imu", 600, &StateEstimator::ImuCallback, this);
     m_odomSub = m_nh.subscribe("/wheel_odom", 300, &StateEstimator::WheelOdomCallback, this);


     boost::thread optimizer(&StateEstimator::GpsHelper,this);

  }

  StateEstimator::~StateEstimator()
  {}

  void StateEstimator::GpsCallback(sensor_msgs::NavSatFixConstPtr fix)
  {
    if (!m_gpsOptQ.pushNonBlocking(fix))
      ROS_WARN("Dropping a GPS measurement due to full queue!!");
  }

  void StateEstimator::GetAccGyro(sensor_msgs::ImuConstPtr imu, Vector3 &acc, Vector3 &gyro)
  {
    double accx, accy, accz;
    if (m_invertx) accx = -imu->linear_acceleration.x;
    else accx = imu->linear_acceleration.x;
    if (m_inverty) accy = -imu->linear_acceleration.y;
    else accy = imu->linear_acceleration.y;
    if (m_invertz) accz = -imu->linear_acceleration.z;
    else accz = imu->linear_acceleration.z;
    acc = Vector3(accx, accy, accz);

    double gx, gy, gz;
    if (m_invertx) gx = -imu->angular_velocity.x;
    else gx = imu->angular_velocity.x;
    if (m_inverty) gy = -imu->angular_velocity.y;
    else gy = imu->angular_velocity.y;
    if (m_invertz) gz = -imu->angular_velocity.z;
    else gz = imu->angular_velocity.z;

    gyro = Vector3(gx, gy, gz);
  }

  void StateEstimator::GpsHelper()
  {
    ros::Rate loopRate(10);  // limit looping rate to 10Hz
    double prevTime = 0;
    double curTime = 0;

    // Kick off the thread, and wait for our GPS measurements to come streaming in
    while (ros::ok())
    {

      sensor_msgs::NavSatFixConstPtr fix;
      bool usingGPS = false;
      bool usingOdom = false;

      // set up for using Odom or GPS but always at ~10Hz
      if (!m_gotFirstFix || (m_gpsOptQ.size() > 0))
      {
        // we are using the GPS measurement or we haven't gotten the first yet
        // only use the latests measurement
        fix = m_gpsOptQ.popBlocking();
        curTime = fix->header.stamp.toSec();
        while (m_gpsOptQ.size() > 0)
        {
          fix = m_gpsOptQ.popBlocking();
          curTime = fix->header.stamp.toSec();
        }
        usingGPS =  true;
      }
      else if (m_gotFirstFix && m_odomOptQ.size() > 0)
      {
        // using odom message instead of GPS measurement to make a factor
        curTime = m_odomOptQ.back()->header.stamp.toSec();
        usingOdom = true;
      }

      if (!m_gotFirstFix)
      {
        NonlinearFactorGraph newFactors;
        Values newVariables;
        m_gotFirstFix = true;

        double E, N, U;
        if (!m_fixedOrigin)
        {
          m_enu.Reset(fix->latitude, fix->longitude, fix->altitude);
          E = 0; N = 0; U = 0; // we're choosing this as the origin
        }
        else
        {
          m_enu.Forward(fix->latitude, fix->longitude, fix->altitude, E, N, U);
        }

        // Add prior factors on pose, vel and bias
        Rot3 initialOrientation = Rot3::Quaternion(m_initialPose.orientation.w,
            m_initialPose.orientation.x,
            m_initialPose.orientation.y,
            m_initialPose.orientation.z);
        std::cout << "Initial orientation" << std::endl;
        std::cout << m_bodyPSensor.rotation() * initialOrientation * m_carENUPcarNED.rotation() << std::endl;
        Pose3 x0(m_bodyPSensor.rotation() * initialOrientation * m_carENUPcarNED.rotation(),
            Point3(E, N, U));
        m_prevPose = x0;
        PriorFactor<Pose3> priorPose(X(0), x0, priorNoisePose);
        newFactors.add(priorPose);
        PriorFactor<Vector3> priorVel(V(0), Vector3(0, 0, 0), priorNoiseVel);
        newFactors.add(priorVel);
        Vector biases((Vector(6) << 0, 0, 0, m_initialPose.bias.x,
            -m_initialPose.bias.y, -m_initialPose.bias.z).finished());
        m_previousBias = imuBias::ConstantBias(biases);
        PriorFactor<imuBias::ConstantBias> priorBias(B(0), imuBias::ConstantBias(biases), priorNoiseBias);
        newFactors.add(priorBias);

        //Factor for imu->gps translation
        BetweenFactor<Pose3> imuPgpsFactor(X(0), G(0), m_imuPgps,
            noiseModel::Diagonal::Sigmas((Vector(6) << 0.001,0.001,0.001,0.03,0.03,0.03).finished()));
        newFactors.add(imuPgpsFactor);

        // add prior values on pose, vel and bias
        newVariables.insert(X(0), x0);
        newVariables.insert(V(0), Vector3(0, 0, 0));
        newVariables.insert(B(0), imuBias::ConstantBias(biases));
        newVariables.insert(G(0), x0.compose(m_imuPgps));

        m_isam->update(newFactors, newVariables);
        //Read IMU measurements up to the first GPS measurement
        m_lastIMU = m_ImuOptQ.popBlocking();
        //If we only pop one, we need some dt
        m_lastImuTgps = m_lastIMU->header.stamp.toSec() - 0.005;
        while(m_lastIMU->header.stamp.toSec() < fix->header.stamp.toSec())
        {
          m_lastImuTgps = m_lastIMU->header.stamp.toSec();
          m_lastIMU = m_ImuOptQ.popBlocking();
        }

        prevTime = curTime;
        loopRate.sleep();
      }
      else if (usingGPS || usingOdom)
      {
        if (fix == NULL)
          std::cout<<"adding factor with no GPS measurement"<<std::endl;

        NonlinearFactorGraph newFactors;
        Values newVariables;

        // toss out old wheel odom messages (ie. before the last optimized time stamp
        while (m_odomOptQ.size() > 0 && m_odomOptQ.front()->header.stamp.toSec() < prevTime)
          m_odomOptQ.popBlocking();

        nav_msgs::OdometryPtr firstOdom = m_odomOptQ.popBlocking();
        nav_msgs::OdometryPtr lastOdom = m_odomOptQ.popBlocking();

        // only want to do the following if the odom messages are behind relative to the GPS
        if (!usingGPS || lastOdom->header.stamp.toSec() < curTime)
        {
          // tossing all odom messages except the last one before the GPS time stamp
          while (m_odomOptQ.size() != 0 && (m_odomOptQ.front()->header.stamp.toSec() < curTime))
            lastOdom = m_odomOptQ.popBlocking();

          Pose3 betweenOdomPose = Pose3(Rot3(gtsam::Quaternion(firstOdom->pose.pose.orientation.w,
              firstOdom->pose.pose.orientation.x, firstOdom->pose.pose.orientation.y, firstOdom->pose.pose.orientation.z)),
              Point3(firstOdom->pose.pose.position.x, firstOdom->pose.pose.position.y, firstOdom->pose.pose.position.z));
          Pose3 lastOdomPose = Pose3(Rot3(gtsam::Quaternion(lastOdom->pose.pose.orientation.w,
              lastOdom->pose.pose.orientation.x, lastOdom->pose.pose.orientation.y, lastOdom->pose.pose.orientation.z)),
              Point3(lastOdom->pose.pose.position.x, lastOdom->pose.pose.position.y, lastOdom->pose.pose.position.z));
          betweenOdomPose.between(lastOdomPose);

          BetweenFactor<Pose3> odomFactor(X(m_poseVelKey), X(m_poseVelKey), betweenOdomPose,
              noiseModel::Diagonal::Sigmas((Vector(6) << 0.1,0.1,100,100,100,0.3).finished())); // TODO add in legitamte noise
          newFactors.add(odomFactor);
        }

        // integrating the IMU measurements
        PreintegratedImuMeasurements pre_int_data(m_preintegrationParams, m_previousBias);
        while(m_lastIMU->header.stamp.toSec() < curTime)
        {
          Vector3 acc, gyro;
          GetAccGyro(m_lastIMU, acc, gyro);
          double imuDT = m_lastIMU->header.stamp.toSec() - m_lastImuTgps;
          m_lastImuTgps = m_lastIMU->header.stamp.toSec();
          pre_int_data.integrateMeasurement(acc, gyro, imuDT);
          m_lastIMU = m_ImuOptQ.popBlocking();
        }
        // adding the integrated IMU measurements to the factor graph
        ImuFactor imuFactor(X(m_poseVelKey), V(m_poseVelKey), X(m_poseVelKey+1), V(m_poseVelKey+1), B(m_biasKey),
                  pre_int_data);

        newFactors.add(imuFactor);
        newFactors.add(BetweenFactor<imuBias::ConstantBias>(B(m_biasKey), B(m_biasKey+1), imuBias::ConstantBias(),
            noiseModel::Diagonal::Sigmas( sqrt(pre_int_data.deltaTij()) * noiseModelBetweenbias_sigma)));

        // Predict forward to get an initialization for the pose and velocity
        NavState curNavState(m_prevPose, m_prevVel);
        NavState nextNavState = pre_int_data.predict(curNavState, m_previousBias);

        if (usingGPS)
        {
          double E, N, U;
          m_enu.Forward(fix->latitude, fix->longitude, fix->altitude, E, N, U);
          SharedDiagonal gpsNoise = noiseModel::Diagonal::Sigmas(Vector3(m_gpsSigma, m_gpsSigma, 3.0 * m_gpsSigma));

          GPSFactor gpsFactor(G(m_poseVelKey+1), Point3(E, N, U), gpsNoise);
          newFactors.add(gpsFactor);

          BetweenFactor<Pose3> imuPgpsFactor(X(m_poseVelKey+1), G(m_poseVelKey+1), m_imuPgps,
              noiseModel::Diagonal::Sigmas((Vector(6) << 0.001,0.001,0.001,0.03,0.03,0.03).finished()));
          newFactors.add(imuPgpsFactor);
        }

        newVariables.insert(X(m_poseVelKey+1), nextNavState.pose());
        newVariables.insert(V(m_poseVelKey+1), nextNavState.v());
        newVariables.insert(B(m_biasKey+1), m_previousBias);
        newVariables.insert(G(m_poseVelKey+1), nextNavState.pose().compose(m_imuPgps));

        m_isam->update(newFactors, newVariables);
        m_prevPose = m_isam->calculateEstimate<Pose3>(X(m_poseVelKey+1));
        m_prevVel = m_isam->calculateEstimate<Vector3>(V(m_poseVelKey+1));
        m_previousBias = m_isam->calculateEstimate<imuBias::ConstantBias>(B(m_biasKey+1));
        //std::cout << m_isam->marginalCovariance(X(m_poseVelKey+1)) << std::endl << std::endl;

        diag_ok("Still ok!");

        {
          boost::mutex::scoped_lock guard(m_optimizedStateMutex);
          m_optimizedState = NavState(m_prevPose, m_prevVel);
          m_optimizedBias = m_previousBias;
          m_optimizedTime = curTime;
        }

        nav_msgs::Odometry poseNew;
        poseNew.header.stamp = ros::Time(curTime);

        geometry_msgs::Point ptAcc;
        ptAcc.x = m_previousBias.vector()[0];
        ptAcc.y = m_previousBias.vector()[1];
        ptAcc.z = m_previousBias.vector()[2];

        geometry_msgs::Point ptGyro;
        ptGyro.x = m_previousBias.vector()[3];
        ptGyro.y = m_previousBias.vector()[4];
        ptGyro.z = m_previousBias.vector()[5];

        m_biasAccPub.publish(ptAcc);
        m_biasGyroPub.publish(ptGyro);

        m_biasKey ++;
        m_poseVelKey ++;
        prevTime = curTime;
        loopRate.sleep(); // limiting frequency of loop - we only do this if we have just added a factor
      }
    }
  }

  void StateEstimator::ImuCallback(sensor_msgs::ImuConstPtr imu)
  {
    double dt;
    if (m_lastImuT == 0) dt = 0.005;
    else dt = imu->header.stamp.toSec() - m_lastImuT;
    m_lastImuT = imu->header.stamp.toSec();
    ros::Time before = ros::Time::now();
    // Push the IMU measurement to the optimization thread
    int qSize = m_ImuOptQ.size();
    if (qSize > m_maxQSize)
    {
      m_maxQSize = qSize;
      if (m_maxQSize > 20) {
        ROS_WARN("Queue size %d", m_maxQSize);
      }
    }
    if (!m_ImuOptQ.pushNonBlocking(imu))
    {
      ROS_WARN("Dropping an IMU measurement due to full queue!!");
    }
    // Each time we get an imu measurement, calculate the incremental pose from the last GTSAM pose
    m_imuMeasurements.push_back(imu);
    //Grab the most current optimized state
    double optimizedTime;
    NavState optimizedState;
    imuBias::ConstantBias optimizedBias;
    {
      boost::mutex::scoped_lock guard(m_optimizedStateMutex);
      optimizedState = m_optimizedState;
      optimizedBias = m_optimizedBias;
      optimizedTime = m_optimizedTime;
    }
    if (optimizedTime == 0) return;

    bool newMeasurements = false;
    int numImuDiscarded = 0;
    Vector3 acc, gyro;
    while (!m_imuMeasurements.empty() && (m_imuMeasurements.front()->header.stamp.toSec() < optimizedTime))
    {
      m_imuQPrevTime = m_imuMeasurements.front()->header.stamp.toSec();
      m_imuMeasurements.pop_front();
      newMeasurements = true;
      numImuDiscarded ++;
    }

    if(newMeasurements)
    {
      //We need to reset integration and iterate through all our IMU measurements
      m_imuPredictor->resetIntegration();
      int numMeasurements = 0;
      for (auto it=m_imuMeasurements.begin(); it!=m_imuMeasurements.end(); ++it)
      {
        double dt_temp =  (*it)->header.stamp.toSec() - m_imuQPrevTime;
        m_imuQPrevTime = (*it)->header.stamp.toSec();
        GetAccGyro(*it, acc, gyro);
        m_imuPredictor->integrateMeasurement(acc, gyro, dt_temp);
        numMeasurements++;
//        ROS_INFO("IMU time %f, dt %f", (*it)->header.stamp.toSec(), dt_temp);
      }
//      ROS_INFO("Resetting Integration, %d measurements integrated, %d discarded", numMeasurements, numImuDiscarded);
    }
    else
    {
      //Just need to add the newest measurement, no new optimized pose
      GetAccGyro(imu, acc, gyro);
      m_imuPredictor->integrateMeasurement(acc, gyro, dt);//m_bodyPSensor);
//      ROS_INFO("Integrating %f, dt %f", m_lastImuT, dt);

    }
    NavState currentPose = m_imuPredictor->predict(optimizedState, optimizedBias);
    nav_msgs::Odometry poseNew;
    poseNew.header.stamp = imu->header.stamp;

    Vector4 q = currentPose.quaternion().coeffs();
    poseNew.pose.pose.orientation.x = q[0];
    poseNew.pose.pose.orientation.y = q[1];
    poseNew.pose.pose.orientation.z = q[2];
    poseNew.pose.pose.orientation.w = q[3];

    poseNew.pose.pose.position.x = currentPose.position().x();
    poseNew.pose.pose.position.y = currentPose.position().y();
    poseNew.pose.pose.position.z = currentPose.position().z();

    poseNew.twist.twist.linear.x = currentPose.velocity().x();
    poseNew.twist.twist.linear.y = currentPose.velocity().y();
    poseNew.twist.twist.linear.z = currentPose.velocity().z();
    
    poseNew.twist.twist.angular.x = gyro.x() + optimizedBias.gyroscope().x();
    poseNew.twist.twist.angular.y = gyro.y() + optimizedBias.gyroscope().y();
    poseNew.twist.twist.angular.z = gyro.z() + optimizedBias.gyroscope().z();

    poseNew.child_frame_id = "base_link";
    poseNew.header.frame_id = "odom";

    m_posePub.publish(poseNew);

    ros::Time after = ros::Time::now();
    geometry_msgs::Point delays;
    delays.x = imu->header.stamp.toSec();
    delays.y = (ros::Time::now() - imu->header.stamp).toSec();
    delays.z = imu->header.stamp.toSec() - optimizedTime;
    m_timePub.publish(delays);
    return;
  }

  void StateEstimator::WheelOdomCallback(nav_msgs::OdometryPtr odom)
  {
      m_odomOptQ.pushNonBlocking(odom);
        //std::cout<<"Dropping a wheel odometry measurement due to full queue!!"<<std::endl;
  }

  void StateEstimator::diagnosticStatus(const ros::TimerEvent& /*time*/)
  {
    //Don't do anything
    //diag_info("Test");
  }

};

int main (int argc, char** argv)
{
  ros::init(argc, argv, "StateEstimator");
  //ros::NodeHandle n;
  autorally_core::StateEstimator wpt;
  ros::spin();
}
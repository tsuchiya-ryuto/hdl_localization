#ifndef POSE_ESTIMATOR_HPP
#define POSE_ESTIMATOR_HPP

#include <memory>
#include <ros/ros.h>
#include <pcl/point_types.h>
#include <pcl/point_cloud.h>

#include <pclomp/ndt_omp.h>
#include <pcl/filters/voxel_grid.h>

#include <hdl_localization/pose_system.hpp>
#include <hdl_localization/odom_system.hpp>
#include <kkl/alg/unscented_kalman_filter.hpp>

namespace hdl_localization {

/**
 * @brief scan matching-based pose estimator
 */
class PoseEstimator {
public:
  using PointT = pcl::PointXYZI;

  /**
   * @brief constructor
   * @param registration        registration method
   * @param stamp               timestamp
   * @param pos                 initial position
   * @param quat                initial orientation
   * @param cool_time_duration  during "cool time", prediction is not performed
   */
  PoseEstimator(pcl::Registration<PointT, PointT>::Ptr& registration, const ros::Time& stamp, const Eigen::Vector3f& pos, const Eigen::Quaternionf& quat, double cool_time_duration = 1.0)
    : init_stamp(stamp),
      registration(registration),
      cool_time_duration(cool_time_duration)
  {
    process_noise = Eigen::MatrixXf::Identity(16, 16);
    process_noise.middleRows(0, 3) *= 1.0;
    process_noise.middleRows(3, 3) *= 1.0;
    process_noise.middleRows(6, 4) *= 0.5;
    process_noise.middleRows(10, 3) *= 1e-6;
    process_noise.middleRows(13, 3) *= 1e-6;

    Eigen::MatrixXf measurement_noise = Eigen::MatrixXf::Identity(7, 7);
    measurement_noise.middleRows(0, 3) *= 0.01;
    measurement_noise.middleRows(3, 4) *= 0.001;

    Eigen::VectorXf mean(16);
    mean.middleRows(0, 3) = pos;
    mean.middleRows(3, 3).setZero();
    mean.middleRows(6, 4) = Eigen::Vector4f(quat.w(), quat.x(), quat.y(), quat.z());
    mean.middleRows(10, 3).setZero();
    mean.middleRows(13, 3).setZero();

    Eigen::MatrixXf cov = Eigen::MatrixXf::Identity(16, 16) * 0.01;

    PoseSystem system;
    ukf.reset(new kkl::alg::UnscentedKalmanFilterX<float, PoseSystem>(system, 16, 6, 7, process_noise, measurement_noise, mean, cov));
  }

  /**
   * @brief predict
   * @param stamp    timestamp
   * @param acc      acceleration
   * @param gyro     angular velocity
   */
  void predict(const ros::Time& stamp, const Eigen::Vector3f& acc, const Eigen::Vector3f& gyro) {
    if((stamp - init_stamp).toSec() < cool_time_duration || prev_stamp.is_zero() || prev_stamp == stamp) {
      prev_stamp = stamp;
      return;
    }

    double dt = (stamp - prev_stamp).toSec();
    prev_stamp = stamp;

    ukf->setProcessNoiseCov(process_noise * dt);
    ukf->system.dt = dt;

    Eigen::VectorXf control(6);
    control.head<3>() = acc;
    control.tail<3>() = gyro;

    ukf->predict(control);
  }

  /**
   * @brief update the state of the odomety-based pose estimation
   */
  void predict_odom(const Eigen::Matrix4f& odom_delta) {
    if(!odom_ukf) {
      Eigen::MatrixXf odom_process_noise = Eigen::MatrixXf::Identity(7, 7);
      Eigen::MatrixXf odom_measurement_noise = Eigen::MatrixXf::Identity(7, 7) * 1e-3;

      Eigen::VectorXf odom_mean(7);
      odom_mean.block<3, 1>(0, 0) = Eigen::Vector3f(ukf->mean[0], ukf->mean[1], ukf->mean[2]);
      odom_mean.block<4, 1>(3, 0) = Eigen::Vector4f(ukf->mean[6], ukf->mean[7], ukf->mean[8], ukf->mean[9]);
      Eigen::MatrixXf odom_cov = Eigen::MatrixXf::Identity(7, 7) * 1e-2;

      OdomSystem odom_system;
      odom_ukf.reset(new kkl::alg::UnscentedKalmanFilterX<float, OdomSystem>(odom_system, 7, 7, 7, odom_process_noise, odom_measurement_noise, odom_mean, odom_cov));
    }

    Eigen::Quaternionf quat(odom_delta.block<3, 3>(0, 0));

    Eigen::VectorXf control(7);
    control.middleRows(0, 3) = odom_delta.block<3, 1>(0, 3);
    control.middleRows(3, 4) = Eigen::Vector4f(quat.w(), quat.x(), quat.y(), quat.z());

    Eigen::MatrixXf process_noise = Eigen::MatrixXf::Identity(7, 7);
    process_noise.topLeftCorner(3, 3) = Eigen::Matrix3f::Identity() * odom_delta.block<3, 1>(0, 3).norm() + Eigen::Matrix3f::Identity() * 1e-3;
    process_noise.bottomRightCorner(4, 4) = Eigen::Matrix4f::Identity() * (1 - std::abs(quat.w())) + Eigen::Matrix4f::Identity() * 1e-3;

    odom_ukf->setProcessNoiseCov(process_noise);
    odom_ukf->predict(control);
  }

  /**
   * @brief correct
   * @param cloud   input cloud
   * @return cloud aligned to the globalmap
   */
  pcl::PointCloud<PointT>::Ptr correct(const ros::Time& stamp, const pcl::PointCloud<PointT>::ConstPtr& cloud) {
    last_correction_stamp = stamp;

    Eigen::Matrix4f imu_guess;
    Eigen::Matrix4f odom_guess;
    Eigen::Matrix4f init_guess = Eigen::Matrix4f::Identity();

    if(!odom_ukf) {
      init_guess = imu_guess = matrix();
    } else {
      imu_guess = matrix();
      odom_guess = odom_matrix();

      Eigen::VectorXf imu_mean(7);
      Eigen::MatrixXf imu_cov = Eigen::MatrixXf::Identity(7, 7);
      imu_mean.block<3, 1>(0, 0) = ukf->mean.block<3, 1>(0, 0);
      imu_mean.block<4, 1>(3, 0) = ukf->mean.block<4, 1>(6, 0);

      imu_cov.block<3, 3>(0, 0) = ukf->cov.block<3, 3>(0, 0);
      imu_cov.block<3, 4>(0, 3) = ukf->cov.block<3, 4>(0, 6);
      imu_cov.block<4, 3>(3, 0) = ukf->cov.block<4, 3>(6, 0);
      imu_cov.block<4, 4>(3, 3) = ukf->cov.block<4, 4>(6, 6);

      Eigen::VectorXf odom_mean = odom_ukf->mean;
      Eigen::MatrixXf odom_cov = odom_ukf->cov;

      Eigen::MatrixXf inv_imu_cov = imu_cov.inverse();
      Eigen::MatrixXf inv_odom_cov = odom_cov.inverse();

      Eigen::MatrixXf fused_cov = (inv_imu_cov + inv_odom_cov).inverse();
      Eigen::VectorXf fused_mean = fused_cov * inv_imu_cov * imu_mean + fused_cov * inv_odom_cov * odom_mean;

      init_guess.block<3, 1>(0, 3) = Eigen::Vector3f(fused_mean[0], fused_mean[1], fused_mean[2]);
      init_guess.block<3, 3>(0, 0) = Eigen::Quaternionf(fused_mean[3], fused_mean[4], fused_mean[5], fused_mean[6]).normalized().toRotationMatrix();
    }

    pcl::PointCloud<PointT>::Ptr aligned(new pcl::PointCloud<PointT>());
    registration->setInputSource(cloud);
    registration->align(*aligned, init_guess);

    Eigen::Matrix4f trans = registration->getFinalTransformation();
    Eigen::Vector3f p = trans.block<3, 1>(0, 3);
    Eigen::Quaternionf q(trans.block<3, 3>(0, 0));

    if(quat().coeffs().dot(q.coeffs()) < 0.0f) {
      q.coeffs() *= -1.0f;
    }

    Eigen::VectorXf observation(7);
    observation.middleRows(0, 3) = p;
    observation.middleRows(3, 4) = Eigen::Vector4f(q.w(), q.x(), q.y(), q.z());

    ukf->correct(observation);
    imu_pred_error = imu_guess.inverse() * registration->getFinalTransformation();

    if(odom_ukf) {
      odom_ukf->correct(observation);
      odom_pred_error = odom_guess.inverse() * registration->getFinalTransformation();
    }

    return aligned;
  }

  /* getters */
  ros::Time last_correction_time() const {
    return last_correction_stamp;
  }

  Eigen::Vector3f pos() const {
    return Eigen::Vector3f(ukf->mean[0], ukf->mean[1], ukf->mean[2]);
  }

  Eigen::Vector3f vel() const {
    return Eigen::Vector3f(ukf->mean[3], ukf->mean[4], ukf->mean[5]);
  }

  Eigen::Quaternionf quat() const {
    return Eigen::Quaternionf(ukf->mean[6], ukf->mean[7], ukf->mean[8], ukf->mean[9]).normalized();
  }

  Eigen::Matrix4f matrix() const {
    Eigen::Matrix4f m = Eigen::Matrix4f::Identity();
    m.block<3, 3>(0, 0) = quat().toRotationMatrix();
    m.block<3, 1>(0, 3) = pos();
    return m;
  }

  Eigen::Vector3f odom_pos() const {
    return Eigen::Vector3f(odom_ukf->mean[0], odom_ukf->mean[1], odom_ukf->mean[2]);
  }

  Eigen::Quaternionf odom_quat() const {
    return Eigen::Quaternionf(odom_ukf->mean[3], odom_ukf->mean[4], odom_ukf->mean[5], odom_ukf->mean[6]).normalized();
  }

  Eigen::Matrix4f odom_matrix() const {
    Eigen::Matrix4f m = Eigen::Matrix4f::Identity();
    m.block<3, 3>(0, 0) = odom_quat().toRotationMatrix();
    m.block<3, 1>(0, 3) = odom_pos();
    return m;
  }

  const boost::optional<Eigen::Matrix4f>& imu_prediction_error() const {
    return imu_pred_error;
  }

  const boost::optional<Eigen::Matrix4f>& odom_prediction_error() const {
    return odom_pred_error;
  }

private:
  ros::Time init_stamp;             // when the estimator was initialized
  ros::Time prev_stamp;             // when the estimator was updated last time
  ros::Time last_correction_stamp;  // when the estimator performed the correction step
  double cool_time_duration;        //

  Eigen::MatrixXf process_noise;
  std::unique_ptr<kkl::alg::UnscentedKalmanFilterX<float, PoseSystem>> ukf;
  std::unique_ptr<kkl::alg::UnscentedKalmanFilterX<float, OdomSystem>> odom_ukf;

  boost::optional<Eigen::Matrix4f> imu_pred_error;
  boost::optional<Eigen::Matrix4f> odom_pred_error;

  pcl::Registration<PointT, PointT>::Ptr registration;
};

}  // namespace hdl_localization

#endif  // POSE_ESTIMATOR_HPP

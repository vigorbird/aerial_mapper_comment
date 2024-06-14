/*
 *    Filename: ortho-backward-grid.cc
 *  Created on: Jun 25, 2017
 *      Author: Timo Hinzmann
 *   Institute: ETH Zurich, Autonomous Systems Lab
 */

// HEADER
#include "aerial-mapper-ortho/ortho-backward-grid.h"

// SYSTEM
#include <fstream>
#include <iostream>
#include <math.h>

// NON-SYSTEM
#include <aerial-mapper-utils/utils-common.h>
#include <aslam/pipeline/undistorter.h>
#include <aslam/pipeline/undistorter-mapped.h>

namespace ortho {

OrthoBackwardGrid::OrthoBackwardGrid(
    const std::shared_ptr<aslam::NCamera> ncameras, const Settings& settings,
    grid_map::GridMap* map)
    : ncameras_(ncameras), settings_(settings) {
  CHECK(ncameras_);
  printParams();

  if (settings_.use_multi_threads) {
    // Create one sample for every cell.
    samples_idx_range_.clear();
    size_t sample_counter = 0u;
    for (grid_map::GridMapIterator it(*map); !it.isPastEnd(); ++it) {
      samples_idx_range_.push_back(sample_counter);
      map_sample_to_cell_index_.insert(std::make_pair(sample_counter, *it));
      ++sample_counter;
    }
  }
}

void OrthoBackwardGrid::updateOrthomosaicLayer(const Poses& T_G_Cs,//所有的位姿
                                               const Images& images,//所有的图像
                                               grid_map::GridMap* map) const {//grid地图
  CHECK(ncameras_);
  const aslam::Camera& camera = ncameras_->getCamera(kFrameIdx);

  grid_map::Matrix& layer_ortho = (*map)["ortho"];
  grid_map::Matrix& layer_num_observations = (*map)["num_observations"];
  grid_map::Matrix& layer_elevation_angle = (*map)["elevation_angle"];
  const grid_map::Matrix& layer_elevation = (*map)["elevation"];
  grid_map::Matrix& layer_observation_index = (*map)["observation_index"];
  grid_map::Matrix& layer_colored_ortho = (*map)["colored_ortho"];

  ros::Time time1 = ros::Time::now();
  //遍历所有的grid
  for (grid_map::GridMapIterator it(*map); !it.isPastEnd(); ++it) {
    grid_map::Position position;
    map->getPosition(*it, position);
    const grid_map::Index index(*it);
    double x = index(0);
    double y = index(1);
    //根据grid信息，获取这个grid对应的xyz坐标
    Eigen::Vector3d landmark_UTM =  Eigen::Vector3d(position.x(), position.y(), layer_elevation(x, y));

    // Loop over all images.
    //遍历所有的图像，判断这个grid是否能被这个图像观测到
    for (size_t i = 0u; i < images.size(); ++i) {
      const Eigen::Vector3d& C_landmark =  T_G_Cs[i].inverse().transform(landmark_UTM);//gird在图像坐标系下的坐标
      Eigen::Vector2d keypoint;
      const aslam::ProjectionResult& projection_result = camera.project3(C_landmark, &keypoint);

      // Check if keypoint visible.
      const bool keypoint_visible =
          (keypoint(0) >= 0.0) && (keypoint(1) >= 0.0) &&
          (keypoint(0) < static_cast<double>(camera.imageWidth())) &&
          (keypoint(1) < static_cast<double>(camera.imageHeight())) &&
          (projection_result.getDetailedStatus() != aslam::ProjectionResult::POINT_BEHIND_CAMERA) &&
          (projection_result.getDetailedStatus() != aslam::ProjectionResult::PROJECTION_INVALID);
      if (keypoint_visible) {
        Eigen::Vector3d u = C_landmark;
        // Observation vector.
        double norm_u = sqrt(u(0) * u(0) + u(1) * u(1) + u(2) * u(2));
        // Angle (observation_in_camera, cell_center).
        double alpha = asin(std::fabs(u(2)) / norm_u);
        CHECK(alpha > 0.0);

        if (std::fabs(alpha) > layer_elevation_angle(x, y)) {
          layer_elevation_angle(x, y) = std::fabs(alpha);
          layer_observation_index(x, y) = i;
          layer_num_observations(x, y) += layer_num_observations(x, y);

          // Retrieve pixel intensity.
          //再将这个点投影到图像上，获取对应的像素坐标
          const Eigen::Vector3d& C_landmark = T_G_Cs[i].inverse().transform(landmark_UTM);
          Eigen::Vector2d keypoint;
          camera.project3(C_landmark, &keypoint);
          const int kp_y = std::min(static_cast<int>(std::round(keypoint(1))),
                                    static_cast<int>(camera.imageHeight()) - 1);
          const int kp_x = std::min(static_cast<int>(std::round(keypoint(0))),
                                    static_cast<int>(camera.imageWidth()) - 1);
          if (settings_.colored_ortho) {
            const cv::Vec3b rgb = images[i].at<cv::Vec3b>(kp_y, kp_x);
            const Eigen::Vector3f color_vector_bgr( static_cast<float>(rgb[2]) / 255.0,
                                                    static_cast<float>(rgb[1]) / 255.0,
                                                    static_cast<float>(rgb[0]) / 255.0);
            float color_concatenated;
            grid_map::colorVectorToValue(color_vector_bgr, color_concatenated);
            layer_colored_ortho(x, y) = color_concatenated;//作者选择一个最好的角度进行赋值
          } else {
            const double gray_value = images[i].at<uchar>(kp_y, kp_x);
            // Update orthomosaic.
            layer_ortho(x, y) = gray_value;
          }
        }  // if better observation angle
      }    // if visible
    }      // loop images
  }        // loop cells

  const ros::Time time2 = ros::Time::now();
  const ros::Duration& delta_time = time2 - time1;
  VLOG(1) << "dt(backward_grid, single-thread): " << delta_time;
}//end function updateOrthomosaicLayer

void OrthoBackwardGrid::updateOrthomosaicLayerMultiThreaded(const Poses& T_G_Cs, 
                                                            const Images& images, 
                                                            grid_map::GridMap* map) const {
  CHECK(ncameras_);
  const aslam::Camera& camera = ncameras_->getCamera(kFrameIdx);

  grid_map::Matrix& layer_ortho = (*map)["ortho"];
  grid_map::Matrix& layer_num_observations = (*map)["num_observations"];
  grid_map::Matrix& layer_elevation_angle = (*map)["elevation_angle"];
  const grid_map::Matrix& layer_elevation = (*map)["elevation"];
  grid_map::Matrix& layer_observation_index = (*map)["observation_index"];
  grid_map::Matrix& layer_colored_ortho = (*map)["colored_ortho"];

  ros::Time time1 = ros::Time::now();

  auto generateCellWiseOrthomosaic =
      [&](const std::vector<size_t>& sample_idx_range_) {
    for (size_t sample_idx : sample_idx_range_) {
      grid_map::Index index = map_sample_to_cell_index_.at(sample_idx);
      double x = index(0);
      double y = index(1);

      grid_map::Position position;
      map->getPosition(index, position);

      Eigen::Vector3d landmark_UTM =
          Eigen::Vector3d(position.x(), position.y(), layer_elevation(x, y));

      // Loop over all images.
      for (size_t i = 0u; i < images.size(); ++i) {
        const Eigen::Vector3d& C_landmark =
            T_G_Cs[i].inverse().transform(landmark_UTM);
        Eigen::Vector2d keypoint;
        const aslam::ProjectionResult& projection_result =
            camera.project3(C_landmark, &keypoint);

        // Check if keypoint visible.
        const bool keypoint_visible =
            (keypoint(0) >= 0.0) && (keypoint(1) >= 0.0) &&
            (keypoint(0) < static_cast<double>(camera.imageWidth())) &&
            (keypoint(1) < static_cast<double>(camera.imageHeight())) &&
            (projection_result.getDetailedStatus() !=
             aslam::ProjectionResult::POINT_BEHIND_CAMERA) &&
            (projection_result.getDetailedStatus() !=
             aslam::ProjectionResult::PROJECTION_INVALID);
        if (keypoint_visible) {
          const Eigen::Vector3d& u = C_landmark;
          // Observation vector.
          double norm_u = sqrt(u(0) * u(0) + u(1) * u(1) + u(2) * u(2));
          // Angle (observation_in_camera, cell_center).
          double alpha = asin(std::fabs(u(2)) / norm_u);
          CHECK(alpha > 0.0);

          if (std::fabs(alpha) > layer_elevation_angle(x, y)) {
            layer_elevation_angle(x, y) = std::fabs(alpha);
            layer_observation_index(x, y) = i;
            layer_num_observations(x, y) += layer_num_observations(x, y);

            // Retrieve pixel intensity.
            const Eigen::Vector3d& C_landmark =
                T_G_Cs[i].inverse().transform(landmark_UTM);
            Eigen::Vector2d keypoint;
            camera.project3(C_landmark, &keypoint);
            const int kp_y = std::min(static_cast<int>(std::round(keypoint(1))),
                                      static_cast<int>(camera.imageHeight()) - 1);
            const int kp_x = std::min(static_cast<int>(std::round(keypoint(0))),
                                      static_cast<int>(camera.imageWidth()) - 1);
            if (settings_.colored_ortho) {
              const cv::Vec3b rgb = images[i].at<cv::Vec3b>(kp_y, kp_x);
              const Eigen::Vector3f color_vector_bgr(
                  static_cast<float>(rgb[2]) / 255.0,
                  static_cast<float>(rgb[1]) / 255.0,
                  static_cast<float>(rgb[0]) / 255.0);
              float color_concatenated;
              grid_map::colorVectorToValue(color_vector_bgr, color_concatenated);
              layer_colored_ortho(x, y) = color_concatenated;
            } else {
              const double gray_value = images[i].at<uchar>(kp_y, kp_x);
              // Update orthomosaic.
              layer_ortho(x, y) = gray_value;
            }
          }  // if better observation angle
        }    // if visible
      }      // loop images
    }        // loop samples
  };         // lambda function

  const size_t num_samples = samples_idx_range_.size();
  const size_t num_threads = std::thread::hardware_concurrency();
  utils::parFor(num_samples, generateCellWiseOrthomosaic, num_threads);

  const ros::Time time2 = ros::Time::now();
  const ros::Duration& delta_time = time2 - time1;
  VLOG(1) << "dt(backward_grid, multi-threads): " << delta_time;
}

void OrthoBackwardGrid::process(const Poses& T_G_Bs, const Images& images,
                                grid_map::GridMap* map) const {
  CHECK(!T_G_Bs.empty());
  CHECK(T_G_Bs.size() == images.size());
  CHECK(map);
  LOG(INFO) << "Num. images = " << images.size();

  Poses T_G_Cs;
  for (const Pose& T_G_B : T_G_Bs) {
    T_G_Cs.push_back(T_G_B * ncameras_->get_T_C_B(0u).inverse());
  }
  if (settings_.use_multi_threads) {
    updateOrthomosaicLayerMultiThreaded(T_G_Cs, images, map);
  } else {
    updateOrthomosaicLayer(T_G_Cs, images, map);//将gird反投影回图像，然后为每个gird赋值颜色
  }
}

void OrthoBackwardGrid::printParams() const {
  std::stringstream out;
  out << std::endl << std::string(50, '*') << std::endl
      << "Orthomosaic parameters:" << std::endl
      << utils::paramToString("Show orthomosaic opencv",
                              settings_.show_orthomosaic_opencv)
      << utils::paramToString("Save orthomosaic jpg",
                              settings_.save_orthomosaic_jpg)
      << utils::paramToString("Orthomosaic filename",
                              settings_.orthomosaic_jpg_filename)
      << std::string(50, '*') << std::endl;
  LOG(INFO) << out.str();
}

}  // namespace ortho

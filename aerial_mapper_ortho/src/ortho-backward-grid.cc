/*
 *    Filename: ortho-backward-grid.cc
 *  Created on: Jun 25, 2017
 *      Author: Timo Hinzmann
 *   Institute: ETH Zurich, Autonomous Systems Lab
 */

// HEADER
#include "aerial-mapper-ortho/ortho-backward-grid.h"

// NON-SYSTEM
#include <aerial-mapper-thirdparty/gps-conversions.h>
#include <aslam/pipeline/undistorter.h>
#include <aslam/pipeline/undistorter-mapped.h>
#include <maplab-common/progress-bar.h>

#include <grid_map_core/iterators/GridMapIterator.hpp>
#include <grid_map_core/GridMap.hpp>
#include <grid_map_cv/grid_map_cv.hpp>
#include <aerial-mapper-io/aerial-mapper-io.h>

namespace ortho {

OrthoBackwardGrid::OrthoBackwardGrid(
    const std::shared_ptr<aslam::NCamera> ncameras, const Poses& T_G_Bs,
    const Images& images, const SettingsGrid& settings)
    : ncameras_(ncameras), settings_(settings) {
  CHECK(ncameras_);
  printParams();

  // Transform to camera frame.
  Poses T_G_Cs;
  for (const Pose& T_G_B : T_G_Bs) {
    T_G_Cs.push_back(T_G_B * ncameras_->get_T_C_B(0u).inverse());
  }
  Images images_undistorted;
  std::unique_ptr<aslam::MappedUndistorter> undistorter_;
  undistorter_ =
      aslam::createMappedUndistorter(ncameras_->getCameraShared(0), 1.0, 1.0,
                                     aslam::InterpolationMethod::Linear);

  // TODO(hitimo): Undistort or not?
  Images images_new;
  for (const cv::Mat& image : images) {
    cv::Mat image_undistorted;
    undistorter_->processImage(image, &image_undistorted);
    images_undistorted.push_back(image_undistorted);
    images_new.push_back(image);
  }
  if (settings_.mode == Mode::Batch) {
    if (settings_.use_grid_map) {
      processBatchGridmap(T_G_Cs, images_new);
    } else {
      processBatch(T_G_Cs, images_new);
    }
  } else if (settings_.mode == Mode::Incremental) {
    if (settings_.use_grid_map) {
      processIncrementalGridmap(T_G_Cs, images_new);
    } else {
      processIncremental(T_G_Cs, images_new);
    }
  }
}

void OrthoBackwardGrid::processBatchGridmap(const Poses& T_G_Cs,
                                            const Images& images) {
  CHECK(!T_G_Cs.empty());
  CHECK(T_G_Cs.size() == images.size());
  LOG(INFO) << "images.size() = " << images.size();
  LOG(INFO) << "T_G_Cs.size() = " << T_G_Cs.size();
  CHECK(ncameras_);
  const aslam::Camera& camera = ncameras_->getCamera(kFrameIdx);

  // Create grid map.
  grid_map::GridMap map({"ortho"});
  map.setFrameId("map");
  const Eigen::Vector2d bottom_left = Eigen::Vector2d(
        settings_.orthomosaic_easting_min,
        settings_.orthomosaic_northing_min);
  const Eigen::Vector2d top_right = Eigen::Vector2d(
        settings_.orthomosaic_easting_max,
        settings_.orthomosaic_northing_max);
  const size_t width_east = std::fabs(bottom_left(0) - top_right(0));
  const size_t height_north = std::fabs(bottom_left(1) - top_right(1));
  std::cout << "width_east = " << width_east << std::endl;
  std::cout << "height_north = " << height_north << std::endl;
  const Eigen::Vector2d top_left =
      bottom_left + Eigen::Vector2d(0.0, height_north);
  const Eigen::Vector2d center = top_left
      + Eigen::Vector2d(static_cast<double>(width_east)/2.0,
                        -static_cast<double>(height_north)/2.0);
  const Eigen::Vector2d bottom_right = center
      + Eigen::Vector2d(static_cast<double>(width_east)/2.0,
                        -static_cast<double>(height_north)/2.0);

  map.setGeometry(grid_map::Length(height_north, width_east), 1.0,
                  grid_map::Position(center(1), center(0)));
  ROS_INFO("Created map with size %f x %f m (%i x %i cells).\n The center of the map is located at (%f, %f) in the %s frame.",
    map.getLength().x(), map.getLength().y(),
    map.getSize()(0), map.getSize()(1),
    map.getPosition().x(), map.getPosition().y(), map.getFrameId().c_str());

  // TODO(hitimo): Re-enable the height map.
  // cv::Mat height_map;
  // loadHeightMap(FLAGS_orthomosaic_filename_height_map, &height_map);

  // Define the grid.
//  const Eigen::Vector2d bottom_left = Eigen::Vector2d(
//      settings_.orthomosaic_easting_min,
//        settings_.orthomosaic_northing_min);
//  const Eigen::Vector2d top_right = Eigen::Vector2d(
//      settings_.orthomosaic_easting_max,
//        settings_.orthomosaic_northing_max);
//  const size_t width_east = std::fabs(bottom_left(0) - top_right(0));
//  const size_t height_north = std::fabs(bottom_left(1) - top_right(1));
//  const Eigen::Vector2d top_left =
//      bottom_left + Eigen::Vector2d(0.0, height_north);

//  // Iterate over all cells.
//  const double d = static_cast<double>(settings_.orthomosaic_resolution);
//  const Eigen::Vector2d& a = Eigen::Vector2d(0.5, -0.5) / d;
//  const Eigen::Vector2d& b = a + top_left;
//  cv::Mat orthomosaic(height_north * d, width_east * d, CV_8UC3,
//                      cv::Scalar(255.0, 255.0, 255.0));
//  Eigen::MatrixXi observation_map = Eigen::MatrixXi::Zero(
//      static_cast<int>(width_east * d), static_cast<int>(height_north * d));

  ros::Time time1 = ros::Time::now();
  std::cout << "top_left = " << top_left.transpose() << std::endl;
  std::cout << "bottom_right = " << bottom_right.transpose() << std::endl;
  for (grid_map::GridMapIterator it(map); !it.isPastEnd(); ++it) {
    grid_map::Position position;
    map.getPosition(*it, position);/*
    std::cout << "(x,y)=" << position.x() << ", " << position.y() << std::endl;*/
    CHECK(position.y() <= settings_.orthomosaic_easting_max);
    CHECK(position.y() >= settings_.orthomosaic_easting_min);
    CHECK(position.x() <= settings_.orthomosaic_northing_max);
    CHECK(position.x() >= settings_.orthomosaic_northing_min);
    double northing = position.x();
    double easting = position.y();
    const Eigen::Vector3d landmark_UTM(easting, northing,
                                       settings_.orthomosaic_elevation_m);
    // Loop over all images.
     map.at("ortho", *it) = 255;
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
        // SIC! Order of keypoint x/y.
        const int kp_y =
            std::min(static_cast<int>(std::round(keypoint(1))),
                     static_cast<int>(camera.imageHeight()) - 1);
        const int kp_x =
            std::min(static_cast<int>(std::round(keypoint(0))),
                     static_cast<int>(camera.imageWidth()) - 1);
        const double gray_value = images[i].at<uchar>(kp_y, kp_x);
        //        orthomosaic.at<cv::Vec3b>(y, x) =
        //            cv::Vec3b(gray_value, gray_value, gray_value);
        map.at("ortho", *it) = gray_value;
        break;
      }
    }
  }
  const ros::Time time2 = ros::Time::now();
  const ros::Duration& delta_time = time2 - time1;
  LOG(INFO) << "Time elapsed: " << delta_time;
  const float minValue = 0;
  const float maxValue = 255;

  // Convert to image.
  cv::Mat image;
  grid_map::GridMapCvConverter::toImage<unsigned char, 1>(map, "ortho",CV_8UC1,
                                                          minValue, maxValue, image);
  cv::imwrite("/tmp/image.jpg", image);
  cv::Mat dst;               // dst must be a different Mat
  cv::flip(image, dst, 1);
  cv::imwrite("/tmp/image_flipped.jpg", dst);
  io::AerialMapperIO io_handler;
//  io_handler.writeDataToDEMGeoTiffColor(image, top_left,
//                                        "/tmp/test.tiff");
  io_handler.toGeoTiff(dst, top_left,  "/tmp/test2.tiff");
  cv::imshow("image", dst);
  cv::waitKey(0);

#ifdef adf
  size_t y_limit = static_cast<int>(height_north * d) - 1;
  size_t x_limit = static_cast<int>(width_east * d) - 1;
  const ros::Time time1 = ros::Time::now();
  for (size_t y = 0u; y < y_limit; ++y) {
    //CHECK(observation_map.cols() > y);
    VLOG(100) << "[ " << y << " / " << height_north * d << " ]";
    for (size_t x = 0u; x < x_limit; ++x) {
      VLOG(200) << "[ " << x << " / " << width_east * d << " ]";
      //CHECK(observation_map.rows() > x);
      const Eigen::Vector2d& cell_center =
          b +
          Eigen::Vector2d(static_cast<double>(x),
                          -static_cast<double>(y)) / d;
      if (settings_.use_digital_elevation_map) {
        // TODO(hitimo): Re-enable the height map.
        // const double height_from_dem = height_map.at<double>(y, x);
        // landmark_UTM =
        // Eigen::Vector3d(cell_center(0), cell_center(1),
        // height_from_dem);
      } else {
        const Eigen::Vector3d landmark_UTM(cell_center(0), cell_center(1),
                                           settings_.orthomosaic_elevation_m);
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
            // SIC! Order of keypoint x/y.
            const int kp_y =
                std::min(static_cast<int>(std::round(keypoint(1))),
                         static_cast<int>(camera.imageHeight()) - 1);
            const int kp_x =
                std::min(static_cast<int>(std::round(keypoint(0))),
                         static_cast<int>(camera.imageWidth()) - 1);
            const double gray_value = images[i].at<uchar>(kp_y, kp_x);
            orthomosaic.at<cv::Vec3b>(y, x) =
                cv::Vec3b(gray_value, gray_value, gray_value);
            break;
          }
        }
      }
    }
    if (settings_.show_orthomosaic_opencv) {
      cv::imshow("Orthomosaic", orthomosaic);
      cv::waitKey(1);
    }
  }

  const ros::Time time2 = ros::Time::now();
  const ros::Duration& delta_time = time2 - time1;
  LOG(INFO) << "Time elapsed: " << delta_time;

  if (settings_.show_orthomosaic_opencv) {
    cv::imshow("Orthomosaic", orthomosaic);
    cv::waitKey(0);
  }
  if (settings_.save_orthomosaic_jpg) {
    cv::imwrite(settings_.orthomosaic_jpg_filename, orthomosaic);
  }
#endif
}

void OrthoBackwardGrid::processBatch(const Poses& T_G_Cs,
                                     const Images& images) {
  CHECK(!T_G_Cs.empty());
  CHECK(T_G_Cs.size() == images.size());
  LOG(INFO) << "images.size() = " << images.size();
  LOG(INFO) << "T_G_Cs.size() = " << T_G_Cs.size();
  CHECK(ncameras_);
  const aslam::Camera& camera = ncameras_->getCamera(kFrameIdx);

  // TODO(hitimo): Re-enable the height map.
  cv::Mat height_map;
  // loadHeightMap(FLAGS_orthomosaic_filename_height_map, &height_map);

  // Define the grid.
  const Eigen::Vector2d bottom_left = Eigen::Vector2d(
      settings_.orthomosaic_easting_min,
        settings_.orthomosaic_northing_min);
  const Eigen::Vector2d top_right = Eigen::Vector2d(
      settings_.orthomosaic_easting_max,
        settings_.orthomosaic_northing_max);
  const size_t width_east = std::fabs(bottom_left(0) - top_right(0));
  const size_t height_north = std::fabs(bottom_left(1) - top_right(1));
  const Eigen::Vector2d top_left =
      bottom_left + Eigen::Vector2d(0.0, height_north);

  // Iterate over all cells.
  const double d = static_cast<double>(settings_.orthomosaic_resolution);
  const Eigen::Vector2d& a = Eigen::Vector2d(0.5, -0.5) / d;
  const Eigen::Vector2d& b = a + top_left;
  cv::Mat orthomosaic(height_north * d, width_east * d, CV_8UC3,
                      cv::Scalar(255.0, 255.0, 255.0));
  Eigen::MatrixXi observation_map = Eigen::MatrixXi::Zero(
      static_cast<int>(width_east * d), static_cast<int>(height_north * d));
  size_t y_limit = static_cast<int>(height_north * d) - 1;
  size_t x_limit = static_cast<int>(width_east * d) - 1;
  const ros::Time time1 = ros::Time::now();
  for (size_t y = 0u; y < y_limit; ++y) {
    //CHECK(observation_map.cols() > y);
    VLOG(100) << "[ " << y << " / " << height_north * d << " ]";
    for (size_t x = 0u; x < x_limit; ++x) {
      VLOG(200) << "[ " << x << " / " << width_east * d << " ]";
      //CHECK(observation_map.rows() > x);
      const Eigen::Vector2d& cell_center =
          b +
          Eigen::Vector2d(static_cast<double>(x),
                          -static_cast<double>(y)) / d;
      if (settings_.use_digital_elevation_map) {
        // TODO(hitimo): Re-enable the height map.
        // const double height_from_dem = height_map.at<double>(y, x);
        // landmark_UTM =
        // Eigen::Vector3d(cell_center(0), cell_center(1),
        // height_from_dem);
      } else {
        const Eigen::Vector3d landmark_UTM(cell_center(0), cell_center(1),
                                           settings_.orthomosaic_elevation_m);
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
            // SIC! Order of keypoint x/y.
            const int kp_y =
                std::min(static_cast<int>(std::round(keypoint(1))),
                         static_cast<int>(camera.imageHeight()) - 1);
            const int kp_x =
                std::min(static_cast<int>(std::round(keypoint(0))),
                         static_cast<int>(camera.imageWidth()) - 1);
            const double gray_value = images[i].at<uchar>(kp_y, kp_x);
            orthomosaic.at<cv::Vec3b>(y, x) =
                cv::Vec3b(gray_value, gray_value, gray_value);
            break;
          }
        }
      }
    }
    if (settings_.show_orthomosaic_opencv) {
      cv::imshow("Orthomosaic", orthomosaic);
      cv::waitKey(1);
    }
  }

  const ros::Time time2 = ros::Time::now();
  const ros::Duration& delta_time = time2 - time1;
  LOG(INFO) << "Time elapsed: " << delta_time;
  std::cout << "Time elapsed: " << delta_time;

  if (settings_.show_orthomosaic_opencv) {
    cv::imshow("Orthomosaic", orthomosaic);
    cv::waitKey(0);
  }
  if (settings_.save_orthomosaic_jpg) {
    cv::imwrite(settings_.orthomosaic_jpg_filename, orthomosaic);
  }
}

void OrthoBackwardGrid::processIncremental(const Poses& T_G_Cs,
                                           const Images& images) {
  CHECK(!T_G_Cs.empty());
  CHECK(T_G_Cs.size() == images.size());
  LOG(INFO) << "images.size() = " << images.size();
  LOG(INFO) << "T_G_Cs.size() = " << T_G_Cs.size();
  CHECK(ncameras_);
  const aslam::Camera& camera = ncameras_->getCamera(kFrameIdx);

  // Define the grid.
  const Eigen::Vector2d bottom_left = Eigen::Vector2d(
      settings_.orthomosaic_easting_min,
        settings_.orthomosaic_northing_min);
  const Eigen::Vector2d top_right = Eigen::Vector2d(
      settings_.orthomosaic_easting_max,
        settings_.orthomosaic_northing_max);
  const size_t width_east = std::fabs(bottom_left(0) - top_right(0));
  const size_t height_north = std::fabs(bottom_left(1) - top_right(1));
  const Eigen::Vector2d top_left =
      bottom_left + Eigen::Vector2d(0.0, height_north);

  // Iterate over all cells.
  const double d = static_cast<double>(settings_.orthomosaic_resolution);
  const Eigen::Vector2d& a = Eigen::Vector2d(0.5, -0.5) / d;
  const Eigen::Vector2d& b = a + top_left;
  cv::Mat orthomosaic(height_north * d, width_east * d, CV_8UC3,
                      cv::Scalar(255.0, 255.0, 255.0));

  // Compute the ground points.
  Eigen::Matrix2Xd border_keypoints_;
  border_keypoints_.resize(Eigen::NoChange, 4);
  const size_t width = ncameras_->getCameraShared(0u)->imageWidth();
  const size_t height = ncameras_->getCameraShared(0u)->imageHeight();
  border_keypoints_.col(0) = Eigen::Vector2d(0.0, 0.0);
  border_keypoints_.col(1) =
      Eigen::Vector2d(static_cast<double>(width - 1u), 0.0);
  border_keypoints_.col(2) = Eigen::Vector2d(
        static_cast<double>(width - 1u), static_cast<double>(height - 1u));
  border_keypoints_.col(3) =
      Eigen::Vector2d(0.0, static_cast<double>(height - 1u));

  const ros::Time time1 = ros::Time::now();
  // Loop over all images.
  common::ProgressBar progress_bar(static_cast<size_t>(images.size()));
  for (size_t i = 0u; i < images.size(); ++i) {
    progress_bar.increment();
    const Pose& T_G_C = T_G_Cs[i];
    std::vector<cv::Point2f> ground_points, image_points;
    CHECK(border_keypoints_.cols() == 4u);
    for (int border_pixel_index = 0;
         border_pixel_index < border_keypoints_.cols();
         ++border_pixel_index) {
      Eigen::Vector3d C_ray;
      const Eigen::Vector2d& keypoint =
          border_keypoints_.col(border_pixel_index);
      ncameras_->getCameraShared(kFrameIdx)->backProject3(keypoint, &C_ray);
      const double scale = -(T_G_C.getPosition()(2) -
                             settings_.orthomosaic_elevation_m) /
                           (T_G_C.getRotationMatrix() * C_ray)(2);
      const Eigen::Vector3d& G_landmark =
          T_G_C.getPosition() + scale * T_G_C.getRotationMatrix() * C_ray;
      ground_points.push_back(cv::Point2f(G_landmark(0), G_landmark(1)));
      image_points.push_back(
          cv::Point2f(border_keypoints_.col(border_pixel_index)(0),
                      border_keypoints_.col(border_pixel_index)(1)));
    }

    double x_max, x_min, y_max, y_min;
    x_max = y_max = 0.0;
    x_min = y_min = std::numeric_limits<double>::max();
    CHECK(ground_points.size() == 4u);
    for (const cv::Point2f& ground_point : ground_points) {
      if (ground_point.x > x_max) {
        x_max = ground_point.x;
      }
      if (ground_point.x < x_min) {
        x_min = ground_point.x;
      }
      if (ground_point.y > y_max) {
        y_max = ground_point.y;
      }
      if (ground_point.y < y_min) {
        y_min = ground_point.y;
      }
    }

    // Find corresponding grid points.
    int x_max_, x_min_, y_max_, y_min_;
    x_max_ = int(x_max) - b(0);
    x_min_ = int(x_min) - b(0);
    y_max_ = -(int(y_max) - b(1));
    y_min_ = -(int(y_min) - b(1));

    for (size_t x = x_min_; x <= x_max_; ++x) {
      for (size_t y = y_max_; y <= y_min_; ++y) {
        if (orthomosaic.at<cv::Vec3b>(y, x)[0] == 255 ||
            orthomosaic.at<cv::Vec3b>(y, x) == cv::Vec3b(0, 255, 0)) {
          const Eigen::Vector2d& cell_center =
              b + Eigen::Vector2d(static_cast<double>(x),
                                  -static_cast<double>(y)) / d;
          const Eigen::Vector3d landmark_UTM(cell_center(0), cell_center(1),
                                             settings_.orthomosaic_elevation_m);
          const Eigen::Vector3d& C_landmark =
              T_G_Cs[i].inverse().transform(landmark_UTM);
          Eigen::Vector2d keypoint;
          const aslam::ProjectionResult& projection_result =
              camera.project3(C_landmark, &keypoint);
          const bool keypoint_visible =
              (keypoint(0) >= 0.0) && (keypoint(1) >= 0.0) &&
              (keypoint(0) < static_cast<double>(camera.imageWidth())) &&
              (keypoint(1) < static_cast<double>(camera.imageHeight())) &&
              (projection_result.getDetailedStatus() !=
               aslam::ProjectionResult::POINT_BEHIND_CAMERA) &&
              (projection_result.getDetailedStatus() !=
               aslam::ProjectionResult::PROJECTION_INVALID);
          if (keypoint_visible) {
            const int kp_y =
                std::min(static_cast<int>(std::round(keypoint(1))),
                         static_cast<int>(camera.imageHeight()) - 1);
            const int kp_x =
                std::min(static_cast<int>(std::round(keypoint(0))),
                         static_cast<int>(camera.imageWidth()) - 1);
            const double gray_value = images[i].at<uchar>(kp_y, kp_x);
            orthomosaic.at<cv::Vec3b>(y, x) =
                cv::Vec3b(gray_value, gray_value, gray_value);
          } else {
            orthomosaic.at<cv::Vec3b>(y, x) = cv::Vec3b(0, 255, 0);
          }
        }
      }
    }
  }  // images

  const ros::Time time2 = ros::Time::now();
  const ros::Duration& delta_time = time2 - time1;
  LOG(INFO) << "Time elapsed: " << delta_time;

  if (settings_.show_orthomosaic_opencv) {
    cv::imshow("Orthomosaic", orthomosaic);
    cv::waitKey(0);
  }
  if (settings_.save_orthomosaic_jpg) {
    cv::imwrite(settings_.orthomosaic_jpg_filename, orthomosaic);
  }
}

void OrthoBackwardGrid::processIncrementalGridmap(const Poses& T_G_Cs,
                                                  const Images& images) {
  CHECK(!T_G_Cs.empty());
  CHECK(T_G_Cs.size() == images.size());
  LOG(INFO) << "images.size() = " << images.size();
  LOG(INFO) << "T_G_Cs.size() = " << T_G_Cs.size();
  CHECK(ncameras_);
  const aslam::Camera& camera = ncameras_->getCamera(kFrameIdx);

  // Compute the border points.
  Eigen::Matrix2Xd border_keypoints_;
  border_keypoints_.resize(Eigen::NoChange, 4);
  const size_t width = ncameras_->getCameraShared(0u)->imageWidth();
  const size_t height = ncameras_->getCameraShared(0u)->imageHeight();
  border_keypoints_.col(0) = Eigen::Vector2d(0.0, 0.0);
  border_keypoints_.col(1) =
      Eigen::Vector2d(static_cast<double>(width - 1u), 0.0);
  border_keypoints_.col(2) = Eigen::Vector2d(
        static_cast<double>(width - 1u), static_cast<double>(height - 1u));
  border_keypoints_.col(3) =
      Eigen::Vector2d(0.0, static_cast<double>(height - 1u));

  // Create grid map.
  grid_map::GridMap map({"ortho_r", "ortho_g", "ortho_b", "observed"});
  map.setFrameId("map");
  const Eigen::Vector2d bottom_left = Eigen::Vector2d(
        settings_.orthomosaic_easting_min,
        settings_.orthomosaic_northing_min);
  const Eigen::Vector2d top_right = Eigen::Vector2d(
        settings_.orthomosaic_easting_max,
        settings_.orthomosaic_northing_max);
  const size_t width_east = std::fabs(bottom_left(0) - top_right(0));
  const size_t height_north = std::fabs(bottom_left(1) - top_right(1));
  std::cout << "width_east = " << width_east << std::endl;
  std::cout << "height_north = " << height_north << std::endl;
  const Eigen::Vector2d top_left =
      bottom_left + Eigen::Vector2d(0.0, height_north);
  const Eigen::Vector2d center = top_left
      + Eigen::Vector2d(static_cast<double>(width_east)/2.0,
                        -static_cast<double>(height_north)/2.0);
  const Eigen::Vector2d bottom_right = center
      + Eigen::Vector2d(static_cast<double>(width_east)/2.0,
                        -static_cast<double>(height_north)/2.0);

  map.setGeometry(grid_map::Length(height_north, width_east), 1.0,
                  grid_map::Position(center(1), center(0)));
  ROS_INFO("Created map with size %f x %f m (%i x %i cells).\n The center of the map is located at (%f, %f) in the %s frame.",
    map.getLength().x(), map.getLength().y(),
    map.getSize()(0), map.getSize()(1),
    map.getPosition().x(), map.getPosition().y(), map.getFrameId().c_str());

  const ros::Time time1 = ros::Time::now();
  // Loop over all images.
  common::ProgressBar progress_bar(static_cast<size_t>(images.size()));
  for (size_t i = 0u; i < images.size(); ++i) {
    progress_bar.increment();
    const Pose& T_G_C = T_G_Cs[i];
    CHECK(border_keypoints_.cols() == 4u);
    grid_map::Polygon polygon;
    for (int border_pixel_index = 0;
         border_pixel_index < border_keypoints_.cols();
         ++border_pixel_index) {
      Eigen::Vector3d C_ray;
      const Eigen::Vector2d& keypoint =
          border_keypoints_.col(border_pixel_index);
      ncameras_->getCameraShared(kFrameIdx)->backProject3(keypoint, &C_ray);
      const double scale = -(T_G_C.getPosition()(2) -
                             settings_.orthomosaic_elevation_m) /
                           (T_G_C.getRotationMatrix() * C_ray)(2);
      const Eigen::Vector3d& G_landmark =
          T_G_C.getPosition() + scale * T_G_C.getRotationMatrix() * C_ray;
      const double northing = G_landmark(1);
      const double easting = G_landmark(0);
      polygon.addVertex(grid_map::Position(northing, -easting));
    }

    for (grid_map::PolygonIterator it(map); !it.isPastEnd(); ++it) {
      grid_map::Position position;
      map.getPosition(*it, position);
      const double northing = position.x();
      const double easting = position.y();
        if (map.at("observed", *it) == 0) {
          const Eigen::Vector3d landmark_UTM(easting, northing,
                                             settings_.orthomosaic_elevation_m);
          const Eigen::Vector3d& C_landmark =
              T_G_Cs[i].inverse().transform(landmark_UTM);
          Eigen::Vector2d keypoint;
          const aslam::ProjectionResult& projection_result =
              camera.project3(C_landmark, &keypoint);
          const bool keypoint_visible =
              (keypoint(0) >= 0.0) && (keypoint(1) >= 0.0) &&
              (keypoint(0) < static_cast<double>(camera.imageWidth())) &&
              (keypoint(1) < static_cast<double>(camera.imageHeight())) &&
              (projection_result.getDetailedStatus() !=
               aslam::ProjectionResult::POINT_BEHIND_CAMERA) &&
              (projection_result.getDetailedStatus() !=
               aslam::ProjectionResult::PROJECTION_INVALID);
          if (keypoint_visible) {
            const int kp_y =
                std::min(static_cast<int>(std::round(keypoint(1))),
                         static_cast<int>(camera.imageHeight()) - 1);
            const int kp_x =
                std::min(static_cast<int>(std::round(keypoint(0))),
                         static_cast<int>(camera.imageWidth()) - 1);
            const double gray_value = images[i].at<uchar>(kp_y, kp_x);
            map.at("ortho_r", *it) =
                map.at("ortho_r", *it) =
                map.at("ortho_r", *it) = gray_value;
          } else {
            map.at("ortho_r", *it) = 0;
            map.at("ortho_g", *it) = 255;
            map.at("ortho_b", *it) = 0;
          }
        }
      }


    }
  }  // images

  const ros::Time time2 = ros::Time::now();
  const ros::Duration& delta_time = time2 - time1;
  LOG(INFO) << "Time elapsed: " << delta_time;

  if (settings_.show_orthomosaic_opencv) {
    cv::imshow("Orthomosaic", orthomosaic);
    cv::waitKey(0);
  }
  if (settings_.save_orthomosaic_jpg) {
    cv::imwrite(settings_.orthomosaic_jpg_filename, orthomosaic);
  }
}

void OrthoBackwardGrid::printParams() const {
  const int nameWidth = 30;
  std::string mode = "";
  if (settings_.mode == Mode::Batch) {
    mode = "batch";
  } else if (settings_.mode == Mode::Incremental) {
    mode = "incremental";
  }
  std::cout << std::string(50, '*') << std::endl
            << "Starting Orthomosaic image generation" << std::endl
            << std::left << std::setw(nameWidth)
            << " - Mode: " << std::left << std::setw(nameWidth)
            << mode << std::endl
            << std::left << std::setw(nameWidth)
            << " - Easting_min: " << std::left << std::setw(nameWidth)
            << std::to_string(settings_.orthomosaic_easting_min) << std::endl
            << std::left << std::setw(nameWidth)
            << " - Northing_min: " << std::left << std::setw(nameWidth)
            << std::to_string(settings_.orthomosaic_northing_min) << std::endl
            << std::left << std::setw(nameWidth)
            << " - Easting_max: " << std::left << std::setw(nameWidth)
            << std::to_string(settings_.orthomosaic_easting_max) << std::endl
            << std::left << std::setw(nameWidth)
            << " - Resolution:" << std::left << std::setw(nameWidth)
            << std::to_string(settings_.orthomosaic_resolution) << std::endl
            << std::left << std::setw(nameWidth)
            << " - Elevation (if no DSM):" << std::left << std::setw(nameWidth)
            << std::to_string(settings_.orthomosaic_elevation_m) << std::endl;
  std::cout << std::string(50, '*') << std::endl;
}

}  // namespace ortho

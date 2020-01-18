#include "plugin_camera_intrinsic_calib.h"
#include <dirent.h>
#include <iostream>

PluginCameraIntrinsicCalibration::PluginCameraIntrinsicCalibration(
    FrameBuffer *buffer, CameraParameters &_camera_params)
    : VisionPlugin(buffer),
      settings(new VarList("Camera Intrinsic Calibration")),
      widget(new CameraIntrinsicCalibrationWidget(_camera_params)),
      camera_params(_camera_params) {
  image_dir = new VarString("pattern image dir", "test-data/intrinsic_calibration");
  settings->addChild(image_dir);

  scale_down_factor = new VarDouble("scale down factor", 0.1);
  settings->addChild(scale_down_factor);

  chessboard_capture_dt = new VarDouble("chessboard capture dT", 5.0);
  settings->addChild(chessboard_capture_dt);

  corner_sub_pixel_windows_size = new VarInt("window size", 5, 1);
  corner_sub_pixel_max_iterations = new VarInt("max iterations", 30, 1);
  corner_sub_pixel_epsilon = new VarDouble("epsilon", 0.1, 1e-10);
  auto corner_sub_pixel_list = new VarList("corner sub pixel detection");
  corner_sub_pixel_list->addChild(corner_sub_pixel_windows_size);
  corner_sub_pixel_list->addChild(corner_sub_pixel_max_iterations);
  corner_sub_pixel_list->addChild(corner_sub_pixel_epsilon);
  settings->addChild(corner_sub_pixel_list);
}

QWidget *PluginCameraIntrinsicCalibration::getControlWidget() {
  return static_cast<QWidget *>(widget.get());
}

ProcessResult
PluginCameraIntrinsicCalibration::process(FrameData *data,
                                          RenderOptions *options) {
  (void)options;

  Image<raw8> *img_greyscale;
  if ((img_greyscale = reinterpret_cast<Image<raw8> *>(
           data->map.get("greyscale"))) == nullptr) {
    std::cerr << "Cannot run camera intrinsic calibration. Greyscale image is "
                 "not available.\n";
    return ProcessingFailed;
  }

  Chessboard *chessboard;
  if ((chessboard = reinterpret_cast<Chessboard *>(
           data->map.get("chessboard"))) == nullptr) {
    chessboard = reinterpret_cast<Chessboard *>(
        data->map.insert("chessboard", new Chessboard()));
  }

  // cv expects row major order and image stores col major.
  // height and width are swapped intentionally!
  cv::Mat greyscale_mat(img_greyscale->getHeight(), img_greyscale->getWidth(),
                        CV_8UC1, img_greyscale->getData());

  if (widget->should_load_images) {
    camera_params.intrinsic_parameters->reset();
    std::vector<cv::Mat> images;
    loadImages(images);

    int n = 0;
    for (cv::Mat &mat : images) {
      Chessboard image_chessboard;
      double scale_factor = 1.0;
      detectChessboard(mat, scale_factor, &image_chessboard);
      if (image_chessboard.pattern_was_found) {
        addChessboard(&image_chessboard);
      } else {
        std::cout << "No chessboard detected" << std::endl;
      }
      n++;
      widget->setImagesLoaded(n, images.size());
    }
    calibrate(greyscale_mat.size());
    widget->imagesLoaded();
  }

  if (widget->patternDetectionEnabled() || widget->isCapturing()) {
    detectChessboard(greyscale_mat, scale_down_factor->getDouble(), chessboard);
  }

  if (widget->isCapturing()) {
    if (!chessboard->pattern_was_found) {
      return ProcessingOk;
    }

    double captureDiff = data->video.getTime() - lastChessboardCaptureFrame;
    if (captureDiff < chessboard_capture_dt->getDouble()) {
      return ProcessingOk;
    }

    saveImage(data);
    addChessboard(chessboard);
    calibrate(greyscale_mat.size());
    lastChessboardCaptureFrame = data->video.getTime();
  }

  if (widget->should_calibrate_extrinsic) {
    widget->should_calibrate_extrinsic = false;

    calibrateExtrinsicModel();
  }

  if (widget->should_clear_data) {
    widget->should_clear_data = false;

    image_points.clear();
    object_points.clear();

    widget->setNumDataPoints(object_points.size());
    widget->setRms(0.0);
  }

  return ProcessingOk;
}

void PluginCameraIntrinsicCalibration::calibrateExtrinsicModel() {
  int k = CameraParameters::AdditionalCalibrationInformation::kNumControlPoints;
  std::vector<cv::Point3d> objectPoints(k);
  std::vector<cv::Point2d> imagePoints(k);
  for (int i = 0; i < k; i++) {
    objectPoints[i].x = this->camera_params.additional_calibration_information
                            ->control_point_field_xs[i]
                            ->getDouble();
    objectPoints[i].y = this->camera_params.additional_calibration_information
                            ->control_point_field_ys[i]
                            ->getDouble();
    objectPoints[i].z = 0;
    imagePoints[i].x = this->camera_params.additional_calibration_information
                           ->control_point_image_xs[i]
                           ->getDouble();
    imagePoints[i].y = this->camera_params.additional_calibration_information
                           ->control_point_image_ys[i]
                           ->getDouble();
  }

  bool useExtrinsicGuess = false;
  bool solved =
      cv::solvePnP(objectPoints, imagePoints,
                   this->camera_params.intrinsic_parameters->camera_mat,
                   this->camera_params.intrinsic_parameters->dist_coeffs,
                   camera_params.extrinsic_parameters->rvec,
                   camera_params.extrinsic_parameters->tvec, useExtrinsicGuess,
                   cv::SOLVEPNP_ITERATIVE);
  camera_params.extrinsic_parameters->updateConfigValues();

  std::cout << "Solved: " << (solved ? "yes" : "no") << std::endl
            << "rvec: " << camera_params.extrinsic_parameters->rvec << std::endl
            << " tvec: " << camera_params.extrinsic_parameters->tvec
            << std::endl;
}

void PluginCameraIntrinsicCalibration::calibrate(
    const cv::Size &imageSize) const {

  std::vector<cv::Mat> rvecs;
  std::vector<cv::Mat> tvecs;

  double rms = -1;
  try {
    rms = cv::calibrateCamera(object_points, image_points, imageSize,
                              camera_params.intrinsic_parameters->camera_mat,
                              camera_params.intrinsic_parameters->dist_coeffs,
                              rvecs, tvecs);
    camera_params.intrinsic_parameters->updateConfigValues();
  } catch (cv::Exception &e) {
    std::cerr << "calibration failed: " << e.err << std::endl;
  }
  this->widget->setRms(rms);
}

void PluginCameraIntrinsicCalibration::addChessboard(
    const Chessboard *chessboard) {
  if (!chessboard->pattern_was_found) {
    return;
  }

  this->image_points.push_back(chessboard->corners);

  std::vector<cv::Point3f> obj;
  for (int y = 0; y < chessboard->pattern_size.height; y++) {
    for (int x = 0; x < chessboard->pattern_size.width; x++) {
      obj.emplace_back(x, y, 0.0f);
    }
  }
  this->object_points.push_back(obj);

  this->widget->setNumDataPoints(this->object_points.size());
}

void PluginCameraIntrinsicCalibration::detectChessboard(
    const cv::Mat &greyscale_mat, const double scale_factor,
    Chessboard *chessboard) {
  chessboard->pattern_size.height =
      this->camera_params.additional_calibration_information->grid_height
          ->getDouble();
  chessboard->pattern_size.width =
      this->camera_params.additional_calibration_information->grid_width
          ->getDouble();
  chessboard->corners.clear();

  cv::Mat greyscale_mat_low_res;
  cv::resize(greyscale_mat, greyscale_mat_low_res, cv::Size(), scale_factor,
             scale_factor);

  std::vector<cv::Point2f> corners_low_res;
  chessboard->pattern_was_found = this->findPattern(
      greyscale_mat_low_res, chessboard->pattern_size, corners_low_res);

  for (auto &corner : corners_low_res) {
    chessboard->corners.push_back(
        cv::Point(corner.x / scale_factor, corner.y / scale_factor));
  }

  if (chessboard->pattern_was_found &&
      this->widget->cornerSubPixCorrectionEnabled()) {
    cv::cornerSubPix(
        greyscale_mat, chessboard->corners,
        cv::Size(corner_sub_pixel_windows_size->getInt(),
                 corner_sub_pixel_windows_size->getInt()),
        cv::Size(-1, -1),
        cv::TermCriteria(cv::TermCriteria::EPS | cv::TermCriteria::MAX_ITER,
                         corner_sub_pixel_max_iterations->getInt(),
                         corner_sub_pixel_epsilon->getDouble()));
  }
}

bool PluginCameraIntrinsicCalibration::findPattern(
    const cv::Mat &image, const cv::Size &pattern_size,
    vector<cv::Point2f> &corners) {
  using Pattern = CameraIntrinsicCalibrationWidget::Pattern;
  int cb_flags = cv::CALIB_CB_ADAPTIVE_THRESH + cv::CALIB_CB_FAST_CHECK +
                 cv::CALIB_CB_NORMALIZE_IMAGE;

  switch (this->widget->getPattern()) {
  case Pattern::CHECKERBOARD:
    return cv::findChessboardCorners(image, pattern_size, corners, cb_flags);
  case Pattern::CIRCLES:
    return cv::findCirclesGrid(image, pattern_size, corners);
  case Pattern::ASYMMETRIC_CIRCLES:
    return cv::findCirclesGrid(image, pattern_size, corners,
                               cv::CALIB_CB_ASYMMETRIC_GRID);
  default:
    return false;
  }
}

void PluginCameraIntrinsicCalibration::saveImage(const FrameData *data) {
  rgbImage output;
  ColorFormat fmt = data->video.getColorFormat();
  output.allocate(data->video.getWidth(), data->video.getHeight());
  if (fmt == COLOR_YUV422_UYVY) {
    Conversions::uyvy2rgb(data->video.getData(), output.getData(),
                          data->video.getWidth(), data->video.getHeight());
  } else if (fmt == COLOR_RGB8) {
    memcpy(output.getData(), data->video.getData(), data->video.getNumBytes());
  } else {
    output.allocate(0, 0);
  }
  if (output.getNumBytes() > 0) {
    QString num = QString::number(this->image_points.size());
    num = "00000" + num;
    num = num.right(5);
    QString filename = QString(image_dir->getString().c_str()) + "/" + num + ".png";
    output.save(filename.toStdString());
  }
}

void PluginCameraIntrinsicCalibration::loadImages(
    std::vector<cv::Mat> &images) {
  DIR *dp;
  if ((dp = opendir(image_dir->getString().c_str())) == nullptr) {
    std::cerr << "Failed to open directory: " << image_dir->getString() << std::endl;
    return;
  }
  struct dirent *dirp;
  std::list<std::string> imgs_to_load(0);
  while ((dirp = readdir(dp))) {
    std::string file_name(dirp->d_name);
    if (file_name[0] != '.') { // not a hidden file or one of '..' or '.'
      imgs_to_load.push_back(image_dir->getString() + "/" + file_name);
    }
  }
  closedir(dp);

  imgs_to_load.sort();
  for (const auto &currentImage : imgs_to_load) {
    std::cout << "Loading " << currentImage << std::endl;
    cv::Mat srcImg = imread(currentImage, cv::IMREAD_GRAYSCALE);
    images.push_back(srcImg);
    widget->setImagesLoaded(0, images.size());
  }
}

VarList *PluginCameraIntrinsicCalibration::getSettings() {
  return settings.get();
}

std::string PluginCameraIntrinsicCalibration::getName() {
  return "Camera Intrinsic Calibration";
}
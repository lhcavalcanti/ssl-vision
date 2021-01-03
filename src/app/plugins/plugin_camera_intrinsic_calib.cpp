#include "plugin_camera_intrinsic_calib.h"
#include <dirent.h>
#include <iostream>

PluginCameraIntrinsicCalibration::PluginCameraIntrinsicCalibration(
    FrameBuffer *buffer, CameraParameters &_camera_params)
    : VisionPlugin(buffer),
      settings(new VarList("Camera Intrinsic Calibration")),
      widget(new CameraIntrinsicCalibrationWidget(_camera_params)),
      camera_params(_camera_params) {

  worker = new PluginCameraIntrinsicCalibrationWorker(_camera_params, widget);

  reduced_image_width = new VarDouble("reduced image width for chessboard detection", 900.0);
  chessboard_capture_dt = new VarDouble("chessboard capture dT", 0.0);

  auto corner_sub_pixel_list = new VarList("corner sub pixel detection");
  corner_sub_pixel_list->addChild(worker->corner_sub_pixel_windows_size);
  corner_sub_pixel_list->addChild(worker->corner_sub_pixel_max_iterations);
  corner_sub_pixel_list->addChild(worker->corner_sub_pixel_epsilon);
  corner_sub_pixel_list->addChild(worker->corner_diff_sq_threshold);

  settings->addChild(worker->image_storage->image_dir);
  settings->addChild(reduced_image_width);
  settings->addChild(chessboard_capture_dt);
  settings->addChild(corner_sub_pixel_list);

  connect(this, SIGNAL(startLoadImages()), worker, SLOT(loadImages()));
  connect(this, SIGNAL(startCalibration()), worker, SLOT(calibrate()));
  connect(this, SIGNAL(startSaveImages()), worker->image_storage, SLOT(saveImages()));
}

PluginCameraIntrinsicCalibration::~PluginCameraIntrinsicCalibration() {
  worker->deleteLater();
  delete widget;
  delete worker;
  delete reduced_image_width;
  delete chessboard_capture_dt;
}

VarList *PluginCameraIntrinsicCalibration::getSettings() {
  return settings.get();
}

std::string PluginCameraIntrinsicCalibration::getName() {
  return "Camera Intrinsic Calibration";
}

QWidget *PluginCameraIntrinsicCalibration::getControlWidget() {
  return static_cast<QWidget *>(worker->widget);
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
  worker->imageSize = greyscale_mat.size();

  if (widget->should_load_images) {
    widget->should_load_images = false;
    emit startLoadImages();
  }

  if (widget->patternDetectionEnabled() || widget->isCapturing()) {
    worker->detectChessboard(greyscale_mat, reduced_image_width->getDouble(),
                             chessboard);
  }

  if (widget->isCapturing() && chessboard->pattern_was_found) {

    double captureDiff = data->video.getTime() - lastChessboardCaptureFrame;
    if (captureDiff < chessboard_capture_dt->getDouble()) {
      return ProcessingOk;
    }
    lastChessboardCaptureFrame = data->video.getTime();

    worker->image_storage->image_save_mutex.lock();
    worker->image_storage->images_to_save.push(greyscale_mat);
    worker->image_storage->image_save_mutex.unlock();
    emit startSaveImages();

    worker->addChessboard(chessboard);
  }

  if(widget->should_calibrate) {
    widget->should_calibrate = false;
    emit startCalibration();
  }

  if (widget->should_clear_data) {
    worker->clearData();
  }

  return ProcessingOk;
}

PluginCameraIntrinsicCalibrationWorker::PluginCameraIntrinsicCalibrationWorker(
    CameraParameters &_camera_params, CameraIntrinsicCalibrationWidget *widget)
    : widget(widget), camera_params(_camera_params) {

  corner_sub_pixel_windows_size = new VarInt("window size", 5, 1);
  corner_sub_pixel_max_iterations = new VarInt("max iterations", 30, 1);
  corner_sub_pixel_epsilon = new VarDouble("epsilon", 0.1, 1e-10);
  corner_diff_sq_threshold = new VarDouble("corner sq_diff threshold", 50);

  image_storage = new ImageStorage(widget);

  thread = new QThread();
  thread->setObjectName("IntrinsicCalibration");
  moveToThread(thread);
  thread->start();
}

PluginCameraIntrinsicCalibrationWorker::
    ~PluginCameraIntrinsicCalibrationWorker() {
  thread->quit();
  thread->deleteLater();

  delete image_storage;
  delete corner_sub_pixel_windows_size;
  delete corner_sub_pixel_epsilon;
  delete corner_sub_pixel_max_iterations;
  delete corner_diff_sq_threshold;
}

void PluginCameraIntrinsicCalibrationWorker::calibrate() {

  calib_mutex.lock();
  widget->calibrating = true;
  widget->updateConfigurationEnabled();

  std::vector<cv::Mat> rvecs;
  std::vector<cv::Mat> tvecs;

  double rms = -1;
  try {
    std::cout << "Start calibrating with " << image_points.size() << " samples" << std::endl;
    rms = cv::calibrateCamera(object_points, image_points, imageSize,
                              camera_params.intrinsic_parameters->camera_mat,
                              camera_params.intrinsic_parameters->dist_coeffs,
                              rvecs, tvecs);
    std::cout << "Calibration finished with a RMS of " << rms << std::endl;
    camera_params.intrinsic_parameters->updateConfigValues();
  } catch (cv::Exception &e) {
    std::cerr << "calibration failed: " << e.err << std::endl;
  }
  this->widget->setRms(rms);

  widget->calibrating = false;
  widget->updateConfigurationEnabled();
  calib_mutex.unlock();
}

bool PluginCameraIntrinsicCalibrationWorker::addChessboard(
    const Chessboard *chessboard) {
  calib_mutex.lock();

  // Check if there is a similar sample already
  for(const auto& img_points : this->image_points) {
    double sq_diff_sum = 0;
    for(int i = 0; i < img_points.size(); i++) {
      double diff = cv::norm(img_points[i] - chessboard->corners[i]);
      sq_diff_sum += diff*diff;
    }
    double sq_diff = sq_diff_sum / img_points.size();
    if(sq_diff < this->corner_diff_sq_threshold->getDouble()) {
      calib_mutex.unlock();
      return false;
    }
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
  calib_mutex.unlock();
  return true;
}

void PluginCameraIntrinsicCalibrationWorker::detectChessboard(
    const cv::Mat &greyscale_mat, const double max_image_width,
    Chessboard *chessboard) const {
  chessboard->pattern_size.height =
      this->camera_params.additional_calibration_information->grid_height
          ->getDouble();
  chessboard->pattern_size.width =
      this->camera_params.additional_calibration_information->grid_width
          ->getDouble();
  chessboard->corners.clear();

  cv::Mat greyscale_mat_low_res;
  double scale_factor = min(1.0, max_image_width / greyscale_mat.size().width);
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

bool PluginCameraIntrinsicCalibrationWorker::findPattern(
    const cv::Mat &image, const cv::Size &pattern_size,
    vector<cv::Point2f> &corners) const {
  using Pattern = CameraIntrinsicCalibrationWidget::Pattern;
  int cb_flags = cv::CALIB_CB_ADAPTIVE_THRESH + cv::CALIB_CB_FAST_CHECK +
                 cv::CALIB_CB_NORMALIZE_IMAGE;

  switch (widget->getPattern()) {
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

void PluginCameraIntrinsicCalibrationWorker::loadImages() {
  std::vector<cv::Mat> images;
  image_storage->readImages(images);

  int n = 0;
  for (cv::Mat &mat : images) {
    Chessboard image_chessboard;
    detectChessboard(mat, mat.size().width, &image_chessboard);
    if (image_chessboard.pattern_was_found) {
      bool added = addChessboard(&image_chessboard);
      if(added) {
        std::cout << "Added chessboard" <<std::endl;
      } else {
        std::cout << "Filtered chessboard" <<std::endl;
      }
    } else {
      std::cout << "No chessboard detected" << std::endl;
    }
    n++;
    widget->setImagesLoaded(n, images.size());
  }
  calibrate();
  widget->imagesLoaded();
}

void PluginCameraIntrinsicCalibrationWorker::clearData() {
  calib_mutex.lock();
  widget->should_clear_data = false;

  image_points.clear();
  object_points.clear();

  widget->setNumDataPoints(object_points.size());
  widget->setRms(0.0);

  calib_mutex.unlock();
}

ImageStorage::ImageStorage(CameraIntrinsicCalibrationWidget *widget)
    : widget(widget) {
  image_dir =
      new VarString("pattern image dir", "test-data/intrinsic_calibration");

  thread = new QThread();
  thread->setObjectName("IntrinsicCalibrationImageStorage");
  moveToThread(thread);
  thread->start();
}

ImageStorage::~ImageStorage() {
  thread->quit();
  thread->deleteLater();
  delete image_dir;
}

void ImageStorage::saveImages() {

  image_save_mutex.lock();
  if (images_to_save.empty()) {
    image_save_mutex.unlock();
  } else {
    cv::Mat image = images_to_save.front();
    images_to_save.pop();
    image_save_mutex.unlock();
    saveImage(image);
    saveImages();
  }
}

void ImageStorage::saveImage(cv::Mat& image) const {
  long t_now = (long)(GetTimeSec() * 1e9);
  QString num = QString::number(t_now);
  QString filename =
      QString(image_dir->getString().c_str()) + "/" + num + ".png";
  cv::imwrite(filename.toStdString(), image);
}

void ImageStorage::readImages(std::vector<cv::Mat> &images) const {
  DIR *dp;
  if ((dp = opendir(image_dir->getString().c_str())) == nullptr) {
    std::cerr << "Failed to open directory: " << image_dir->getString()
              << std::endl;
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

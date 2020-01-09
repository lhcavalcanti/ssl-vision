#ifndef CAMERA_INTRINSIC_CALIB_WIDGET_H
#define CAMERA_INTRINSIC_CALIB_WIDGET_H

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QWidget>
#include <camera_calibration.h>

class CameraIntrinsicCalibrationWidget : public QWidget {
  Q_OBJECT
public:
  enum class Pattern : int { CHECKERBOARD = 0, CIRCLES, ASYMMETRIC_CIRCLES };

public:
  explicit CameraIntrinsicCalibrationWidget(CameraParameters &camera_params);
  ~CameraIntrinsicCalibrationWidget() override = default;

  CameraParameters &camera_params;

protected:
  QComboBox *pattern_selector;
  QSpinBox *grid_width;
  QSpinBox *grid_height;
  QCheckBox *detect_pattern_checkbox;
  QCheckBox *corner_subpixel_correction_checkbox;
  QLabel *num_data_points_label;
  QLabel *rms_label;
  QLabel *images_loaded_label;
  QPushButton *clear_data_button;
  QPushButton *capture_button;
  QPushButton *load_images_button;
  QPushButton *calibrate_extrinsic_model_button;

public:
  bool patternDetectionEnabled() const {
    return detect_pattern_checkbox->checkState() != Qt::Unchecked;
  }
  bool cornerSubPixCorrectionEnabled() const {
    return corner_subpixel_correction_checkbox->checkState() != Qt::Unchecked;
  }
  bool isCapturing() const { return capture_button->isChecked(); }
  bool isLoadingFiles() const { return should_load_images; }
  bool isConfigurationEnabled() const {
    return !isCapturing() && !isLoadingFiles();
  }
  void setNumDataPoints(int n);
  Pattern getPattern() const {
    return static_cast<Pattern>(pattern_selector->currentIndex());
  }
  void imagesLoaded(int n, int total);

public slots:
  void clearDataClicked();
  void updateConfigurationEnabled();
  void loadImagesClicked();
  void grid_height_changed(int);
  void grid_width_changed(int);
  void calibrateExtrinsicModel();

public:
  void setRms(double rms);

public:
  bool should_clear_data = false;
  bool should_load_images = false;
  bool should_calibrate_extrinsic = false;
};

#endif /* CAMERA_INTRINSIC_CALIB_WIDGET_H */

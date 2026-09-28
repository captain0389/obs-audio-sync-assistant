#pragma once

#include "audio-capture.hpp"
#include "sync-engine.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QSlider>
#include <QWidget>

#include <atomic>
#include <thread>
#include <vector>

class QMouseEvent;
class QWheelEvent;
class QPainter;

class WaveformWidget final : public QWidget {
    Q_OBJECT
public:
    explicit WaveformWidget(QWidget *parent = nullptr);

    void set_waveforms(const std::vector<float> &reference,
                       const std::vector<float> &target,
                       int sample_rate,
                       double alignment_ms);
    void set_alignment(double alignment_ms);

    double zoom() const { return zoom_factor_; }
    void set_zoom(double zoom_factor);
    double vertical_zoom() const { return vertical_zoom_factor_; }
    void set_vertical_zoom(double zoom_factor);
    void fit_to_recording();
    void set_center_ms(double center_ms);
    void set_overlay(bool enabled);
    bool overlay() const { return overlay_; }

    QSize minimumSizeHint() const override { return QSize(500, 300); }

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void draw_waveform(QPainter &painter, const std::vector<float> &samples,
                       int y_center, int height, double display_shift_ms,
                       double view_start_ms, double view_end_ms,
                       int sample_rate, int width);
    double total_duration_ms() const;
    double visible_duration_ms() const;
    double time_at_x(double x) const;
    double clamp_center(double center_ms) const;

    std::vector<float> reference_;
    std::vector<float> target_;
    int sample_rate_ = 48000;
    double alignment_ms_ = 0.0;
    double zoom_factor_ = 1.0;
    double vertical_zoom_factor_ = 1.0;
    double center_ms_ = 0.0;
    bool dragging_ = false;
    bool overlay_ = false;
    QPoint last_mouse_pos_;
};

class SyncDock final : public QWidget {
    Q_OBJECT
public:
    explicit SyncDock(QWidget *parent = nullptr);
    ~SyncDock() override;

private slots:
    void refresh_sources();
    void start_measurement();
    void apply_result();
    void alignment_slider_changed(int value);
    void zoom_slider_changed(int value);
    void vertical_zoom_slider_changed(int value);
    void fit_waveform();
    void overlay_toggled(bool checked);
    void remember_source_selection();

private:
    void stop_capture();
    static bool enum_source(void *param, obs_source_t *source);
    void set_status(const QString &text);
    void update_alignment_display();
    static double zoom_from_slider(int value);
    static int slider_from_zoom(double zoom);
    static QString zoom_text(double zoom);
    static double vertical_zoom_from_slider(int value);
    static int slider_from_vertical_zoom(double zoom);
    static QString vertical_zoom_text(double zoom);

    QComboBox *reference_combo_ = nullptr;
    QComboBox *target_combo_ = nullptr;
    QComboBox *duration_combo_ = nullptr;
    QPushButton *measure_button_ = nullptr;
    QPushButton *apply_button_ = nullptr;
    QLabel *result_label_ = nullptr;
    QLabel *confidence_label_ = nullptr;
    QLabel *alignment_label_ = nullptr;
    QLabel *zoom_label_ = nullptr;
    QLabel *vertical_zoom_label_ = nullptr;
    QCheckBox *overlay_checkbox_ = nullptr;
    QProgressBar *progress_ = nullptr;
    QSlider *alignment_slider_ = nullptr;
    QSlider *zoom_slider_ = nullptr;
    QSlider *vertical_zoom_slider_ = nullptr;
    QPushButton *fit_button_ = nullptr;
    WaveformWidget *waveform_ = nullptr;

    AudioCaptureBuffer reference_buffer_;
    AudioCaptureBuffer target_buffer_;
    SourceAudioTap reference_tap_;
    SourceAudioTap target_tap_;

    std::thread worker_;
    std::atomic<bool> cancel_{false};

    SyncResult last_result_;
    QString target_name_;
    double selected_alignment_ms_ = 0.0;
    double configured_difference_ms_ = 0.0;
    bool has_recording_ = false;
    bool restoring_sources_ = false;
};

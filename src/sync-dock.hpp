#pragma once

#include "audio-capture.hpp"
#include "sync-engine.hpp"

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QSlider>
#include <QWidget>

#include <atomic>
#include <thread>
#include <vector>

class WaveformWidget final : public QWidget {
    Q_OBJECT
public:
    explicit WaveformWidget(QWidget *parent = nullptr);

    void set_waveforms(const std::vector<float> &reference,
                       const std::vector<float> &target,
                       int sample_rate,
                       double alignment_ms);
    void set_alignment(double alignment_ms);
    QSize minimumSizeHint() const override { return QSize(500, 220); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    static void draw_waveform(QPainter &painter, const std::vector<float> &samples,
                              int y_center, int height, double shift_samples,
                              int sample_rate, int width);

    std::vector<float> reference_;
    std::vector<float> target_;
    int sample_rate_ = 48000;
    double alignment_ms_ = 0.0;
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

private:
    void stop_capture();
    static bool enum_source(void *param, obs_source_t *source);
    void set_status(const QString &text);
    void update_alignment_display();

    QComboBox *reference_combo_ = nullptr;
    QComboBox *target_combo_ = nullptr;
    QComboBox *duration_combo_ = nullptr;
    QPushButton *measure_button_ = nullptr;
    QPushButton *apply_button_ = nullptr;
    QLabel *result_label_ = nullptr;
    QLabel *confidence_label_ = nullptr;
    QLabel *alignment_label_ = nullptr;
    QProgressBar *progress_ = nullptr;
    QSlider *alignment_slider_ = nullptr;
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
};

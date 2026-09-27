#pragma once

#include "audio-capture.hpp"
#include "sync-engine.hpp"

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QWidget>

#include <atomic>
#include <thread>

class SyncDock final : public QWidget {
    Q_OBJECT
public:
    explicit SyncDock(QWidget *parent = nullptr);
    ~SyncDock() override;

private slots:
    void refresh_sources();
    void start_measurement();
    void apply_result();
    void start_calibration();
    void apply_calibration();

private:
    void stop_capture();
    void stop_calibration_capture();
    void cleanup_calibration_source();
    static bool enum_source(void *param, obs_source_t *source);
    void set_status(const QString &text);
    void set_calibration_status(const QString &text);

    QComboBox *reference_combo_ = nullptr;
    QComboBox *target_combo_ = nullptr;
    QComboBox *duration_combo_ = nullptr;
    QPushButton *measure_button_ = nullptr;
    QPushButton *apply_button_ = nullptr;
    QLabel *result_label_ = nullptr;
    QLabel *confidence_label_ = nullptr;
    QProgressBar *progress_ = nullptr;

    QComboBox *calibration_mic_combo_ = nullptr;
    QPushButton *calibration_button_ = nullptr;
    QPushButton *apply_calibration_button_ = nullptr;
    QLabel *calibration_result_label_ = nullptr;
    QLabel *calibration_help_label_ = nullptr;

    AudioCaptureBuffer reference_buffer_;
    AudioCaptureBuffer target_buffer_;
    SourceAudioTap reference_tap_;
    SourceAudioTap target_tap_;

    AudioCaptureBuffer calibration_buffer_;
    AudioCaptureBuffer microphone_buffer_;
    SourceAudioTap calibration_tap_;
    SourceAudioTap microphone_tap_;

    obs_source_t *calibration_source_ = nullptr;
    obs_source_t *calibration_scene_ = nullptr;
    obs_sceneitem_t *calibration_scene_item_ = nullptr;

    std::thread worker_;
    std::atomic<bool> cancel_{false};

    SyncResult last_result_;
    SyncResult last_calibration_result_;
    QString target_name_;
    QString calibration_mic_name_;
};

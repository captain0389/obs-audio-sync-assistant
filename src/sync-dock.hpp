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

private:
    void stop_capture();
    static bool enum_source(void *param, obs_source_t *source);
    void set_status(const QString &text);

    QComboBox *reference_combo_ = nullptr;
    QComboBox *target_combo_ = nullptr;
    QComboBox *duration_combo_ = nullptr;
    QPushButton *measure_button_ = nullptr;
    QPushButton *apply_button_ = nullptr;
    QLabel *result_label_ = nullptr;
    QLabel *confidence_label_ = nullptr;
    QProgressBar *progress_ = nullptr;

    AudioCaptureBuffer reference_buffer_;
    AudioCaptureBuffer target_buffer_;
    SourceAudioTap reference_tap_;
    SourceAudioTap target_tap_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    SyncResult last_result_;
    QString target_name_;
};

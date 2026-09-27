#pragma once

#include "audio-capture.hpp"
#include "sync-engine.hpp"

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QWidget>

#include <atomic>
#include <cstdint>
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
    bool setup_probe_routing(obs_source_t *reference, obs_source_t *target);
    void restore_probe_routing();
    static bool enum_source(void *param, obs_source_t *source);
    static bool enum_used_mixers(void *param, obs_source_t *source);
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
    RawMixAudioTap reference_tap_;
    RawMixAudioTap target_tap_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    SyncResult last_result_;
    QString target_name_;

    obs_source_t *reference_source_ = nullptr;
    obs_source_t *target_source_ = nullptr;
    uint32_t reference_original_mixers_ = 0;
    uint32_t target_original_mixers_ = 0;
    uint32_t reference_probe_bit_ = 0;
    uint32_t target_probe_bit_ = 0;
    uint32_t sample_rate_ = 48000;
    bool probe_routing_active_ = false;
};

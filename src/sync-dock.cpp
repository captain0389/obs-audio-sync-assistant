#include "sync-dock.hpp"

#include "calibration-source.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QMetaObject>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

double sync_offset_ms(obs_source_t *source)
{
    if (!source)
        return 0.0;
    return static_cast<double>(obs_source_get_sync_offset(source)) / 1000000.0;
}

} // namespace

SyncDock::SyncDock(QWidget *parent) : QWidget(parent)
{
    setWindowTitle("Audio Sync Assistant");
    auto *layout = new QVBoxLayout(this);

    auto *ref_row = new QHBoxLayout();
    ref_row->addWidget(new QLabel("Reference source:"));
    reference_combo_ = new QComboBox();
    ref_row->addWidget(reference_combo_, 1);
    layout->addLayout(ref_row);

    auto *target_row = new QHBoxLayout();
    target_row->addWidget(new QLabel("Source to synchronize:"));
    target_combo_ = new QComboBox();
    target_row->addWidget(target_combo_, 1);
    layout->addLayout(target_row);

    auto *duration_row = new QHBoxLayout();
    duration_row->addWidget(new QLabel("Analysis length:"));
    duration_combo_ = new QComboBox();
    duration_combo_->addItem("3 seconds", 3);
    duration_combo_->addItem("5 seconds", 5);
    duration_combo_->addItem("8 seconds", 8);
    duration_combo_->setCurrentIndex(1);
    duration_row->addWidget(duration_combo_, 1);
    layout->addLayout(duration_row);

    measure_button_ = new QPushButton("Start measurement");
    apply_button_ = new QPushButton("Apply correction to target");
    apply_button_->setEnabled(false);
    layout->addWidget(measure_button_);

    progress_ = new QProgressBar();
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->setTextVisible(false);
    layout->addWidget(progress_);

    result_label_ = new QLabel("No measurement yet.");
    result_label_->setWordWrap(true);
    confidence_label_ = new QLabel();
    layout->addWidget(result_label_);
    layout->addWidget(confidence_label_);
    layout->addWidget(apply_button_);

    auto *refresh = new QPushButton("Refresh source list");
    layout->addWidget(refresh);

    // Dedicated calibration mode.  This deliberately uses a known test signal
    // instead of arbitrary program audio, because microphone/speaker paths can
    // radically reshape ordinary waveforms and confuse correlation.
    auto *cal_group = new QGroupBox("Microphone / speaker calibration");
    auto *cal_layout = new QVBoxLayout(cal_group);

    auto *mic_row = new QHBoxLayout();
    mic_row->addWidget(new QLabel("Microphone source:"));
    calibration_mic_combo_ = new QComboBox();
    mic_row->addWidget(calibration_mic_combo_, 1);
    cal_layout->addLayout(mic_row);

    calibration_help_label_ = new QLabel(
        "Use speakers for this test. OBS will temporarily play a known "
        "calibration sweep through the monitor output and listen for it on "
        "the selected microphone. Keep the room quiet and do not switch "
        "scenes during the test.");
    calibration_help_label_->setWordWrap(true);
    cal_layout->addWidget(calibration_help_label_);

    calibration_button_ = new QPushButton("Run speaker-to-microphone calibration");
    apply_calibration_button_ = new QPushButton("Apply correction to microphone");
    apply_calibration_button_->setEnabled(false);
    calibration_result_label_ = new QLabel("No calibration yet.");
    calibration_result_label_->setWordWrap(true);

    cal_layout->addWidget(calibration_button_);
    cal_layout->addWidget(calibration_result_label_);
    cal_layout->addWidget(apply_calibration_button_);
    layout->addWidget(cal_group);

    layout->addStretch();

    connect(refresh, &QPushButton::clicked, this, &SyncDock::refresh_sources);
    connect(measure_button_, &QPushButton::clicked, this,
            &SyncDock::start_measurement);
    connect(apply_button_, &QPushButton::clicked, this,
            &SyncDock::apply_result);
    connect(calibration_button_, &QPushButton::clicked, this,
            &SyncDock::start_calibration);
    connect(apply_calibration_button_, &QPushButton::clicked, this,
            &SyncDock::apply_calibration);

    refresh_sources();
}

SyncDock::~SyncDock()
{
    cancel_.store(true);
    stop_capture();
    stop_calibration_capture();

    if (worker_.joinable())
        worker_.join();

    cleanup_calibration_source();
}

void SyncDock::set_status(const QString &text)
{
    result_label_->setText(text);
}

void SyncDock::set_calibration_status(const QString &text)
{
    calibration_result_label_->setText(text);
}

bool SyncDock::enum_source(void *param, obs_source_t *source)
{
    auto *dock = static_cast<SyncDock *>(param);
    if (!dock || !source)
        return true;

    const uint32_t flags = obs_source_get_output_flags(source);
    if (!(flags & OBS_SOURCE_AUDIO))
        return true;

    const char *name = obs_source_get_name(source);
    const char *uuid = obs_source_get_uuid(source);
    if (name && uuid) {
        const QString qname = QString::fromUtf8(name);
        const QString quuid = QString::fromUtf8(uuid);
        dock->reference_combo_->addItem(qname, quuid);
        dock->target_combo_->addItem(qname, quuid);
        dock->calibration_mic_combo_->addItem(qname, quuid);
    }
    return true;
}

void SyncDock::refresh_sources()
{
    reference_combo_->clear();
    target_combo_->clear();
    calibration_mic_combo_->clear();

    obs_enum_sources(enum_source, this);

    if (target_combo_->count() > 1)
        target_combo_->setCurrentIndex(1);
}

void SyncDock::stop_capture()
{
    reference_buffer_.stop();
    target_buffer_.stop();
    reference_tap_.detach();
    target_tap_.detach();
}

void SyncDock::stop_calibration_capture()
{
    calibration_buffer_.stop();
    microphone_buffer_.stop();
    calibration_tap_.detach();
    microphone_tap_.detach();
}

void SyncDock::cleanup_calibration_source()
{
    if (calibration_scene_item_) {
        obs_sceneitem_remove(calibration_scene_item_);
        calibration_scene_item_ = nullptr;
    }

    if (calibration_source_) {
        obs_source_release(calibration_source_);
        calibration_source_ = nullptr;
    }

    if (calibration_scene_) {
        obs_source_release(calibration_scene_);
        calibration_scene_ = nullptr;
    }
}

void SyncDock::start_measurement()
{
    if (reference_combo_->currentData().toString().isEmpty() ||
        target_combo_->currentData().toString().isEmpty()) {
        set_status("Select two audio sources first.");
        return;
    }

    if (reference_combo_->currentData() == target_combo_->currentData()) {
        set_status("Reference and target must be different sources.");
        return;
    }

    if (worker_.joinable())
        worker_.join();

    cancel_.store(false);
    apply_button_->setEnabled(false);
    apply_calibration_button_->setEnabled(false);
    measure_button_->setEnabled(false);
    calibration_button_->setEnabled(true);
    progress_->setValue(0);
    set_status("Listening for shared audio…");
    confidence_label_->clear();

    const QByteArray ref_uuid =
        reference_combo_->currentData().toString().toUtf8();
    const QByteArray target_uuid =
        target_combo_->currentData().toString().toUtf8();

    obs_source_t *ref = obs_get_source_by_uuid(ref_uuid.constData());
    obs_source_t *target = obs_get_source_by_uuid(target_uuid.constData());

    if (!ref || !target) {
        if (ref)
            obs_source_release(ref);
        if (target)
            obs_source_release(target);
        measure_button_->setEnabled(true);
        set_status("Could not access one of the selected sources.");
        return;
    }

    const double reference_sync_ms = sync_offset_ms(ref);
    const double target_sync_ms = sync_offset_ms(target);
    const double configured_difference_ms =
        target_sync_ms - reference_sync_ms;

    target_name_ = target_combo_->currentText();

    reference_buffer_.start();
    target_buffer_.start();

    const bool ref_attached =
        reference_tap_.attach(ref, &reference_buffer_);
    const bool target_attached =
        target_tap_.attach(target, &target_buffer_);

    obs_source_release(ref);
    obs_source_release(target);

    if (!ref_attached || !target_attached) {
        stop_capture();
        measure_button_->setEnabled(true);
        set_status("Could not attach to one of the selected audio sources.");
        return;
    }

    const int seconds = duration_combo_->currentData().toInt();

    worker_ = std::thread([this, seconds, configured_difference_ms,
                           reference_sync_ms, target_sync_ms] {
        const int total_ms = seconds * 1000;

        for (int elapsed = 0;
             elapsed < total_ms && !cancel_.load(); elapsed += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const int pct = std::min(100, elapsed * 100 / total_ms);
            QMetaObject::invokeMethod(
                this, [this, pct] { progress_->setValue(pct); },
                Qt::QueuedConnection);
        }

        reference_buffer_.stop();
        target_buffer_.stop();

        const auto ref =
            reference_buffer_.snapshot(static_cast<size_t>(seconds) * 48000);
        const auto target =
            target_buffer_.snapshot(static_cast<size_t>(seconds) * 48000);

        SyncResult result =
            estimate_sync_gcc_phat(ref, target, 48000, 500);
        const double raw_offset_ms = result.offset_ms;
        result.offset_ms = raw_offset_ms - configured_difference_ms;

        QMetaObject::invokeMethod(
            this,
            [this, result, raw_offset_ms, reference_sync_ms,
             target_sync_ms] {
                stop_capture();

                last_result_ = result;
                measure_button_->setEnabled(true);
                progress_->setValue(100);

                if (!result.valid) {
                    apply_button_->setEnabled(false);
                    set_status(QString::fromUtf8(
                        result.message ? result.message
                                       : "Measurement failed."));
                    confidence_label_->setText(
                        QString("Peak ratio: %1")
                            .arg(result.peak_ratio, 0, 'f', 2));
                    return;
                }

                const QString direction =
                    result.offset_ms >= 0.0 ? "Target leads reference"
                                           : "Target lags reference";

                if (std::abs(result.offset_ms) <= 0.05) {
                    set_status("Sources are aligned (0.00 ms)");
                } else {
                    set_status(
                        QString("%1 by %2 ms")
                            .arg(direction)
                            .arg(std::abs(result.offset_ms), 0, 'f', 2));
                }

                confidence_label_->setText(
                    QString(
                        "Confidence: %1%  •  %2 samples analyzed  •  "
                        "Raw: %3 ms  •  Configured target-reference: %4 ms")
                        .arg(result.confidence * 100.0, 0, 'f', 0)
                        .arg(result.samples_used)
                        .arg(raw_offset_ms, 0, 'f', 2)
                        .arg(target_sync_ms - reference_sync_ms, 0, 'f',
                             2));

                apply_button_->setEnabled(true);
            },
            Qt::QueuedConnection);
    });
}

void SyncDock::apply_result()
{
    if (!last_result_.valid)
        return;

    const QByteArray uuid =
        target_combo_->currentData().toString().toUtf8();
    obs_source_t *target = obs_get_source_by_uuid(uuid.constData());
    if (!target)
        return;

    const int64_t correction_ns =
        static_cast<int64_t>(last_result_.offset_ms * 1000000.0);
    const int64_t current = obs_source_get_sync_offset(target);
    obs_source_set_sync_offset(target, current + correction_ns);
    obs_source_release(target);

    set_status(QString("Applied %1 ms correction to %2.")
                   .arg(last_result_.offset_ms, 0, 'f', 2)
                   .arg(target_name_));
    apply_button_->setEnabled(false);
}

void SyncDock::start_calibration()
{
    if (calibration_mic_combo_->currentData().toString().isEmpty()) {
        set_calibration_status("Select a microphone source first.");
        return;
    }

    if (worker_.joinable())
        worker_.join();

    cancel_.store(false);
    calibration_button_->setEnabled(false);
    apply_calibration_button_->setEnabled(false);
    measure_button_->setEnabled(true);

    cleanup_calibration_source();
    stop_calibration_capture();

    obs_source_t *microphone = obs_get_source_by_uuid(
        calibration_mic_combo_->currentData().toString().toUtf8().constData());
    if (!microphone) {
        calibration_button_->setEnabled(true);
        set_calibration_status("Could not access the selected microphone.");
        return;
    }

    calibration_scene_ = obs_frontend_get_current_scene();
    if (!calibration_scene_) {
        obs_source_release(microphone);
        calibration_button_->setEnabled(true);
        set_calibration_status("There is no active OBS scene.");
        return;
    }

    calibration_source_ = calibration_source::create_private();
    if (!calibration_source_) {
        obs_source_release(microphone);
        obs_source_release(calibration_scene_);
        calibration_scene_ = nullptr;
        calibration_button_->setEnabled(true);
        set_calibration_status(
            "Could not create the temporary calibration source.");
        return;
    }

    calibration_scene_item_ =
        obs_scene_add(obs_scene_from_source(calibration_scene_),
                      calibration_source_);
    if (!calibration_scene_item_) {
        obs_source_release(microphone);
        cleanup_calibration_source();
        calibration_button_->setEnabled(true);
        set_calibration_status(
            "Could not add the calibration signal to the current scene.");
        return;
    }

    // Force the temporary source to the monitor device as well as the normal
    // OBS outputs.  This uses the legacy API intentionally so the plugin also
    // builds against OBS versions before 33, where the newer
    // obs_source_set_monitoring_enabled API does not exist.
    obs_source_set_monitoring_type(
        calibration_source_, OBS_MONITORING_TYPE_MONITOR_ONLY);
    // A private source is not guaranteed to be considered audio-active just
    // because it has a scene item. Explicitly activate its audio path so the
    // generated samples reach OBS' monitor mixer.
    obs_source_set_audio_active(calibration_source_, true);
    obs_source_set_enabled(calibration_source_, true);
    obs_source_set_volume(calibration_source_, 0.5f);

    calibration_mic_name_ = calibration_mic_combo_->currentText();

    calibration_buffer_.start();
    microphone_buffer_.start();

    const bool calibration_attached =
        calibration_tap_.attach(calibration_source_, &calibration_buffer_);
    const bool microphone_attached =
        microphone_tap_.attach(microphone, &microphone_buffer_);

    obs_source_release(microphone);

    if (!calibration_attached || !microphone_attached) {
        stop_calibration_capture();
        cleanup_calibration_source();
        calibration_button_->setEnabled(true);
        set_calibration_status(
            "Could not attach to the calibration or microphone source.");
        return;
    }

    set_calibration_status(
        "Playing calibration sweep… Keep the microphone and speakers "
        "stationary.");

    calibration_source::trigger(calibration_source_);

    worker_ = std::thread([this] {
        // 250 ms lead-in + 750 ms sweep pattern + room for the microphone
        // capture path and its tail.
        constexpr int wait_ms = 3000;
        for (int elapsed = 0;
             elapsed < wait_ms && !cancel_.load(); elapsed += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        calibration_buffer_.stop();
        microphone_buffer_.stop();

        const auto reference =
            calibration_buffer_.snapshot(48000 * 5);
        const auto microphone =
            microphone_buffer_.snapshot(48000 * 5);

        SyncResult result =
            estimate_sync_gcc_phat(reference, microphone, 48000, 1000);

        QMetaObject::invokeMethod(
            this, [this, result] {
                stop_calibration_capture();
                cleanup_calibration_source();

                last_calibration_result_ = result;
                calibration_button_->setEnabled(true);

                if (!result.valid) {
                    apply_calibration_button_->setEnabled(false);
                    set_calibration_status(
                        "Calibration signal was not detected reliably. "
                        "Make sure OBS is monitoring to speakers and that "
                        "the microphone can hear them.");
                    return;
                }

                // Positive means the microphone leads the calibration
                // playback, so adding a positive OBS Sync Offset to the
                // microphone delays it into alignment.
                const QString relation =
                    result.offset_ms >= 0.0
                        ? QString("Microphone leads speaker playback by %1 ms.")
                              .arg(result.offset_ms, 0, 'f', 2)
                        : QString("Microphone lags speaker playback by %1 ms.")
                              .arg(std::abs(result.offset_ms), 0, 'f', 2);

                set_calibration_status(
                    relation +
                    QString("  Confidence: %1%")
                        .arg(result.confidence * 100.0, 0, 'f', 0));

                apply_calibration_button_->setEnabled(
                    result.offset_ms > 0.0);
            },
            Qt::QueuedConnection);
    });
}

void SyncDock::apply_calibration()
{
    if (!last_calibration_result_.valid ||
        last_calibration_result_.offset_ms <= 0.0) {
        return;
    }

    const QByteArray uuid =
        calibration_mic_combo_->currentData().toString().toUtf8();
    obs_source_t *microphone = obs_get_source_by_uuid(uuid.constData());
    if (!microphone)
        return;

    const int64_t correction_ns = static_cast<int64_t>(
        last_calibration_result_.offset_ms * 1000000.0);
    const int64_t current = obs_source_get_sync_offset(microphone);
    obs_source_set_sync_offset(microphone, current + correction_ns);
    obs_source_release(microphone);

    set_calibration_status(
        QString("Applied +%1 ms correction to %2.")
            .arg(last_calibration_result_.offset_ms, 0, 'f', 2)
            .arg(calibration_mic_name_));
    apply_calibration_button_->setEnabled(false);
}

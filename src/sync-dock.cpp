#include "sync-dock.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <QHBoxLayout>
#include <QMetaObject>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>

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
    layout->addStretch();

    connect(refresh, &QPushButton::clicked, this, &SyncDock::refresh_sources);
    connect(measure_button_, &QPushButton::clicked, this, &SyncDock::start_measurement);
    connect(apply_button_, &QPushButton::clicked, this, &SyncDock::apply_result);

    refresh_sources();
}

SyncDock::~SyncDock()
{
    cancel_.store(true);
    stop_capture();
    if (worker_.joinable())
        worker_.join();
}

void SyncDock::set_status(const QString &text)
{
    result_label_->setText(text);
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
    if (name) {
        dock->reference_combo_->addItem(QString::fromUtf8(name), QString::fromUtf8(obs_source_get_uuid(source)));
        dock->target_combo_->addItem(QString::fromUtf8(name), QString::fromUtf8(obs_source_get_uuid(source)));
    }
    return true;
}

void SyncDock::refresh_sources()
{
    reference_combo_->clear();
    target_combo_->clear();
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
    measure_button_->setEnabled(false);
    progress_->setValue(0);
    set_status("Listening for shared audio…");
    confidence_label_->clear();

    obs_source_t *ref = obs_get_source_by_uuid(reference_combo_->currentData().toString().toUtf8().constData());
    obs_source_t *target = obs_get_source_by_uuid(target_combo_->currentData().toString().toUtf8().constData());
    if (!ref || !target) {
        if (ref) obs_source_release(ref);
        if (target) obs_source_release(target);
        measure_button_->setEnabled(true);
        set_status("Could not access one of the selected sources.");
        return;
    }

    target_name_ = target_combo_->currentText();
    reference_buffer_.start();
    target_buffer_.start();
    reference_tap_.attach(ref, &reference_buffer_);
    target_tap_.attach(target, &target_buffer_);
    obs_source_release(ref);
    obs_source_release(target);

    const int seconds = duration_combo_->currentData().toInt();
    worker_ = std::thread([this, seconds] {
        const int total_ms = seconds * 1000;
        for (int elapsed = 0; elapsed < total_ms && !cancel_.load(); elapsed += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const int pct = std::min(100, elapsed * 100 / total_ms);
            QMetaObject::invokeMethod(this, [this, pct] { progress_->setValue(pct); }, Qt::QueuedConnection);
        }

        reference_buffer_.stop();
        target_buffer_.stop();
        const auto ref = reference_buffer_.snapshot(static_cast<size_t>(seconds) * 48000);
        const auto target = target_buffer_.snapshot(static_cast<size_t>(seconds) * 48000);
        const SyncResult result = estimate_sync_gcc_phat(ref, target, 48000, 500);

        QMetaObject::invokeMethod(this, [this, result] {
            last_result_ = result;
            measure_button_->setEnabled(true);
            progress_->setValue(100);
            if (!result.valid) {
                apply_button_->setEnabled(false);
                set_status(QString::fromUtf8(result.message ? result.message : "Measurement failed."));
                confidence_label_->setText(QString("Peak ratio: %1").arg(result.peak_ratio, 0, 'f', 2));
                return;
            }

            const QString direction = result.offset_ms >= 0.0 ? "Target leads reference" : "Target lags reference";
            set_status(QString("%1 by %2 ms").arg(direction).arg(std::abs(result.offset_ms), 0, 'f', 2));
            confidence_label_->setText(QString("Confidence: %1%  •  %2 samples analyzed")
                .arg(result.confidence * 100.0, 0, 'f', 0)
                .arg(result.samples_used));
            apply_button_->setEnabled(true);
        }, Qt::QueuedConnection);
    });
}

void SyncDock::apply_result()
{
    if (!last_result_.valid)
        return;

    const QByteArray uuid = target_combo_->currentData().toString().toUtf8();
    obs_source_t *target = obs_get_source_by_uuid(uuid.constData());
    if (!target)
        return;

    const int64_t delta_ns = static_cast<int64_t>(last_result_.offset_ms * 1000000.0);
    const int64_t current = obs_source_get_sync_offset(target);
    obs_source_set_sync_offset(target, current + delta_ns);
    obs_source_release(target);

    set_status(QString("Applied %1 ms to %2.").arg(last_result_.offset_ms, 0, 'f', 2).arg(target_name_));
    apply_button_->setEnabled(false);
}

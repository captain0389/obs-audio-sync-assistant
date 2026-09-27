#include "sync-dock.hpp"

#include <obs.h>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QMetaObject>

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

constexpr int SLIDER_MIN_MS = -500;
constexpr int SLIDER_MAX_MS = 500;

} // namespace

WaveformWidget::WaveformWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void WaveformWidget::set_waveforms(const std::vector<float> &reference,
                                   const std::vector<float> &target,
                                   int sample_rate,
                                   double alignment_ms)
{
    reference_ = reference;
    target_ = target;
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    alignment_ms_ = alignment_ms;
    update();
}

void WaveformWidget::set_alignment(double alignment_ms)
{
    alignment_ms_ = alignment_ms;
    update();
}

void WaveformWidget::draw_waveform(QPainter &painter,
                                   const std::vector<float> &samples,
                                   int y_center, int height, double shift_samples,
                                   int sample_rate, int width)
{
    if (samples.empty() || width <= 0)
        return;

    const int count = static_cast<int>(samples.size());
    const double center = static_cast<double>(count - 1) / 2.0;
    const double px_per_sample = static_cast<double>(width) / static_cast<double>(count);

    for (int x = 0; x < width; ++x) {
        const double relative = (static_cast<double>(x) - width / 2.0) / px_per_sample;
        const double sample_pos = center + relative - shift_samples;
        const int i = static_cast<int>(std::floor(sample_pos));
        if (i < 0 || i >= count)
            continue;

        const int span = std::max(1, static_cast<int>(std::ceil(1.0 / px_per_sample)));
        const int end = std::min(count, i + span);
        float min_v = 1.0f;
        float max_v = -1.0f;
        for (int j = i; j < end; ++j) {
            min_v = std::min(min_v, samples[static_cast<size_t>(j)]);
            max_v = std::max(max_v, samples[static_cast<size_t>(j)]);
        }

        const int y1 = y_center - static_cast<int>(max_v * height * 0.45f);
        const int y2 = y_center - static_cast<int>(min_v * height * 0.45f);
        painter.drawLine(x, y1, x, y2);
    }

    Q_UNUSED(sample_rate);
}

void WaveformWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(rect(), palette().base());

    const int w = width();
    const int top = 40;
    const int bottom = height() - 10;
    const int half = (bottom - top) / 2;

    painter.setPen(palette().color(QPalette::Mid));
    painter.drawLine(0, top + half / 2, w, top + half / 2);
    painter.drawLine(0, top + half + half / 2, w, top + half + half / 2);
    painter.drawLine(w / 2, top, w / 2, bottom);

    painter.setPen(palette().text().color());
    painter.drawText(8, 18, "Reference");
    painter.drawText(8, top + half + 18, "Target");

    if (reference_.empty() || target_.empty()) {
        painter.drawText(8, height() / 2, "Record two sources to display their waveforms.");
        return;
    }

    const double shift_samples = alignment_ms_ * sample_rate_ / 1000.0;

    painter.setPen(QPen(palette().text().color(), 1));
    draw_waveform(painter, reference_, top + half / 2, half, 0.0,
                  sample_rate_, w);

    painter.setPen(QPen(palette().highlight().color(), 1));
    draw_waveform(painter, target_, top + half + half / 2, half,
                  shift_samples, sample_rate_, w);
}

SyncDock::SyncDock(QWidget *parent) : QWidget(parent)
{
    setWindowTitle("Audio Sync Assistant");

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *content = new QWidget();
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(8, 8, 8, 8);

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
    duration_row->addWidget(new QLabel("Recording length:"));
    duration_combo_ = new QComboBox();
    duration_combo_->addItem("3 seconds", 3);
    duration_combo_->addItem("5 seconds", 5);
    duration_combo_->addItem("8 seconds", 8);
    duration_combo_->setCurrentIndex(1);
    duration_row->addWidget(duration_combo_, 1);
    layout->addLayout(duration_row);

    measure_button_ = new QPushButton("Record & analyze");
    apply_button_ = new QPushButton("Apply correction to target");
    apply_button_->setEnabled(false);
    layout->addWidget(measure_button_);

    progress_ = new QProgressBar();
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->setTextVisible(false);
    layout->addWidget(progress_);

    result_label_ = new QLabel("No recording yet.");
    result_label_->setWordWrap(true);
    confidence_label_ = new QLabel();
    confidence_label_->setWordWrap(true);
    layout->addWidget(result_label_);
    layout->addWidget(confidence_label_);

    auto *wave_group = new QGroupBox("Waveform alignment");
    auto *wave_layout = new QVBoxLayout(wave_group);
    waveform_ = new WaveformWidget();
    wave_layout->addWidget(waveform_);

    alignment_label_ = new QLabel("Alignment: 0.00 ms");
    alignment_label_->setAlignment(Qt::AlignCenter);
    wave_layout->addWidget(alignment_label_);

    alignment_slider_ = new QSlider(Qt::Horizontal);
    alignment_slider_->setRange(SLIDER_MIN_MS, SLIDER_MAX_MS);
    alignment_slider_->setValue(0);
    alignment_slider_->setSingleStep(1);
    alignment_slider_->setPageStep(10);
    alignment_slider_->setEnabled(false);
    wave_layout->addWidget(alignment_slider_);

    auto *hint = new QLabel(
        "Positive values delay the target. Drag the slider until the two "
        "waveforms visually line up, then apply the correction.");
    hint->setWordWrap(true);
    wave_layout->addWidget(hint);

    wave_layout->addWidget(apply_button_);
    layout->addWidget(wave_group);

    auto *refresh = new QPushButton("Refresh source list");
    layout->addWidget(refresh);
    layout->addStretch();

    scroll->setWidget(content);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    connect(refresh, &QPushButton::clicked, this, &SyncDock::refresh_sources);
    connect(measure_button_, &QPushButton::clicked, this,
            &SyncDock::start_measurement);
    connect(apply_button_, &QPushButton::clicked, this,
            &SyncDock::apply_result);
    connect(alignment_slider_, &QSlider::valueChanged, this,
            &SyncDock::alignment_slider_changed);

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
    const char *uuid = obs_source_get_uuid(source);
    if (name && uuid) {
        const QString qname = QString::fromUtf8(name);
        const QString quuid = QString::fromUtf8(uuid);
        dock->reference_combo_->addItem(qname, quuid);
        dock->target_combo_->addItem(qname, quuid);
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
    alignment_slider_->setEnabled(false);
    measure_button_->setEnabled(false);
    has_recording_ = false;
    progress_->setValue(0);
    set_status("Recording both sources…");
    confidence_label_->clear();

    const QByteArray ref_uuid =
        reference_combo_->currentData().toString().toUtf8();
    const QByteArray target_uuid =
        target_combo_->currentData().toString().toUtf8();

    obs_source_t *ref = obs_get_source_by_uuid(ref_uuid.constData());
    obs_source_t *target = obs_get_source_by_uuid(target_uuid.constData());

    if (!ref || !target) {
        if (ref) obs_source_release(ref);
        if (target) obs_source_release(target);
        measure_button_->setEnabled(true);
        set_status("Could not access one of the selected sources.");
        return;
    }

    const double reference_sync_ms = sync_offset_ms(ref);
    const double target_sync_ms = sync_offset_ms(target);
    const double configured_difference_ms = target_sync_ms - reference_sync_ms;
    configured_difference_ms_ = configured_difference_ms;
    target_name_ = target_combo_->currentText();

    reference_buffer_.start();
    target_buffer_.start();

    const bool ref_attached = reference_tap_.attach(ref, &reference_buffer_);
    const bool target_attached = target_tap_.attach(target, &target_buffer_);

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
        for (int elapsed = 0; elapsed < total_ms && !cancel_.load(); elapsed += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const int pct = std::min(100, elapsed * 100 / total_ms);
            QMetaObject::invokeMethod(this, [this, pct] {
                progress_->setValue(pct);
            }, Qt::QueuedConnection);
        }

        reference_buffer_.stop();
        target_buffer_.stop();

        auto ref = reference_buffer_.snapshot(static_cast<size_t>(seconds) * 48000);
        auto target = target_buffer_.snapshot(static_cast<size_t>(seconds) * 48000);

        SyncResult result = estimate_sync_gcc_phat(ref, target, 48000, 500);
        const double raw_offset_ms = result.offset_ms;
        result.offset_ms = raw_offset_ms - configured_difference_ms;

        QMetaObject::invokeMethod(this,
            [this, result, raw_offset_ms, reference_sync_ms, target_sync_ms,
             ref = std::move(ref), target = std::move(target)]() mutable {
                stop_capture();
                last_result_ = result;
                measure_button_->setEnabled(true);
                progress_->setValue(100);

                has_recording_ = !ref.empty() && !target.empty();
                if (!has_recording_) {
                    alignment_slider_->setEnabled(false);
                    apply_button_->setEnabled(false);
                    set_status("No usable audio was captured from one or both sources.");
                    confidence_label_->clear();
                    return;
                }

                selected_alignment_ms_ = result.valid
                    ? std::clamp(result.offset_ms, static_cast<double>(SLIDER_MIN_MS), static_cast<double>(SLIDER_MAX_MS))
                    : 0.0;
                alignment_slider_->blockSignals(true);
                alignment_slider_->setValue(static_cast<int>(std::lround(selected_alignment_ms_)));
                alignment_slider_->blockSignals(false);
                alignment_slider_->setEnabled(true);
                // The waveform view represents the final OBS timing: the
                // configured target-reference sync offset is already part of
                // the displayed target shift, while the slider is the new
                // correction the user is choosing.
                waveform_->set_waveforms(ref, target, 48000,
                                         configured_difference_ms_ + selected_alignment_ms_);
                update_alignment_display();

                if (result.valid) {
                    const QString direction = result.offset_ms >= 0.0
                        ? "Target leads reference"
                        : "Target lags reference";
                    set_status(QString("Automatic alignment: %1 by %2 ms. Verify it visually, then apply or adjust the slider.")
                                   .arg(direction)
                                   .arg(std::abs(result.offset_ms), 0, 'f', 2));
                } else {
                    set_status("Automatic alignment was inconclusive. The waveforms were captured successfully; use the slider to align them manually.");
                }
                confidence_label_->setText(
                    QString("Confidence: %1%  •  %2 samples analyzed  •  Raw: %3 ms  •  Configured target-reference: %4 ms")
                        .arg(result.confidence * 100.0, 0, 'f', 0)
                        .arg(result.samples_used)
                        .arg(raw_offset_ms, 0, 'f', 2)
                        .arg(target_sync_ms - reference_sync_ms, 0, 'f', 2));
                apply_button_->setEnabled(true);
            }, Qt::QueuedConnection);
    });
}

void SyncDock::alignment_slider_changed(int value)
{
    selected_alignment_ms_ = static_cast<double>(value);
    update_alignment_display();
}

void SyncDock::update_alignment_display()
{
    alignment_label_->setText(
        QString("Alignment: %1 ms (%2 samples)")
            .arg(selected_alignment_ms_, 0, 'f', 2)
            .arg(selected_alignment_ms_ * 48.0, 0, 'f', 1));
    waveform_->set_alignment(configured_difference_ms_ + selected_alignment_ms_);
    apply_button_->setEnabled(has_recording_);
}

void SyncDock::apply_result()
{
    const QByteArray uuid = target_combo_->currentData().toString().toUtf8();
    obs_source_t *target = obs_get_source_by_uuid(uuid.constData());
    if (!target)
        return;

    const int64_t correction_ns =
        static_cast<int64_t>(selected_alignment_ms_ * 1000000.0);
    const int64_t current = obs_source_get_sync_offset(target);
    obs_source_set_sync_offset(target, current + correction_ns);
    obs_source_release(target);

    set_status(QString("Applied %1 ms correction to %2.")
                   .arg(selected_alignment_ms_, 0, 'f', 2)
                   .arg(target_name_));
    has_recording_ = false;
    apply_button_->setEnabled(false);
}

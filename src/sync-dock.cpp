#include "sync-dock.hpp"

#include <obs.h>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWheelEvent>
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
constexpr int ZOOM_SLIDER_MAX = 100;
constexpr double MIN_ZOOM = 1.0;
constexpr double MAX_ZOOM = 1000.0;
constexpr int VERTICAL_ZOOM_SLIDER_MAX = 100;
constexpr double MIN_VERTICAL_ZOOM = 1.0;
constexpr double MAX_VERTICAL_ZOOM = 20.0;

} // namespace

WaveformWidget::WaveformWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(300);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

double WaveformWidget::total_duration_ms() const
{
    const size_t count = std::max(reference_.size(), target_.size());
    if (count == 0 || sample_rate_ <= 0)
        return 1.0;
    return static_cast<double>(count) * 1000.0 / static_cast<double>(sample_rate_);
}

double WaveformWidget::visible_duration_ms() const
{
    return std::max(0.01, total_duration_ms() / zoom_factor_);
}

double WaveformWidget::clamp_center(double center_ms) const
{
    const double total = total_duration_ms();
    const double visible = visible_duration_ms();
    const double half_total = total / 2.0;
    const double half_visible = visible / 2.0;
    if (visible >= total)
        return 0.0;
    return std::clamp(center_ms, -half_total + half_visible,
                      half_total - half_visible);
}

double WaveformWidget::time_at_x(double x) const
{
    const double visible = visible_duration_ms();
    const double left = center_ms_ - visible / 2.0;
    if (width() <= 1)
        return center_ms_;
    return left + (x / static_cast<double>(width() - 1)) * visible;
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
    zoom_factor_ = 1.0;
    vertical_zoom_factor_ = 1.0;
    center_ms_ = 0.0;
    update();
}

void WaveformWidget::set_alignment(double alignment_ms)
{
    alignment_ms_ = alignment_ms;
    update();
}

void WaveformWidget::set_zoom(double zoom_factor)
{
    const double old_visible = visible_duration_ms();
    const double old_center = center_ms_;
    zoom_factor_ = std::clamp(zoom_factor, MIN_ZOOM, MAX_ZOOM);
    const double new_visible = visible_duration_ms();

    // Preserve the current center while zooming. The mouse-wheel handler
    // additionally adjusts the center so the point under the cursor stays put.
    Q_UNUSED(old_visible);
    Q_UNUSED(old_center);
    center_ms_ = clamp_center(center_ms_);
    update();
}

void WaveformWidget::set_vertical_zoom(double zoom_factor)
{
    vertical_zoom_factor_ = std::clamp(zoom_factor, MIN_VERTICAL_ZOOM, MAX_VERTICAL_ZOOM);
    update();
}

void WaveformWidget::fit_to_recording()
{
    zoom_factor_ = 1.0;
    vertical_zoom_factor_ = 1.0;
    center_ms_ = 0.0;
    update();
}

void WaveformWidget::set_center_ms(double center_ms)
{
    center_ms_ = clamp_center(center_ms);
    update();
}

void WaveformWidget::set_overlay(bool enabled)
{
    overlay_ = enabled;
    update();
}

void WaveformWidget::draw_waveform(QPainter &painter,
                                   const std::vector<float> &samples,
                                   int y_center, int height,
                                   double display_shift_ms,
                                   double view_start_ms, double view_end_ms,
                                   int sample_rate, int width)
{
    if (samples.empty() || width <= 0 || sample_rate <= 0 || view_end_ms <= view_start_ms)
        return;

    const double ms_per_sample = 1000.0 / static_cast<double>(sample_rate);
    const double view_duration = view_end_ms - view_start_ms;
    const double samples_per_pixel =
        view_duration * static_cast<double>(sample_rate) /
        (1000.0 * static_cast<double>(width));
    const double center_sample = static_cast<double>(samples.size() - 1) / 2.0;

    // Convert a timeline position to a raw sample position. A positive
    // display shift moves the target waveform to the right, representing a
    // delay applied to the target.
    const auto sample_at_time = [&](double timeline_ms) {
        const double raw_ms = timeline_ms - display_shift_ms;
        return center_sample + raw_ms / ms_per_sample;
    };

    QPainterPath path;
    path.reserve(width * 2);

    // Keep each waveform inside its own lane while allowing substantial
    // vertical magnification. This makes quiet details easier to inspect
    // without letting the reference and target traces overlap.
    painter.save();
    painter.setClipRect(QRectF(0.0,
                               static_cast<double>(y_center) - height / 2.0,
                               static_cast<double>(width),
                               static_cast<double>(height)),
                        Qt::IntersectClip);

    for (int x = 0; x < width; ++x) {
        const double t0 = view_start_ms +
            (static_cast<double>(x) / static_cast<double>(width)) * view_duration;
        const double t1 = view_start_ms +
            (static_cast<double>(x + 1) / static_cast<double>(width)) * view_duration;
        double a = sample_at_time(t0);
        double b = sample_at_time(t1);
        if (a > b)
            std::swap(a, b);

        const int first = std::max(0, static_cast<int>(std::floor(a)));
        const int last = std::min(static_cast<int>(samples.size()) - 1,
                                  static_cast<int>(std::ceil(b)));
        if (first > last)
            continue;

        float min_v = 1.0f;
        float max_v = -1.0f;
        for (int i = first; i <= last; ++i) {
            min_v = std::min(min_v, samples[static_cast<size_t>(i)]);
            max_v = std::max(max_v, samples[static_cast<size_t>(i)]);
        }

        const double amplitude = static_cast<double>(height) * 0.45 * vertical_zoom_factor_;
        const double y1 = y_center - static_cast<double>(max_v) * amplitude;
        const double y2 = y_center - static_cast<double>(min_v) * amplitude;
        const double xd = static_cast<double>(x);
        path.moveTo(xd, y1);
        path.lineTo(xd, y2);
    }

    painter.drawPath(path);
    painter.restore();
}

void WaveformWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(rect(), palette().base());

    const int w = width();
    const int top = 52;
    const int bottom = height() - 24;
    const int half = std::max(40, (bottom - top) / 2);
    const int ref_center = top + half / 2;
    const int target_center = top + half + half / 2;
    const int overlay_center = top + (bottom - top) / 2;

    painter.setPen(palette().mid().color());
    if (overlay_) {
        painter.drawLine(0, overlay_center, w, overlay_center);
    } else {
        painter.drawLine(0, ref_center, w, ref_center);
        painter.drawLine(0, target_center, w, target_center);
    }

    if (reference_.empty() || target_.empty()) {
        painter.setPen(palette().text().color());
        painter.drawText(8, height() / 2, "Record two sources to display their waveforms.");
        return;
    }

    const double visible = visible_duration_ms();
    const double view_start = center_ms_ - visible / 2.0;
    const double view_end = center_ms_ + visible / 2.0;

    // Time ruler. At high zoom this naturally reaches sub-millisecond detail.
    const double ideal_major = visible / 10.0;
    const double bases[] = {0.001, 0.002, 0.005, 0.01, 0.02, 0.05,
                            0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0,
                            50.0, 100.0, 200.0, 500.0, 1000.0};
    double major = bases[0];
    for (double candidate : bases) {
        if (candidate >= ideal_major) {
            major = candidate;
            break;
        }
        major = candidate;
    }

    painter.setPen(QPen(palette().mid(), 1));
    const double first_tick = std::floor(view_start / major) * major;
    for (double t = first_tick; t <= view_end + major; t += major) {
        if (t < view_start || t > view_end)
            continue;
        const double x = (t - view_start) / visible * w;
        painter.drawLine(QPointF(x, 30), QPointF(x, 45));
        const QString label = QString("%1 ms").arg(t, 0, 'f', major < 1.0 ? 3 : 1);
        painter.drawText(QPointF(x + 3, 25), label);
    }

    // Strong center/reference line.
    painter.setPen(QPen(palette().mid(), 1, Qt::DashLine));
    const double center_x = (0.0 - view_start) / visible * w;
    if (center_x >= 0.0 && center_x <= w)
        painter.drawLine(QPointF(center_x, top), QPointF(center_x, bottom));

    if (overlay_) {
        painter.setPen(palette().text().color());
        painter.drawText(8, 18, "Reference + Target (overlaid)");

        const int overlay_height = std::max(40, bottom - top);
        painter.setPen(QPen(palette().text().color(), 1));
        draw_waveform(painter, reference_, overlay_center, overlay_height,
                      0.0, view_start, view_end, sample_rate_, w);

        painter.setPen(QPen(palette().highlight().color(), 1));
        draw_waveform(painter, target_, overlay_center, overlay_height,
                      alignment_ms_, view_start, view_end, sample_rate_, w);
    } else {
        painter.setPen(palette().text().color());
        painter.drawText(8, 18, "Reference");
        painter.drawText(8, top + half + 18, "Target");

        painter.setPen(QPen(palette().text().color(), 1));
        draw_waveform(painter, reference_, ref_center, half,
                      0.0, view_start, view_end, sample_rate_, w);

        painter.setPen(QPen(palette().highlight().color(), 1));
        draw_waveform(painter, target_, target_center, half,
                      alignment_ms_, view_start, view_end, sample_rate_, w);
    }

    painter.setPen(QPen(palette().highlight().color(), 2));
    if (center_x >= 0.0 && center_x <= w)
        painter.drawLine(QPointF(center_x, top), QPointF(center_x, bottom));

    painter.setPen(palette().text().color());
    const QString zoom_text = QString("%1x time  •  %2x amplitude  •  %3 ms visible  •  %4  •  Wheel: time zoom  •  Shift+Wheel: amplitude zoom  •  Drag: pan")
        .arg(zoom_factor_, 0, 'f', zoom_factor_ < 10.0 ? 1 : 0)
        .arg(vertical_zoom_factor_, 0, 'f', vertical_zoom_factor_ < 10.0 ? 1 : 0)
        .arg(visible, 0, 'f', visible < 1.0 ? 3 : 1)
        .arg(overlay_ ? "Overlay: ON" : "Overlay: OFF");
    painter.drawText(8, height() - 7, zoom_text);
}

void WaveformWidget::wheelEvent(QWheelEvent *event)
{
    if (reference_.empty() || target_.empty()) {
        event->ignore();
        return;
    }

    const double wheel_factor = event->angleDelta().y() > 0 ? 1.35 : 1.0 / 1.35;

    // Shift+wheel controls vertical amplitude zoom. Normal wheel behavior
    // remains horizontal/time zoom so existing workflows are unchanged.
    if (event->modifiers() & Qt::ShiftModifier) {
        set_vertical_zoom(vertical_zoom_factor_ * wheel_factor);
        event->accept();
        return;
    }

    const QPointF pos = event->position();
    const double anchor_time = time_at_x(pos.x());
    const double new_zoom = std::clamp(zoom_factor_ * wheel_factor, MIN_ZOOM, MAX_ZOOM);

    if (std::abs(new_zoom - zoom_factor_) < 1e-9) {
        event->accept();
        return;
    }

    zoom_factor_ = new_zoom;
    const double visible = visible_duration_ms();
    const double normalized_x = width() > 1
        ? pos.x() / static_cast<double>(width() - 1)
        : 0.5;
    center_ms_ = anchor_time - (normalized_x - 0.5) * visible;
    center_ms_ = clamp_center(center_ms_);
    update();
    event->accept();
}

void WaveformWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !reference_.empty()) {
        dragging_ = true;
        last_mouse_pos_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void WaveformWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!dragging_) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const double visible = visible_duration_ms();
    const double delta_px = static_cast<double>(event->pos().x() - last_mouse_pos_.x());
    center_ms_ -= delta_px / std::max(1, width()) * visible;
    center_ms_ = clamp_center(center_ms_);
    last_mouse_pos_ = event->pos();
    update();
    event->accept();
}

void WaveformWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
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

    alignment_label_ = new QLabel("Alignment: 0.00 ms (0.0 samples)");
    alignment_label_->setAlignment(Qt::AlignCenter);
    wave_layout->addWidget(alignment_label_);

    alignment_slider_ = new QSlider(Qt::Horizontal);
    alignment_slider_->setRange(SLIDER_MIN_MS, SLIDER_MAX_MS);
    alignment_slider_->setValue(0);
    alignment_slider_->setSingleStep(1);
    alignment_slider_->setPageStep(10);
    alignment_slider_->setEnabled(false);
    alignment_slider_->setToolTip("Fine alignment in milliseconds. Use the waveform zoom for visual precision.");
    wave_layout->addWidget(alignment_slider_);

    auto *zoom_row = new QHBoxLayout();
    zoom_row->addWidget(new QLabel("Waveform zoom:"));
    zoom_slider_ = new QSlider(Qt::Horizontal);
    zoom_slider_->setRange(0, ZOOM_SLIDER_MAX);
    zoom_slider_->setValue(0);
    zoom_slider_->setEnabled(false);
    zoom_slider_->setToolTip("Zoom from the complete recording down to a few milliseconds.");
    zoom_row->addWidget(zoom_slider_, 1);
    zoom_label_ = new QLabel("1.0x");
    zoom_label_->setMinimumWidth(90);
    zoom_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    zoom_row->addWidget(zoom_label_);
    fit_button_ = new QPushButton("Fit to recording");
    fit_button_->setEnabled(false);
    zoom_row->addWidget(fit_button_);
    wave_layout->addLayout(zoom_row);

    auto *vertical_zoom_row = new QHBoxLayout();
    vertical_zoom_row->addWidget(new QLabel("Vertical waveform zoom:"));
    vertical_zoom_slider_ = new QSlider(Qt::Horizontal);
    vertical_zoom_slider_->setRange(0, VERTICAL_ZOOM_SLIDER_MAX);
    vertical_zoom_slider_->setValue(0);
    vertical_zoom_slider_->setEnabled(false);
    vertical_zoom_slider_->setToolTip("Magnify the waveform amplitude from 1x up to 20x.");
    vertical_zoom_row->addWidget(vertical_zoom_slider_, 1);
    vertical_zoom_label_ = new QLabel("1.0x");
    vertical_zoom_label_->setMinimumWidth(90);
    vertical_zoom_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    vertical_zoom_row->addWidget(vertical_zoom_label_);
    wave_layout->addLayout(vertical_zoom_row);

    auto *overlay_row = new QHBoxLayout();
    overlay_checkbox_ = new QCheckBox("Overlay reference and target waveforms");
    overlay_checkbox_->setEnabled(false);
    overlay_checkbox_->setToolTip("Draw both waveforms in the same lane so matching features can be compared directly.");
    overlay_row->addWidget(overlay_checkbox_);
    overlay_row->addStretch();
    wave_layout->addLayout(overlay_row);

    auto *hint = new QLabel(
        "Positive values delay the target. Use the mouse wheel over the waveform to zoom in/out, "
        "and drag left/right to pan. Shift+mouse-wheel or the vertical slider magnifies waveform amplitude up to 20x. "
        "Turn on overlay mode to draw both sources in the same lane for direct visual comparison. "
        "At maximum time zoom only a few milliseconds of audio are visible. Line up matching waveform features, then apply the correction.");
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
    connect(zoom_slider_, &QSlider::valueChanged, this,
            &SyncDock::zoom_slider_changed);
    connect(vertical_zoom_slider_, &QSlider::valueChanged, this,
            &SyncDock::vertical_zoom_slider_changed);
    connect(fit_button_, &QPushButton::clicked, this,
            &SyncDock::fit_waveform);
    connect(overlay_checkbox_, &QCheckBox::toggled, this,
            &SyncDock::overlay_toggled);

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
    zoom_slider_->setEnabled(false);
    vertical_zoom_slider_->setEnabled(false);
    fit_button_->setEnabled(false);
    overlay_checkbox_->setEnabled(false);
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
                    zoom_slider_->setEnabled(false);
                    vertical_zoom_slider_->setEnabled(false);
                    fit_button_->setEnabled(false);
                    overlay_checkbox_->setEnabled(false);
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
                zoom_slider_->setEnabled(true);
                vertical_zoom_slider_->setEnabled(true);
                fit_button_->setEnabled(true);
                overlay_checkbox_->setEnabled(true);

                waveform_->set_waveforms(ref, target, 48000,
                                         configured_difference_ms_ + selected_alignment_ms_);
                zoom_slider_->blockSignals(true);
                zoom_slider_->setValue(0);
                zoom_slider_->blockSignals(false);
                zoom_label_->setText("1.0x");
                vertical_zoom_slider_->blockSignals(true);
                vertical_zoom_slider_->setValue(0);
                vertical_zoom_slider_->blockSignals(false);
                vertical_zoom_label_->setText("1.0x");
                waveform_->set_vertical_zoom(1.0);
                update_alignment_display();

                if (result.valid) {
                    const QString direction = result.offset_ms >= 0.0
                        ? "Target leads reference"
                        : "Target lags reference";
                    set_status(QString("Automatic alignment: %1 by %2 ms. Verify it visually, then apply or adjust the slider.")
                                   .arg(direction)
                                   .arg(std::abs(result.offset_ms), 0, 'f', 2));
                } else {
                    set_status("Automatic alignment was inconclusive. The waveforms were captured successfully; use zoom and the slider to align them manually.");
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

double SyncDock::zoom_from_slider(int value)
{
    const double normalized = std::clamp(value, 0, ZOOM_SLIDER_MAX) /
                              static_cast<double>(ZOOM_SLIDER_MAX);
    return std::pow(MAX_ZOOM / MIN_ZOOM, normalized) * MIN_ZOOM;
}

int SyncDock::slider_from_zoom(double zoom)
{
    const double normalized = std::log(std::clamp(zoom, MIN_ZOOM, MAX_ZOOM) / MIN_ZOOM) /
                              std::log(MAX_ZOOM / MIN_ZOOM);
    return static_cast<int>(std::lround(normalized * ZOOM_SLIDER_MAX));
}

QString SyncDock::zoom_text(double zoom)
{
    if (zoom < 10.0)
        return QString("%1x").arg(zoom, 0, 'f', 1);
    return QString("%1x").arg(zoom, 0, 'f', 0);
}

double SyncDock::vertical_zoom_from_slider(int value)
{
    const double normalized = std::clamp(value, 0, VERTICAL_ZOOM_SLIDER_MAX) /
                              static_cast<double>(VERTICAL_ZOOM_SLIDER_MAX);
    return std::pow(MAX_VERTICAL_ZOOM / MIN_VERTICAL_ZOOM, normalized) * MIN_VERTICAL_ZOOM;
}

int SyncDock::slider_from_vertical_zoom(double zoom)
{
    const double normalized = std::log(std::clamp(zoom, MIN_VERTICAL_ZOOM, MAX_VERTICAL_ZOOM) / MIN_VERTICAL_ZOOM) /
                              std::log(MAX_VERTICAL_ZOOM / MIN_VERTICAL_ZOOM);
    return static_cast<int>(std::lround(normalized * VERTICAL_ZOOM_SLIDER_MAX));
}

QString SyncDock::vertical_zoom_text(double zoom)
{
    if (zoom < 10.0)
        return QString("%1x").arg(zoom, 0, 'f', 1);
    return QString("%1x").arg(zoom, 0, 'f', 0);
}

void SyncDock::vertical_zoom_slider_changed(int value)
{
    const double zoom = vertical_zoom_from_slider(value);
    waveform_->set_vertical_zoom(zoom);
    vertical_zoom_label_->setText(vertical_zoom_text(zoom));
}

void SyncDock::zoom_slider_changed(int value)
{
    const double zoom = zoom_from_slider(value);
    waveform_->set_zoom(zoom);
    zoom_label_->setText(zoom_text(zoom));
}

void SyncDock::fit_waveform()
{
    waveform_->fit_to_recording();
    zoom_slider_->blockSignals(true);
    zoom_slider_->setValue(slider_from_zoom(1.0));
    zoom_slider_->blockSignals(false);
    zoom_label_->setText("1.0x");
    vertical_zoom_slider_->blockSignals(true);
    vertical_zoom_slider_->setValue(slider_from_vertical_zoom(1.0));
    vertical_zoom_slider_->blockSignals(false);
    vertical_zoom_label_->setText("1.0x");
}

void SyncDock::overlay_toggled(bool checked)
{
    waveform_->set_overlay(checked);
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

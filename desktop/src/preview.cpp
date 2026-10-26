#include "tcp_socket.h"
#include "preview.h"
#include "application.h"
#include "virtual_camera.h"
#include <QSignalBlocker>

#include <QScreen>
#include <QWheelEvent>
#include <QAbstractSpinBox>
#include <QTabWidget>
#include <QScrollBar>

#include <QFontDatabase>

#include <QApplication>
#include <QCloseEvent>
#include <QImage>
#include <QIcon>
#include <QPixmap>

#include <QImageWriter>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QTimer>

#include <QWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QAbstractItemView>
#include <QStyledItemDelegate>

#include <QStandardItemModel>
#include <QDateTime>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>

#include <QHBoxLayout>
#include <QLabel>
#include <QSysInfo>
#ifdef CAMDROID_GUI_TESTS
#include <QTcpServer>

#include <QTcpSocket>
#endif
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>

#include <QSpinBox>
#include <QDoubleSpinBox>

#include <QScrollArea>
#include <QTransform>
#include <QVBoxLayout>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/dict.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <iostream>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

#include <cmath>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>

#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace camdroid {

    namespace utils {

        void apply_theme(QApplication& qt) {

            QFont font("Segoe UI", 10);
            qt.setFont(font);
            QPalette palette;
            palette.setColor(QPalette::Window, QColor("#171b22"));
            palette.setColor(QPalette::WindowText, QColor("#edf1f7"));

            palette.setColor(QPalette::Base, QColor("#101318"));
            palette.setColor(QPalette::Text, QColor("#edf1f7"));
            palette.setColor(QPalette::Button, QColor("#262d38"));
            palette.setColor(QPalette::ButtonText, QColor("#edf1f7"));
            palette.setColor(QPalette::Highlight, QColor("#a4c88a"));

            palette.setColor(QPalette::HighlightedText, QColor("#101318"));
            palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#7c8798"));
            palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#7c8798"));
            qt.setPalette(palette);

            qt.setStyleSheet(
                "QWidget { font-family: 'Segoe UI'; font-size: 13px; }"
                "QLabel#brand { font-size: 24px; font-weight: 600; } QLabel#muted { color: #9aa6b6; } QLabel#telemetry { "
                "color: #c5cfdb; }"
                "QLabel#receiver_status { color: #bdd5aa; padding: 8px 12px; background: #232e25; border-radius: 6px; }"

                "QGroupBox { border: 1px solid #323a46; border-radius: 8px; margin-top: 12px; padding: 16px 10px 10px; "
                "font-weight: 600; }"
                "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; color: #d6dfeb; }"
                "QPushButton { background: #29313d; border: 1px solid #3a4554; border-radius: 6px; padding: 7px 12px; "
                "min-height: 20px; }"
                "QPushButton:hover { background: #354151; } QPushButton:pressed { background: #202731; }"

                "QPushButton:disabled { color: #778392; background: #20262f; border-color: #2b333e; }"
                "QPushButton#start_receiver { background: #a4c88a; color: #121910; border-color: #a4c88a; font-weight: 600; }"
                "QPushButton#start_receiver:disabled { background: #26312a; color: #7d9081; border-color: #354339; }"
                "QComboBox, QSpinBox, QDoubleSpinBox { background: #10151c; border: 1px solid #3a4554; border-radius: 5px; "
                "padding: 6px; min-height: 22px; }"
                "QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QPushButton:focus { border-color: #a4c88a; }"

                "QComboBox QAbstractItemView { background: #171b22; color: #edf1f7; border: 1px solid #3a4554; outline: 0; "
                "selection-background-color: #354151; selection-color: #edf1f7; }"
                "QComboBox QAbstractItemView::item { min-height: 26px; padding: 4px 8px; }"
                "QComboBox QAbstractItemView::item:hover, QComboBox QAbstractItemView::item:selected { background: #354151; "
                "color: #edf1f7; }"

                "QTabWidget::pane { border: 0; } QTabBar::tab { padding: 9px 12px; color: #9aa6b6; border-bottom: 2px solid "
                "#303844; }"
                "QTabBar::tab:selected { color: #edf1f7; border-bottom-color: #a4c88a; }"
                "QCheckBox { spacing: 8px; padding: 4px 0; }"
                "QPlainTextEdit { border: 1px solid #303844; border-radius: 6px; padding: 8px; }"
                "QScrollBar:vertical { background: #171b22; width: 9px; margin: 0; }"

                "QScrollBar::handle:vertical { background: #3a4554; border-radius: 4px; min-height: 28px; }"
                "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
            );

        }

        class settings_option_delegate : public QStyledItemDelegate {

            public:
            explicit settings_option_delegate(QObject* parent) : QStyledItemDelegate(parent) {}

            void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {

                if (index.flags() & Qt::ItemIsEnabled) {

                    QStyledItemDelegate::paint(painter, option, index);
                    return;

                }

                // Draw disabled entries explicitly. The popup stylesheet overrides the disabled palette.
                painter->save();
                painter->fillRect(option.rect, QColor("#171b22"));
                painter->setFont(option.font);
                painter->setPen(QColor("#747d8b"));

                const auto text =
                    option.fontMetrics.elidedText(index.data().toString(), Qt::ElideRight, option.rect.width() - 16);
                painter->drawText(option.rect.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
                painter->restore();

            }

        };

        class settings_wheel_filter : public QObject {

            public:
            explicit settings_wheel_filter(QObject* parent) : QObject(parent) {}

            protected:
            bool eventFilter(QObject* watched, QEvent* event) override {

                if (event->type() != QEvent::Wheel)
                    return false;

                auto* widget = qobject_cast<QWidget*>(watched);

                if (!widget || (!qobject_cast<QComboBox*>(widget) && !qobject_cast<QAbstractSpinBox*>(widget)))
                    return false;

                auto* wheel = static_cast<QWheelEvent*>(event);

                for (auto* parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {

                    if (auto* scroll = qobject_cast<QScrollArea*>(parent)) {

                        QWheelEvent forwarded(
                            scroll->viewport()->mapFromGlobal(wheel->globalPosition().toPoint()),
                            wheel->globalPosition(),
                            wheel->pixelDelta(),
                            wheel->angleDelta(),

                            wheel->buttons(),
                            wheel->modifiers(),
                            wheel->phase(),
                            wheel->inverted()
                        );

                        QApplication::sendEvent(scroll->viewport(), &forwarded);
                        break;

                    }

                }

                wheel->accept();

                return true;

            }

        };

        class frame_converter {

            private:
            SwsContext* context = nullptr;

            public:
            frame_converter() = default;

            frame_converter(const frame_converter&) = delete;
            frame_converter& operator=(const frame_converter&) = delete;

            ~frame_converter() {
                sws_freeContext(context);
            }

            QImage convert(const AVFrame& frame, int bound_width = 0, int bound_height = 0) {

                if (frame.width <= 0 || frame.height <= 0)
                    throw std::runtime_error("Preview failure: Invalid frame dimensions.");

                const auto source_format = static_cast<AVPixelFormat>(frame.format);
                const double ratio = bound_width > 0 && bound_height > 0
                                         ? std::min(
                                               {1.0,
                                                static_cast<double>(bound_width) / frame.width,

                                                static_cast<double>(bound_height) / frame.height}
                                           )
                                         : 1.0;
                const int width = std::max(1, static_cast<int>(frame.width * ratio));
                const int height = std::max(1, static_cast<int>(frame.height * ratio));

                context = sws_getCachedContext(
                    context,
                    frame.width,
                    frame.height,
                    source_format,

                    width,
                    height,
                    AV_PIX_FMT_RGBA,
                    SWS_BILINEAR,
                    nullptr,

                    nullptr,
                    nullptr
                );

                if (context == nullptr)
                    throw std::runtime_error("Preview failure: Cannot create image converter.");

                int colorspace;

                switch (frame.colorspace) {

                    case AVCOL_SPC_BT709:
                        colorspace = SWS_CS_ITU709;
                        break;

                    case AVCOL_SPC_FCC:
                        colorspace = SWS_CS_FCC;
                        break;

                    case AVCOL_SPC_BT470BG:
                    case AVCOL_SPC_SMPTE170M:
                        colorspace = SWS_CS_ITU601;
                        break;

                    case AVCOL_SPC_SMPTE240M:
                        colorspace = SWS_CS_SMPTE240M;
                        break;

                    case AVCOL_SPC_BT2020_NCL:
                    case AVCOL_SPC_BT2020_CL:
                        colorspace = SWS_CS_BT2020;
                        break;

                    default:
                        colorspace = frame.height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
                        break;

                }

                const int* coefficients = sws_getCoefficients(colorspace);

                const int source_full_range = frame.color_range == AVCOL_RANGE_JPEG ? 1 : 0;

                if (sws_setColorspaceDetails(context, coefficients, source_full_range, coefficients, 1, 0, 1 << 16, 1 << 16) <
                    0) {

                    throw std::runtime_error("Preview failure: Cannot configure image colorspace.");

                }

                QImage image(width, height, QImage::Format_RGBA8888);

                if (image.isNull())
                    throw std::runtime_error("Preview failure: Image allocation failed.");

                std::uint8_t* destination[4]{image.bits(), nullptr, nullptr, nullptr};

                int destination_stride[4]{static_cast<int>(image.bytesPerLine()), 0, 0, 0};

                const int rows =
                    sws_scale(context, frame.data, frame.linesize, 0, frame.height, destination, destination_stride);

                if (rows != height)
                    throw std::runtime_error("Preview failure: Incomplete image conversion.");

                return image;

            }

        };

    };

    struct preview_state {

        std::mutex mutex;
        QImage pending_image;
        std::shared_ptr<AVFrame> snapshot_frame;
        std::chrono::steady_clock::time_point pending_submitted{};
        double queue_ms = 0, decode_ms = 0, conversion_ms = 0, webcam_ms = 0;

        std::atomic<int> preview_width{1280}, preview_height{720}, preview_period_us{16667};
        std::atomic<bool> preview_enabled{true};

        QString status = QStringLiteral("Starting receiver");
        QString error;
        QStringList endpoints;

        std::deque<QString> events;
        std::uint64_t frame_count = 0;
        std::uint64_t received_bytes = 0;
        std::uint64_t encoded_frames = 0;
        std::uint64_t video_bytes = 0;

        protocol::video_settings settings;
        std::optional<protocol::camera_capabilities> capabilities;
        std::optional<protocol::capture_selection> selection;
        std::uint64_t capture_revision = 1;
        decoder_selection decoder;

        std::uint64_t decoder_revision = 1;
        QSize capture_size;
        bool capture_pending = false;

        QString capture_notice;
        bool connected = false;

        bool reset_image = false;
        bool fatal = false;
        std::uint16_t port = 0;
        std::atomic<bool> finished = false;
        std::atomic<bool> mirror = false;
        std::atomic<int> rotation = -1;

    };

    namespace utils {

        static QImage orient_image(QImage image, const AVFrame& frame, int override, bool mirrored) {

            const auto* orientation = av_dict_get(frame.metadata, "camdroid.rotation", nullptr, 0);
            const auto* mirror = av_dict_get(frame.metadata, "camdroid.mirrored", nullptr, 0);
            const int rotation =
                override >= 0 ? override : (orientation ? QString::fromLatin1(orientation->value).toInt() : 0);

            if (rotation != 0)
                image = image.transformed(QTransform().rotate(rotation));

            if ((mirror && mirror->value[0] == '1') != mirrored)
                image = image.flipped(Qt::Horizontal);

            return image;

        }

        static void record(preview_state& state, const QString& text) {
            state.events.push_back(QDateTime::currentDateTime().toString("HH:mm:ss  ") + text);

            if (state.events.size() > 200)
                state.events.pop_front();
        }

    };

    namespace utils {

        class frame_output {

            private:
            std::shared_ptr<preview_state> shared;
            std::shared_ptr<virtual_camera> camera;
            const bool webcam_output;
            std::mutex mutex;
            std::condition_variable changed;

            std::shared_ptr<AVFrame> pending;
            std::chrono::steady_clock::time_point submitted{};
            std::uint64_t generation = 0;
            std::jthread worker;

            void run(std::stop_token stop) {

                frame_converter converter;
                auto next_preview = std::chrono::steady_clock::now();

                while (!stop.stop_requested()) {

                    std::shared_ptr<AVFrame> frame;
                    std::chrono::steady_clock::time_point queued;
                    std::uint64_t epoch;

                    {

                        std::unique_lock lock(mutex);

                        changed.wait(lock, [&] {
                            return stop.stop_requested() || pending;
                        });

                        if (stop.stop_requested())
                            return;

                        if (!webcam_output)
                            changed.wait_until(lock, next_preview, [&] {
                                return stop.stop_requested();
                            });

                        if (stop.stop_requested())
                            return;

                        frame = std::move(pending);
                        queued = submitted;
                        epoch = generation;

                    }

                    if (!frame || (webcam_output && !camera->active()) ||
                        std::chrono::steady_clock::now() - queued > std::chrono::milliseconds(75))
                        continue;

                    try {

                        const auto start = std::chrono::steady_clock::now();
                        const int rotation_override = shared->rotation;
                        const bool mirrored = shared->mirror;
                        QImage image;

                        if (!webcam_output && shared->preview_enabled && start >= next_preview) {

                            int width = std::min(1920, shared->preview_width.load());
                            int height = std::min(1080, shared->preview_height.load());
                            const auto* orientation = av_dict_get(frame->metadata, "camdroid.rotation", nullptr, 0);
                            const int rotation = rotation_override >= 0 ? rotation_override
                                                 : orientation          ? std::atoi(orientation->value)
                                                                        : 0;

                            if (rotation == 90 || rotation == 270)
                                std::swap(width, height);

                            image = converter.convert(*frame, width, height);
                            image = orient_image(std::move(image), *frame, rotation_override, mirrored);
                            next_preview = start + std::chrono::microseconds(shared->preview_period_us.load());

                        }

                        const auto converted = std::chrono::steady_clock::now();
                        std::lock_guard lock(mutex);

                        if (epoch != generation || stop.stop_requested())
                            continue;

                        if (webcam_output)
                            camera->publish_frame(*frame, rotation_override, mirrored);

                        const auto published = std::chrono::steady_clock::now();
                        std::lock_guard state_lock(shared->mutex);

                        if (webcam_output) {

                            shared->webcam_ms = std::chrono::duration<double, std::milli>(published - converted).count();

                        } else {

                            shared->snapshot_frame = frame;
                            shared->conversion_ms = std::chrono::duration<double, std::milli>(converted - start).count();

                        }

                        if (!image.isNull()) {

                            shared->pending_image = std::move(image);
                            shared->pending_submitted = queued;

                        }

                    } catch (const std::exception& error) {

                        std::lock_guard lock(shared->mutex);
                        shared->error = QString::fromUtf8(error.what());

                    }

                }

            }

            public:
            frame_output(std::shared_ptr<preview_state> state, std::shared_ptr<virtual_camera> webcam, bool output)
                : shared(std::move(state)), camera(std::move(webcam)), webcam_output(output),
                  worker([this](std::stop_token stop) {
                      run(stop);
                  }) {}

            ~frame_output() {
                worker.request_stop();
                changed.notify_all();
                worker.join();
            }

            void submit(const AVFrame& frame) {

                std::unique_lock lock(mutex, std::try_to_lock);

                if (!lock.owns_lock())
                    return;

                auto* copy = av_frame_clone(&frame);

                if (!copy)
                    throw std::bad_alloc();

                pending = std::shared_ptr<AVFrame>(copy, [](AVFrame* value) {
                    av_frame_free(&value);
                });

                submitted = std::chrono::steady_clock::now();
                changed.notify_all();

            }

            void clear() {

                std::lock_guard lock(mutex);
                ++generation;
                pending.reset();
                if (webcam_output)
                    camera->clear();

            }

        };

        class frame_presenter {

            private:
            frame_output preview;
            frame_output webcam;

            public:
            frame_presenter(std::shared_ptr<preview_state> state, std::shared_ptr<virtual_camera> camera)
                : preview(state, camera, false), webcam(std::move(state), std::move(camera), true) {}

            void submit(const AVFrame& frame) {
                preview.submit(frame);
                webcam.submit(frame);
            }

            void clear() {
                preview.clear();
                webcam.clear();
            }

        };

    }

    class video_canvas : public QWidget {

        private:
        QImage image;
        QString status = QStringLiteral("Waiting for video");
        std::chrono::steady_clock::time_point submitted{};
        std::uint64_t presented = 0;

        bool new_frame = false;
        double painting_ms = 0, waiting_ms = 0;

        protected:
        void paintEvent(QPaintEvent*) override {

            const auto start = std::chrono::steady_clock::now();
            QPainter painter(this);
            painter.fillRect(rect(), QColor("#101318"));

            if (image.isNull()) {

                painter.setPen(QColor("#a7b0be"));
                auto title_font = font();
                title_font.setPointSize(16);
                title_font.setBold(true);
                painter.setFont(title_font);

                painter.drawText(rect().adjusted(24, 24, -24, -48), Qt::AlignCenter | Qt::TextWordWrap, status);
                title_font.setPointSize(10);
                title_font.setBold(false);
                painter.setFont(title_font);
                painter.setPen(QColor("#778392"));

                painter.drawText(
                    rect().adjusted(24, 72, -24, -24),
                    Qt::AlignCenter | Qt::TextWordWrap,
                    "Open CamDroid on your phone and choose this PC."
                );

                return;

            }

            const auto fitted = image.size().scaled(size(), Qt::KeepAspectRatio);
            const QRect destination(
                (width() - fitted.width()) / 2,
                (height() - fitted.height()) / 2,
                fitted.width(),

                fitted.height()
            );
            painter.drawImage(destination, image);
            const auto finished = std::chrono::steady_clock::now();
            painting_ms = std::chrono::duration<double, std::milli>(finished - start).count();

            if (new_frame) {

                ++presented;
                new_frame = false;
                waiting_ms = submitted == std::chrono::steady_clock::time_point{}
                                 ? 0
                                 : std::chrono::duration<double, std::milli>(finished - submitted).count();

            }

        }

        public:
        explicit video_canvas(QWidget* parent = nullptr) : QWidget(parent) {
            setMinimumSize(320, 180);
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            setAccessibleName("Camera video preview");
        }

        void display(QImage next, const QString& message, bool reset, std::chrono::steady_clock::time_point queued = {}) {

            if (reset)
                image = {};

            if (!next.isNull()) {

                image = std::move(next);
                submitted = queued;
                new_frame = true;

            }

            status = message;
            update();

        }

        std::uint64_t presented_frames() const {
            return presented;
        }

        double paint_ms() const {
            return painting_ms;
        }

        double presentation_ms() const {
            return waiting_ms;
        }

        const QImage& current_image() const {
            return image;
        }

    };

    class desktop_window : public QWidget {

        private:
        app_config configuration;
        bool closing = false;
        std::shared_ptr<preview_state> state;
        std::shared_ptr<application> core;

        std::optional<std::jthread> worker;
        QTimer timer;
        std::shared_ptr<virtual_camera> webcam = std::make_shared<virtual_camera>();
        QLabel* configured_label = nullptr;
        QLabel* webcam_label = nullptr;

        QPushButton* flip_camera = nullptr;
        QComboBox* capture_resolution = nullptr;
        QComboBox* capture_fps = nullptr;
        QComboBox* bitrate_preset = nullptr;
        QDoubleSpinBox* capture_bitrate = nullptr;

        QTimer capture_timer;
        bool capture_dirty = false;
        std::optional<protocol::camera_capabilities> capture_capabilities;
        std::optional<protocol::capture_selection> selected_capture;
        std::uint64_t displayed_capture_revision = 0;

        QLabel* capture_label = nullptr;
        QCheckBox* allow_app_outputs = nullptr;
        QPushButton* webcam_button = nullptr;
        video_canvas* canvas;
        QLabel* status_label = nullptr;

        QLabel* metrics_label = nullptr;
        QLabel* endpoint_label = nullptr;
        QSpinBox* port_input = nullptr;
        QPushButton* listen_button = nullptr;
        QPushButton* stop_button = nullptr;

        QPushButton* disconnect_button = nullptr;
        QPlainTextEdit* event_log = nullptr;
        QCheckBox* mirror_input = nullptr;
        QComboBox* rotation_input = nullptr;
        QComboBox* decoder_input = nullptr;

        QLabel* decoder_label = nullptr;
        QPushButton* identify_decoder = nullptr;
        QPushButton* automatic_decoder = nullptr;
        QLabel* automatic_led = nullptr;
        std::string decoder_preference = "auto";

        std::uint64_t displayed_decoder_revision = 0;
        std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();

        std::uint64_t previous_presented = 0;
        std::uint64_t previous_frames = 0;
        std::uint64_t previous_bytes = 0;
        std::uint64_t previous_encoded = 0;
        std::uint64_t previous_video_bytes = 0;

        void populate_capture_options(bool synchronize) {

            const QSignalBlocker resolution_blocker(capture_resolution), fps_blocker(capture_fps),
                bitrate_blocker(bitrate_preset);
            auto* resolutions = qobject_cast<QStandardItemModel*>(capture_resolution->model());

            for (int i = 0; i < capture_resolution->count(); ++i) {

                const auto size = capture_resolution->itemData(i).toSize();
                const bool supported = capture_capabilities && std::any_of(
                                                                   capture_capabilities->resolutions.begin(),
                                                                   capture_capabilities->resolutions.end(),

                                                                   [&](const auto& entry) {
                                                                       return entry.format.width == size.width() &&
                                                                              entry.format.height == size.height();
                                                                   }
                                                               );

                if (resolutions && resolutions->item(i))
                    resolutions->item(i)->setEnabled(supported);

            }

            if (synchronize && capture_capabilities) {

                const QSize size(capture_capabilities->width, capture_capabilities->height);
                const int index = capture_resolution->findData(size);
                if (index >= 0)
                    capture_resolution->setCurrentIndex(index);

            }

            const auto size = capture_resolution->currentData().toSize();
            const protocol::resolution_capabilities* supported = nullptr;
            if (capture_capabilities)
                for (const auto& entry : capture_capabilities->resolutions)
                    if (entry.format.width == size.width() && entry.format.height == size.height())
                        supported = &entry;

            int desired = capture_fps->currentData().toInt();
            if (synchronize && selected_capture)
                desired = selected_capture->fps;

            if (!desired)
                desired = 30;

            std::vector<int> rates{15, 24, 25, 30, 48, 50, 60, 90, 120, 144, 165, 180, 200, 240};
            if (supported)
                rates.insert(rates.end(), supported->rates.begin(), supported->rates.end());

            std::sort(rates.begin(), rates.end());
            rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
            capture_fps->clear();
            auto* fps_model = qobject_cast<QStandardItemModel*>(capture_fps->model());
            int nearest = -1, distance = 1000;

            for (const int rate : rates) {

                capture_fps->addItem(QString::number(rate) + " FPS", rate);
                const int index = capture_fps->count() - 1;
                const bool available =
                    supported && std::find(supported->rates.begin(), supported->rates.end(), rate) != supported->rates.end();
                if (fps_model && fps_model->item(index))
                    fps_model->item(index)->setEnabled(available);

                if (available && std::abs(rate - desired) < distance) {

                    nearest = index;
                    distance = std::abs(rate - desired);

                }

            }

            capture_fps->setCurrentIndex(nearest);
            auto* bitrate_model = qobject_cast<QStandardItemModel*>(bitrate_preset->model());

            for (int i = 0; i < bitrate_preset->count(); ++i) {

                const auto bits = bitrate_preset->itemData(i).toUInt();
                const bool available = supported && (bits ? bits >= supported->min_bitrate && bits <= supported->max_bitrate
                                                          : std::max(2000000u, supported->min_bitrate) <=
                                                                std::min(40000000u, supported->max_bitrate));
                if (bitrate_model && bitrate_model->item(i))
                    bitrate_model->item(i)->setEnabled(available);

            }

            if (synchronize && selected_capture) {

                const auto bits = selected_capture->bitrate;
                int index = bitrate_preset->findData(bits);

                if (index < 0) {

                    bitrate_preset->addItem(QString("Custom | %1 Mbps").arg(bits / 1000000.0, 0, 'f', 1), bits);
                    index = bitrate_preset->count() - 1;

                }

                bitrate_preset->setCurrentIndex(index);

            }

            const int current = bitrate_preset->currentIndex();
            if (bitrate_model && current >= 0 && !bitrate_model->item(current)->isEnabled())
                for (int i = 0; i < bitrate_preset->count(); ++i)
                    if (bitrate_model->item(i)->isEnabled()) {

                        bitrate_preset->setCurrentIndex(i);
                        break;

                    }

        }

        void start_receiver() {

            if (worker && !state->finished)
                return;

            if (worker) {

                worker->join();
                worker.reset();

            }

            core.reset();
            state = std::make_shared<preview_state>();
            state->decoder.message = "Available decoders are checked when streaming starts.";
            displayed_decoder_revision = 0;
            displayed_capture_revision = 0;

            capture_capabilities.reset();
            selected_capture.reset();
            state->mirror = mirror_input && mirror_input->isChecked();

            state->rotation = rotation_input ? rotation_input->currentData().toInt() : -1;
            previous_frames = previous_bytes = previous_encoded = previous_video_bytes = 0;
            previous_presented = canvas ? canvas->presented_frames() : 0;
            last_report = std::chrono::steady_clock::now();

            if (event_log)
                event_log->clear();

            canvas->display({}, "Waiting for the Android app", true);

            if (metrics_label)
                metrics_label->setText("No video received");

            const auto shared = state;
            const auto camera = webcam;
            const auto presenter = std::make_shared<utils::frame_presenter>(shared, camera);
            application_callbacks callbacks;

            callbacks.on_video_timing = [shared](double queue_ms, double decode_ms) {
                std::lock_guard lock(shared->mutex);
                shared->queue_ms = queue_ms;
                shared->decode_ms = decode_ms;
            };

            callbacks.on_negotiation = [shared](const std::string& message) {
                std::lock_guard lock(shared->mutex);
                utils::record(*shared, QString::fromStdString(message));
            };

            callbacks.on_decoder_selection = [shared](const decoder_selection& selection) {

                std::lock_guard lock(shared->mutex);
                shared->decoder = selection;
                ++shared->decoder_revision;

                if (!selection.message.empty())
                    utils::record(*shared, QString::fromStdString(selection.message));

            };

            callbacks.on_camera_capabilities = [shared](const protocol::camera_capabilities& capabilities) {
                std::lock_guard lock(shared->mutex);
                shared->capabilities = capabilities;
                ++shared->capture_revision;
            };

            callbacks.on_capture_selection = [shared](protocol::capture_selection selected) {
                std::lock_guard lock(shared->mutex);
                shared->selection = selected;
                ++shared->capture_revision;
            };

            callbacks.on_video_request_result = [shared](protocol::video_control result) {

                std::lock_guard lock(shared->mutex);
                shared->capture_pending = false;
                if (result.status != 0)
                    ++shared->capture_revision;

                shared->capture_notice = result.status == 0   ? "Capture settings applied."
                                         : result.status == 1 ? "Unsupported settings; previous capture retained."
                                         : result.status == 2 ? "Phone is busy; try again after capture starts."
                                                              : "Settings could not be applied; previous capture retained.";

            };

            callbacks.on_listening = [shared](std::uint16_t port) {

                std::lock_guard lock(shared->mutex);
                shared->port = port;
                shared->endpoints.clear();
                std::cout << "Device: " << utils::local_device_name() << '\n';

                for (const auto& ip : utils::local_ipv4_addresses()) {

                    const auto endpoint = QString::fromStdString(ip) + ':' + QString::number(port);
                    shared->endpoints.append(endpoint);
                    std::cout << "Listening: " << endpoint.toStdString() << std::endl;

                }

                shared->connected = false;
                shared->status = QString("Listening on port %1").arg(port);
                utils::record(*shared, shared->status);

            };

            callbacks.on_connected = [shared] {

                std::lock_guard lock(shared->mutex);
                shared->connected = true;
                shared->reset_image = true;
                shared->pending_image = {};
                shared->status = "Phone connected; completing handshake";
                utils::record(*shared, shared->status);

            };

            callbacks.on_established = [shared] {
                std::lock_guard lock(shared->mutex);
                shared->status = "Connected; start streaming on your phone";
                utils::record(*shared, "Protocol handshake complete");
            };

            callbacks.on_received = [shared](std::size_t bytes) {
                std::lock_guard lock(shared->mutex);
                shared->received_bytes += bytes;
            };

            callbacks.on_video_settings = [shared, camera](protocol::video_settings settings) {
                camera->set_settings(settings);
                std::lock_guard lock(shared->mutex);
                shared->settings = settings;
            };

            callbacks.on_encoded_frame = [shared](std::size_t bytes) {
                std::lock_guard lock(shared->mutex);
                shared->encoded_frames++;
                shared->video_bytes += bytes;
            };

            callbacks.on_video_config = [shared, presenter](const protocol::video_config& config) {

                presenter->clear();

                std::lock_guard lock(shared->mutex);
                shared->status = "Video configured";
                utils::record(
                    *shared,
                    QString("H.264 configured; rotation %1 degrees; %2")

                        .arg(config.rotation)
                        .arg(config.mirrored ? "mirrored" : "unmirrored")
                );

            };

            callbacks.on_video_ended = [shared, presenter](std::uint64_t) {

                presenter->clear();
                std::lock_guard lock(shared->mutex);
                shared->status = "Stream stopped; phone remains connected";
                utils::record(*shared, "Video stream ended");

            };

            callbacks.on_disconnected = [shared, presenter](std::uint64_t) {

                presenter->clear();
                std::lock_guard lock(shared->mutex);
                shared->connected = false;
                shared->snapshot_frame.reset();
                shared->queue_ms = shared->decode_ms = shared->conversion_ms = shared->webcam_ms = 0;

                shared->reset_image = true;
                shared->pending_image = {};

                shared->settings = {};
                shared->capabilities.reset();
                shared->selection.reset();
                ++shared->capture_revision;
                shared->decoder.active.clear();

                shared->decoder.message = "Connect a phone to select a decoder.";
                ++shared->decoder_revision;
                shared->capture_size = {};
                shared->capture_pending = false;

                shared->capture_notice.clear();
                shared->status = "Phone disconnected";
                utils::record(*shared, shared->status);

            };

            callbacks.on_error = [shared](const std::string& error) {
                std::lock_guard lock(shared->mutex);
                shared->error = QString::fromUtf8(error.c_str());
                utils::record(*shared, shared->error);
            };

            callbacks.on_frame = [shared, presenter](const AVFrame& frame, std::uint64_t) {

                {

                    std::lock_guard lock(shared->mutex);
                    shared->capture_size = QSize(frame.width, frame.height);
                    shared->frame_count++;
                    shared->status = "Streaming";

                }

                presenter->submit(frame);

            };

            const auto port = static_cast<std::uint16_t>(port_input ? port_input->value() : configuration.port);
            application_config receiver_config{port};
            receiver_config.decoder = decoder_preference;
            receiver_config.automatic_decoder = automatic_decoder && automatic_decoder->isChecked();
            core = std::make_shared<application>(receiver_config, std::move(callbacks));
            const auto receiver = core;

            worker.emplace([shared, receiver](std::stop_token token) {

                std::stop_callback cancellation(token, [&] {
                    receiver->request_stop();
                });

                try {

                    receiver->run();

                } catch (const std::exception& error) {

                    std::lock_guard lock(shared->mutex);
                    shared->error = QString::fromUtf8(error.what());
                    shared->status = shared->error;
                    shared->fatal = true;
                    utils::record(*shared, shared->error);

                } catch (...) {

                    std::lock_guard lock(shared->mutex);
                    shared->status = shared->error = "Unexpected receiver failure";
                    shared->fatal = true;

                }

                {

                    std::lock_guard lock(shared->mutex);

                    if (!shared->fatal)
                        shared->status = "Receiver stopped";

                    shared->connected = false;
                    shared->reset_image = true;
                    shared->pending_image = {};
                    shared->port = 0;

                }

                shared->finished = true;

            });

        }

        void refresh() {

            if (!state)
                return;

            QImage image;
            std::chrono::steady_clock::time_point image_submitted;
            double queue_ms, decode_ms, conversion_ms, webcam_ms;
            QString status;
            QString error;

            std::deque<QString> events;
            QStringList endpoints;

            std::uint64_t frames;
            std::uint64_t bytes;
            std::uint64_t encoded;
            std::uint64_t video_bytes;
            protocol::video_settings settings;

            decoder_selection decoder;
            std::uint64_t decoder_revision;
            std::uint64_t capture_revision;

            QSize capture_size;
            bool capture_pending;
            QString capture_notice;
            bool connected;
            bool reset;
            std::uint16_t port;

            {

                std::lock_guard lock(state->mutex);
                image = std::move(state->pending_image);
                image_submitted = state->pending_submitted;
                queue_ms = state->queue_ms;
                decode_ms = state->decode_ms;

                conversion_ms = state->conversion_ms;
                webcam_ms = state->webcam_ms;
                status = state->status;
                error = state->error;
                frames = state->frame_count;

                bytes = state->received_bytes;
                encoded = state->encoded_frames;
                video_bytes = state->video_bytes;
                settings = state->settings;

                decoder = state->decoder;
                decoder_revision = state->decoder_revision;
                capture_revision = state->capture_revision;
                capture_capabilities = state->capabilities;
                selected_capture = state->selection;

                capture_size = state->capture_size;
                capture_pending = state->capture_pending;
                capture_notice = state->capture_notice;
                connected = state->connected;
                port = state->port;

                endpoints = state->endpoints;
                reset = std::exchange(state->reset_image, false);
                events.swap(state->events);

            }

            state->preview_width = std::max(1, canvas->width());
            state->preview_height = std::max(1, canvas->height());
            state->preview_enabled = isVisible() && !isMinimized();
            const double reported_rate = screen() ? screen()->refreshRate() : 60;
            const double refresh_rate = reported_rate > 0 ? reported_rate : 60;

            state->preview_period_us = static_cast<int>(1'000'000 / refresh_rate);
            timer.setInterval(std::max(1, static_cast<int>(std::lround(1000 / refresh_rate))));
            canvas->display(std::move(image), status, reset, image_submitted);
            setWindowTitle("CamDroid | " + status);

            if (decoder_input && decoder_revision != displayed_decoder_revision && !decoder_input->view()->isVisible()) {

                if (decoder.message == "Selected best for you")
                    decoder_preference = decoder.active;

                const QSignalBlocker blocker(decoder_input);
                decoder_input->clear();

                QString active_decoder;

                auto ordered_options = decoder.options;

                const auto rank = [&](const decoder_option& option) {
                    const auto it = std::find(decoder.recommended.begin(), decoder.recommended.end(), option.id);
                    return std::distance(decoder.recommended.begin(), it);
                };

                std::stable_sort(ordered_options.begin(), ordered_options.end(), [&](const auto& a, const auto& b) {
                    return rank(a) < rank(b);
                });

                QStringList recommendations;

                for (const auto& option : ordered_options) {

                    const auto name = QString::fromStdString(option.name);
                    const auto label = QString("%1 | %2 ms/frame").arg(name).arg(option.decode_ms, 0, 'f', 2);
                    const auto position = rank(option);
                    const bool recommended = position < static_cast<decltype(position)>(decoder.recommended.size());

                    decoder_input->addItem(
                        recommended ? QString("#%1 %2").arg(position + 1).arg(label) : label,
                        QString::fromStdString(option.id)
                    );

                    if (recommended)
                        recommendations.push_back(QString("%1. %2").arg(position + 1).arg(name));

                    decoder_input->setItemData(
                        decoder_input->count() - 1,

                        QString("%1. %2 buffered frame(s). Automatic mode considers throughput and buffering delay.")
                            .arg(option.hardware ? "Hardware decoding" : "Software decoding")
                            .arg(option.buffered_frames),
                        Qt::ToolTipRole
                    );

                    if (option.id == decoder.active)
                        active_decoder = label;

                }

                const auto selected =
                    automatic_decoder->isChecked() || decoder_preference == "auto" ? decoder.active : decoder_preference;
                int index = decoder_input->findData(QString::fromStdString(selected));

                if (index < 0) {

                    decoder_input->addItem(
                        selected.empty() ? "Start streaming to identify decoders" : "Requested decoder (unavailable)",
                        QString::fromStdString(selected)
                    );
                    index = decoder_input->count() - 1;

                }

                decoder_input->setCurrentIndex(index);
                decoder_label->setText(
                    !active_decoder.isEmpty() ? "Active: " + active_decoder : QString::fromStdString(decoder.message)
                );

                if (!active_decoder.isEmpty() &&
                    (decoder.message.starts_with("Decoder change") || decoder.message.starts_with("Comparing") ||
                     decoder.message == "Selected best for you"))
                    decoder_label->setText(decoder_label->text() + "\n" + QString::fromStdString(decoder.message));

                if (!recommendations.empty())
                    decoder_label->setText(decoder_label->text() + "\nRecommended:\n" + recommendations.join("\n"));

                displayed_decoder_revision = decoder_revision;

            }

            if (decoder_input) {

                decoder_input->setEnabled(!automatic_decoder->isChecked() && !decoder.options.empty());
                if (flip_camera)
                    flip_camera->setEnabled(connected && status == "Streaming");

                identify_decoder->setEnabled(
                    connected && status == "Streaming" && !decoder.active.empty() && !decoder.identifying
                );
                identify_decoder->setText(decoder.identifying ? "Identifying..." : "Identify best");

            }

            if (status_label) {

                configured_label->setText(
                    settings.fps ? QString("Configured encoder: %1 FPS | %2 Mbps")
                                       .arg(settings.fps)
                                       .arg(settings.bitrate / 1000000.0, 0, 'f', 2)
                                 : "Configured encoder: not reported"
                );

                webcam_button->setEnabled(!webcam->pending());
                webcam_button->setText(webcam->active() ? "Disable virtual webcam" : "Enable virtual webcam");
                const auto camera_error = QString::fromStdString(webcam->error());

                webcam_label->setText(
                    !camera_error.isEmpty() ? camera_error
                    : webcam->pending()     ? "Updating webcam..."
                    : webcam->active()      ? QString("Webcam enabled. Capture follows your Android or desktop settings.")
                                            : QString("Virtual webcam disabled")
                );

                {

                    const QSignalBlocker blocker(allow_app_outputs);
                    allow_app_outputs->setChecked(webcam->app_outputs_allowed());

                }

                if (capture_revision != displayed_capture_revision && !capture_dirty && !capture_pending &&
                    !capture_fps->view()->isVisible() && !bitrate_preset->view()->isVisible()) {

                    populate_capture_options(true);
                    displayed_capture_revision = capture_revision;

                }

                capture_bitrate->setValue(settings.bitrate / 1000000.0);
                const bool can_configure = connected && capture_capabilities && capture_fps->currentIndex() >= 0 &&
                                           !capture_size.isEmpty() && settings.fps && !capture_pending && !closing;
                capture_resolution->setEnabled(can_configure || capture_pending);
                capture_fps->setEnabled(can_configure || capture_pending);
                bitrate_preset->setEnabled(can_configure || capture_pending);

                if (!connected) {

                    capture_dirty = false;
                    capture_timer.stop();

                }

                if (can_configure && capture_dirty && !capture_timer.isActive()) {

                    std::lock_guard lock(state->mutex);
                    const auto selected = capture_resolution->currentData().toSize();
                    const auto size = selected.isEmpty() ? state->capture_size : selected;
                    const int fps = capture_fps->currentData().toInt();
                    const auto selected_bitrate = bitrate_preset->currentData().toUInt();
                    const auto bitrate = selected_bitrate ? selected_bitrate : protocol::adaptive_bitrate;

                    core->request_video(
                        {static_cast<std::uint16_t>(size.width()),
                         static_cast<std::uint16_t>(size.height()),
                         static_cast<std::uint16_t>(fps)},

                        bitrate
                    );
                    state->capture_pending = true;
                    state->capture_notice.clear();
                    capture_dirty = false;

                }

                capture_label->setText(
                    (capture_size.isEmpty() ? QString("Start streaming to change capture settings.")
                                            : QString("Current: %1 x %2 at %3 FPS | %4 Mbps")
                                                  .arg(capture_size.width())
                                                  .arg(capture_size.height())

                                                  .arg(settings.fps)
                                                  .arg(settings.bitrate / 1000000.0, 0, 'f', 2)) +
                    (capture_pending            ? "\nApplying settings..."
                     : capture_notice.isEmpty() ? QString()
                                                : "\n" + capture_notice)
                );

                const auto installation_notice = QString::fromStdString(webcam->notice());

                if (!installation_notice.isEmpty() && camera_error.isEmpty())
                    webcam_label->setText(webcam_label->text() + "\n" + installation_notice);

                status_label->setText(status + (error.isEmpty() ? QString() : "\nLast error: " + error));
                endpoint_label->setText(
                    port == 0 ? "Receiver stopped"
                              : (endpoints.isEmpty() ? QString("Listening port: %1").arg(port) : endpoints.join("\n"))
                );

                const bool running = !state->finished;
                listen_button->setVisible(!running);
                stop_button->setVisible(running);
                disconnect_button->setVisible(connected);
                listen_button->setEnabled(!running && !closing);

                stop_button->setEnabled(running && !closing);
                disconnect_button->setEnabled(running && connected && !closing);
                port_input->setEnabled(!running && !closing);

                for (const auto& event : events)
                    event_log->appendPlainText(event);
                const auto now = std::chrono::steady_clock::now();
                const double seconds = std::chrono::duration<double>(now - last_report).count();

                if (seconds >= 1) {

                    metrics_label->setText(
                        QString(
                            "%1 x %2 capture | Decoded %3 FPS | Transport %4 Mbps | %5 frames\nReceived video: %6 FPS | %7 "
                            "Mbps"
                        )

                            .arg(capture_size.width())
                            .arg(capture_size.height())
                            .arg((frames - previous_frames) / seconds, 0, 'f', 1)
                            .arg((bytes - previous_bytes) * 8.0 / seconds / 1000000, 0, 'f', 2)
                            .arg(frames)

                            .arg((encoded - previous_encoded) / seconds, 0, 'f', 1)
                            .arg((video_bytes - previous_video_bytes) * 8.0 / seconds / 1000000, 0, 'f', 2)
                    );
                    const auto presented = canvas->presented_frames();

                    metrics_label->setText(
                        metrics_label->text() +
                        QString(
                            "\nPresented: %1 FPS | Video queue: %2 ms | Decode: %3 ms\nPreview conversion: %4 ms | Webcam: "
                            "%5 ms | Presentation: %6 ms | Paint CPU: %7 ms"

                        )
                            .arg((presented - previous_presented) / seconds, 0, 'f', 1)
                            .arg(queue_ms, 0, 'f', 1)
                            .arg(decode_ms, 0, 'f', 1)
                            .arg(conversion_ms, 0, 'f', 1)

                            .arg(webcam_ms, 0, 'f', 1)
                            .arg(canvas->presentation_ms(), 0, 'f', 1)
                            .arg(canvas->paint_ms(), 0, 'f', 1)
                    );

                    metrics_label->setToolTip(
                        "Local pipeline timings. Presentation starts at decoded-frame delivery; paint measures CPU "
                        "submission, not physical screen latency. Phone and desktop clocks are not synchronized."
                    );
                    previous_presented = presented;

                    previous_encoded = encoded;
                    previous_video_bytes = video_bytes;
                    previous_frames = frames;
                    previous_bytes = bytes;
                    last_report = now;

                }

            }

            if (closing && state->finished)
                close();

        }

        void stop_receiver() {
            webcam->clear();

            if (core)
                core->request_stop();
        }

        protected:
        void closeEvent(QCloseEvent* event) override {

            if ((!worker || state->finished) && !webcam->pending() && !webcam->active()) {

                event->accept();

                return;

            }

            closing = true;
            stop_receiver();

            if (webcam->active() && !webcam->pending())
                webcam->enable(false);

            event->ignore();

        }

        public:
        desktop_window(const app_config& config, bool gui) : configuration(config) {

            resize(gui ? 1180 : 960, gui ? 780 : 540);
            auto* wheel_filter = new utils::settings_wheel_filter(this);
            canvas = new video_canvas(this);
            auto* root = new QVBoxLayout(this);
            root->setContentsMargins(gui ? 20 : 0, gui ? 20 : 0, gui ? 20 : 0, gui ? 20 : 0);
            root->setSpacing(10);

            if (gui) {

                auto* heading = new QLabel("CamDroid", this);
                auto font = heading->font();
                font.setPointSize(20);
                font.setBold(true);
                heading->setFont(font);

                heading->setObjectName("brand");
                auto* brand = new QVBoxLayout;
                brand->setSpacing(2);
                brand->addWidget(heading);
                auto* subtitle = new QLabel("CAMERA WORKSPACE", this);

                subtitle->setObjectName("muted");
                brand->addWidget(subtitle);
                auto* header = new QHBoxLayout;
                auto* logo = new QLabel(this);
                logo->setPixmap(QApplication::windowIcon().pixmap(QSize(56, 56), devicePixelRatioF()));

                logo->setFixedSize(56, 56);
                logo->setAccessibleName("CamDroid logo");

                header->setSpacing(12);
                header->addWidget(logo);
                header->addLayout(brand);
                header->addStretch();

                status_label = new QLabel("Starting receiver", this);
                status_label->setObjectName("receiver_status");
                status_label->setWordWrap(true);
                status_label->setMinimumWidth(240);
                status_label->setMaximumWidth(460);

                status_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
                header->addWidget(status_label);
                root->addLayout(header);

                auto* content = new QHBoxLayout;
                content->setSpacing(20);
                auto* sidebar_widget = new QWidget(this);
                auto* sidebar = new QVBoxLayout(sidebar_widget);
                sidebar->setContentsMargins(0, 0, 8, 0);

                sidebar->setSpacing(12);
                auto* controls = new QScrollArea(this);
                controls->setWidgetResizable(true);
                controls->setFrameShape(QFrame::NoFrame);
                controls->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

                controls->setMinimumWidth(320);
                controls->setMaximumWidth(360);
                controls->setWidget(sidebar_widget);
                auto* connection = new QGroupBox("Connection", this);
                connection->setMaximumWidth(320);

                auto* form = new QVBoxLayout(connection);
                auto* addresses = new QLabel(this);
                addresses->setTextInteractionFlags(Qt::TextSelectableByMouse);
                addresses->setWordWrap(true);
                addresses->setText(QSysInfo::machineHostName());

                addresses->setObjectName("muted");
                form->addWidget(addresses);
                auto* port_row = new QFormLayout;
                port_input = new QSpinBox(this);
                port_input->setRange(4097, 65535);

                port_input->setValue(config.port);
                port_input->setAccessibleName("Listening TCP port");
                port_row->addRow("Port", port_input);
                form->addLayout(port_row);
                endpoint_label = new QLabel("Receiver starting", this);

                endpoint_label->setObjectName("listening_endpoint");
                endpoint_label->setWordWrap(true);
                endpoint_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
                form->addWidget(endpoint_label);
                auto* hint = new QLabel("Connect from the Android app.", this);

                hint->setWordWrap(true);
                hint->setObjectName("muted");
                form->addWidget(hint);
                listen_button = new QPushButton("Start listening", this);
                listen_button->setObjectName("start_receiver");

                stop_button = new QPushButton("Stop receiver", this);
                stop_button->setObjectName("stop_receiver");
                disconnect_button = new QPushButton("Disconnect phone", this);
                disconnect_button->setObjectName("disconnect_phone");
                form->addWidget(listen_button);

                form->addWidget(stop_button);
                form->addWidget(disconnect_button);
                sidebar->addWidget(connection);

                auto* settings_tabs = new QTabWidget(this);
                settings_tabs->setObjectName("settings_tabs");
                sidebar->addWidget(settings_tabs);
                auto* capture = new QGroupBox("Capture settings", this);
                auto* capture_form = new QFormLayout(capture);

                capture_resolution = new QComboBox(this);
                capture_resolution->setObjectName("capture_resolution");

                for (const auto size : {QSize(640, 480), QSize(1280, 720), QSize(1920, 1080), QSize(3840, 2160)})
                    capture_resolution->addItem(QString("%1 x %2").arg(size.width()).arg(size.height()), size);
                capture_fps = new QComboBox(this);
                capture_fps->setObjectName("capture_fps");
                capture_fps->setToolTip("Supported by the current camera, resolution and AVC encoder.");

                capture_bitrate = new QDoubleSpinBox(this);
                capture_bitrate->setRange(0, 1000);
                capture_bitrate->setDecimals(1);
                capture_bitrate->setSingleStep(0.1);

                capture_bitrate->setSuffix(" Mbps");
                capture_bitrate->setKeyboardTracking(false);
                capture_bitrate->setObjectName("capture_bitrate");
                capture_bitrate->setReadOnly(true);
                capture_bitrate->setEnabled(false);
                capture_bitrate->setButtonSymbols(QAbstractSpinBox::NoButtons);

                bitrate_preset = new QComboBox(this);
                bitrate_preset->setObjectName("bitrate_preset");
                bitrate_preset->addItem("Adaptive | 2–40 Mbps", 0u);
                bitrate_preset->addItem("Low | 5 Mbps", 5000000u);
                bitrate_preset->addItem("Medium | 12 Mbps", 12000000u);

                bitrate_preset->addItem("High | 18 Mbps", 18000000u);
                bitrate_preset->addItem("Extreme | 28 Mbps", 28000000u);
                bitrate_preset->addItem("Ig bro | 40 Mbps", 40000000u);
                capture_label = new QLabel("Start streaming to change capture settings.", this);
                capture_label->setWordWrap(true);

                auto* policy = new QLabel(
                    "Changes apply automatically to your phone. Webcam output conversion is configured in the Webcam tab.",
                    this
                );
                policy->setWordWrap(true);

                policy->setObjectName("muted");
                capture_form->addRow("Resolution", capture_resolution);
                capture_form->addRow("FPS", capture_fps);
                capture_form->addRow("Bitrate", capture_bitrate);
                capture_form->addRow("Target", bitrate_preset);
                capture_form->addRow(capture_label);

                capture_form->addRow(policy);

                const auto add_settings_tab = [&](QWidget* content, const QString& title) {

                    auto* page = new QWidget(settings_tabs);
                    auto* layout = new QVBoxLayout(page);
                    layout->setContentsMargins(0, 16, 0, 0);
                    layout->addWidget(content);
                    settings_tabs->addTab(page, title);

                };

                add_settings_tab(capture, "Camera");
                populate_capture_options(false);
                capture_timer.setSingleShot(true);
                capture_timer.setInterval(400);

                const auto queue_capture = [this] {
                    capture_dirty = true;
                    capture_timer.start();
                };

                QObject::connect(capture_resolution, &QComboBox::currentIndexChanged, this, [this, queue_capture] {
                    populate_capture_options(false);
                    queue_capture();
                });

                QObject::connect(capture_fps, &QComboBox::currentIndexChanged, this, queue_capture);
                QObject::connect(bitrate_preset, &QComboBox::currentIndexChanged, this, queue_capture);

                auto* display = new QGroupBox("Display", this);
                auto* options = new QVBoxLayout(display);
                mirror_input = new QCheckBox("Mirror horizontally", this);
                mirror_input->setToolTip("Output is unmirrored by default, including the front camera.");
                rotation_input = new QComboBox(this);
                rotation_input->addItem("Automatic orientation", -1);

                for (const int angle : {0, 90, 180, 270})
                    rotation_input->addItem(QString("%1 degrees clockwise").arg(angle), angle);
                rotation_input->setAccessibleName("Video orientation");
                options->addWidget(rotation_input);
                options->addWidget(mirror_input);

                decoder_input = new QComboBox(this);
                decoder_input->setObjectName("video_decoder");
                decoder_input->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
                decoder_input->setMinimumContentsLength(12);
                decoder_input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

                decoder_input->addItem("Start streaming to identify decoders", QString());
                decoder_input->setToolTip(
                    "Compares codec-compatible decoders, including GPU download time. Changes apply at a keyframe."
                );

                decoder_label = new QLabel("Available decoders are checked when streaming starts.", this);
                decoder_label->setWordWrap(true);
                flip_camera = new QPushButton("Flip camera", this);
                flip_camera->setEnabled(false);
                options->addWidget(flip_camera);

                QObject::connect(flip_camera, &QPushButton::clicked, this, [this] {
                    if (core)
                        core->flip_camera();
                });

                options->addWidget(new QLabel("Video decoder", this));
                auto* decoder_row = new QHBoxLayout;
                identify_decoder = new QPushButton("Identify best", this);

                identify_decoder->setObjectName("identify_decoder");
                identify_decoder->setToolTip(
                    "Runs one background comparison and ranks up to three working decoders. Select a recommendation from the "
                    "list."
                );

                identify_decoder->setEnabled(false);
                decoder_row->addWidget(decoder_input, 1);
                decoder_row->addWidget(identify_decoder);
                options->addLayout(decoder_row);

                auto* automatic_row = new QHBoxLayout;
                automatic_decoder = new QPushButton("Automatic toggle", this);
                automatic_decoder->setObjectName("automatic_decoder");
                automatic_decoder->setToolTip(
                    "Periodically compares decoders and switches at a keyframe. Uses additional CPU/GPU resources."
                );

                automatic_decoder->setCheckable(true);
                automatic_led = new QLabel(this);
                automatic_led->setObjectName("automatic_decoder_led");
                automatic_led->setFixedSize(10, 10);

                automatic_led->setStyleSheet("background: #667080; border-radius: 5px;");
                automatic_led->setAccessibleName("Automatic decoder switching disabled");
                automatic_row->addWidget(automatic_led);
                automatic_row->addWidget(automatic_decoder);
                automatic_row->addStretch();

                options->addLayout(automatic_row);
                auto* warning = new QLabel("consumes extra resources", this);
                warning->setStyleSheet("color: #ff7777;");
                options->addWidget(warning);
                options->addWidget(decoder_label);

                QObject::connect(identify_decoder, &QPushButton::clicked, this, [this] {
                    if (core)
                        core->identify_decoder();
                });

                QObject::connect(automatic_decoder, &QPushButton::toggled, this, [this](bool enabled) {

                    automatic_led->setStyleSheet(
                        enabled ? "background: #a4e778; border-radius: 5px;" : "background: #667080; border-radius: 5px;"
                    );
                    automatic_led->setAccessibleName(
                        enabled ? "Automatic decoder switching enabled" : "Automatic decoder switching disabled"
                    );

                    decoder_input->setEnabled(!enabled);

                    if (core) {

                        if (!enabled) {

                            std::lock_guard lock(state->mutex);
                            decoder_preference = state->decoder.active;
                            core->request_decoder(decoder_preference);

                        }

                        core->set_automatic_decoder(enabled);

                    }

                });

                QObject::connect(decoder_input, &QComboBox::currentIndexChanged, this, [this](int) {

                    const auto choice = decoder_input->currentData().toString().toStdString();

                    if (choice.empty() || automatic_decoder->isChecked())
                        return;

                    decoder_preference = choice;

                    if (core)
                        core->request_decoder(decoder_preference);

                });

                auto* snapshot = new QPushButton("Save snapshot...", this);
                options->addWidget(snapshot);
                add_settings_tab(display, "Display");
                auto* camera_group = new QGroupBox("Virtual webcam", this);
                auto* camera_layout = new QVBoxLayout(camera_group);

                webcam_label = new QLabel("Windows 11 | selectable resolution and frame rate", this);
                webcam_label->setWordWrap(true);
                webcam_button = new QPushButton("Enable virtual webcam", this);
                allow_app_outputs = new QCheckBox("Allow apps to configure\nwebcam outputs", this);
                allow_app_outputs->setObjectName("allow_app_outputs");

                allow_app_outputs->setChecked(webcam->app_outputs_allowed());
                allow_app_outputs->setToolTip(
                    "Allows independent per-app output conversion. Capture settings stay under your control. Enabled by "
                    "default for browser compatibility; capture settings remain unchanged."
                );

                QObject::connect(allow_app_outputs, &QCheckBox::toggled, this, [this](bool allow) {
                    webcam->allow_app_outputs(allow);
                });

                camera_layout->addWidget(webcam_label);
                camera_layout->addWidget(webcam_button);
                camera_layout->addWidget(allow_app_outputs);
                add_settings_tab(camera_group, "Webcam");

                QObject::connect(webcam_button, &QPushButton::clicked, this, [this] {
                    webcam->enable(!webcam->active());
                });

                sidebar->addStretch();
                content->addWidget(controls);
                content->addWidget(canvas, 1);
                root->addLayout(content, 1);
                metrics_label = new QLabel("No video received", this);

                metrics_label->setWordWrap(true);
                configured_label = new QLabel("Configured encoder: not reported", this);
                configured_label->setWordWrap(true);
                auto* telemetry = new QHBoxLayout;
                configured_label->setObjectName("muted");

                metrics_label->setObjectName("telemetry");
                telemetry->addWidget(configured_label, 1);
                telemetry->addWidget(metrics_label, 2);
                root->addLayout(telemetry);
                auto* activity = new QPushButton("Activity log", this);

                activity->setCheckable(true);
                activity->setObjectName("activity_toggle");
                root->addWidget(activity, 0, Qt::AlignLeft);
                event_log = new QPlainTextEdit(this);
                event_log->setReadOnly(true);

                event_log->setMaximumBlockCount(200);
                event_log->setMaximumHeight(110);
                event_log->setAccessibleName("Connection events and errors");
                root->addWidget(event_log);
                event_log->setVisible(false);
                QObject::connect(activity, &QPushButton::toggled, event_log, &QWidget::setVisible);

                for (auto* combo : findChildren<QComboBox*>()) {

                    combo->setItemDelegate(new utils::settings_option_delegate(combo));
                    combo->view()->setMouseTracking(true);
                    combo->installEventFilter(wheel_filter);

                }

                for (auto* spin : findChildren<QAbstractSpinBox*>())
                    spin->installEventFilter(wheel_filter);

                QObject::connect(listen_button, &QPushButton::clicked, this, [this] {
                    start_receiver();
                });

                QObject::connect(stop_button, &QPushButton::clicked, this, [this] {
                    stop_receiver();
                });

                QObject::connect(disconnect_button, &QPushButton::clicked, this, [this] {
                    if (core)
                        core->disconnect_peer();
                });

                QObject::connect(mirror_input, &QCheckBox::toggled, this, [this](bool value) {
                    if (state)
                        state->mirror = value;
                });

                QObject::connect(rotation_input, &QComboBox::currentIndexChanged, this, [this](int) {
                    if (state)
                        state->rotation = rotation_input->currentData().toInt();
                });

                QObject::connect(snapshot, &QPushButton::clicked, this, [this] {

                    if (canvas->current_image().isNull()) {

                        status_label->setText("Start streaming before saving a snapshot.");

                        return;

                    }

                    const bool png = QImageWriter::supportedImageFormats().contains("png");
                    const auto file = QFileDialog::getSaveFileName(
                        this,
                        "Save camera snapshot",
                        png ? "camdroid.png" : "camdroid.bmp",

                        png ? "PNG image (*.png);;Bitmap image (*.bmp)" : "Bitmap image (*.bmp)"
                    );

                    if (file.isEmpty())
                        return;

                    std::shared_ptr<AVFrame> frame;

                    {

                        std::lock_guard lock(state->mutex);
                        frame = state->snapshot_frame;

                    }

                    QImage snapshot_image = canvas->current_image();

                    try {

                        if (frame) {

                            utils::frame_converter converter;
                            snapshot_image =
                                utils::orient_image(converter.convert(*frame), *frame, state->rotation, state->mirror);

                        }

                    } catch (const std::exception& error) {

                        std::lock_guard lock(state->mutex);
                        state->error = QString::fromUtf8(error.what());
                        return;

                    }

                    if (!snapshot_image.save(file)) {

                        std::lock_guard lock(state->mutex);
                        state->error = "Could not save snapshot to the selected location.";

                    }

                });

            } else {

                root->addWidget(canvas);

            }

            auto* fullscreen = new QShortcut(QKeySequence(Qt::Key_F11), this);

            QObject::connect(fullscreen, &QShortcut::activated, this, [this] {
                isFullScreen() ? showNormal() : showFullScreen();
            });

            auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);

            QObject::connect(escape, &QShortcut::activated, this, [this] {
                if (isFullScreen())
                    showNormal();
            });

            QObject::connect(&timer, &QTimer::timeout, this, [this] {
                refresh();
            });

            timer.setTimerType(Qt::PreciseTimer);
            timer.start(16);
            start_receiver();

            if (config.webcam)
                webcam->enable(true);

        }

        ~desktop_window() override {

            stop_receiver();

            if (worker) {

                worker->request_stop();
                worker->join();

            }

        }

    };

    int run_preview(int argc, char** argv, const app_config& configuration) {

        QApplication qt(argc, argv);
        qt.setApplicationName("CamDroid");
        QPixmap logo;

        if (!logo.load(":/branding/camdroid.png", "PNG"))
            throw std::runtime_error("Cannot load the embedded CamDroid logo.");

        qt.setWindowIcon(QIcon(logo));
        qt.setDesktopFileName("camdroid");
        QApplication::setStyle("Fusion");

        utils::apply_theme(qt);
        desktop_window window(configuration, !configuration.preview);
        window.show();

        return qt.exec();

    }

    #ifdef CAMDROID_GUI_TESTS
    int test_gui(int argc, char** argv) {

        QApplication qt(argc, argv);
        QApplication::setStyle("Fusion");
        utils::apply_theme(qt);
        QTcpServer reservation;

        if (!reservation.listen(QHostAddress::LocalHost, 0))
            return 1;

        app_config config;
        config.port = reservation.serverPort();
        config.gui = true;
        reservation.close();

        QImage sample(2, 3, QImage::Format_RGBA8888);
        sample.fill(Qt::black);
        sample.setPixelColor(0, 2, Qt::red);
        AVFrame frame{};
        av_dict_set(&frame.metadata, "camdroid.rotation", "90", 0);

        auto oriented = utils::orient_image(sample, frame, -1, false);
        auto mirrored = utils::orient_image(sample, frame, -1, true);
        av_dict_free(&frame.metadata);

        if (oriented.size() != QSize(3, 2) || oriented.pixelColor(0, 0) != QColor(Qt::red) ||
            mirrored.pixelColor(2, 0) != QColor(Qt::red))
            return 1;

        frame.width = sample.width();
        frame.height = sample.height();
        frame.format = AV_PIX_FMT_RGBA;
        frame.data[0] = sample.bits();
        frame.linesize[0] = static_cast<int>(sample.bytesPerLine());
        utils::frame_converter converter;

        if (converter.convert(frame).pixelColor(0, 2) != QColor(Qt::red))
            return 1;

        desktop_window window(config, true);

        if (window.findChild<QPushButton*>("apply_capture"))
            return 1;

        const auto* output_policy = window.findChild<QCheckBox*>("allow_app_outputs");

        if (!output_policy || !output_policy->isChecked())
            return 1;

        window.resize(900, 600);
        window.show();
        qt.processEvents();
        auto* controls = window.findChild<QScrollArea*>();

        if (!controls || window.height() > 600)
            return 1;

        for (auto* button : controls->findChildren<QPushButton*>()) {

            if (button->isVisible() && button->height() < button->minimumSizeHint().height())
                return 1;

        }

        if (argc > 2)
            window.grab().save(QString::fromLocal8Bit(argv[2]));

        auto* resolution = window.findChild<QComboBox*>("capture_resolution");
        auto* fps_input = window.findChild<QComboBox*>("capture_fps");

        if (!resolution || !fps_input)
            return 1;

        resolution->setEnabled(true);
        fps_input->setEnabled(true);
        resolution->setCurrentIndex(1);
        fps_input->setCurrentIndex(fps_input->findData(60));

        for (QWidget* control : {static_cast<QWidget*>(resolution), static_cast<QWidget*>(fps_input)}) {

            const QPointF position(control->rect().center());
            QWheelEvent wheel(
                position,
                control->mapToGlobal(position.toPoint()),
                QPoint(),

                QPoint(0, 120),
                Qt::NoButton,
                Qt::NoModifier,
                Qt::NoScrollPhase,

                false
            );
            QApplication::sendEvent(control, &wheel);

        }

        if (resolution->currentIndex() != 1 || fps_input->currentData().toInt() != 60)
            return 1;

        window.resize(1920, 1080);
        qt.processEvents();

        for (auto* button : controls->findChildren<QPushButton*>()) {

            if (button->isVisible() && button->height() < button->minimumSizeHint().height())
                return 1;

        }

        QTcpSocket phone;
        QTimer tick;
        int phase = 0;
        bool timed_out = false;
        const auto start = std::chrono::steady_clock::now();

        QObject::connect(&tick, &QTimer::timeout, &window, [&] {

            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {

                timed_out = true;
                window.close();

                return;

            }

            auto* listen = window.findChild<QPushButton*>("start_receiver");
            auto* stop = window.findChild<QPushButton*>("stop_receiver");
            auto* disconnect = window.findChild<QPushButton*>("disconnect_phone");
            auto* endpoint = window.findChild<QLabel*>("listening_endpoint");
            auto* status = window.findChild<QLabel*>("receiver_status");

            if (!listen || !stop || !disconnect || !endpoint || !status) {

                timed_out = true;
                window.close();

                return;

            }

            if (phase == 0 && endpoint->text().endsWith(QString::number(config.port))) {

                phone.connectToHost(QHostAddress::LocalHost, config.port);
                phase = 1;

            } else if (phase == 1 && disconnect->isEnabled()) {

                disconnect->click();
                phase = 2;

            } else if (phase == 2 && !disconnect->isEnabled()) {

                if (status->text().contains("Last error"))
                    timed_out = true;

                stop->click();
                phase = 3;

            } else if (phase == 3 && listen->isEnabled()) {

                listen->click();
                phase = 4;

            } else if (phase == 4 && !listen->isEnabled() && stop->isEnabled()) {

                phone.abort();
                phone.connectToHost(QHostAddress::LocalHost, config.port);
                phase = 5;

            } else if (phase == 5 && disconnect->isEnabled()) {

                window.close();
                phase = 6;

            }

        });

        tick.start(20);
        qt.exec();

        return !timed_out && phase == 6 ? 0 : 1;

    }

    #endif

};
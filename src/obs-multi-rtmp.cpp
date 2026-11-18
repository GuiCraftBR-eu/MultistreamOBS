// Modified for Multistream OBS on 2026-11-16.
#include "pch.h"

#include <list>
#include <regex>
#include <filesystem>
#include <unordered_map>

#include <QFileDialog>
#include <QFile>
#include <QIODevice>
#include <QFileInfo>
#include <QMenu>
#include <QPointer>

#include "push-widget.h"
#include "plugin-support.h"
#include "update-manager.h"

#include "output-config.h"
#include "config-store.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#define ConfigSection "multistream-obs"

static class GlobalServiceImpl : public GlobalService
{
public:
    bool RunInUIThread(std::function<void()> task) override {
        if (uiThread_ == nullptr)
            return false;
        QMetaObject::invokeMethod(uiThread_, [func = std::move(task)]() {
            func();
        });
        return true;
    }

    QThread* uiThread_ = nullptr;
} s_service;


GlobalService& GetGlobalService() {
    return s_service;
}


class OutputsListWidget : public QListWidget
{
public:
    using QListWidget::QListWidget;

    QSize sizeHint() const override
    {
        QSize hint = QListWidget::sizeHint();
        hint.setHeight(ContentHeight());
        return hint;
    }

    QSize minimumSizeHint() const override
    {
        return sizeHint();
    }

protected:
    bool event(QEvent *event) override
    {
        const bool handled = QListWidget::event(event);

        switch (event->type()) {
        case QEvent::FontChange:
        case QEvent::LayoutRequest:
        case QEvent::PolishRequest:
        case QEvent::Show:
        case QEvent::StyleChange:
            RefreshItemSizeHints();
            updateGeometry();
            break;
        default:
            break;
        }

        return handled;
    }

private:
    void RefreshItemSizeHints()
    {
        for (int i = 0; i < count(); ++i) {
            auto item = this->item(i);
            auto widget = itemWidget(item);
            if (item && widget)
                item->setSizeHint(widget->sizeHint());
        }
    }

    int ContentHeight() const
    {
        auto *widget = const_cast<OutputsListWidget *>(this);
        widget->doItemsLayout();

        int totalHeight = frameWidth() * 2;
        const int itemCount = count();
        for (int i = 0; i < itemCount; ++i) {
            totalHeight += sizeHintForRow(i);
        }

        if (itemCount > 1) {
            totalHeight += (itemCount - 1) * spacing();
        }

        return (std::max)(totalHeight, frameWidth() * 2);
    }
};


class MultiOutputWidget : public QWidget
{
public:
    MultiOutputWidget(QWidget* parent = 0)
        : QWidget(parent)
    {
        setWindowTitle(obs_module_text("Title"));

        container_ = new QWidget(&scroll_);
        layout_ = new QVBoxLayout(container_);
        layout_->setAlignment(Qt::AlignmentFlag::AlignTop);
        layout_->setSizeConstraint(QLayout::SetMinAndMaxSize);

        // init widget
        // Modified 2026-11-16: platform presets with separate encoders by default.
        auto addButton = new QPushButton(obs_module_text("Btn.NewTarget"), container_);
        auto addMenu = new QMenu(addButton);
        auto addTarget = [this](const char *name, const char *server) {
            auto& global = GlobalMultiOutputConfig();
            auto newId = GenerateId(global);
            auto target = std::make_shared<OutputTargetConfig>();
            target->id = newId;
            target->name = name;
            target->platform = name;
            target->serviceParam = {{"server", server}, {"key", ""}};
            global.targets.emplace_back(target);
            auto video = std::make_shared<VideoEncoderConfig>();
            video->id = GenerateId(global);
            video->encoderId = "obs_x264";
            video->encoderParams = {{"rate_control", "CBR"}, {"bitrate", 6000}, {"keyint_sec", 2}};
            global.videoConfig.emplace_back(video);
            target->videoConfig = video->id;
            auto audio = std::make_shared<AudioEncoderConfig>();
            audio->id = GenerateId(global);
            audio->encoderId = "ffmpeg_aac";
            audio->mixerId = 0;
            audio->encoderParams = {{"bitrate", 160}};
            global.audioConfig.emplace_back(audio);
            target->audioConfig = audio->id;
            auto pushWidget = AddPushWidget(newId);
            if (pushWidget->ShowEditDlg()) {
                SaveConfig();
            } else {
                DeletePushWidget(newId);
                global.videoConfig.remove(video);
                global.audioConfig.remove(audio);
            }
        };
        QObject::connect(addMenu->addAction("Twitch"), &QAction::triggered, [addTarget]() {
            addTarget("Twitch", "rtmps://ingest.global-contribute.live-video.net/app");
        });
        QObject::connect(addMenu->addAction("YouTube"), &QAction::triggered, [addTarget]() {
            addTarget("YouTube", "rtmps://a.rtmps.youtube.com:443/live2");
        });
        QObject::connect(addMenu->addAction("Kick"), &QAction::triggered, [addTarget]() {
            addTarget("Kick", "");
        });
        addButton->setMenu(addMenu);
        layout_->addWidget(addButton);

        auto configWarning = new QLabel(container_);
        configWarning->setWordWrap(true);
        configWarning->hide();
        layout_->addWidget(configWarning);
        SetConfigStatusCallback([warning = QPointer<QLabel>(configWarning),
                                 add = QPointer<QPushButton>(addButton)](ConfigStatus status) {
            if (add)
                add->setEnabled(status != ConfigStatus::ReadError);
            if (!warning)
                return;
            const char *message = nullptr;
            if (status == ConfigStatus::Recovered)
                message = "Config.Recovered";
            else if (status == ConfigStatus::ReadError)
                message = "Config.ReadError";
            else if (status == ConfigStatus::WriteError)
                message = "Config.WriteError";
            warning->setText(message ? obs_module_text(message) : "");
            warning->setVisible(message != nullptr);
        });

        // start all, stop all
        auto allBtnContainer = new QWidget(this);
        auto allBtnLayout = new QHBoxLayout();
        auto startAllButton = new QPushButton(obs_module_text("Btn.StartAll"), allBtnContainer);
        allBtnLayout->addWidget(startAllButton);
        auto stopAllButton = new QPushButton(obs_module_text("Btn.StopAll"), allBtnContainer);
        allBtnLayout->addWidget(stopAllButton);
        allBtnContainer->setLayout(allBtnLayout);
        layout_->addWidget(allBtnContainer);

        QObject::connect(startAllButton, &QPushButton::clicked, [this]() {
            for (auto x : GetAllPushWidgets())
                x->StartStreaming();
        });
        QObject::connect(stopAllButton, &QPushButton::clicked, [this]() {
            for (auto x : GetAllPushWidgets())
                x->StopStreaming();
        });

        // load and show outputs
        outputsContainer_ = new OutputsListWidget(container_);
        outputsContainer_->setDragDropMode(QAbstractItemView::InternalMove);
        outputsContainer_->setSelectionMode(QAbstractItemView::SingleSelection);
        outputsContainer_->setDropIndicatorShown(true);
        outputsContainer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        outputsContainer_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
        outputsContainer_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        outputsContainer_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        outputsContainer_->setStyleSheet(
            "QListWidget {"
            "   padding: 0px;"
            "   margin: 0px;"
            "   border: none;"
            "   background: transparent;"
            "}"
            "QListWidget::item:selected, QListWidget::item:active {"
            "   border: none;"
            "   background: transparent;"
            "}"
            "QListWidget::item:hover, QListWidget::item:hover:selected, QListWidget::item:hover:active {"
            "   border: none;"
            "   background: rgba(127, 127, 127, 0.1);"
            "   cursor: move;"
            "}"
        );
        LoadConfig();
        connect(
            outputsContainer_->model(),
            &QAbstractItemModel::rowsMoved,
            this,
            &MultiOutputWidget::OnOutputMoved
        );
        layout_->addWidget(outputsContainer_);

        // export / import config, so a target list (including stream keys)
        // can be backed up or moved to another profile/PC instead of only
        // ever living inside the current profile's obs-multi-rtmp.json
        auto ioBtnContainer = new QWidget(this);
        auto ioBtnLayout = new QHBoxLayout();
        auto exportButton = new QPushButton(obs_module_text("Btn.ExportConfig"), ioBtnContainer);
        ioBtnLayout->addWidget(exportButton);
        auto importButton = new QPushButton(obs_module_text("Btn.ImportConfig"), ioBtnContainer);
        ioBtnLayout->addWidget(importButton);
        ioBtnContainer->setLayout(ioBtnLayout);
        layout_->addWidget(ioBtnContainer);

        QObject::connect(exportButton, &QPushButton::clicked, this, &MultiOutputWidget::ExportConfig);
        QObject::connect(importButton, &QPushButton::clicked, this, &MultiOutputWidget::ImportConfig);

        // Modified 2026-11-16: show updater status without interrupting a stream.
#ifdef _WIN32
        auto updateStatus = new QLabel(container_);
        updateStatus->setWordWrap(true);
        layout_->addWidget(updateStatus);
        const char *modulePath = obs_get_module_binary_path(obs_current_module());
        if (modulePath && *modulePath)
            new UpdateManager(QFileInfo(QString::fromUtf8(modulePath)).absolutePath(),
                              QString::fromUtf8(PLUGIN_VERSION), updateStatus, this);
#endif
        scroll_.setWidgetResizable(true);
        scroll_.setWidget(container_);

        auto fullLayout = new QGridLayout(this);
        fullLayout->setContentsMargins(0, 0, 0, 0);
        fullLayout->setRowStretch(0, 1);
        fullLayout->setColumnStretch(0, 1);
        fullLayout->addWidget(&scroll_, 0, 0);
    }

    std::list<PushWidget*> GetAllPushWidgets()
    {
        std::list<PushWidget*> result;
        for (int row = 0; row < outputsContainer_->count(); ++row) {
            auto item = outputsContainer_->item(row);
            if (!item) {
                continue;
            }

            auto widget = outputsContainer_->itemWidget(item);
            auto pushWidget = dynamic_cast<PushWidget*>(widget);
            if (pushWidget) {
                result.push_back(pushWidget);
            }
        }
        return result;
    }

    void SaveConfig()
    {
        SaveMultiOutputConfig();
    }

    void ExportConfig()
    {
        // Modified 2026-11-16: exported configurations contain credentials.
        if (QMessageBox::warning(this, obs_module_text("Notice.Title"),
                                 obs_module_text("Export.ContainsKeys"),
                                 QMessageBox::Ok | QMessageBox::Cancel,
                                 QMessageBox::Cancel) != QMessageBox::Ok)
            return;
        auto fileName = QFileDialog::getSaveFileName(
            this,
            obs_module_text("Btn.ExportConfig"),
            "multistream-obs-config.json",
            "JSON (*.json)"
        );
        if (fileName.isEmpty())
            return;
        if (!fileName.endsWith(".json", Qt::CaseInsensitive))
            fileName += ".json";

        auto content = SerializeMultiOutputConfig(GlobalMultiOutputConfig());

        if (!WriteConfigAtomically(fileName, QByteArray(content.data(), static_cast<qsizetype>(content.size())))) {
            QMessageBox(
                QMessageBox::Icon::Critical,
                obs_module_text("Notice.Title"),
                obs_module_text("Error.ExportConfig"),
                QMessageBox::StandardButton::Ok,
                this
            ).exec();
            return;
        }
    }

    void ImportConfig()
    {
        auto fileName = QFileDialog::getOpenFileName(
            this,
            obs_module_text("Btn.ImportConfig"),
            QString(),
            "JSON (*.json)"
        );
        if (fileName.isEmpty())
            return;

        QFile file(fileName);
        if (!file.open(QIODevice::ReadOnly) || file.size() > MaxConfigBytes) {
            QMessageBox(
                QMessageBox::Icon::Critical,
                obs_module_text("Notice.Title"),
                obs_module_text("Error.ImportConfig"),
                QMessageBox::StandardButton::Ok,
                this
            ).exec();
            return;
        }
        auto bytes = file.read(MaxConfigBytes + 1);

        auto imported = file.error() == QFileDevice::NoError
            ? DeserializeMultiOutputConfig(std::string(bytes.constData(), static_cast<size_t>(bytes.size())))
            : std::nullopt;
        if (!imported.has_value()) {
            QMessageBox(
                QMessageBox::Icon::Critical,
                obs_module_text("Notice.Title"),
                obs_module_text("Error.ImportConfig"),
                QMessageBox::StandardButton::Ok,
                this
            ).exec();
            return;
        }

        for (auto x : GetAllPushWidgets()) {
            if (x->IsBusy()) {
                QMessageBox(
                    QMessageBox::Icon::Warning,
                    obs_module_text("Notice.Title"),
                    obs_module_text("Error.ImportActive"),
                    QMessageBox::StandardButton::Ok,
                    this
                ).exec();
                return;
            }
        }

        auto confirm = QMessageBox(
            QMessageBox::Icon::Question,
            obs_module_text("Question.Title"),
            obs_module_text("Question.Import"),
            QMessageBox::Yes | QMessageBox::No,
            this
        ).exec();
        if (confirm != QMessageBox::Yes)
            return;

        if (!SaveMultiOutputConfig(&*imported, true)) {
            QMessageBox::critical(this, obs_module_text("Notice.Title"), obs_module_text("Error.SaveConfig"));
            return;
        }
        LoadConfig();
    }

    void OnOutputMoved(
        const QModelIndex &parent,
        int start,
        int end,
        const QModelIndex &destination,
        int row
    )
    {
        // QListWidget uses a single root parent for internal move operations.
        if (parent != destination) {
            return;
        }

        const int count = outputsContainer_->count();
        if (count <= 0 || start < 0 || end < start || row < 0 || row > count) {
            return;
        }

        auto &targets = GlobalMultiOutputConfig().targets;
        std::unordered_map<std::string, OutputTargetConfigPtr> targetById;
        targetById.reserve(targets.size());
        for (auto &target : targets) {
            if (target) {
                targetById.emplace(target->id, target);
            }
        }

        std::remove_reference_t<decltype(targets)> reordered;
        for (int i = 0; i < count; ++i) {
            auto item = outputsContainer_->item(i);
            if (!item) {
                continue;
            }

            auto id = item->data(Qt::UserRole).toString().toStdString();
            auto it = targetById.find(id);
            if (it == targetById.end()) {
                continue;
            }

            reordered.emplace_back(it->second);
            targetById.erase(it);
        }

        // Keep unmatched items in their previous order to avoid accidental loss.
        if (!targetById.empty()) {
            for (auto &target : targets) {
                if (!target) {
                    continue;
                }
                auto it = targetById.find(target->id);
                if (it == targetById.end()) {
                    continue;
                }
                reordered.emplace_back(it->second);
                targetById.erase(it);
            }
        }

        targets.swap(reordered);

        SaveConfig();
        outputsContainer_->clearSelection();
    }

    void LoadConfig()
    {
        outputsContainer_->clear();

        GlobalMultiOutputConfig() = {};
        if (!LoadMultiOutputConfig()) {
            return;
        }

        for(auto x: GlobalMultiOutputConfig().targets)
        {
            AddPushWidget(x->id);
        }
    }

private:
    // Main widget of this module's dock
    QWidget* container_ = 0;
    // The layout of the root widget
    QVBoxLayout* layout_ = 0;
    // Scrollable area in case of overflows of content
    QScrollArea scroll_;
    // Widget, that contains output source widgets
    QListWidget* outputsContainer_ = 0;

    void DeletePushWidget(const std::string& targetId)
    {
        // Delete from model
        auto outputTargets = &(GlobalMultiOutputConfig().targets);
        auto currentTarget = std::find_if(outputTargets->begin(), outputTargets->end(), [&targetId](auto& x) { return x->id == targetId; });
        if (currentTarget == outputTargets->end()) {
            return;
        }
        outputTargets->erase(currentTarget);

        // Delete from List View
        const QString id = QString::fromStdString(targetId);
        for(auto listItem: outputsContainer_->findItems("", Qt::MatchContains)) {
            if (listItem->data(Qt::UserRole).toString() != id) {
                continue;
            }
            int row = outputsContainer_->row(listItem);
            auto removedItem = outputsContainer_->takeItem(row);
            auto pushWidget = outputsContainer_->itemWidget(removedItem);
            delete removedItem;
            if (pushWidget) {
                pushWidget->deleteLater();
            }
        }
    }

    PushWidget* AddPushWidget(const std::string& targetId)
    {
        auto pushWidget = createPushWidget(targetId, outputsContainer_->viewport());

        QListWidgetItem* listItem = new QListWidgetItem();
        listItem->setData(Qt::UserRole, QString::fromStdString(targetId));
        listItem->setSizeHint(pushWidget->sizeHint());
        outputsContainer_->addItem(listItem);
        outputsContainer_->setItemWidget(listItem, pushWidget);

        QObject::connect(pushWidget->GetDeleteButton(), &QPushButton::clicked, [this, targetId]() {
            auto msgbox = new QMessageBox(
                QMessageBox::Icon::Question,
                obs_module_text("Question.Title"),
                obs_module_text("Question.Delete"),
                QMessageBox::Yes | QMessageBox::No,
                this
            );
            if (msgbox->exec() != QMessageBox::Yes) {
                return;
            }
            DeletePushWidget(targetId);
            SaveConfig();
        });

        return pushWidget;
    }
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("multistream-obs", "en-US")
OBS_MODULE_AUTHOR("雷鳴 (@sorayukinoyume)")

bool obs_module_load()
{
    auto mainwin = (QMainWindow*)obs_frontend_get_main_window();
    if (mainwin == nullptr)
        return false;
    QMetaObject::invokeMethod(mainwin, []() {
        s_service.uiThread_ = QThread::currentThread();
    });

    auto dock = new MultiOutputWidget();
    dock->setObjectName("multistream-obs-dock");
    if (!obs_frontend_add_dock_by_id("multistream-obs-dock", obs_module_text("Title"), dock))
    {
        delete dock;
        return false;
    }

    obs_hotkey_register_frontend(
        "multistream-obs.start_all",
        obs_module_text("Hotkey.StartAllTargets"),
        [](void* private_data, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
            auto dock = static_cast<MultiOutputWidget*>(private_data);
            if (!pressed || !dock)
                return;
            for (auto x : dock->GetAllPushWidgets())
                x->StartStreaming();
        },
        dock
    );
    obs_hotkey_register_frontend(
        "multistream-obs.stop_all",
        obs_module_text("Hotkey.StopAllTargets"),
        [](void* private_data, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
            auto dock = static_cast<MultiOutputWidget*>(private_data);
            if (!pressed || !dock)
                return;
            for (auto x : dock->GetAllPushWidgets())
                x->StopStreaming();
        },
        dock
    );

    blog(LOG_INFO, TAG "version: %s by SoraYuki https://github.com/sorayuki/obs-multi-rtmp/", PLUGIN_VERSION);

    obs_frontend_add_event_callback(
        [](enum obs_frontend_event event, void *private_data) {
            auto dock = static_cast<MultiOutputWidget*>(private_data);

            for(auto x: dock->GetAllPushWidgets())
                x->OnOBSEvent(event);

            if (event == obs_frontend_event::OBS_FRONTEND_EVENT_EXIT)
            {   
                dock->SaveConfig();
            }
            else if (event == obs_frontend_event::OBS_FRONTEND_EVENT_PROFILE_CHANGED)
            {
                dock->LoadConfig();
            }
        }, dock
    );

    return true;
}

const char *obs_module_description(void)
{
    return "Multiple RTMP Output Plugin";
}


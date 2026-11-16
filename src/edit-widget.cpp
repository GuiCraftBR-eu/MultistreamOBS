#include "edit-widget.h"
#include "output-config.h"
#include "json-util.hpp"
#include "obs.hpp"
#include <QMenu>
#include <QTabWidget>
#include <qevent.h>
#include <QComboBox>
#include <QSignalBlocker>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <regex>

#include "obs-properties-widget.h"
#include "helpers.h"
#include "protocols.h"
#include "platform-helpers.h"
#include "account-connect.h"

static std::optional<int> ParseStringToInt(const QString& str) {
    try {
        return std::stoi(tostdu8(str));
    }
    catch(...)
    {
        return {};
    }
}


static nlohmann::json to_json(obs_data* data) {
    if (!data)
        return {};
    auto jsonstr = obs_data_get_json(data);
    if (!jsonstr)
        return {};
    try {
        return nlohmann::json::parse(jsonstr);
    }
    catch (...) {
        return {};
    }
}

static OBSData from_json(nlohmann::json j) {
    if (j.type() == nlohmann::json::value_t::null)
        return OBSDataAutoRelease(obs_data_create()).Get();
    
    auto jstr = j.dump();
    OBSData r = obs_data_create_from_json(jstr.c_str());
    if (!r)
        return {};
    obs_data_release(r);
    return r;
}

static obs_properties* AddBF(obs_properties* p) {
    auto bfp = obs_properties_get(p, "bf");
    if (!bfp) {
        bfp = obs_properties_get(p, "bframes");
        if (!bfp)
            obs_properties_add_int(p, "bf", obs_module_text("BFrames"), 0, 16, 1);
    }
    return p;
}

class EditOutputWidgetImpl: public EditOutputWidget
{
    class ContentSizeTabWidget: public QTabWidget {
    public:
        using QTabWidget::QTabWidget;

        QSize sizeHint() const override {
            return CalculateTabSize([](QWidget* widget) { return widget->sizeHint(); });
        }

        QSize minimumSizeHint() const override {
            return CalculateTabSize([](QWidget* widget) { return widget->minimumSizeHint(); });
        }

    private:
        template<class T>
        QSize CalculateTabSize(T getWidgetSize) const {
            auto current = currentWidget();
            auto bar = tabBar();
            if (!current || !bar)
                return QTabWidget::sizeHint();

            auto pageSize = getWidgetSize(current);
            if (!pageSize.isValid())
                pageSize = current->sizeHint();

            auto tabSize = bar->sizeHint();
            auto result = pageSize;
            switch (tabPosition()) {
            case QTabWidget::West:
            case QTabWidget::East:
                result.rwidth() += tabSize.width();
                result.setHeight((std::max)(result.height(), tabSize.height()));
                break;
            case QTabWidget::North:
            case QTabWidget::South:
            default:
                result.setWidth((std::max)(result.width(), tabSize.width()));
                result.rheight() += tabSize.height();
                break;
            }

            const int frame = style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, this) * 2;
            result.rwidth() += frame;
            result.rheight() += frame;
            return result;
        }
    };

    class ContentSizeScrollArea: public QScrollArea {
    public:
        using QScrollArea::QScrollArea;

        QSize sizeHint() const override {
            auto hint = viewportSizeHint();
            if (!hint.isValid())
                return QScrollArea::sizeHint();

            const int frame = frameWidth() * 2;
            hint.rwidth() += frame;
            hint.rheight() += frame;
            return hint;
        }

        QSize minimumSizeHint() const override {
            return QScrollArea::minimumSizeHint();
        }

    protected:
        QSize viewportSizeHint() const override {
            auto content = widget();
            if (!content)
                return QScrollArea::viewportSizeHint();

            if (auto contentLayout = content->layout()) {
                auto hint = contentLayout->sizeHint();
                if (hint.isValid())
                    return hint;
            }

            auto hint = content->sizeHint();
            if (hint.isValid())
                return hint;

            return QScrollArea::viewportSizeHint();
        }
    };

    QWidget* container_;
    ContentSizeScrollArea* scroll_;
    ContentSizeTabWidget* outputSettingsTabs_ = nullptr;

    std::string targetid_;
    OutputTargetConfigPtr config_ = nullptr;

    QLineEdit* name_ = 0;
    bool resizeToContentPending_ = false;

    class PropertiesWidget: public QWidget {
    public:
        PropertiesWidget(std::function<void()> onContentSizeChanged, QWidget* parent)
            : QWidget(parent)
            , onContentSizeChanged_(std::move(onContentSizeChanged))
        {
            layout_ = new QGridLayout(this);
            layout_->setColumnStretch(0, 1);
            layout_->setContentsMargins(0, 0, 0, 0);
            layout_->setSizeConstraint(QLayout::SetMinAndMaxSize);
            setLayout(layout_);
        }

        // owner of props and settings is transfered to the created widget
        void UpdateProperties(OBSPropertiesPtr props, obs_data* settings) {
            ResetWid();
            wid_ = createPropertyWidget(std::move(props), settings_ = settings, this);
            layout_->addWidget(wid_, 0, 0, 1, 1, Qt::AlignTop);
            wid_->SetGeometryChangeCallback(onContentSizeChanged_);
            NotifyContentSizeChanged();
        }

        void ClearProperties() {
            ResetWid();
            placeholder_ = new QWidget(this);
            layout_->addWidget(placeholder_, 0, 0, 1, 1);
            NotifyContentSizeChanged();
        }

        nlohmann::json Save() {
            if (!wid_ || !settings_)
                return {};
            wid_->Save();
            return to_json(settings_);
        }

    private:
        QGridLayout* layout_ = nullptr;
        QPropertiesWidget* wid_ = nullptr;
        QWidget* placeholder_ = nullptr;
        OBSData settings_;
        std::function<void()> onContentSizeChanged_;

        void ResetWid() {
            if (wid_) {
                delete wid_;
                wid_ = nullptr;
            }
            if (placeholder_) {
                layout_->removeWidget(placeholder_);
                placeholder_->hide();
                placeholder_->deleteLater();
                placeholder_ = nullptr;
            }
        }

        void NotifyContentSizeChanged() {
            layout_->activate();
            updateGeometry();
            adjustSize();
            if (onContentSizeChanged_)
                onContentSizeChanged_();
        }
    };

    PropertiesWidget* serviceSettings_ = nullptr;
    QWidget* platformService_ = nullptr;
    QComboBox* twitchRegion_ = nullptr;
    QLineEdit* kickServer_ = nullptr;
    QLineEdit* streamKey_ = nullptr;
    QNetworkAccessManager* serviceNetwork_ = nullptr;
    QString platform_;
    PropertiesWidget* outputSettings_;
    std::string supported_video_encoders_;
    std::string supported_audio_encoders_;
    PropertiesWidget* videoEncoderSettings_;
    PropertiesWidget* audioEncoderSettings_;
   
    QComboBox* protocolSelector_ = 0;
    QComboBox* venc_ = 0;
    QComboBox* v_scene_ = 0;
    QComboBox* v_resolution_ = 0;
    QComboBox* v_fpsdenumerator_ = 0;
    QLabel* v_share_notify_ = 0;

    QComboBox* aenc_ = 0;
    QComboBox* a_mixer_ = 0;
    QComboBox* a_vod_track_ = nullptr;
    QLabel* a_share_notify_ = 0;

    QCheckBox* syncStart_ = 0;
    QCheckBox *syncStop_ = 0;

    std::vector<std::string> EnumEncodersByCodec(const char* codec)
    {
        if (!codec)
            return {};

        std::vector<std::string> result;
        int i = 0;
        for(;;)
        {
            const char* encid;
            if (!obs_enum_encoder_types(i++, &encid))
                break;
            auto caps = obs_get_encoder_caps(encid);
            if (caps & OBS_ENCODER_CAP_DEPRECATED)
                continue;
            auto enc_codec = obs_get_encoder_codec(encid);
            if (strcmp(enc_codec, codec) == 0)
                result.emplace_back(encid);
        }
        return result;
    }


    template<class T>
    static std::optional<std::string> DuplicateConfig(std::list<T>& list, std::string oldid) {
        auto cfg = FindById(list, oldid);
        if (!cfg) {
            return {};
        }
        auto& global = GlobalMultiOutputConfig();
        auto newid = GenerateId(global);
        auto newcfg = std::make_shared<typename T::element_type>(*cfg);
        newcfg->id = newid;
        list.push_back(newcfg);
        return newid;
    }


    void PopupShareMenu(bool isAudio) {
        auto menu = new QMenu(this);
        auto& global = GlobalMultiOutputConfig();

        {
            auto action = menu->addAction(obs_module_text("NoShare"));
            QObject::connect(action, &QAction::triggered, [=]() {
                auto& global = GlobalMultiOutputConfig();
                if (isAudio) {
                    if (!config_->audioConfig.has_value())
                        return;
                    auto newid = DuplicateConfig(global.audioConfig, *config_->audioConfig);
                    if (!newid.has_value()) {
                        config_->audioConfig.reset();
                        return;
                    }
                    config_->audioConfig = *newid;
                }
                else {
                    if (!config_->videoConfig.has_value())
                        return;
                    auto newid = DuplicateConfig(global.videoConfig, *config_->videoConfig);
                    if (!newid.has_value()) {
                        config_->videoConfig.reset();
                        return;
                    }
                    config_->videoConfig = *newid;
                }
            });
        }

        std::unordered_map<std::string, std::string> items;
        for(auto& target: global.targets) {
            if (isAudio) {
                if (target->audioConfig.has_value()) {
                    auto id = *target->audioConfig;
                    auto it = items.find(id);
                    if (it != items.end()) {
                        it->second += ", " + target->name;
                    }
                    else {
                        items.insert(std::make_pair(id, target->name));
                    }
                }
            } else {
                if (target->videoConfig.has_value()) {
                    auto id = *target->videoConfig;
                    auto it = items.find(id);
                    if (it != items.end()) {
                        it->second += ", " + target->name;
                    }
                    else {
                        items.insert(std::make_pair(id, target->name));
                    }
                }
            }
        }

        for (auto& item : items) {
            auto action = menu->addAction(QString::fromUtf8(item.second));
            QObject::connect(action, &QAction::triggered, [=]() {
                if (isAudio)
                    config_->audioConfig = item.first;
                else
                    config_->videoConfig = item.first;
            });
        }

        menu->exec(QCursor::pos());
    }

    void RefreshContentGeometry() {
        layout()->activate();
        if (outputSettingsTabs_) {
            outputSettingsTabs_->adjustSize();
            outputSettingsTabs_->updateGeometry();
        }
        container_->layout()->activate();
        container_->adjustSize();
        container_->updateGeometry();
        scroll_->adjustSize();
        scroll_->updateGeometry();
    }

    QSize CalculateMaximumDialogSize() const {
        QSize maxSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        auto currentScreen = screen();
        if (!currentScreen)
            return maxSize;

        auto available = currentScreen->availableGeometry().size();
        auto frame = frameSize() - size();
        return QSize(
            (std::max)(0, available.width() - frame.width()),
            (std::max)(0, available.height() - frame.height()));
    }

    void ApplyAutoDialogSize() {
        auto targetSize = sizeHint();
        if (!targetSize.isValid())
            return;

        auto maxSize = CalculateMaximumDialogSize();
        setMaximumSize(maxSize);
        resize(targetSize.boundedTo(maxSize).expandedTo(minimumSizeHint()));
    }

    void ScheduleResizeToContent() {
        if (resizeToContentPending_)
            return;

        resizeToContentPending_ = true;
        QTimer::singleShot(0, this, [this]() {
            resizeToContentPending_ = false;
            ResizeToContent();
        });
    }

    void ResizeToContent() {
        if (!layout() || !container_ || !container_->layout() || !scroll_)
            return;

        RefreshContentGeometry();
        ApplyAutoDialogSize();
    }

protected:
    void showEvent(QShowEvent* event) override {
        QDialog::showEvent(event);
        ScheduleResizeToContent();
    }

    QWidget* CreatePlatformService(QWidget* parent) {
        auto page = new QWidget(parent);
        auto form = new QFormLayout(page);
        auto currentServer = QString::fromStdString(GetJsonField<std::string>(config_->serviceParam, "server").value_or(""));
        const auto currentKey = QString::fromStdString(GetJsonField<std::string>(config_->serviceParam, "key").value_or(""));
        if (platform_ == "twitch") {
            twitchRegion_ = new QComboBox(page);
            twitchRegion_->addItem(obs_module_text("Twitch.Automatic"), "rtmps://ingest.global-contribute.live-video.net/app");
            // Snapshot of Twitch's public ingest list; refreshed from Twitch when online.
            const std::pair<const char*, const char*> regions[] = {
                {"South America (Sao Paulo)", "sae10"}, {"US East (N. Virginia)", "use10"},
                {"US East (Ohio)", "use20"}, {"Europe (Ireland)", "euw10"},
                {"Europe (Paris)", "euw30"}, {"Europe (Frankfurt)", "euc10"},
                {"US West (Oregon)", "usw20"}, {"Europe (Stockholm)", "eun10"},
                {"Asia Pacific (Sydney)", "aps20"}, {"Asia Pacific (Mumbai)", "aps30"},
                {"Asia Pacific (Singapore)", "aps10"}, {"Asia Pacific (Seoul)", "apn20"},
                {"Asia Pacific (Tokyo)", "apn10"}
            };
            for (const auto &region : regions)
                twitchRegion_->addItem(region.first, QString("rtmps://%1.contribute.live-video.net/app").arg(region.second));
            if (!currentServer.isEmpty() && twitchRegion_->findData(currentServer) < 0)
                twitchRegion_->addItem(obs_module_text("Twitch.CurrentServer"), currentServer);
            const int current = twitchRegion_->findData(currentServer);
            twitchRegion_->setCurrentIndex(current >= 0 ? current : 0);
            form->addRow(obs_module_text("Twitch.Server"), twitchRegion_);
            serviceNetwork_ = new QNetworkAccessManager(this);
            QNetworkRequest req{QUrl("https://ingest.twitch.tv/ingests")};
            req.setRawHeader("User-Agent", "MultistreamOBS/1.2");
            req.setTransferTimeout(15000);
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            auto reply = serviceNetwork_->get(req);
            connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64 total) {
                if (received > 1024 * 1024 || total > 1024 * 1024) reply->abort();
            });
            connect(reply, &QNetworkReply::finished, this, [this, reply] {
                const auto error = reply->error();
                const auto data = reply->readAll();
                reply->deleteLater();
                if (error != QNetworkReply::NoError || data.size() > 1024 * 1024 || !twitchRegion_) return;
                const auto list = QJsonDocument::fromJson(data).object().value("ingests").toArray();
                const auto selected = twitchRegion_->currentData().toString();
                QSignalBlocker blocker(twitchRegion_);
                for (const auto &item : list) {
                    const auto entry = item.toObject();
                    auto server = entry.value("url_template_secure").toString();
                    if (!server.endsWith("/{stream_key}")) continue;
                    server.chop(QStringLiteral("/{stream_key}").size());
                    const QUrl url(server);
                    const auto name = entry.value("name").toString().trimmed();
                    if (name.isEmpty() || url.scheme() != "rtmps" || url.host().isEmpty() ||
                        !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
                        !url.host().endsWith(".contribute.live-video.net")) continue;
                    const int known = twitchRegion_->findData(server);
                    if (known > 0) twitchRegion_->setItemText(known, name);
                    else if (known < 0) twitchRegion_->addItem(name, server);
                }
                const int index = twitchRegion_->findData(selected);
                if (index >= 0) twitchRegion_->setCurrentIndex(index);
                ScheduleResizeToContent();
            });
        } else {
            kickServer_ = new QLineEdit(NormalizeKickServer(currentServer), page);
            kickServer_->setPlaceholderText(obs_module_text("Kick.ServerHint"));
            form->addRow(obs_module_text("Kick.Server"), kickServer_);
        }
        streamKey_ = new QLineEdit(currentKey, page);
        streamKey_->setEchoMode(QLineEdit::Password);
        form->addRow(obs_module_text("Account.StreamKey"), streamKey_);
        auto connectButton = new QPushButton(obs_module_text("Account.Connect"), page);
        form->addRow(QString(), connectButton);
        connect(connectButton, &QPushButton::clicked, this, [this] {
            ConnectPlatformAccount(platform_, this, [this](QString server, QString key, QString account) {
                if (kickServer_ && !server.isEmpty()) kickServer_->setText(NormalizeKickServer(server));
                streamKey_->setText(key);
                QMessageBox::information(this, obs_module_text("Account.Title"),
                    QString::fromUtf8(obs_module_text("Account.Imported")).arg(account));
            });
        });
        return page;
    }

    QTabWidget* CreateOutputSettingsWidget(QWidget* parent) {
        auto tab = outputSettingsTabs_ = new ContentSizeTabWidget(parent);

        // service
        {
            if (platform_ == "twitch" || platform_ == "kick") {
                platformService_ = CreatePlatformService(tab);
                tab->addTab(platformService_, obs_module_text("Tab.Service"));
            } else {
                serviceSettings_ = new PropertiesWidget([this]() { ScheduleResizeToContent(); }, tab);
                updateServiceTab();
                tab->addTab(serviceSettings_, obs_module_text("Tab.Service"));
            }
        }

        // output
        {
            outputSettings_ = new PropertiesWidget([this]() { ScheduleResizeToContent(); }, tab);
            updateOutputTab();
            tab->addTab(outputSettings_, obs_module_text("Tab.Output"));
        }

        QObject::connect(tab, &QTabWidget::currentChanged, [this](int) {
            ScheduleResizeToContent();
        });

        tab->setCurrentIndex(0);

        return tab;
    }

    void updateServiceTab()
    {
        if (!serviceSettings_) return;
        auto protocol_info = GetProtocolInfos()->GetInfo(config_->protocol.c_str());
        assert(protocol_info);
        if (!protocol_info) {
        	blog(LOG_ERROR, TAG "Invalid protocol \"%s\", maybe broken config file.", config_->protocol.c_str());
            protocol_info = GetProtocolInfos()->GetList();
        }

        auto service = obs_service_create(protocol_info->serviceId, ("tmp_service_" + targetid_).c_str(), from_json(config_->serviceParam), nullptr);
        OBSPropertiesPtr properties(obs_service_properties(service), [service](obs_properties_t* properties) {
            if (properties)
                obs_properties_destroy(properties);
            obs_service_release(service);
        });
        serviceSettings_->UpdateProperties(
            std::move(properties),
            obs_service_get_settings(service));
    }

    void updateOutputTab()
    {
        auto protocol_info = GetProtocolInfos()->GetInfo(config_->protocol.c_str());
        assert(protocol_info);
        if (!protocol_info) {
        	blog(LOG_ERROR, TAG "Invalid protocol \"%s\", maybe broken config file.", config_->protocol.c_str());
            protocol_info = GetProtocolInfos()->GetList();
        }

        auto output = obs_output_create(protocol_info->outputId, ("tmp_output_" + targetid_).c_str(), from_json(config_->outputParam), nullptr);
        OBSPropertiesPtr properties(obs_output_properties(output), [output](obs_properties_t* properties) {
            if (properties)
                obs_properties_destroy(properties);
            obs_output_release(output);
        });
        outputSettings_->UpdateProperties(
            std::move(properties),
            obs_output_get_settings(output));
        supported_audio_encoders_ = obs_output_get_supported_audio_codecs(output);
        supported_video_encoders_ = obs_output_get_supported_video_codecs(output);

        if (aenc_ && venc_) {
            // LoadEncoders() no longer touches the config, sync UI explicitly.
            LoadEncoders();
            LoadConfig();
            UpdateUI();
        }
    }

public:
    EditOutputWidgetImpl(const std::string& targetid, QWidget* parent = 0)
        : QDialog(parent)
        , targetid_(targetid)
    {
        auto& global = GlobalMultiOutputConfig();
        config_ = FindById(global.targets, targetid_);
        if (config_ == nullptr) {
            return;
        }
        config_ = std::make_shared<OutputTargetConfig>(*config_);
        platform_ = QString::fromStdString(config_->platform).trimmed().toLower();
        if (platform_.isEmpty()) {
            const auto legacyName = QString::fromUtf8(config_->name).trimmed().toLower();
            if (legacyName == "twitch" || legacyName == "kick") {
                platform_ = legacyName;
                config_->platform = tostdu8(platform_);
            }
        }

        setWindowTitle(obs_module_text("StreamingSettings"));

        scroll_ = new ContentSizeScrollArea(this);
        scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAsNeeded);
        scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAsNeeded);
        scroll_->setSizePolicy(QSizePolicy::Policy::Preferred, QSizePolicy::Policy::Preferred);
        scroll_->setSizeAdjustPolicy(QScrollArea::SizeAdjustPolicy::AdjustToContents);

        container_ = new QWidget(scroll_);
        container_->setSizePolicy(QSizePolicy::Policy::Preferred, QSizePolicy::Policy::Preferred);

        auto layout = new QVBoxLayout(container_);
        layout->setAlignment(Qt::AlignTop);

        int currow = 0;
        {
            auto sublayout = new QGridLayout(container_);
            sublayout->setColumnStretch(0, 0);
            sublayout->setColumnStretch(1, 1);
            sublayout->addWidget(new QLabel(obs_module_text("StreamingName"), container_), 0, 0);
            sublayout->addWidget(name_ = new QLineEdit("", container_), 0, 1);
            sublayout->addWidget(new QLabel(obs_module_text("Protocol"), container_), 1, 0);
            sublayout->addWidget(protocolSelector_ = new QComboBox(container_), 1, 1);
            layout->addLayout(sublayout);
        }
        ++currow;

        {
            auto w = CreateOutputSettingsWidget(container_);
            layout->addWidget(w);
        }
        ++currow;
        {
            auto sub_grid = new QGridLayout();
            sub_grid->setColumnStretch(0, 1);
            sub_grid->setColumnStretch(1, 0);
            {
                {
                    auto gp = new QGroupBox(obs_module_text("VideoSettings"), container_);
                    sub_grid->addWidget(gp, 0, 0, 2, 1);
                    auto encLayout = new QGridLayout();
                    int currow = 0;
                    {
                        int curcol = 0;
                        auto sublayout = new QHBoxLayout();
                        sublayout->addWidget(v_share_notify_ = new QLabel(gp), 1);
                        auto shareButton = new QPushButton(obs_module_text("Btn.EncoderShare"), gp);
                        sublayout->addWidget(shareButton);
                        encLayout->addLayout(sublayout, currow, curcol++, 1, 2);

                        QObject::connect(shareButton, &QPushButton::clicked, [this]() {
                            SaveConfig();
                            PopupShareMenu(false);
                            LoadConfig();
                            UpdateUI();
                        });
                    }
                    ++currow;
                    {
                        int curcol = 0;
                        encLayout->addWidget(new QLabel(obs_module_text("Encoder"), gp), currow, curcol++);
                        encLayout->addWidget(venc_ = new QComboBox(gp), currow, curcol++);
                    }
                    ++currow;
                    {
                        int curcol = 0;
                        encLayout->addWidget(new QLabel(obs_module_text("Scene"), gp), currow, curcol++);
                        encLayout->addWidget(v_scene_ = new QComboBox(gp), currow, curcol++);
                    }
                    ++currow;
                    {
                        int curcol = 0;
                        encLayout->addWidget(new QLabel(obs_module_text("VideoResolution"), gp), currow, curcol++);
                        encLayout->addWidget(v_resolution_ = new QComboBox(gp), currow, curcol++);
                        v_resolution_->setEditable(true);
                        v_resolution_->setInsertPolicy(QComboBox::NoInsert);
                        v_resolution_->lineEdit()->setPlaceholderText(obs_module_text("SameAsOBSNow"));
                        v_resolution_->addItem("");
                        obs_video_info videoInfo{};
                        if (obs_get_video_info(&videoInfo)) {
                            // The same scale factors and alignment as OBS's output resolution picker.
                            const double scales[] = {1.0, 1.25, 1.0 / 0.75, 1.5, 1.0 / 0.6, 1.75,
                                                     2.0, 2.25, 2.5, 2.75, 3.0};
                            for (double scale : scales) {
                                uint32_t w = uint32_t(double(videoInfo.base_width) / scale) & ~uint32_t(3);
                                uint32_t h = uint32_t(double(videoInfo.base_height) / scale) & ~uint32_t(1);
                                const auto text = QString::number(w) + "x" + QString::number(h);
                                if (w && h && v_resolution_->findText(text) < 0)
                                    v_resolution_->addItem(text);
                            }
                        }
                    }
                    ++currow;
                    {
                        int curcol = 0;
                        encLayout->addWidget(new QLabel(obs_module_text("VideoFPSDenumerator"), gp), currow, curcol++);
                        encLayout->addWidget(v_fpsdenumerator_ = new QComboBox(gp), currow, curcol++);
                    }
                    ++currow;
                    {
                        encLayout->addWidget(videoEncoderSettings_ = new PropertiesWidget([this]() { ScheduleResizeToContent(); }, gp), currow, 0, 1, 2);
                    }
                    gp->setLayout(encLayout);
                }

                {
                    auto gp = new QGroupBox(obs_module_text("AudioSettings"), container_);
                    sub_grid->addWidget(gp, 0, 1, 1, 1);
                    auto encLayout = new QGridLayout();
                    int currow = 0;
                    {
                        int curcol = 0;
                        auto sublayout = new QHBoxLayout();
                        sublayout->addWidget(a_share_notify_ = new QLabel(gp), 1);
                        auto shareButton = new QPushButton(obs_module_text("Btn.EncoderShare"), gp);
                        sublayout->addWidget(shareButton);
                        encLayout->addLayout(sublayout, currow, curcol++, 1, 2);

                        QObject::connect(shareButton, &QPushButton::clicked, [this]() {
                            SaveConfig();
                            PopupShareMenu(true);
                            LoadConfig();
                            UpdateUI();
                        });
                    }
                    ++currow;
                    {
                        int curcol = 0;
                        encLayout->addWidget(new QLabel(obs_module_text("Encoder"), gp), currow, curcol++);
                        encLayout->addWidget(aenc_ = new QComboBox(gp), currow, curcol++);
                    }
                    ++currow;
                    const int NUM_MIXER_TRACKS = 6;
                    {
                        int curcol = 0;
                        encLayout->addWidget(new QLabel(obs_module_text("AudioMixerID"), gp), currow, curcol++);
                        encLayout->addWidget(a_mixer_ = new QComboBox(gp), currow, curcol++);

                        for(int i = 1; i <= NUM_MIXER_TRACKS; ++i)
                            a_mixer_->addItem(QString(std::to_string(i).c_str()), i - 1);
                    }
                    ++currow;
                    {
                        int curcol = 0;
                        encLayout->addWidget(new QLabel(obs_module_text("AudioVODTrack"), gp), currow, curcol++);
                        encLayout->addWidget(a_vod_track_ = new QComboBox(gp), currow, curcol++);

                        // Placeholder item for setting no VOD track
                        a_vod_track_->addItem("", -1);

                        for(int i = 1; i <= NUM_MIXER_TRACKS; ++i)
                            a_vod_track_->addItem(QString(std::to_string(i).c_str()), i - 1);
                    }
                    ++currow;
                    {
                        encLayout->addWidget(audioEncoderSettings_ = new PropertiesWidget([this]() { ScheduleResizeToContent(); }, gp), currow, 0, 1, 2);
                    }
                    gp->setLayout(encLayout);
                }

                {
                    auto gp = new QGroupBox(obs_module_text("OtherSettings"), container_);
                    sub_grid->addWidget(gp, 1, 1, 1, 1);
                    auto otherLayout = new QGridLayout();
                    otherLayout->addWidget(syncStart_ = new QCheckBox(obs_module_text("SyncStart"), gp), 0, 0);
                    otherLayout->addWidget(syncStop_ = new QCheckBox(obs_module_text("SyncStop"), gp), 1, 0);
                    gp->setLayout(otherLayout);
                }
            }
            layout->addLayout(sub_grid, 1);
        }
        ++currow;
        {
            auto okbtn = new QPushButton(obs_module_text("OK"), container_);
            QObject::connect(okbtn, &QPushButton::clicked, [this]() {
                SaveConfig();
                auto& global = GlobalMultiOutputConfig();
                auto it = FindById(global.targets, config_->id);
                if (it != nullptr) {
                    *it = *config_;
                }
                done(DialogCode::Accepted);
            });
            layout->addWidget(okbtn);
        }

        layout->setSizeConstraint(QLayout::SetMinAndMaxSize);
        container_->setLayout(layout);

        scroll_->setWidget(container_);
        scroll_->setWidgetResizable(false);

        auto fullLayout = new QGridLayout(this);
        fullLayout->setContentsMargins(0, 0, 0, 0);
        fullLayout->addWidget(scroll_, 0, 0);
        fullLayout->setRowStretch(0, 1);
        fullLayout->setColumnStretch(0, 1);
        setLayout(fullLayout);

        LoadProtocols();
        LoadFPSDenumerator();
        LoadEncoders();
        LoadScenes();

        LoadConfig();
        ConnectWidgetSignals();
        UpdateUI();
    }

    void ConnectWidgetSignals()
    {
        QObject::connect(venc_, (void (QComboBox::*)(int)) &QComboBox::currentIndexChanged, [this](){
            SaveConfig();
            LoadConfig();
            UpdateUI();
        });
        QObject::connect(aenc_, (void (QComboBox::*)(int)) &QComboBox::currentIndexChanged, [this](){
            SaveConfig();
            LoadConfig();
            UpdateUI();
        });

        QObject::connect(protocolSelector_, (void (QComboBox::*)(int)) &QComboBox::currentIndexChanged, [this](){
            blog(LOG_DEBUG, "Changing protocol to %s", tostdu8(protocolSelector_->currentText()).c_str());
            SaveConfig();
            LoadConfig();
            UpdateUI();
            updateServiceTab();
            updateOutputTab();
        });
    }

    void LoadProtocols()
    {
        auto protocol_cur = GetProtocolInfos()->GetList();
        while(protocol_cur->protocol) {
            if ((platform_ != "twitch" && platform_ != "kick") || std::string_view(protocol_cur->protocol) == "RTMP")
                protocolSelector_->addItem(protocol_cur->label, protocol_cur->protocol);
            ++protocol_cur;
        }
    }

    int max_video_encoder_placeholder_index = 0;
    int max_audio_encoder_placeholder_index = 0;
    void LoadEncoders()
    {
        auto ui_text = [](auto id) {
            return std::string(obs_encoder_get_display_name(id.c_str())) + " [" + id + "]";
        };

        std::regex sp(";");
        {
            // Video encoders
            // Rebuilding emits currentIndexChanged; the handler would then
            // rewrite the config id and break encoder sharing.
            QSignalBlocker blocker(venc_);
            auto old_venc = venc_->currentData();
            std::sregex_token_iterator it(supported_video_encoders_.begin(), supported_video_encoders_.end(), sp, -1), itend;
            venc_->clear();
	        venc_->addItem(obs_module_text("SameAsOBS"), OBS_STREAMING_ENC_PLACEHOLDER);
	        venc_->addItem(obs_module_text("SameAsOBSRecording"), OBS_RECORDING_ENC_PLACEHOLDER);
            max_video_encoder_placeholder_index = venc_->count() - 1;

	        while(it != itend) {
                for(auto x : EnumEncodersByCodec(it->str().c_str()))
                    venc_->addItem(ui_text(x).c_str(), x.c_str());
                ++it;
            }
            auto idx = venc_->findData(old_venc);
            if (idx >= 0)
                venc_->setCurrentIndex(idx);
        }

        {
            // Audio encoders
            QSignalBlocker blocker(aenc_);
            auto old_aenc = aenc_->currentData();
            std::sregex_token_iterator it(supported_audio_encoders_.begin(), supported_audio_encoders_.end(), sp, -1), itend;
            aenc_->clear();
	        aenc_->addItem(obs_module_text("SameAsOBS"), OBS_STREAMING_ENC_PLACEHOLDER);
	        aenc_->addItem(obs_module_text("SameAsOBSRecording"), OBS_RECORDING_ENC_PLACEHOLDER);
            max_audio_encoder_placeholder_index = aenc_->count() - 1;
            
            while(it != itend) {
                for(auto x : EnumEncodersByCodec(it->str().c_str()))
                    aenc_->addItem(ui_text(x).c_str(), x.c_str());
                ++it;
            }
            auto idx = aenc_->findData(old_aenc);
            if (idx >= 0)
                aenc_->setCurrentIndex(idx);
        }
    }

    void LoadFPSDenumerator()
    {
        static const char* scales[] = {
            nullptr,
            (const char*)u8"1x",
            (const char*)u8"½x", 
            (const char*)u8"⅓x", 
            (const char*)u8"¼x", 
        };
        obs_video_info ovi;
        obs_get_video_info(&ovi);
        for(int i = 1; i <= 4; ++i) {
            auto fps = 1.0 * ovi.fps_num / ovi.fps_den / i;
            auto fps_decimal_part = (int)(fps * 100) - (int)((int)fps * 100);
            std::string strfps;
            if (fps_decimal_part)
                strfps = std::to_string((int)fps) + "." + std::to_string(fps_decimal_part);
            else
                strfps = std::to_string((int)fps);
            auto text = std::string(scales[i]) + " (" + strfps + " FPS)";
            v_fpsdenumerator_->addItem(text.c_str(), i);
        }
    }

    void LoadScenes()
    {
        v_scene_->addItem(obs_module_text("SameAsOBSScene"), "");

        using EnumParam = std::vector<std::string>;
        std::vector<std::string> scenes;
        obs_enum_scenes([](void* p, obs_source_t* src) {
            auto scenes = (EnumParam*)p;
            auto name = obs_source_get_name(src);
            if (name && *name)
                scenes->emplace_back(name);
            return true;
        }, &scenes);

        for(auto& x: scenes) {
            v_scene_->addItem(x.c_str(), x.c_str());
        }
    }

    void UpdateUI()
    {
        auto newProtocolIndex = protocolSelector_->findData(QString::fromUtf8(config_->protocol));
        // fallback to RTMP if protocol is invalid - do we really need this line?
        if (newProtocolIndex == -1) newProtocolIndex = 0;
        protocolSelector_->setCurrentIndex(newProtocolIndex);

        auto ve = venc_->currentData();

        // Check for special cases where the encoder is grabbed from some other source (like streaming / recording)
        if (ve.isValid() && IsSpecialEncoder(ve.toString().toStdString()))
        {
            v_scene_->setEnabled(false);
            v_resolution_->setEnabled(false);
            v_fpsdenumerator_->setEnabled(false);
        }
        else
        {
            v_scene_->setEnabled(true);
            v_resolution_->setEnabled(true);
            v_fpsdenumerator_->setEnabled(true);
        }

        auto makeShareNotify = [&](auto& targets) {
            std::string sharedTargets;
            for (auto& x : targets) {
                if (!sharedTargets.empty())
                    sharedTargets += ", ";
                sharedTargets += x;
            }
            if (sharedTargets.empty())
                sharedTargets = obs_module_text("NoShare");
            return sharedTargets;
        };

        auto sharedVideoTargets = GetEncoderShareTargets(false);
        v_share_notify_->setText(QString::fromUtf8(obs_module_text("EncoderShare") + makeShareNotify(sharedVideoTargets)));

        auto ae = aenc_->currentData();
        if (ae.isValid() && IsSpecialEncoder(ae.toString().toStdString()))
        {
            a_mixer_->setEnabled(false);
            a_vod_track_->setEnabled(false);
        }
        else
        {
            a_mixer_->setEnabled(true);
            a_vod_track_->setEnabled(true);
        }

        auto sharedAudioTargets = GetEncoderShareTargets(true);
        a_share_notify_->setText(QString::fromUtf8(obs_module_text("EncoderShare") + makeShareNotify(sharedAudioTargets)));
    }

    std::vector<std::string> GetEncoderShareTargets(bool isAudio) {
        std::string configId;
        if (isAudio) {
            if (!config_->audioConfig.has_value())
                return { "OBS" };
            configId = *config_->audioConfig;
        }
        else {
            if (!config_->videoConfig.has_value())
                return { "OBS" };
            configId = *config_->videoConfig;
        }

        if (IsSpecialEncoder(configId)) {
		    return { obs_module_text("NoShare") };
	    }

        std::vector<std::string> ret;
        auto& global = GlobalMultiOutputConfig();
        for(auto& x: global.targets) {
            if (isAudio) {
                if (x->id != config_->id && x->audioConfig.has_value() && *x->audioConfig == configId)
                    ret.emplace_back(x->name);
            }
            else {
                if (x->id != config_->id && x->videoConfig.has_value() && *x->videoConfig == configId)
                    ret.emplace_back(x->name);
            }
        }
        return ret;
    }



    void SaveVideoConfig() {
        if (!config_->videoConfig.has_value() || IsSpecialEncoder(*config_->videoConfig))
            return;
        
        auto& global = GlobalMultiOutputConfig();
        auto it = FindById(global.videoConfig, *config_->videoConfig);
        if (it == nullptr) {
            auto new_item = global.videoConfig.emplace_back(new VideoEncoderConfig{});
            it = new_item;
        }
        it->id = *config_->videoConfig;
        it->encoderId = tostdu8(venc_->currentData().toString());

        if (v_scene_->currentIndex() > 0)
            it->outputScene = tostdu8(v_scene_->currentData().toString());
        else
            it->outputScene.reset();
        
        auto resolution = v_resolution_->currentText().trimmed().toUtf8();
        if (!resolution.isEmpty())
            it->resolution = resolution.constData();
        else
            it->resolution.reset();

        it->fpsDenumerator = v_fpsdenumerator_->currentData().toInt();
        
        it->encoderParams = videoEncoderSettings_->Save();
    }

    void SaveAudioConfig() {
        if (!config_->audioConfig.has_value() || IsSpecialEncoder(*config_->audioConfig))
            return;
        
        auto& global = GlobalMultiOutputConfig();
        auto it = FindById(global.audioConfig, *config_->audioConfig);
        if (it == nullptr) {
            auto new_item = global.audioConfig.emplace_back(new AudioEncoderConfig{});
            it = new_item;
        }
        it->id = *config_->audioConfig;
        it->encoderId = tostdu8(aenc_->currentData().toString());
        it->mixerId = a_mixer_->currentData().toInt();
        it->encoderParams = audioEncoderSettings_->Save();
        
        it->audioTracks.clear();

        // First item is a placeholder
        if (a_vod_track_->currentIndex() > 0) {
            auto audio_track = std::make_shared<AudioTrackConfig>();

            audio_track->mixer_track = a_vod_track_->currentData().toInt();
            audio_track->output_track = 1;
            it->audioTracks.push_back(audio_track);
        }
    }

    void SaveConfig()
    {
        auto& global = GlobalMultiOutputConfig();

        config_->name = tostdu8(name_->text());
        config_->protocol = tostdu8(protocolSelector_->itemData(protocolSelector_->currentIndex()).toString());
        config_->syncStart = syncStart_->isChecked();
        config_->syncStop = syncStop_->isChecked();
        config_->outputParam = outputSettings_->Save();
        if (platformService_) {
            if (!config_->serviceParam.is_object()) config_->serviceParam = nlohmann::json::object();
            const auto server = twitchRegion_ ? twitchRegion_->currentData().toString() : kickServer_->text().trimmed();
            config_->serviceParam["server"] = tostdu8(platform_ == "kick" ? NormalizeKickServer(server) : server);
            config_->serviceParam["key"] = tostdu8(streamKey_->text().trimmed());
        } else {
            config_->serviceParam = serviceSettings_->Save();
        }

        if (venc_->currentData().isValid()) {
            std::string encoderId = venc_->currentData().toString().toStdString();
            if (IsSpecialEncoder(encoderId))
                config_->videoConfig = encoderId;
	        else if (!config_->videoConfig.has_value() || IsSpecialEncoder(*config_->videoConfig)) {
                config_->videoConfig = GenerateId(global);
            }
		    SaveVideoConfig();
        }
        else
        {
            config_->videoConfig.reset();
        }

        if (aenc_->currentData().isValid()) {
            std::string encoderId = aenc_->currentData().toString().toStdString();

            if (IsSpecialEncoder(encoderId))
                config_->audioConfig = encoderId;
	        else if (!config_->audioConfig.has_value() || IsSpecialEncoder(*config_->audioConfig)) {
                config_->audioConfig = GenerateId(global);
            }
		    SaveAudioConfig();
        }
        else
        {
            config_->audioConfig.reset();
        }
    }


    void LoadTargetConfig(OutputTargetConfig& target) {
        name_->setText(QString::fromUtf8(target.name));
        auto protocolIndex = protocolSelector_->findData(QString::fromUtf8(target.protocol));
        if (protocolIndex < 0) protocolIndex = 0;
        protocolSelector_->setCurrentIndex(protocolIndex);
        syncStart_->setChecked(target.syncStart);
        syncStop_->setChecked(target.syncStop);
    }

    using IdOrVideoConfig = std::variant<std::string_view, VideoEncoderConfig*>;
    void LoadVideoConfig(IdOrVideoConfig idOrConfig) {
        VideoEncoderConfig* pconfig = nullptr;
        {
            QString encoderId;
            if (auto placeholder = std::get_if<std::string_view>(&idOrConfig)) {
                encoderId = QString::fromStdString(std::string(*placeholder));
            } else if (auto rconfig = std::get_if<VideoEncoderConfig*>(&idOrConfig)) {
                encoderId = QString::fromStdString((*rconfig)->encoderId);
                pconfig = *rconfig;
            } else {
                return;
            }

            auto idx = venc_->findData(encoderId);
            if (idx > max_video_encoder_placeholder_index)
                venc_->setCurrentIndex(idx);
            else {
                if (idx < 0)
                    idx = 0;
                venc_->setCurrentIndex(idx);
                videoEncoderSettings_->ClearProperties();
                return;
            }
        }

        assert(pconfig != nullptr);
        if (pconfig == nullptr)
            return;

        auto& config = *pconfig;

        if (config.outputScene.has_value()) {
            auto idx = v_scene_->findData(QString::fromUtf8(*config.outputScene));
            if (idx >= 0)
                v_scene_->setCurrentIndex(idx);
        } else {
            v_scene_->setCurrentIndex(0);
        }

        v_resolution_->setEditText(QString::fromUtf8(
            config.resolution.value_or("")
        ));

        {
            auto idx = v_fpsdenumerator_->findData(config.fpsDenumerator);
            if (idx < 0)
                idx = 0;
            v_fpsdenumerator_->setCurrentIndex(idx);
        }

        {
            auto encoder = obs_video_encoder_create(config.encoderId.c_str(), ("tmp_video_encoder_" + targetid_ + "_" + config.id).c_str(), from_json(config.encoderParams), nullptr);
            OBSPropertiesPtr properties(AddBF(obs_encoder_properties(encoder)), [encoder](obs_properties_t* properties) {
                if (properties)
                    obs_properties_destroy(properties);
                obs_encoder_release(encoder);
            });
            videoEncoderSettings_->UpdateProperties(
                std::move(properties),
                obs_encoder_get_settings(encoder)
            );
        }
    }

    using IdOrAudioConfig = std::variant<std::string_view, AudioEncoderConfig*>;
    void LoadAudioConfig(IdOrAudioConfig idOrConfig) {
        AudioEncoderConfig* pconfig = nullptr;
        {
            QString encoderId;
            if (auto placeholder = std::get_if<std::string_view>(&idOrConfig)) {
                encoderId = QString::fromStdString(std::string(*placeholder));
            } else if (auto rconfig = std::get_if<AudioEncoderConfig*>(&idOrConfig)) {
                encoderId = QString::fromStdString((*rconfig)->encoderId);
                pconfig = *rconfig;
            } else {
                return;
            }

            auto idx = aenc_->findData(encoderId);
            if (idx > max_audio_encoder_placeholder_index)
                aenc_->setCurrentIndex(idx);
            else {
                if (idx < 0)
                    idx = 0;
                aenc_->setCurrentIndex(idx);
                audioEncoderSettings_->ClearProperties();
                return;
            }
        }

        assert(pconfig != nullptr);
        if (pconfig == nullptr)
            return;

        auto& config = *pconfig;

        {
            auto idx = a_mixer_->findData(config.mixerId);
            if (idx < 0)
                idx = 0;
            a_mixer_->setCurrentIndex(idx);
        }

        {
            if (config.audioTracks.empty()) {
                a_vod_track_->setCurrentIndex(0);
            } else {
                // NOTE: at this time we assume only one audio track. This needs to be updated
                // if we ever actually add UI support for more.
                auto idx = a_vod_track_->findData(config.audioTracks.front()->mixer_track);
                if (idx < 0)
                    idx = 0;
                // This is a combobox index, not the track index
                a_vod_track_->setCurrentIndex(idx);
            }
        }

        {
            auto encoder = obs_audio_encoder_create(config.encoderId.c_str(), ("tmp_audio_encoder_" + targetid_ + "_" + config.id).c_str(), from_json(config.encoderParams), 0, nullptr);
            OBSPropertiesPtr properties(obs_encoder_properties(encoder), [encoder](obs_properties_t* properties) {
                if (properties)
                    obs_properties_destroy(properties);
                obs_encoder_release(encoder);
            });
            audioEncoderSettings_->UpdateProperties(
                std::move(properties),
                obs_encoder_get_settings(encoder)
            );
        }
    }

    void LoadConfig() {
        auto& global = GlobalMultiOutputConfig();

        if (config_->name.empty())
            config_->name = obs_module_text("NewStreaming");

        LoadTargetConfig(*config_);

        auto videoConfigId = config_->videoConfig.value_or(OBS_STREAMING_ENC_PLACEHOLDER);
        if (IsSpecialEncoder(videoConfigId)) {
            LoadVideoConfig(videoConfigId);
        } else {
            auto it = FindById(global.videoConfig, *config_->videoConfig);
            if (it != nullptr) {
                LoadVideoConfig(&*it);
            }
        }

        auto audioConfigId = config_->audioConfig.value_or(OBS_STREAMING_ENC_PLACEHOLDER);
        if (IsSpecialEncoder(audioConfigId)) {
            LoadAudioConfig(audioConfigId);
        } else {
            auto it = FindById(global.audioConfig, *config_->audioConfig);
            if (it != nullptr) {
                LoadAudioConfig(&*it);
            }
        }
        
        UpdateUI();
    }
};


EditOutputWidget* createEditOutputWidget(const std::string& targetid, QWidget* parent) {
    return new EditOutputWidgetImpl(targetid, parent);
}

// 无 GUI 的 IOrganizer 实现：只提供游戏插件实际需要的部分，其余返回安全默认值。
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <typeindex>
#include <vector>

#include <QDir>
#include <QVariant>
#include <uibase/game_features/igamefeatures.h>
#include <uibase/iplugingame.h>
#include <uibase/imoinfo.h>
#include <uibase/iprofile.h>
#include <uibase/ipluginlist.h>

namespace mogame {

class FeatureRegistry final : public MOBase::IGameFeatures {
public:
    bool registerFeature(QStringList const&, std::shared_ptr<MOBase::GameFeature> f, int prio, bool replace) override;
    bool registerFeature(MOBase::IPluginGame*, std::shared_ptr<MOBase::GameFeature> f, int prio, bool replace) override;
    bool registerFeature(std::shared_ptr<MOBase::GameFeature> f, int prio, bool replace) override;
    bool unregisterFeature(std::shared_ptr<MOBase::GameFeature> f) override;

protected:
    std::shared_ptr<MOBase::GameFeature> gameFeatureImpl(std::type_info const& info) const override;
    int unregisterFeaturesImpl(std::type_info const& info) override;

private:
    struct Entry { std::shared_ptr<MOBase::GameFeature> feature; int priority; };
    std::map<std::type_index, std::vector<Entry>> features_;
};

class Profile final : public MOBase::IProfile {
public:
    Profile(QString name, QString path, QString documentsDir)
        : name_(std::move(name)), path_(std::move(path)), documents_(std::move(documentsDir)) {}
    QString name() const override { return name_; }
    QString absolutePath() const override { return path_; }
    bool localSavesEnabled() const override { return false; }
    bool localSettingsEnabled() const override { return false; }
    bool invalidationActive(bool* supported) const override { if (supported) *supported = false; return false; }
    QString absoluteIniFilePath(QString iniFile) const override;

private:
    QString name_, path_, documents_;
};

class PluginList final : public MOBase::IPluginList {
public:
    struct Item { QString name; PluginStates state = STATE_INACTIVE; };
    void set(std::vector<Item> items) { items_ = std::move(items); }
    QStringList pluginNames() const override;
    PluginStates state(const QString& name) const override;
    void setState(const QString& name, PluginStates state) override;
    int priority(const QString& name) const override { return loadOrder(name); }
    bool setPriority(const QString&, int) override { return false; }
    int loadOrder(const QString& name) const override;
    void setLoadOrder(const QStringList&) override {}
    bool isMaster(const QString&) const override { return false; }
    QStringList masters(const QString&) const override { return {}; }
    QString origin(const QString&) const override { return {}; }
    bool onRefreshed(const std::function<void()>&) override { return true; }
    bool onPluginMoved(const std::function<void(const QString&, int, int)>&) override { return true; }
    bool onPluginStateChanged(const std::function<void(const std::map<QString, PluginStates>&)>&) override { return true; }
    bool hasMasterExtension(const QString& n) const override;
    bool hasLightExtension(const QString& n) const override;
    bool isMasterFlagged(const QString&) const override { return false; }
    bool isMediumFlagged(const QString&) const override { return false; }
    bool isLightFlagged(const QString&) const override { return false; }
    bool isBlueprintFlagged(const QString&) const override { return false; }
    bool hasNoRecords(const QString&) const override { return false; }
    int formVersion(const QString&) const override { return 0; }
    float headerVersion(const QString&) const override { return 0.f; }
    QString author(const QString&) const override { return {}; }
    QString description(const QString&) const override { return {}; }

private:
    std::vector<Item> items_;
};

struct OrganizerConfig {
    QString basePath, profileName, profilePath, modsPath, downloadsPath, overwritePath, documentsDir;
};

class Organizer final : public MOBase::IOrganizer {
public:
    explicit Organizer(OrganizerConfig cfg);
    void setGame(MOBase::IPluginGame* g) { game_ = g; }
    PluginList& plugins() { return plugins_; }
    // 运行 onAboutToRun 回调（游戏插件在此准备 ini）。
    bool runAboutToRun(const QString& binary);
    // 更新 profile/实例路径；空字符串表示保持不变。
    void setProfile(const QString& name, const QString& profilePath, const QString& modsPath,
                    const QString& overwritePath, const QString& basePath);

    // --- IOrganizer ---
    MOBase::IModRepositoryBridge* createNexusBridge() const override { return nullptr; }
    QString instanceName() const override { return QStringLiteral("mo-linux"); }
    QString profileName() const override { return cfg_.profileName; }
    QString profilePath() const override { return cfg_.profilePath; }
    QString downloadsPath() const override { return cfg_.downloadsPath; }
    QString overwritePath() const override { return cfg_.overwritePath; }
    QString basePath() const override { return cfg_.basePath; }
    QString modsPath() const override { return cfg_.modsPath; }
    MOBase::VersionInfo appVersion() const override { return MOBase::VersionInfo(2, 5, 2); }
    MOBase::Version version() const override { return MOBase::Version(2, 5, 2); }
    MOBase::IModInterface* createMod(MOBase::GuessedValue<QString>&) override { return nullptr; }
    MOBase::IPluginGame* getGame(const QString&) const override { return game_; }
    void modDataChanged(MOBase::IModInterface*) override {}
    bool isPluginEnabled(MOBase::IPlugin*) const override { return true; }
    bool isPluginEnabled(QString const&) const override { return true; }
    QVariant pluginSetting(const QString&, const QString&) const override { return {}; }
    void setPluginSetting(const QString&, const QString&, const QVariant&) override {}
    QVariant persistent(const QString&, const QString&, const QVariant& def) const override { return def; }
    void setPersistent(const QString&, const QString&, const QVariant&, bool) override {}
    QString pluginDataPath() const override { return {}; }
    MOBase::IModInterface* installMod(const QString&, const QString&) override { return nullptr; }
    QString resolvePath(const QString& f) const override { return f; }
    QStringList listDirectories(const QString&) const override { return {}; }
    QStringList findFiles(const QString&, const std::function<bool(const QString&)>&) const override { return {}; }
    QStringList findFiles(const QString&, const QStringList&) const override { return {}; }
    QStringList getFileOrigins(const QString&) const override { return {}; }
    QList<FileInfo> findFileInfos(const QString&, const std::function<bool(const FileInfo&)>&) const override { return {}; }
    std::shared_ptr<const MOBase::IFileTree> virtualFileTree() const override { return nullptr; }
    MOBase::IInstanceManager* instanceManager() const override { return nullptr; }
    MOBase::IDownloadManager* downloadManager() const override { return nullptr; }
    MOBase::IPluginList* pluginList() const override { return const_cast<PluginList*>(&plugins_); }
    MOBase::IModList* modList() const override { return nullptr; }
    MOBase::IExecutablesList* executablesList() const override { return nullptr; }
    std::shared_ptr<MOBase::IProfile> profile() const override { return profile_; }
    QStringList profileNames() const override { return {cfg_.profileName}; }
    std::shared_ptr<const MOBase::IProfile> getProfile(const QString& n) const override { return n == cfg_.profileName ? profile_ : nullptr; }
    MOBase::IGameFeatures* gameFeatures() const override { return const_cast<FeatureRegistry*>(&features_); }
    HANDLE startApplication(const QString&, const QStringList&, const QString&, const QString&, const QString&, bool) override { return INVALID_HANDLE_VALUE; }
    bool waitForApplication(HANDLE, bool, LPDWORD) const override { return false; }
    void refresh(bool) override {}
    MOBase::IPluginGame const* managedGame() const override { return game_; }
    bool onAboutToRun(const std::function<bool(const QString&)>& f) override { aboutToRun_.push_back(f); return true; }
    bool onAboutToRun(const std::function<bool(const QString&, const QDir&, const QString&)>&) override { return true; }
    bool onFinishedRun(const std::function<void(const QString&, unsigned int)>&) override { return true; }
    bool onUserInterfaceInitialized(std::function<void(QMainWindow*)> const&) override { return true; }
    bool onNextRefresh(std::function<void()> const&, bool) override { return true; }
    bool onProfileCreated(std::function<void(MOBase::IProfile*)> const&) override { return true; }
    bool onProfileRenamed(std::function<void(MOBase::IProfile*, QString const&, QString const&)> const&) override { return true; }
    bool onProfileRemoved(std::function<void(QString const&)> const&) override { return true; }
    bool onProfileChanged(std::function<void(MOBase::IProfile*, MOBase::IProfile*)> const&) override { return true; }
    bool onPluginSettingChanged(std::function<void(QString const&, const QString&, const QVariant&, const QVariant&)> const&) override { return true; }
    bool onPluginEnabled(std::function<void(const MOBase::IPlugin*)> const&) override { return true; }
    bool onPluginEnabled(const QString&, std::function<void()> const&) override { return true; }
    bool onPluginDisabled(std::function<void(const MOBase::IPlugin*)> const&) override { return true; }
    bool onPluginDisabled(const QString&, std::function<void()> const&) override { return true; }

private:
    OrganizerConfig cfg_;
    MOBase::IPluginGame* game_ = nullptr;
    FeatureRegistry features_;
    PluginList plugins_;
    std::shared_ptr<Profile> profile_;
    std::vector<std::function<bool(const QString&)>> aboutToRun_;
};

}  // namespace mogame

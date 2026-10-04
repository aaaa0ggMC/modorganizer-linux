#include "fake_organizer.hpp"

#include <uibase/game_features/game_feature.h>

namespace mogame {

// ---- FeatureRegistry ---------------------------------------------------
bool FeatureRegistry::registerFeature(QStringList const&, std::shared_ptr<MOBase::GameFeature> f, int prio, bool replace) {
    return registerFeature(std::move(f), prio, replace);
}
bool FeatureRegistry::registerFeature(MOBase::IPluginGame*, std::shared_ptr<MOBase::GameFeature> f, int prio, bool replace) {
    return registerFeature(std::move(f), prio, replace);
}
bool FeatureRegistry::registerFeature(std::shared_ptr<MOBase::GameFeature> f, int prio, bool replace) {
    if (!f) return false;
    auto& list = features_[std::type_index(f->typeInfo())];
    if (replace) list.clear();
    list.push_back({std::move(f), prio});
    return true;
}
bool FeatureRegistry::unregisterFeature(std::shared_ptr<MOBase::GameFeature> f) {
    if (!f) return false;
    auto it = features_.find(std::type_index(f->typeInfo()));
    if (it == features_.end()) return false;
    auto& l = it->second;
    auto n = std::erase_if(l, [&](const Entry& e) { return e.feature == f; });
    return n > 0;
}
std::shared_ptr<MOBase::GameFeature> FeatureRegistry::gameFeatureImpl(std::type_info const& info) const {
    auto it = features_.find(std::type_index(info));
    if (it == features_.end() || it->second.empty()) return nullptr;
    const Entry* best = &it->second.front();
    for (const auto& e : it->second)
        if (e.priority > best->priority) best = &e;
    return best->feature;
}
int FeatureRegistry::unregisterFeaturesImpl(std::type_info const& info) {
    auto it = features_.find(std::type_index(info));
    if (it == features_.end()) return 0;
    int n = static_cast<int>(it->second.size());
    features_.erase(it);
    return n;
}

// ---- Profile -----------------------------------------------------------
QString Profile::absoluteIniFilePath(QString iniFile) const {
    // 不启用本地设置：ini 位于游戏文档目录；绝对路径原样返回。
    if (QDir::isAbsolutePath(iniFile)) return iniFile;
    return QDir(documents_).absoluteFilePath(iniFile);
}

// ---- PluginList --------------------------------------------------------
QStringList PluginList::pluginNames() const {
    QStringList r;
    for (const auto& i : items_) r << i.name;
    return r;
}
MOBase::IPluginList::PluginStates PluginList::state(const QString& name) const {
    for (const auto& i : items_)
        if (i.name.compare(name, Qt::CaseInsensitive) == 0) return i.state;
    return STATE_MISSING;
}
void PluginList::setState(const QString& name, PluginStates st) {
    for (auto& i : items_)
        if (i.name.compare(name, Qt::CaseInsensitive) == 0) i.state = st;
}
int PluginList::loadOrder(const QString& name) const {
    int idx = 0;
    for (const auto& i : items_) {
        if (i.name.compare(name, Qt::CaseInsensitive) == 0) return idx;
        ++idx;
    }
    return -1;
}
bool PluginList::hasMasterExtension(const QString& n) const {
    return n.endsWith(".esm", Qt::CaseInsensitive);
}
bool PluginList::hasLightExtension(const QString& n) const {
    return n.endsWith(".esl", Qt::CaseInsensitive);
}

// ---- Organizer ---------------------------------------------------------
Organizer::Organizer(OrganizerConfig cfg) : cfg_(std::move(cfg)) {
    profile_ = std::make_shared<Profile>(cfg_.profileName, cfg_.profilePath, cfg_.documentsDir);
}
void Organizer::setProfile(const QString& name, const QString& profilePath, const QString& modsPath,
                           const QString& overwritePath, const QString& basePath) {
    if (!name.isEmpty()) cfg_.profileName = name;
    if (!profilePath.isEmpty()) cfg_.profilePath = profilePath;
    if (!modsPath.isEmpty()) cfg_.modsPath = modsPath;
    if (!overwritePath.isEmpty()) cfg_.overwritePath = overwritePath;
    if (!basePath.isEmpty()) cfg_.basePath = basePath;
    profile_ = std::make_shared<Profile>(cfg_.profileName, cfg_.profilePath, cfg_.documentsDir);
}

bool Organizer::runAboutToRun(const QString& binary) {
    bool ok = true;
    for (auto& f : aboutToRun_) ok = f(binary) && ok;
    return ok;
}

}  // namespace mogame

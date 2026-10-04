#include "mo_game.h"

#include <cstdlib>
#include <cstring>
#include <memory>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <uibase/executableinfo.h>
#include <uibase/ipluginfilemapper.h>
#include <uibase/game_features/dataarchives.h>
#include <uibase/game_features/scriptextender.h>
#include <uibase/log.h>

#include "fake_organizer.hpp"
#include "gameskyrimse.h"
#include "winshim_internal.h"

struct mo_game {
    std::unique_ptr<mogame::Organizer> organizer;
    std::unique_ptr<GameSkyrimSE> game;
    QString gameDir;
};

namespace {

char* dup_str(const QString& s) {
    QByteArray b = s.toUtf8();
    char* p = static_cast<char*>(std::malloc(b.size() + 1));
    std::memcpy(p, b.constData(), b.size() + 1);
    return p;
}

void set_err(char** err, const QString& msg) {
    if (err) *err = dup_str(msg);
}

void ensure_qt() {
    static int argc = 1;
    static char arg0[] = "mo-linux";
    static char* argv[] = {arg0, nullptr};
    if (!QCoreApplication::instance()) {
        new QCoreApplication(argc, argv);  // 进程内单例，随进程结束
        QCoreApplication::setApplicationName("mo-linux");
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        MOBase::log::LoggerConfiguration conf;
        conf.name = "mo-game";
        conf.maxLevel = MOBase::log::Levels::Warning;
        MOBase::log::createDefault(conf);
    }
}

QJsonArray to_json(const QStringList& l) {
    QJsonArray a;
    for (const auto& s : l) a.append(s);
    return a;
}

}  // namespace

extern "C" {

mo_game* mo_game_create(const char* game_id, const char* game_dir, const char* wine_prefix,
                        const char* wine_user, char** err) {
    if (err) *err = nullptr;
    if (!game_id || QString::fromUtf8(game_id) != "skyrimse") {
        set_err(err, QStringLiteral("unsupported game id (only 'skyrimse')"));
        return nullptr;
    }
    mol_shim_configure(wine_prefix, wine_user);
    ensure_qt();
    try {
        auto g = std::make_unique<mo_game>();
        mogame::OrganizerConfig cfg;
        cfg.profileName = QStringLiteral("Default");
        g->organizer = std::make_unique<mogame::Organizer>(cfg);
        g->game = std::make_unique<GameSkyrimSE>();
        g->organizer->setGame(g->game.get());
        if (!g->game->init(g->organizer.get())) {
            set_err(err, QStringLiteral("game plugin init failed"));
            return nullptr;
        }
        if (game_dir && *game_dir) {
            g->gameDir = QString::fromUtf8(game_dir);
            g->game->setGamePath(g->gameDir);
        }
        return g.release();
    } catch (const std::exception& e) {
        set_err(err, QString::fromUtf8(e.what()));
        return nullptr;
    }
}

void mo_game_destroy(mo_game* g) { delete g; }

char* mo_game_info_json(mo_game* g) {
    if (!g) return nullptr;
    MOBase::IPluginGame& game = *g->game;  // 经基类接口访问（GameSkyrimSE 把部分接口改为 protected）
    QJsonObject o;
    o["name"] = game.gameName();
    o["shortName"] = game.gameShortName();
    o["steamAppId"] = game.steamAPPId();
    o["binaryName"] = game.binaryName();
    o["launcherName"] = game.getLauncherName();
    o["nexusGameId"] = game.nexusGameID();
    o["gameDirectory"] = game.gameDirectory().absolutePath();
    o["dataDirectory"] = game.dataDirectory().absolutePath();
    o["documentsDirectory"] = game.documentsDirectory().absolutePath();
    o["savesDirectory"] = game.savesDirectory().absolutePath();
    o["installed"] = game.isInstalled();
    o["looksValid"] = game.looksValid(game.gameDirectory());
    o["version"] = game.gameVersion();
    o["primaryPlugins"] = to_json(game.primaryPlugins());
    o["dlcPlugins"] = to_json(game.DLCPlugins());
    o["ccPlugins"] = to_json(game.CCPlugins());
    o["iniFiles"] = to_json(game.iniFiles());
    o["variants"] = to_json(game.gameVariants());
    QJsonArray exes;
    for (const auto& e : game.executables()) {
        QJsonObject x;
        x["title"] = e.title();
        x["binary"] = e.binary().absoluteFilePath();
        x["arguments"] = to_json(e.arguments());
        x["workingDirectory"] = e.workingDirectory().absolutePath();
        exes.append(x);
    }
    o["executables"] = exes;
    if (auto se = g->organizer->gameFeatures()->gameFeature<MOBase::ScriptExtender>()) {
        QJsonObject s;
        s["name"] = se->BinaryName();
        s["loader"] = se->loaderName();
        s["loaderPath"] = se->loaderPath();
        s["installed"] = se->isInstalled();
        s["version"] = se->getExtenderVersion();
        s["savegameExtension"] = se->savegameExtension();
        o["scriptExtender"] = s;
    }
    return dup_str(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

int mo_game_set_profile(mo_game* g, const char* name, const char* profile_dir, const char* mods_dir,
                        const char* overwrite_dir, const char* base_dir) {
    if (!g) return 1;
    auto q = [](const char* s) { return s ? QString::fromUtf8(s) : QString(); };
    g->organizer->setProfile(q(name), q(profile_dir), q(mods_dir), q(overwrite_dir), q(base_dir));
    return 0;
}

char* mo_game_mappings_json(mo_game* g) {
    if (!g) return nullptr;
    QJsonArray arr;
    if (auto* mapper = dynamic_cast<MOBase::IPluginFileMapper*>(g->game.get())) {
        for (const auto& m : mapper->mappings()) {
            QJsonObject o;
            o["source"] = m.source;
            o["destination"] = m.destination;
            o["isDirectory"] = m.isDirectory;
            o["createTarget"] = m.createTarget;
            arr.append(o);
        }
    }
    return dup_str(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

int mo_game_initialize_profile(mo_game* g, const char* dir, unsigned flags, char** err) {
    if (err) *err = nullptr;
    if (!g || !dir) {
        set_err(err, QStringLiteral("invalid argument"));
        return 1;
    }
    try {
        MOBase::IPluginGame& game = *g->game;
        game.initializeProfile(QDir(QString::fromUtf8(dir)), MOBase::IPluginGame::ProfileSettings(flags));
        return 0;
    } catch (const std::exception& e) {
        set_err(err, QString::fromUtf8(e.what()));
        return 2;
    }
}

int mo_game_about_to_run(mo_game* g, const char* binary) {
    if (!g) return 1;
    // 文档目录未知（没有前缀）时上游的 prepareIni 会把 ini 写到「当前目录」——宁可跳过，也不要在用户的目录里乱写文件。
    {
        const MOBase::IPluginGame& pg = *g->game;
        const QString docs = pg.documentsDirectory().absolutePath();
        if (docs.isEmpty() || !QDir::isAbsolutePath(docs)) return 0;
    }
    return g->organizer->runAboutToRun(QString::fromUtf8(binary ? binary : "")) ? 0 : 1;
}

void mo_free(char* p) { std::free(p); }

}  // extern "C"

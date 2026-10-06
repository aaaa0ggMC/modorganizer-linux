/* 测试用的假游戏层（libmo-game.so 的 C ABI）：没有 Qt/上游源码也能测「依赖游戏版本」的逻辑。
 * 版本取环境变量 FAKE_GAME_VERSION（默认 1.7.104.0）。只给 doctor/next/collection 用到的字段。 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

typedef struct { char dir[4096]; } Game;

EXPORT void* mo_game_create(const char* id, const char* dir, const char* prefix, const char* user, char** err) {
    (void)id; (void)prefix; (void)user;
    if (!dir) { if (err) *err = strdup("no game directory"); return NULL; }
    Game* g = calloc(1, sizeof *g);
    snprintf(g->dir, sizeof g->dir, "%s", dir);
    return g;
}
EXPORT void mo_game_destroy(void* g) { free(g); }
EXPORT void mo_free(char* p) { free(p); }
EXPORT char* mo_game_info_json(void* gp) {
    const Game* g = gp;
    const char* v = getenv("FAKE_GAME_VERSION");
    char* out = NULL;
    if (asprintf(&out,
                 "{\"name\":\"Skyrim Special Edition\",\"shortName\":\"SkyrimSE\",\"steamAppId\":\"489830\",\"binaryName\":\"SkyrimSE.exe\","
                 "\"launcherName\":\"SkyrimSELauncher.exe\",\"nexusGameId\":1704,\"gameDirectory\":\"%s\",\"dataDirectory\":\"%s/Data\","
                 "\"documentsDirectory\":\"\",\"savesDirectory\":\"\",\"installed\":true,\"looksValid\":true,\"version\":\"%s\","
                 "\"primaryPlugins\":[\"Skyrim.esm\"],\"dlcPlugins\":[],\"ccPlugins\":[],\"iniFiles\":[],\"variants\":[],\"executables\":[],"
                 "\"scriptExtender\":{\"name\":\"SKSE64\",\"loader\":\"skse64_loader.exe\",\"loaderPath\":\"%s/skse64_loader.exe\",\"installed\":false,"
                 "\"version\":\"\",\"savegameExtension\":\"skse\"}}",
                 g->dir, g->dir, v && *v ? v : "1.7.104.0", g->dir) < 0)
        return NULL;
    return out;
}
EXPORT int mo_game_set_profile(void* g, const char* a, const char* b, const char* c, const char* d, const char* e) {
    (void)g; (void)a; (void)b; (void)c; (void)d; (void)e;
    return 0;
}
EXPORT char* mo_game_mappings_json(void* g) { (void)g; return strdup("[]"); }
EXPORT int mo_game_initialize_profile(void* g, const char* dir, unsigned flags, char** err) { (void)g; (void)dir; (void)flags; (void)err; return 0; }
EXPORT int mo_game_about_to_run(void* g, const char* bin) { (void)g; (void)bin; return 0; }

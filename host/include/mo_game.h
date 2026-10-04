/* libmo-game 的 C ABI。Qt/uibase/上游游戏插件全部隐藏在 .so 内部；
 * 数据一律以 UTF-8 JSON 字符串出入，返回的字符串用 mo_free 释放。 */
#ifndef MO_GAME_H
#define MO_GAME_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mo_game mo_game;

/* 创建游戏适配器。
 *  game_id      "skyrimse"（目前仅支持这一个）
 *  game_dir     游戏安装目录（Steam 的 steamapps/common/...）；可为 NULL（则只能查询静态信息）
 *  wine_prefix  Wine/Proton 前缀（含 drive_c 的目录）；可为 NULL
 *  wine_user    前缀内的用户名；NULL 则 "steamuser"
 *  err          失败时写入错误信息（需 mo_free）；可为 NULL
 * 成功返回非 NULL。 */
mo_game* mo_game_create(const char* game_id, const char* game_dir, const char* wine_prefix,
                        const char* wine_user, char** err);
void mo_game_destroy(mo_game* g);

/* 游戏静态/派生信息（目录、可执行文件、基础插件、ini 文件、SKSE 信息……）。 */
char* mo_game_info_json(mo_game* g);

void mo_free(char* p);

#ifdef __cplusplus
}
#endif
#endif

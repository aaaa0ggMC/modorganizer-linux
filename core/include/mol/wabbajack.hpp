#pragma once
// Wabbajack 整合包（.wabbajack）：解析清单、判断我们能装多少、（见 wabbajack_install）按清单重建实例目录。
//
// .wabbajack = zip：`modlist`（JSON：Archives[] + Directives[]）、`modlist-image.png`、以及以 GUID 命名的内联数据/补丁。
// Archives[]：{Hash(xxh64 的 base64), Name, Size, State{$type,…}}；State 决定怎么下载（Nexus / Http / WabbajackCDN / 游戏文件 / 手动 …）。
// Directives[]：怎么用这些压缩包拼出最终的实例目录：
//   FromArchive            从压缩包里（可嵌套）取一个文件放到 To
//   PatchedFromArchive     取文件后用 OctoDiff 补丁（PatchID）打补丁
//   InlineFile             清单自带的数据（SourceDataID）原样写出
//   RemappedInlineFile     同上，但写出前把路径占位符替换成本机路径
//   CreateBSA              用其它指令产出的文件打包 BSA（尚未支持）
//   TransformedTexture     重编码贴图（尚未支持）
//   MergedPatch / 其它      尚未支持
// 路径用反斜杠、大小写不敏感。
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "mol/error.hpp"
#include "mol/instance.hpp"
#include "mol/nexus.hpp"

namespace mol::wabbajack {

struct Source {
    std::string kind;       // nexus | http | cdn | gamefile | manual | mega | gdrive | mediafire | moddb | loverslab | vectorplexus | unknown
    std::string type_name;  // 清单里的 $type 原文
    std::string url, prompt;
    std::string game_domain;  // nexus：站点域名（小写）
    std::int64_t mod_id = 0, file_id = 0;
    std::string game, game_file, game_version;  // gamefile
    std::vector<std::string> headers;           // http："Name: value"
};

struct Archive {
    std::string hash, name, meta;
    std::int64_t size = 0;
    Source src;
};

enum class Kind { FromArchive, PatchedFromArchive, InlineFile, RemappedInlineFile, CreateBSA, TransformedTexture, MergedPatch, Ignored, Other };

struct Directive {
    std::string type;  // $type 原文
    Kind kind = Kind::Other;
    std::vector<std::string> archive_path;  // [archive hash, path in archive, (nested path…)]
    std::string to, hash, from_hash, source_data_id, patch_id;
    std::int64_t size = 0;
};

struct Modlist {
    std::string name, author, description, version, game_type;
    bool nsfw = false;
    std::vector<Archive> archives;
    std::vector<Directive> directives;
};

Modlist parse_modlist(std::string_view json);
// 读 .wabbajack 里的 modlist JSON（外部 7z）。不是 zip/没有 modlist → Error{invalid_argument}。
std::string read_modlist_json(std::string_view wabbajack_file);

bool supported(Kind k);
const char* kind_name(Kind k);
// "SkyrimSpecialEdition" → "skyrimse"；未知返回小写原文
std::string game_id_of(std::string_view game_type);

// 画廊（Wabbajack 官方仓库的列表）。
struct GalleryEntry {
    std::string title, machine_url, repository, author, game, version, download_url, description;
    std::int64_t download_size = 0, archives_size = 0, installed_size = 0, archive_count = 0;
    bool nsfw = false;
    bool unavailable = false;  // 站方标记为下线
};
// 取所有仓库的列表（联网）。game 非空时只保留该游戏（Wabbajack 的小写域名，如 "skyrimspecialedition"）。
// 各仓库的列表并行获取，并缓存到 cache_dir（空 → ~/.cache/mo-linux/gallery）；缓存未过期（6 小时）直接用，
// 联网失败时退回过期缓存。
std::vector<GalleryEntry> fetch_gallery(std::string_view game_domain, std::string_view cache_dir = {});

// 把画廊里的 .wabbajack 下载到 dest（authored-files 的分片：definition.json.gz + parts/N，校验整体 xxh64）。
void download_authored(std::string_view download_url, std::string_view dest, const std::function<bool(std::uint64_t, std::uint64_t)>& progress = {});

// OctoDiff 补丁应用：out = patch(basis)。格式错误 → Error{invalid_argument}。
void octodiff_apply(std::string_view basis_path, std::string_view delta_path, std::string_view out_path);

}  // namespace mol::wabbajack

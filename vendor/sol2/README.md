sol2 headers, version v3.3.0 (Lua C++ binding API version 3).
Source: https://github.com/ThePhD/sol2/releases/tag/v3.3.0
License: LICENSE.txt (MIT).
Local patch: optional<T&>::emplace now binds an existing reference and returns it;
upstream called a nonexistent construct member, rejected by GCC 16 template checks.

Archive SHA-256: b82c5de030e18cb2bcbcefcd5f45afd526920c517a96413f0b59b4332d752a1e

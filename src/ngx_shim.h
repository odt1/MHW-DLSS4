#pragma once

#include <string>

namespace mhwdlss::ngx {

// Redirects the game's NGX 1.x calls to the shim. `dataDir` is where NGX may write files and
// where the plugin keeps its DLSS DLLs; `gameDir` is the game folder.
bool Install(const std::wstring& dataDir, const std::wstring& gameDir);

// Wine's version when the game runs under Wine or Proton, otherwise null.
const char* WineVersion();

}  // namespace mhwdlss::ngx

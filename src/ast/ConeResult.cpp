//------------------------------------------------------------------------------
// ConeResult.cpp
// Driver and load cone result conversions
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#include "ast/ConeResult.h"

#include "util/Converters.h"
#include <algorithm>
#include <filesystem>
#include <tuple>

#include "slang/text/SourceManager.h"

namespace server {

namespace {

std::string_view siteKindName(ConeSiteKind kind) {
    switch (kind) {
        case ConeSiteKind::Data:
            return "data";
        case ConeSiteKind::Control:
            return "control";
        case ConeSiteKind::Port:
            return "port";
    }
    SLANG_UNREACHABLE;
}

auto siteOrder(const ConeEntrySite& site) {
    const auto& start = site.location.range.start;
    return std::tuple(site.location.uri.str(), start.line, start.character, site.kind);
}

/// A signal passed to a macro is written at the argument, so point there instead of at the macro.
slang::SourceRange throughMacroArguments(slang::SourceRange range,
                                         const slang::SourceManager& sourceManager) {
    auto start = range.start();
    auto end = range.end();
    while (sourceManager.isMacroArgLoc(start) && sourceManager.isMacroArgLoc(end)) {
        start = sourceManager.getOriginalLoc(start);
        end = sourceManager.getOriginalLoc(end);
    }
    return {start, end};
}

/// Several expansions of a macro can land in one place, so duplicates are dropped here.
std::vector<ConeEntrySite> toEntrySites(const std::set<ConeSite>& sites,
                                        const slang::SourceManager& sourceManager) {
    std::vector<ConeEntrySite> result;
    for (const auto& site : sites) {
        if (!site.range.start().valid()) {
            continue;
        }
        auto range = throughMacroArguments(site.range, sourceManager);
        auto fullPath = std::filesystem::absolute(sourceManager.getFileName(range.start()));
        result.push_back({.location = {.uri = URI::fromFile(fullPath),
                                       .range = toRange(range, sourceManager)},
                          .kind = std::string(siteKindName(site.kind))});
    }
    std::ranges::sort(result, {}, siteOrder);
    auto duplicates = std::ranges::unique(result, {}, siteOrder);
    result.erase(duplicates.begin(), duplicates.end());
    return result;
}

} // namespace

std::vector<ConeEntry> ConeResult::getLocations() const {
    std::vector<ConeEntry> result;
    for (const auto& [leaf, sites] : m_leaves) {
        auto range = leaf.getDeclarationRange();
        if (range.start().valid()) {
            auto fullPath = std::filesystem::absolute(m_sourceManager.getFileName(range.start()));
            result.push_back({.path = leaf.getHierarchicalPath(),
                              .location = {.uri = URI::fromFile(fullPath),
                                           .range = toRange(range, m_sourceManager)},
                              .sites = toEntrySites(sites, m_sourceManager)});
        }
    }
    return result;
}

std::vector<std::string> ConeResult::getPaths() const {
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const auto& [leaf, sites] : m_leaves) {
        std::string hier = leaf.getHierarchicalPath();
        if (seen.insert(hier).second) {
            result.push_back(hier);
        }
    }
    return result;
}

} // namespace server

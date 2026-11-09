//------------------------------------------------------------------------------
// ConeResult.h
// Driver and load cone results
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "ConeTracer.h"
#include "lsp/LspTypes.h"
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slang {
class SourceManager;
}

namespace server {

/// A place in the source where a cone endpoint connects to the traced signal.
struct ConeEntrySite {
    /// The use of the signal being traced from, in the assignment or port connection. For a
    /// "constant" or "call", the value assigned.
    lsp::Location location;

    /// One of "data", "wire", "control", "port", "constant" or "call".
    std::string kind;
};

/// A single endpoint of a driver or load cone.
struct ConeEntry {
    /// The hierarchical RTL path of the signal.
    std::string path;

    /// The declaration location of the signal.
    lsp::Location location;

    /// Where this endpoint connects to the traced signal, in source order.
    std::vector<ConeEntrySite> sites;
};

/// The drivers of a signal, and where it is assigned a value that no signal feeds.
struct ConeDrivers {
    std::vector<ConeEntry> drivers;
    std::vector<ConeEntrySite> constants;
};

/// Cone leaves with conversions to hierarchical paths and source locations.
/// The source manager and compilation containing the leaves must outlive this result.
class ConeResult {
public:
    /// Own the trace and use the source manager to resolve its locations.
    ConeResult(ConeTrace trace, const slang::SourceManager& sourceManager) :
        m_leaves(std::move(trace.leaves)), m_constants(std::move(trace.constants)),
        m_sourceManager(sourceManager) {}

    /// Return endpoints with valid declaration locations.
    std::vector<ConeEntry> getLocations() const;

    /// Return where the traced signal is assigned a value that no signal feeds, in source order.
    /// Only driver cones have these.
    std::vector<ConeEntrySite> getConstants() const;

    /// Return distinct hierarchical paths in the cone.
    std::vector<std::string> getPaths() const;

private:
    /// The unique symbol endpoints collected by tracing.
    ConeLeaves m_leaves;

    /// The assignments that have no symbol endpoint.
    std::set<ConeSite> m_constants;

    /// Source manager for the compilation containing the leaves.
    const slang::SourceManager& m_sourceManager;
};

} // namespace server

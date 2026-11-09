//------------------------------------------------------------------------------
//! @file ConeTracer.h
//! @brief Cone tracer
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "util/ScopedRestore.h"
#include <functional>
#include <map>
#include <set>
#include <vector>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Expression.h"
#include "slang/ast/SemanticFacts.h"
#include "slang/ast/Statement.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/text/SourceLocation.h"
#include "slang/util/Util.h"

using server::utils::ScopedReset;
using server::utils::ScopedRestore;

class ConeLeaf {
public:
    ConeLeaf(const slang::ast::PortSymbol* port) : symbol(port->internalSymbol) {}
    ConeLeaf(const slang::ast::ValueExpressionBase* expr) : symbol(concreteSymbol(&expr->symbol)) {}

    std::string getHierarchicalPath() const {
        SLANG_ASSERT(symbol);
        return symbol->getHierarchicalPath();
    }

    slang::SourceRange getDeclarationRange() const {
        SLANG_ASSERT(symbol);
        auto startLoc = symbol->location;
        return slang::SourceRange(startLoc, startLoc + symbol->name.length());
    }

    bool isParameter() const { return symbol->kind == slang::ast::SymbolKind::Parameter; }

    /// Whether this names a fixed value rather than a signal.
    bool isConstant() const {
        return isParameter() || symbol->kind == slang::ast::SymbolKind::EnumValue;
    }

    bool operator<(const ConeLeaf& other) const {
        return std::less<const slang::ast::Symbol*>{}(symbol, other.symbol);
    }

    static const slang::ast::Symbol* concreteSymbol(const slang::ast::Symbol* symbol) {
        if (const auto modport = symbol->as_if<slang::ast::ModportPortSymbol>()) {
            return modport->internalSymbol;
        }
        return symbol;
    }

private:
    const slang::ast::Symbol* symbol;
};

/// How a signal takes part in the connection a cone edge was found on.
enum class ConeSiteKind {
    /// Read as a value in the connecting assignment.
    Data,
    /// Copied whole to the other signal by logic that has no condition or clock, so the two are
    /// effectively one wire.
    Wire,
    /// Decides whether the connecting assignment executes.
    Control,
    /// Connected through an instance's port map.
    Port,
    /// Assigned a value that reads no signal.
    Constant,
};

/// Where a cone edge is made in the source: the use of the signal being traced from, in the
/// assignment or port connection that links it to the other end.
struct ConeSite {
    slang::SourceRange range;
    ConeSiteKind kind;

    bool operator<(const ConeSite& other) const {
        if (auto order = range.start() <=> other.range.start(); order != 0) {
            return order < 0;
        }
        if (auto order = range.end() <=> other.range.end(); order != 0) {
            return order < 0;
        }
        return kind < other.kind;
    }
};

/// Every signal in a cone, with the places it connects to the root.
using ConeLeaves = std::map<ConeLeaf, std::set<ConeSite>>;

/// What tracing a signal found.
struct ConeTrace {
    ConeLeaves leaves;

    /// Where the root is assigned a value that reads no signal, so no leaf stands for it. Only
    /// drivers have these.
    std::set<ConeSite> constants;
};

template<typename TDerived>
struct ConeTracer : public slang::ast::ASTVisitor<TDerived, slang::ast::VisitFlags::AllGood> {
protected:
    const slang::ast::Symbol* root;

    // Whether visited assignments are neither clocked nor latched.
    bool inCombinational = false;
    // Whether visited statements only execute under a condition.
    bool underCondition = false;

    bool isWire(const slang::ast::AssignmentExpression& expr) const {
        return inCombinational && !underCondition && isPlainCopy(expr);
    }

    void addLeaf(const ConeLeaf& leaf, const ConeSite& site) {
        if (!leaf.isParameter()) {
            trace.leaves[leaf].insert(site);
        }
    }

    void addConstant(const slang::ast::Expression& value) {
        trace.constants.insert({value.sourceRange, ConeSiteKind::Constant});
    }

private:
    ConeTracer(const slang::ast::Symbol* root) : root(ConeLeaf::concreteSymbol(root)) {}

    ConeTrace trace;

    // Only a whole signal on each side: a select, an expression or a change of width is logic, and
    // a parameter or an enum value isn't a signal.
    static bool isPlainCopy(const slang::ast::AssignmentExpression& expr) {
        using namespace slang::ast;
        if (expr.isCompound() || !ValueExpressionBase::isKind(expr.left().kind)) {
            return false;
        }
        const Expression* source = &expr.right();
        while (const auto conversion = source->as_if<ConversionExpression>()) {
            if (!conversion->isImplicit()) {
                return false;
            }
            source = &conversion->operand();
        }
        return ValueExpressionBase::isKind(source->kind) &&
               !ConeLeaf(&source->as<ValueExpressionBase>()).isConstant() &&
               source->type->getBitWidth() == expr.left().type->getBitWidth();
    }

public:
    ConeTrace getTrace() { return trace; }

    void handle(const slang::ast::ContinuousAssignSymbol& symbol) {
        ScopedReset combinationalScope(inCombinational, true);
        this->visitDefault(symbol);
    }

    void handle(const slang::ast::ProceduralBlockSymbol& symbol) {
        bool combinational = symbol.procedureKind == slang::ast::ProceduralBlockKind::AlwaysComb;
        ScopedReset combinationalScope(inCombinational, combinational);
        this->visitDefault(symbol);
    }

    friend TDerived;
};

struct DriversTracer : public ConeTracer<DriversTracer> {
private:
    // Signals that control whether the current assignment executes, and where they are read.
    ConeLeaves conditionDrivers;

    // Whether the current expression is an assignment target.
    bool isLhs = false;
    // Whether visited value expressions should be recorded as drivers of the root.
    bool isDriven = false;
    // Whether visited value expressions control which assignments execute.
    bool inCondition = false;
    // Whether visited value expressions are the connection of an input port.
    bool inPortConnection = false;
    // Whether visited value expressions are copied whole to the root.
    bool inWire = false;
    // Whether the value driving the root has read a signal so far.
    bool readsSignal = false;

    // Where the current assignment writes the root.
    slang::SourceRange rootRange;

    const slang::ast::PortSymbol* portSymbol = nullptr;

    ConeSiteKind readKind() const {
        if (inPortConnection) {
            return ConeSiteKind::Port;
        }
        return inWire ? ConeSiteKind::Wire : ConeSiteKind::Data;
    }

public:
    using ConeTracer::handle;

    void handle(const slang::ast::ValueExpressionBase& symbol) {
        if (isLhs && ConeLeaf::concreteSymbol(&symbol.symbol) == root) {
            isDriven = true;
            rootRange = symbol.sourceRange;
        }
        else if (!isLhs && isDriven) {
            readsSignal = readsSignal || !ConeLeaf(&symbol).isConstant();
            addLeaf(&symbol, {symbol.sourceRange, readKind()});
        }
        if (inCondition) {
            conditionDrivers[&symbol].insert({symbol.sourceRange, ConeSiteKind::Control});
        }
    }

    void handle(const slang::ast::AssignmentExpression& expr) {
        ScopedReset drivenScope(isDriven);
        {
            ScopedReset lhsScope(isLhs, true);
            expr.left().visit(*this);
        }
        if (isDriven) {
            ScopedReset readsScope(readsSignal);
            {
                ScopedReset wireScope(inWire, isWire(expr));
                expr.right().visit(*this);
            }
            if (portSymbol) {
                addLeaf(portSymbol, {rootRange, ConeSiteKind::Port});
            }
            // (A compound assignment reads the root, which its right side doesn't show.)
            else if (!readsSignal && !expr.isCompound()) {
                addConstant(expr.right());
            }
            for (const auto& [driver, sites] : conditionDrivers) {
                for (const auto& site : sites) {
                    addLeaf(driver, site);
                }
            }
        }
    }

    void handle(const slang::ast::ConditionalStatement& stmt) {
        ScopedRestore conditionDriversScope(conditionDrivers);
        ScopedRestore underConditionScope(underCondition);
        underCondition = true;
        {
            ScopedReset conditionScope(inCondition, true);
            for (const auto condition : stmt.conditions) {
                condition.expr->visit(*this);
            }
        }
        stmt.ifTrue.visit(*this);
        if (stmt.ifFalse) {
            stmt.ifFalse->visit(*this);
        }
    }

    void handle(const slang::ast::CaseStatement& stmt) {
        ScopedRestore conditionDriversScope(conditionDrivers);
        ScopedRestore underConditionScope(underCondition);
        underCondition = true;
        {
            ScopedReset conditionScope(inCondition, true);
            stmt.expr.visit(*this);
        }
        for (const auto item : stmt.items) {
            {
                ScopedReset conditionScope(inCondition, true);
                for (const auto expr : item.expressions) {
                    expr->visit(*this);
                }
            }
            item.stmt->visit(*this);
        }
        if (stmt.defaultCase) {
            stmt.defaultCase->visit(*this);
        }
    }

    void handle(const slang::ast::InstanceSymbol& symbol) {
        for (auto const connection : symbol.getPortConnections()) {
            // TODO -- interfaces, etc.
            const auto port = connection->port.as_if<slang::ast::PortSymbol>();
            const auto expr = connection->getExpression();
            if (!expr) {
                continue;
            }
            if (port) {
                if (port->direction == slang::ast::ArgumentDirection::In &&
                    port->internalSymbol == root) {
                    ScopedReset drivenScope(isDriven, true);
                    ScopedReset portConnectionScope(inPortConnection, true);
                    ScopedReset readsScope(readsSignal);
                    expr->visit(*this);
                    if (!readsSignal) {
                        addConstant(*expr);
                    }
                }
                else if (port->direction == slang::ast::ArgumentDirection::Out) {
                    ScopedReset portScope(portSymbol, port);
                    expr->visit(*this);
                }
            }
        }
    }

    DriversTracer(const slang::ast::Symbol* root) : ConeTracer(root) {}
};

struct LoadsTracer : public ConeTracer<LoadsTracer> {
private:
    // Whether visited value expressions should be recorded as loads of the root.
    bool isLhs = false;
    // Whether visited value expressions decide which assignments execute.
    bool inCondition = false;
    // Whether visited value expressions are the connection of an input port.
    bool inPortConnection = false;
    // Whether visited value expressions are copied whole to the assignment's target.
    bool inWire = false;
    // Whether the current assignment is the connection of the root, which is an output port.
    bool inRootOutputConnection = false;
    // Where the root is read on the current data or control path.
    std::vector<ConeSite> rootSites;

    ConeSiteKind readKind() const {
        if (inPortConnection) {
            return ConeSiteKind::Port;
        }
        if (inCondition) {
            return ConeSiteKind::Control;
        }
        return inWire ? ConeSiteKind::Wire : ConeSiteKind::Data;
    }

public:
    using ConeTracer::handle;

    void handle(const slang::ast::ValueExpressionBase& symbol) {
        if (isLhs) {
            if (inRootOutputConnection) {
                addLeaf(&symbol, {symbol.sourceRange, ConeSiteKind::Port});
            }
            for (const auto& site : rootSites) {
                addLeaf(&symbol, site);
            }
        }
        else if (ConeLeaf::concreteSymbol(&symbol.symbol) == root) {
            rootSites.push_back({symbol.sourceRange, readKind()});
        }
    }

    void handle(const slang::ast::AssignmentExpression& expr) {
        ScopedRestore rootSitesScope(rootSites);
        {
            ScopedReset wireScope(inWire, isWire(expr));
            expr.right().visit(*this);
        }
        if (inRootOutputConnection || !rootSites.empty()) {
            ScopedReset lhsScope(isLhs, true);
            expr.left().visit(*this);
        }
    }

    void handle(const slang::ast::ConditionalStatement& stmt) {
        ScopedRestore rootSitesScope(rootSites);
        ScopedRestore underConditionScope(underCondition);
        underCondition = true;
        {
            ScopedReset conditionScope(inCondition, true);
            for (const auto condition : stmt.conditions) {
                condition.expr->visit(*this);
            }
        }
        stmt.ifTrue.visit(*this);
        if (stmt.ifFalse) {
            stmt.ifFalse->visit(*this);
        }
    }

    void handle(const slang::ast::CaseStatement& stmt) {
        ScopedRestore rootSitesScope(rootSites);
        ScopedRestore underConditionScope(underCondition);
        underCondition = true;
        {
            ScopedReset conditionScope(inCondition, true);
            stmt.expr.visit(*this);
        }
        const auto selectorSites = rootSites;
        for (const auto item : stmt.items) {
            rootSites = selectorSites;
            {
                ScopedReset conditionScope(inCondition, true);
                for (const auto expr : item.expressions) {
                    expr->visit(*this);
                }
            }
            item.stmt->visit(*this);
        }
        if (stmt.defaultCase) {
            rootSites = selectorSites;
            stmt.defaultCase->visit(*this);
        }
    }

    void handle(const slang::ast::InstanceSymbol& symbol) {
        for (auto const connection : symbol.getPortConnections()) {
            // TODO -- interfaces, etc.
            const auto port = connection->port.as_if<slang::ast::PortSymbol>();
            const auto expr = connection->getExpression();
            if (!expr) {
                continue;
            }
            if (port) {
                if (port->direction == slang::ast::ArgumentDirection::Out &&
                    port->internalSymbol == root) {
                    ScopedReset outputScope(inRootOutputConnection, true);
                    expr->visit(*this);
                }
                else if (port->direction == slang::ast::ArgumentDirection::In) {
                    ScopedRestore rootSitesScope(rootSites);
                    ScopedReset portConnectionScope(inPortConnection, true);
                    expr->visit(*this);
                    for (const auto& site : rootSites) {
                        addLeaf(port, site);
                    }
                }
            }
        }
    }

    LoadsTracer(const slang::ast::Symbol* root) : ConeTracer(root) {}
};

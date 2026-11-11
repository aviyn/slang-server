//------------------------------------------------------------------------------
//! @file ConeTracer.h
//! @brief Cone tracer
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "util/ScopedRestore.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Expression.h"
#include "slang/ast/SemanticFacts.h"
#include "slang/ast/Statement.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/statements/MiscStatements.h"
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
    /// Copied whole to the other signal, its only assignment there, by logic that has no condition
    /// or clock, so the two are effectively one wire.
    Wire,
    /// Decides whether the connecting assignment executes.
    Control,
    /// Connected through an instance's port map, without logic in the connection.
    Port,
    /// Assigned a value that reads no signal.
    Constant,
    /// Assigned the result of a call that is given no signal, so what it reads can't be seen here.
    Call,
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

    /// Where the root is assigned a value that no signal feeds, so no leaf stands for it. Only
    /// driver cones have these.
    std::set<ConeSite> constants;
};

/// What the value of an expression comes from.
struct ConeValueSources
    : public slang::ast::ASTVisitor<ConeValueSources, slang::ast::VisitFlags::AllGood> {
    bool signal = false;
    bool call = false;

    void handle(const slang::ast::ValueExpressionBase& expr) {
        signal = signal || !ConeLeaf(&expr).isConstant();
    }

    void handle(const slang::ast::CallExpression& expr) {
        call = true;
        visitDefault(expr);
    }
};

/// How many assignments in a block write each signal.
struct ConeBlockWrites
    : public slang::ast::ASTVisitor<ConeBlockWrites, slang::ast::VisitFlags::AllGood> {
    std::map<const slang::ast::Symbol*, int> counts;

    void handle(const slang::ast::AssignmentExpression& expr) {
        count(expr.left());
        visitDefault(expr);
    }

private:
    void count(const slang::ast::Expression& target) {
        if (const auto concat = target.as_if<slang::ast::ConcatenationExpression>()) {
            for (const auto operand : concat->operands()) {
                count(*operand);
            }
        }
        else if (const auto symbol = target.getSymbolReference()) {
            counts[ConeLeaf::concreteSymbol(symbol)]++;
        }
    }
};

template<typename TDerived>
struct ConeTracer : public slang::ast::ASTVisitor<TDerived, slang::ast::VisitFlags::AllGood> {
protected:
    const slang::ast::Symbol* root;

    // Whether the current expression is an assignment target.
    bool isLhs = false;
    // Whether visited assignments are neither clocked nor latched.
    bool inCombinational = false;
    // Whether visited statements only execute under a condition.
    bool underCondition = false;
    // Whether visited value expressions are routed by an input port's connection.
    bool inPortConnection = false;
    // Whether visited value expressions are copied whole to the assignment's target.
    bool inWire = false;

    ConeSiteKind readKind() const {
        if (inPortConnection) {
            return ConeSiteKind::Port;
        }
        return inWire ? ConeSiteKind::Wire : ConeSiteKind::Data;
    }

    void visitAssignedValue(const slang::ast::AssignmentExpression& expr) {
        ScopedReset wireScope(inWire, isWire(expr));
        expr.right().visit(*this);
    }

    // A connection that only routes bits to the port: anything else is logic.
    static bool isWiring(const slang::ast::Expression& expr) {
        using namespace slang::ast;
        switch (expr.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue:
            case ExpressionKind::IntegerLiteral:
            case ExpressionKind::UnbasedUnsizedIntegerLiteral:
                return true;
            case ExpressionKind::ElementSelect: {
                const auto& select = expr.as<ElementSelectExpression>();
                return !readsSignal(select.selector()) && isWiring(select.value());
            }
            case ExpressionKind::RangeSelect: {
                const auto& select = expr.as<RangeSelectExpression>();
                return !readsSignal(select.left()) && !readsSignal(select.right()) &&
                       isWiring(select.value());
            }
            case ExpressionKind::MemberAccess:
                return isWiring(expr.as<MemberAccessExpression>().value());
            case ExpressionKind::Concatenation:
                return std::ranges::all_of(expr.as<ConcatenationExpression>().operands(),
                                           [](auto operand) { return isWiring(*operand); });
            case ExpressionKind::Replication:
                return isWiring(expr.as<ReplicationExpression>().concat());
            case ExpressionKind::Conversion: {
                const auto& conversion = expr.as<ConversionExpression>();
                return conversion.isImplicit() && isWiring(conversion.operand());
            }
            default:
                return false;
        }
    }

    void addLeaf(const ConeLeaf& leaf, const ConeSite& site) {
        if (!leaf.isParameter()) {
            trace.leaves[leaf].insert(site);
        }
    }

    // Records a value that no signal feeds, which no leaf can stand for.
    void addIfNoSignal(const slang::ast::Expression& value) {
        ConeValueSources sources;
        value.visit(sources);
        if (!sources.signal) {
            trace.constants.insert(
                {value.sourceRange, sources.call ? ConeSiteKind::Call : ConeSiteKind::Constant});
        }
    }

private:
    ConeTracer(const slang::ast::Symbol* root) : root(ConeLeaf::concreteSymbol(root)) {}

    ConeTrace trace;
    // How many assignments write each signal in the current combinational block.
    std::map<const slang::ast::Symbol*, int> blockWrites;

    bool isWire(const slang::ast::AssignmentExpression& expr) const {
        return inCombinational && !underCondition && isPlainCopy(expr) && isOnlyWrite(expr);
    }

    // A default assignment that later statements replace doesn't make the two signals one.
    bool isOnlyWrite(const slang::ast::AssignmentExpression& expr) const {
        const auto& target = expr.left().as<slang::ast::ValueExpressionBase>();
        const auto writes = blockWrites.find(ConeLeaf::concreteSymbol(&target.symbol));
        return writes == blockWrites.end() || writes->second == 1;
    }

    static bool readsSignal(const slang::ast::Expression& expr) {
        ConeValueSources sources;
        expr.visit(sources);
        return sources.signal;
    }

    static bool isCombinational(const slang::ast::ProceduralBlockSymbol& symbol) {
        using namespace slang::ast;
        if (symbol.procedureKind == ProceduralBlockKind::AlwaysComb) {
            return true;
        }
        const auto timed = symbol.getBody().template as_if<TimedStatement>();
        return symbol.procedureKind == ProceduralBlockKind::Always && timed &&
               timed->timing.kind == TimingControlKind::ImplicitEvent;
    }

    // An index on the left only picks which bits are written: it isn't a target.
    template<typename TSelect>
    void visitSelect(const TSelect& expr) {
        if (isLhs) {
            expr.value().visit(*this);
        }
        else {
            this->visitDefault(expr);
        }
    }

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
    ConeTrace getTrace() && { return std::move(trace); }

    void handle(const slang::ast::ContinuousAssignSymbol& symbol) {
        ScopedReset combinationalScope(inCombinational, true);
        this->visitDefault(symbol);
    }

    void handle(const slang::ast::ProceduralBlockSymbol& symbol) {
        bool combinational = isCombinational(symbol);
        ScopedReset combinationalScope(inCombinational, combinational);
        if (combinational) {
            ConeBlockWrites writes;
            symbol.getBody().visit(writes);
            blockWrites = std::move(writes.counts);
        }
        this->visitDefault(symbol);
        blockWrites.clear();
    }

    void handle(const slang::ast::ElementSelectExpression& expr) { visitSelect(expr); }

    void handle(const slang::ast::RangeSelectExpression& expr) { visitSelect(expr); }

    friend TDerived;
};

struct DriversTracer : public ConeTracer<DriversTracer> {
private:
    // Signals that control whether the current assignment executes, and where they are read.
    ConeLeaves conditionDrivers;

    // Whether visited value expressions should be recorded as drivers of the root.
    bool isDriven = false;
    // Whether visited value expressions control which assignments execute.
    bool inCondition = false;

    // Where the current assignment writes the root.
    slang::SourceRange rootRange;

    const slang::ast::PortSymbol* portSymbol = nullptr;

public:
    using ConeTracer::handle;

    void handle(const slang::ast::ValueExpressionBase& symbol) {
        if (isLhs && ConeLeaf::concreteSymbol(&symbol.symbol) == root) {
            isDriven = true;
            rootRange = symbol.sourceRange;
        }
        else if (!isLhs && isDriven) {
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
            visitAssignedValue(expr);
            if (portSymbol) {
                addLeaf(portSymbol, {rootRange, ConeSiteKind::Port});
            }
            // (A compound assignment reads the root, which its right side doesn't show.)
            else if (!expr.isCompound()) {
                addIfNoSignal(expr.right());
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
        ScopedRestore underConditionScope(underCondition, true);
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
        ScopedRestore underConditionScope(underCondition, true);
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
                    ScopedReset portConnectionScope(inPortConnection, isWiring(*expr));
                    expr->visit(*this);
                    addIfNoSignal(*expr);
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
    // Whether visited value expressions decide which assignments execute.
    bool inCondition = false;
    // Whether the current assignment is the connection of the root, which is an output port.
    bool inRootOutputConnection = false;
    // Where the root is read on the current data or control path.
    std::vector<ConeSite> rootSites;

    ConeSiteKind loadKind() const {
        return inCondition && !inPortConnection ? ConeSiteKind::Control : readKind();
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
            rootSites.push_back({symbol.sourceRange, loadKind()});
        }
    }

    // A block's event control clocks only that block's assignments.
    void handle(const slang::ast::ProceduralBlockSymbol& symbol) {
        ScopedRestore rootSitesScope(rootSites, {});
        ConeTracer::handle(symbol);
    }

    void handle(const slang::ast::AssignmentExpression& expr) {
        ScopedRestore rootSitesScope(rootSites);
        visitAssignedValue(expr);
        if (inRootOutputConnection || !rootSites.empty()) {
            ScopedReset lhsScope(isLhs, true);
            expr.left().visit(*this);
        }
    }

    void handle(const slang::ast::ConditionalStatement& stmt) {
        ScopedRestore rootSitesScope(rootSites);
        ScopedRestore underConditionScope(underCondition, true);
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
        ScopedRestore underConditionScope(underCondition, true);
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
                    ScopedReset portConnectionScope(inPortConnection, isWiring(*expr));
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

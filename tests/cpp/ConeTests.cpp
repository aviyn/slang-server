// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "lsp/LspTypes.h"
#include "utils/ServerHarness.h"

TEST_CASE("Cone Tracing") {
    ServerHarness server("");

    server.loadConfig(Config{.build = "cone_test.f"});

    // This will actually load the compilation
    server.onInitialized(lsp::InitializedParams{});

    auto doc = server.openFile("cone_test.sv");

    // TODO -- prepare for interfaces -- list signals?

    SECTION("Prepare Multiple") {
        auto cursor = doc.before("x <= a + b;");
        server.checkPrepareCallHierarchy(cursor, {"test.the_sub_1.x", "test.the_sub_2.x"});
    }

    SECTION("Prepare Module Outside Design") {
        auto unusedDoc = server.openFile("cone_unused.sv");
        auto cursor = unusedDoc.before("unused_signal");
        server.checkPrepareCallHierarchy(cursor, {});
        server.client.expectWarning("The selected signal is not part of the current design");
    }

    SECTION("Prepare Unsupported") {
        auto cursor = doc.before("the_sub_1(");
        server.checkPrepareCallHierarchy(cursor, {});
        server.client.expectWarning("Only simple logic vectors are currently supported");
    }

    SECTION("Prepare Single") {
        auto cursor = doc.before("a,");
        server.checkPrepareCallHierarchy(cursor, {"test.a"});
    }

    SECTION("Drivers Multiple") {
        auto cursor_a = doc.after("module sub(").after("input logic [31:0] ");
        auto cursor_b = cursor_a.after("input logic [31:0] ");
        server.checkConeCommand("slang.getDriversWithLocation", "test.the_sub_2.x",
                                {{"test.the_sub_2.a", &cursor_a}, {"test.the_sub_2.b", &cursor_b}});
    }

    SECTION("Drivers Single") {
        auto cursor = doc.before("x1;");
        server.checkConeCommand("slang.getDriversWithLocation", "test.the_sub_2.b",
                                {{"test.x1", &cursor}});
    }

    SECTION("Drivers Single2") {
        // This points at the port declartion.  It would be more consistent to point at the
        // port map instead, but that location information doesn't appear to be attached to
        // PortSymbol
        auto cursor = doc.after("module sub").after("output logic [31:0] ");
        server.checkConeCommand("slang.getDriversWithLocation", "test.x1",
                                {{"test.the_sub_1.x", &cursor}});
    }

    SECTION("Drivers Constant") {
        auto cursor_foo = doc.after("module sub_sub(").after("input logic ");
        auto cursor_bar = cursor_foo.after("input logic ");
        server.checkConeCommand("slang.getDriversWithLocation", "test.the_sub_2.the_sub_sub.result",
                                {{"test.the_sub_2.the_sub_sub.foo", &cursor_foo},
                                 {"test.the_sub_2.the_sub_sub.bar", &cursor_bar}});
    }

    SECTION("Drivers Switched") {
        auto cursor_foo = doc.after("module sub_sub(").after("input logic ");
        auto cursor_bar = cursor_foo.after("input logic ");
        server.checkConeCommand("slang.getDriversWithLocation",
                                "test.the_sub_2.the_sub_sub.switched_result",
                                {{"test.the_sub_2.the_sub_sub.bar", &cursor_bar},
                                 {"test.the_sub_2.the_sub_sub.foo", &cursor_foo}});
    }

    SECTION("Drivers Interface") {
        auto cursor_qux = doc.before("qux;");
        auto cursor_b =
            doc.after("module sub(").after("input logic [31:0] ").after("input logic [31:0] ");
        server.checkConeCommand("slang.getDriversWithLocation", "test.the_intfs[2].qux",
                                {{"test.the_intfs[1].qux", &cursor_qux},
                                 {"test.the_sub_2.b", &cursor_b}});
    }

    SECTION("Drivers Interface Reference") {
        auto cursor_qux = doc.before("qux;");
        auto cursor_b =
            doc.after("module sub(").after("input logic [31:0] ").after("input logic [31:0] ");
        server.checkConeCommand("slang.getDriversWithLocation", "test.the_sub_1.qux_out.qux",
                                {{"test.the_intfs[0].qux", &cursor_qux},
                                 {"test.the_sub_1.b", &cursor_b}});
    }

    SECTION("Parameters Are Not Drivers") {
        server.checkConeCommand("slang.getDriversWithLocation", "test.parameter_driven", {});
    }

    SECTION("Loads Multiple") {
        auto cursor = doc.after("module sub(").after("input logic [31:0] ");
        server.checkConeCommand("slang.getLoadsWithLocation", "test.a",
                                {{"test.the_sub_2.a", &cursor}, {"test.the_sub_1.a", &cursor}});
    }

    SECTION("Loads Up Down") {
        auto cursor_x = doc.after("module sub").after("output logic [31:0] ");
        auto cursor_foo = doc.after("module sub_sub(").after("input logic ");
        server.checkConeCommand("slang.getLoadsWithLocation", "test.the_sub_2.a",
                                {{"test.the_sub_2.x", &cursor_x},
                                 {"test.the_sub_2.the_sub_sub.foo", &cursor_foo}});
    }

    SECTION("Loads Single") {
        auto cursor = doc.after("module test(").after("output logic [31:0] ");
        server.checkConeCommand("slang.getLoadsWithLocation", "test.the_sub_2.x",
                                {{"test.x", &cursor}});
    }

    SECTION("Loads Conditional") {
        auto cursor_result = doc.before("result;");
        auto cursor_switched = doc.before("switched_result;");
        server.checkConeCommand("slang.getLoadsWithLocation", "test.the_sub_1.the_sub_sub.foo",
                                {{"test.the_sub_1.the_sub_sub.result", &cursor_result},
                                 {"test.the_sub_1.the_sub_sub.switched_result", &cursor_switched}});
    }

    SECTION("Loads Switched") {
        auto cursor_result = doc.before("result;");
        auto cursor_switched = doc.before("switched_result;");
        server.checkConeCommand("slang.getLoadsWithLocation", "test.the_sub_2.the_sub_sub.bar",
                                {{"test.the_sub_2.the_sub_sub.result", &cursor_result},
                                 {"test.the_sub_2.the_sub_sub.switched_result", &cursor_switched}});
    }

    SECTION("Loads Through Instance With Unconnected Input") {
        auto cursor = doc.after("module open_ports(").after("input logic ");
        server.checkConeCommand("slang.getLoadsWithLocation", "test.open_src",
                                {{"test.the_open.in", &cursor}});
    }

    SECTION("Loads Of Unconnected Output") {
        server.checkConeCommand("slang.getLoadsWithLocation", "test.the_open.out", {});
    }

    SECTION("Loads Interface") {
        auto cursor = doc.before("quz;");
        server.checkConeCommand("slang.getLoadsWithLocation", "test.the_intfs[1].quz",
                                {{"test.the_intfs[0].quz", &cursor}});
    }

    SECTION("Loads Interface Reference") {
        auto cursor = doc.before("qux;");
        server.checkConeCommand("slang.getLoadsWithLocation", "test.the_sub_1.qux_out.qux",
                                {{"test.the_intfs[2].qux", &cursor}});
    }
}

TEST_CASE("Cone locations deduplicate generated multi-bit drivers") {
    ServerHarness server("");
    server.loadConfig(Config{.build = "cone_test.f"});
    server.onInitialized(lsp::InitializedParams{});
    auto doc = server.openFile("cone_test.sv");
    auto declaration = doc.before("bit_driver;");

    server.checkConeCommand("slang.getDriversWithLocation", "test.bit_driven",
                            {{"test.bit_driver", &declaration}});
}

TEST_CASE("Undriven signals have empty driver cones") {
    ServerHarness server;
    auto doc = server.openFile("undriven_cone.sv", R"(
module top;
    logic undriven;
    logic unused;
    logic result;
    assign result = undriven;
endmodule
)");
    server.setTopLevel(std::string(doc.m_uri.getPath()));

    server.checkConeCommand("slang.getDriversWithLocation", "top.undriven", {});
    server.checkConeCommand("slang.getDriversWithLocation", "top.unused", {});
}

TEST_CASE("Cone sites show where signals connect") {
    ServerHarness server;
    auto doc = server.openFile("cone_sites.sv", R"(
`define ASSIGN(dst, src) assign dst = src
`define ASSIGN_FIXED(dst) assign dst = fixed_src

module child(input logic in, output logic out);
    assign out = in;
endmodule

module wide_child(input logic in, output logic [1:0] pair);
    assign pair = {in, in};
endmodule

module wild_child(input logic wild_in, output logic wild_out);
    assign wild_out = wild_in;
endmodule

module top;
    logic data_src, data_dst, twice_src, twice_dst, en, gated, gated_d;
    logic sel, cased, named_src, named_dst, in, out, high, low;
    logic wild_in, wild_out, macro_src, macro_dst, fixed_src, fixed_dst;
    logic last;

    assign data_dst = data_src + 1'b1;

    always_comb begin
        twice_dst = twice_src;
        twice_dst = ~twice_src;
    end

    always_comb begin
        gated = 1'b0;
        if (en) gated = gated_d;
    end

    always_comb begin
        case (sel)
            1'b0: cased = 1'b1;
            default: cased = 1'b0;
        endcase
    end

    child named(.in (named_src), .out (named_dst));
    child implicit(.in, .out);
    wide_child concat(.in (1'b0), .pair ({high, low}));
    wild_child wild(.*);

    `ASSIGN(macro_dst, macro_src);
    `ASSIGN_FIXED(fixed_dst);
endmodule
)");
    server.setTopLevel(std::string(doc.m_uri.getPath()));

    const std::string drivers = "slang.getDriversWithLocation";
    const std::string loads = "slang.getLoadsWithLocation";

    SECTION("Assignment") {
        auto read = doc.before("data_src + ");
        server.checkConeSites(drivers, "top.data_dst", "top.data_src", {{&read, "data"}});
        server.checkConeSites(loads, "top.data_src", "top.data_dst", {{&read, "data"}});
    }

    SECTION("Every assignment is a site") {
        auto first = doc.before("twice_src;");
        auto second = first.after("twice_src;").before("twice_src;");
        server.checkConeSites(drivers, "top.twice_dst", "top.twice_src",
                              {{&first, "data"}, {&second, "data"}});
        server.checkConeSites(loads, "top.twice_src", "top.twice_dst",
                              {{&first, "data"}, {&second, "data"}});
    }

    SECTION("If condition") {
        auto read = doc.before("en) gated");
        server.checkConeSites(drivers, "top.gated", "top.en", {{&read, "control"}});
        server.checkConeSites(loads, "top.en", "top.gated", {{&read, "control"}});
    }

    SECTION("Assignment under a condition") {
        auto read = doc.after("if (en) gated = ");
        server.checkConeSites(drivers, "top.gated", "top.gated_d", {{&read, "data"}});
    }

    SECTION("Case selector") {
        auto read = doc.before("sel)");
        server.checkConeSites(drivers, "top.cased", "top.sel", {{&read, "control"}});
        server.checkConeSites(loads, "top.sel", "top.cased", {{&read, "control"}});
    }

    SECTION("Named input port") {
        auto read = doc.before("named_src)");
        server.checkConeSites(drivers, "top.named.in", "top.named_src", {{&read, "port"}});
        server.checkConeSites(loads, "top.named_src", "top.named.in", {{&read, "port"}});
    }

    SECTION("Named output port") {
        auto read = doc.before("named_dst)");
        server.checkConeSites(drivers, "top.named_dst", "top.named.out", {{&read, "port"}});
        server.checkConeSites(loads, "top.named.out", "top.named_dst", {{&read, "port"}});
    }

    SECTION("Implicit ports") {
        auto input = doc.before("in, .out)");
        auto output = doc.after("implicit(.in, .");
        server.checkConeSites(drivers, "top.implicit.in", "top.in", {{&input, "port"}});
        server.checkConeSites(drivers, "top.out", "top.implicit.out", {{&output, "port"}});
    }

    SECTION("Wildcard ports") {
        auto wildcard = doc.before(".*)");
        server.checkConeSites(drivers, "top.wild.wild_in", "top.wild_in", {{&wildcard, "port"}});
        server.checkConeSites(loads, "top.wild.wild_out", "top.wild_out", {{&wildcard, "port"}});
    }

    SECTION("One signal of a concatenated output") {
        auto read = doc.before("low})");
        server.checkConeSites(drivers, "top.low", "top.concat.pair", {{&read, "port"}});
        server.checkConeSites(loads, "top.concat.pair", "top.low", {{&read, "port"}});
    }

    SECTION("Macro argument") {
        auto read = doc.before("macro_src);");
        server.checkConeSites(drivers, "top.macro_dst", "top.macro_src", {{&read, "data"}});
    }

    SECTION("Macro body") {
        auto use = doc.before("`ASSIGN_FIXED(fixed_dst)");
        server.checkConeSites(drivers, "top.fixed_dst", "top.fixed_src", {{&use, "data"}});
    }
}

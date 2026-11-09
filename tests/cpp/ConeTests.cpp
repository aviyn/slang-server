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
    logic wire_src, wire_dst, comb_src, comb_dst, flop_d, flop_q, cond_src, cond_dst, clk;
    logic [1:0] sliced, shrunk;
    logic [3:0] whole_a, whole_b;
    logic last;

    assign wire_dst = wire_src;
    always_comb comb_dst = comb_src;
    always_ff @(posedge clk) flop_q <= flop_d;
    always_comb if (en) cond_dst = cond_src;
    assign sliced = whole_a[1:0];
    assign shrunk = whole_b;

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

    parameter logic PARAM = 1'b1;
    typedef enum logic { IDLE, BUSY } state_e;
    state_e state;
    logic lit, param_dst, reset_flop, counted, rd_only;

    assign lit = 1'b1;
    assign param_dst = PARAM;
    assign state = IDLE;
    always_ff @(posedge clk) begin
        if (en) reset_flop <= '0;
        else reset_flop <= flop_d;
    end
    always_comb begin
        counted = 1'b0;
        counted += 1'b1;
    end

    function logic make_bit();
        return 1'b1;
    endfunction
    logic from_call;
    assign from_call = make_bit();

    logic dflt_src, dflt_alt, dflt_dst, star_src, star_dst, idx, idx_src, mem_d;
    logic [3:0] mem, mem2, bus;
    always_comb begin
        dflt_dst = dflt_src;
        if (en) dflt_dst = dflt_alt;
    end
    always @* star_dst = star_src;
    always_comb begin
        idx = idx_src;
        mem[idx] = 1'b0;
        mem2[idx] = mem_d;
    end

    child logic_port(.in (en & sel), .out ());
    child bit_port(.in (bus[0]), .out ());
    child picked_port(.in (bus[sel]), .out ());
    wide_child padded_port(.in ({bus[1:1]}), .pair ());
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

    SECTION("A plain copy is a wire") {
        auto assigned = doc.before("wire_src;");
        server.checkConeSites(drivers, "top.wire_dst", "top.wire_src", {{&assigned, "wire"}});
        server.checkConeSites(loads, "top.wire_src", "top.wire_dst", {{&assigned, "wire"}});
        auto comb = doc.before("comb_src;");
        server.checkConeSites(drivers, "top.comb_dst", "top.comb_src", {{&comb, "wire"}});
        server.checkConeSites(loads, "top.comb_src", "top.comb_dst", {{&comb, "wire"}});
    }

    SECTION("A copy that is logic is not a wire") {
        auto flop = doc.before("flop_d;");
        server.checkConeSites(drivers, "top.flop_q", "top.flop_d", {{&flop, "data"}});
        server.checkConeSites(loads, "top.flop_d", "top.flop_q", {{&flop, "data"}});
        auto gated = doc.before("cond_src;");
        server.checkConeSites(drivers, "top.cond_dst", "top.cond_src", {{&gated, "data"}});
        server.checkConeSites(loads, "top.cond_src", "top.cond_dst", {{&gated, "data"}});
        auto slice = doc.before("whole_a[1:0]");
        server.checkConeSites(drivers, "top.sliced", "top.whole_a", {{&slice, "data"}});
        auto narrowed = doc.after("assign shrunk = ");
        server.checkConeSites(drivers, "top.shrunk", "top.whole_b", {{&narrowed, "data"}});
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
        server.checkConeSites(drivers, "top.macro_dst", "top.macro_src", {{&read, "wire"}});
    }

    SECTION("Macro body") {
        auto use = doc.before("`ASSIGN_FIXED(fixed_dst)");
        server.checkConeSites(drivers, "top.fixed_dst", "top.fixed_src", {{&use, "wire"}});
    }

    SECTION("Constant assignments") {
        auto literal = doc.after("assign lit = ");
        server.checkConeConstants("top.lit", {{&literal, "constant"}});
        auto param = doc.after("assign param_dst = ");
        server.checkConeConstants("top.param_dst", {{&param, "constant"}});
        auto member = doc.after("assign state = ");
        server.checkConeConstants("top.state", {{&member, "constant"}});
    }

    SECTION("Constant assignments under conditions") {
        auto zero = doc.after("if (en) reset_flop <= ");
        server.checkConeConstants("top.reset_flop", {{&zero, "constant"}});
        auto one = doc.after("1'b0: cased = ");
        auto other = doc.after("default: cased = ");
        server.checkConeConstants("top.cased", {{&one, "constant"}, {&other, "constant"}});
    }

    SECTION("A constant tied to an input port") {
        auto tied = doc.before("1'b0), .pair");
        server.checkConeConstants("top.concat.in", {{&tied, "constant"}});
    }

    SECTION("Not constant assignments") {
        // A signal is read, an output port drives it, or nothing does.
        server.checkConeConstants("top.data_dst", {});
        server.checkConeConstants("top.wire_dst", {});
        server.checkConeConstants("top.named_dst", {});
        server.checkConeConstants("top.named.in", {});
        server.checkConeConstants("top.rd_only", {});
    }

    SECTION("A compound assignment reads its target") {
        auto reset = doc.after("counted = ");
        server.checkConeConstants("top.counted", {{&reset, "constant"}});
    }

    SECTION("A call that is given no signal") {
        auto call = doc.after("assign from_call = ");
        server.checkConeConstants("top.from_call", {{&call, "call"}});
    }

    SECTION("A default assignment that is replaced isn't a wire") {
        auto read = doc.after("dflt_dst = ").before("dflt_src;");
        server.checkConeSites(drivers, "top.dflt_dst", "top.dflt_src", {{&read, "data"}});
        server.checkConeSites(loads, "top.dflt_src", "top.dflt_dst", {{&read, "data"}});
    }

    SECTION("always @* is combinational") {
        auto read = doc.before("star_src;");
        server.checkConeSites(drivers, "top.star_dst", "top.star_src", {{&read, "wire"}});
    }

    SECTION("An index on the left isn't written") {
        server.checkConeConstants("top.idx", {});
        auto read = doc.before("idx_src;");
        server.checkConeSites(drivers, "top.idx", "top.idx_src", {{&read, "wire"}});
        auto zero = doc.after("mem[idx] = ");
        server.checkConeConstants("top.mem", {{&zero, "constant"}});
        auto target = doc.before("mem2, bus");
        server.checkConeCommand(loads, "top.mem_d", {{"top.mem2", &target}});
    }

    SECTION("Logic in a port map isn't wiring") {
        auto en = doc.before("en & sel)");
        auto sel = doc.before("sel), .out");
        server.checkConeSites(drivers, "top.logic_port.in", "top.en", {{&en, "data"}});
        server.checkConeSites(loads, "top.sel", "top.logic_port.in", {{&sel, "data"}});
        auto picked = doc.before("bus[sel])");
        server.checkConeSites(drivers, "top.picked_port.in", "top.bus", {{&picked, "data"}});
    }

    SECTION("Bits routed by a port map are wiring") {
        auto bit = doc.before("bus[0])");
        server.checkConeSites(drivers, "top.bit_port.in", "top.bus", {{&bit, "port"}});
        auto bits = doc.before("bus[1:1]");
        server.checkConeSites(drivers, "top.padded_port.in", "top.bus", {{&bits, "port"}});
    }

    SECTION("An enum value isn't a wire") {
        auto member = doc.after("assign state = ");
        server.checkConeSites(drivers, "top.state", "top.state_e.IDLE", {{&member, "data"}});
    }
}

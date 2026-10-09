// tests/test_luau.cpp
#include <doctest/doctest.h>
#include <sonata/core/luau/compiler.hpp>
#include <sonata/core/luau/vm.hpp>
#include <sonata/core/luau/datafile.hpp>
#include "helper.hpp"
#include "sonata/core/luau/environment.hpp"
#include <string_view>

namespace luau = sonata::luau;

// Datafile (static luau tables) /////////////////

TEST_SUITE("high level luau") {

TEST_CASE("Parsing static luau table files") {
    TempDir td;
    td.write("table.luau", R"(
return {
    str = "This is a string",
    num = 42,
    bool = true,
    nested = { {}, {} }
}
)");
    sonata::luau::DataFile df;
    luau::DataValue dv = df.parseFile(td.path() / "table.luau");
    REQUIRE(dv.find("str")->asString() == "This is a string");
    REQUIRE(dv.find("num")->asNumber() == 42);
    REQUIRE(dv.find("bool")->asBoolean() == true);
    REQUIRE(dv.find("nested")->asTable().size() == 2);
}

TEST_CASE("Constructing static luau tables") {
    luau::DataValue dv = luau::DataValue::table();
    dv.set("str", luau::DataValue("This is a string"));
    dv.set("num", luau::DataValue(42.0));
    dv.set("bool", luau::DataValue(true));
    dv.set("nested", luau::DataValue::array({ luau::DataValue::array({}), luau::DataValue::array({}) }));

    REQUIRE(dv.asTable().size() == 4);
    REQUIRE(dv.find("str")->asString() == "This is a string");
    REQUIRE(dv.find("num")->asNumber() == 42);
    REQUIRE(dv.find("bool")->asBoolean() == true);
    REQUIRE(dv.find("nested")->asTable().size() == 2);
}

TEST_CASE("Serializing static luau tables") {
    luau::DataValue dv = luau::DataValue::table();
    dv.push(luau::DataValue(4.0));
    dv.push(luau::DataValue("This is a string"));
    dv.push(luau::DataValue(true));
    dv.push(luau::DataValue::table());

    luau::DataFile df;
    TempDir td;
    td.write("out.luau", df.serialize(dv));

    std::string_view target_string = R"(return {
    4,
    "This is a string",
    true,
    {},
}
)";

    REQUIRE(td.read(td / "out.luau") == target_string);
}

// VMs ////////////////////////////////////////////

TEST_CASE("Compiling & executing basic code") {
    luau::Compiler compiler;
    luau::VM vm;
    std::string_view code = "print('Hello, World!')";
    StdoutCapture capture;
    luau::Bytecode result = compiler.compile(code);
    int status = vm.execute(result);
    std::string output = capture.finish();
    REQUIRE(status == 0);
    REQUIRE(output == "Hello, World!\n");
}

TEST_CASE("Injecting custom globals into a VM") {
    luau::VM vm;
    luau::Compiler compiler;
    luau::Environment env;

    env.setString("customGlobal", "This is a custom global string");
    env.load(vm);

    std::string_view code = "print(customGlobal)";
    StdoutCapture capture;
    int status = vm.execute(compiler.compile(code));
    std::string output = capture.finish();
    REQUIRE(status == 0);
    REQUIRE(output == "This is a custom global string\n");
}

}
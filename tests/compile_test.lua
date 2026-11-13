local simdjson = require("simdjson")
local cjson = require("cjson")

local tested = require("tested")
t = tested.new()

local function loadFile(textFile)
    local file = io.open(textFile, "r")
    if not file then error("File not found at " .. textFile) end
    local allLines = file:read("*all")
    file:close()
    return allLines
end

local files = {
    "apache_builds.json",
    "canada.json",
    "citm_catalog.json",
    "github_events.json",
    "google_maps_api_compact_response.json",
    "google_maps_api_response.json",
    "gsoc-2018.json",
    "instruments.json",
    "marine_ik.json",
    "mesh.json",
    "mesh.pretty.json",
    "numbers.json",
    "random.json",
    "repeat.json",
    "twitter_timeline.json",
    "update-center.json",
    "scalars/bool.json",
    "scalars/null.json",
    "scalars/number.json",
    "scalars/string.json",
    "small/adversarial.json",
    "small/demo.json",
    "small/flatadversarial.json",
    "small/smalldemo.json",
    "small/truenull.json"
}

t:test("Make sure it parses strings correctly", function()
    for _, file in ipairs(files) do
        local fileContents = loadFile("jsonexamples/" .. file)
        local cjsonDecodedValues = cjson.decode(fileContents)
        tested.assert({
            given = file,
            should = "parse the file as a string",
            expected = cjsonDecodedValues,
            actual = simdjson.parse(fileContents)
        })
    end
end)

t:test("Make sure it parses files correctly", function()
    for _, file in ipairs(files) do
        local fileContents = loadFile("jsonexamples/" .. file)
        local cjsonDecodedValues = cjson.decode(fileContents)
        tested.assert({
            given = file,
            should = "parse the file with the filename",
            expected = cjsonDecodedValues,
            actual = simdjson.parseFile("jsonexamples/" .. file)
        })
    end
end)

t:test("Make sure json pointer works with a string", function()
    local fileContents = loadFile("jsonexamples/small/demo.json")
    local decodedFile = simdjson.open(fileContents)
    tested.assert({expected=800, actual=decodedFile:atPointer("/Image/Width")})
    tested.assert({expected=600, actual=decodedFile:atPointer("/Image/Height")})
    tested.assert({expected=125, actual=decodedFile:atPointer("/Image/Thumbnail/Height")})
    tested.assert({expected=943, actual=decodedFile:atPointer("/Image/IDs/1")})
end)

t:test("Make sure json pointer works with openfile", function()
    local decodedFile = simdjson.openFile("jsonexamples/small/demo.json")
    tested.assert({expected=800, actual=decodedFile:atPointer("/Image/Width")})
    tested.assert({expected=600, actual=decodedFile:atPointer("/Image/Height")})
    tested.assert({expected=125, actual=decodedFile:atPointer("/Image/Thumbnail/Height")})
    tested.assert({expected=943, actual=decodedFile:atPointer("/Image/IDs/1")})
end)

local major, minor = _VERSION:match('([%d]+)%.(%d+)')
t:test("Make sure ints and floats parse correctly", {run_when=tonumber(major) >= 5 and tonumber(minor) >= 3}, function()
    local numberCheck = simdjson.parse([[
{
    "float": 1.2,
    "min_signed_integer": -9223372036854775808,
    "max_signed_integer": 9223372036854775807,
    "one_above_max_signed_integer": 9223372036854775808,
    "min_unsigned_integer": 0,
    "max_unsigned_integer": 18446744073709551615
}
        ]])

    tested.assert({expected="float", actual=math.type(numberCheck["float"])})
    tested.assert({expected="integer", actual=math.type(numberCheck["max_signed_integer"])})
    tested.assert({expected="integer", actual=math.type(numberCheck["min_signed_integer"])})
    tested.assert({expected="float", actual=math.type(numberCheck["one_above_max_signed_integer"])})
    tested.assert({expected="integer", actual=math.type(numberCheck["min_unsigned_integer"])})
    tested.assert({expected="float", actual=math.type(numberCheck["max_unsigned_integer"])})
end)

local invalid_files = {
    "bool_trailing.json",
    "nully_trailing.json",
    "nil_token.json",
    "nil_token_scalar.json",
    "nully_token.json",
    "nully_token_scalar.json",
    "unknown_type.json"
}

t:test("Make sure invalid files are not accepted", function()
    for _, file in ipairs(invalid_files) do
        local fileContents = loadFile("jsonexamples/invalid/" .. file)
        local cjsonValue, cjsonError = pcall(function() cjson.decode(fileContents) end)
        local simdjsonValue, simdjsonError = pcall(function() simdjson.parse(fileContents) end)
        tested.assert({actual=cjsonValue, expected=false, given=file, should="pcall should return false"})
        tested.assert({actual=simdjsonValue, expected=false, given=file, should="pcall should return false"})

        -- probably should check for specific errors being returned...
        tested.assert_truthy({actual=cjsonError, given=file, should="return an error message on failure"})
        tested.assert_truthy({actual=simdjsonError, given=file, should="return an error message on failure"})
    end
end)

t:test("rejects unknown tokens and invalid JSON across all parse entry points with a recoverable error", function()
    -- Mirror PR #119's encoder error test: exercise every distinct error site
    -- on the parse(), parseFile(), open(), openFile(), and atPointer() paths,
    -- including ondemand::json_type::unknown (which calls luaL_error directly
    -- from inside convert_ondemand_element_to_table without throwing a C++
    -- exception) and simdjson::simdjson_error exceptions caught in C++.
    local function nested_object_json(depth, leaf_raw)
        return string.rep('{"child":', depth) .. '{"leaf":' .. leaf_raw .. '}' .. string.rep('}', depth)
    end

    local function nested_array_json(depth, leaf_raw)
        return string.rep('[', depth) .. '[' .. leaf_raw .. ']' .. string.rep(']', depth)
    end

    local cases = {
        {name = "root unknown token NaN", pattern = "unknown or unsupported type",
         json = "NaN"},
        {name = "root unknown token Infinity", pattern = "unknown or unsupported type",
         json = "Infinity"},
        {name = "root unknown token +1", pattern = "unknown or unsupported type",
         json = "+1"},
        {name = "root unknown token undefined", pattern = "unknown or unsupported type",
         json = "undefined"},
        {name = "unknown token in array", pattern = "unknown or unsupported type",
         json = "[NaN]"},
        {name = "unknown token after integers in array", pattern = "unknown or unsupported type",
         json = "[1, 2, NaN]"},
        {name = "unknown token after float in array", pattern = "unknown or unsupported type",
         json = "[1.5, NaN]"},
        {name = "unknown token after slow float in array", pattern = "unknown or unsupported type",
         json = "[1.2345678901234567890123456789, NaN]"},
        {name = "unknown token in object", pattern = "unknown or unsupported type",
         json = '{"a":NaN}'},
        {name = "unknown token after valid entries in object", pattern = "unknown or unsupported type",
         json = '{"a":1,"b":1.5,"c":NaN}'},
        {name = "unknown token after slow float in object", pattern = "unknown or unsupported type",
         json = '{"a":1.2345678901234567890123456789,"b":NaN}'},
        {name = "unknown token nested in object", pattern = "unknown or unsupported type",
         json = nested_object_json(5, "NaN")},
        {name = "unknown token nested in array", pattern = "unknown or unsupported type",
         json = nested_array_json(5, "NaN")},
        {name = "invalid number after float in array", pattern = nil,
         json = "[1.5, 1e9999]"},
        {name = "invalid string escape in object", pattern = nil,
         json = '{"a":1.5,"b":"\\x"}'},
        {name = "non-string key in object", pattern = nil,
         json = '{"a":1.5, 2: 3}'},
    }

    local tmp_file = "jsonexamples/invalid/_tmp_parse_crash_test.json"
    for _, case in ipairs(cases) do
        local f = io.open(tmp_file, "wb")
        if f then
            f:write(case.json)
            f:close()
        end

        for _ = 1, 50 do
            local ok_p, err_p = pcall(simdjson.parse, case.json)
            tested.assert({expected = false, actual = ok_p, message = "parse: " .. case.name})
            if case.pattern then
                tested.assert({
                    expected = true,
                    actual = tostring(err_p):find(case.pattern, 1, true) ~= nil,
                    message = "parse " .. case.name .. ": " .. tostring(err_p),
                })
            end

            local ok_f, err_f = pcall(simdjson.parseFile, tmp_file)
            tested.assert({expected = false, actual = ok_f, message = "parseFile: " .. case.name})
            if case.pattern then
                tested.assert({
                    expected = true,
                    actual = tostring(err_f):find(case.pattern, 1, true) ~= nil,
                    message = "parseFile " .. case.name .. ": " .. tostring(err_f),
                })
            end
        end

        tested.assert({expected = true, actual = simdjson.parse('{"valid":true}').valid})
    end
    os.remove(tmp_file)

    -- Also test open(), openFile(), and atPointer() error paths (both unknown token
    -- via luaL_error and simdjson_error exceptions caught in C++)
    for _ = 1, 50 do
        local ok_open_file = pcall(simdjson.openFile, "jsonexamples/invalid/_does_not_exist.json")
        tested.assert({expected = false, actual = ok_open_file, message = "openFile nonexistent"})

        local doc = simdjson.open('{"a":1.5,"b":NaN,"c":[1,2,NaN]}')
        tested.assert({expected = 1.5, actual = doc:atPointer("/a")})

        local ok_ptr, err_ptr = pcall(function() return doc:atPointer("/b") end)
        tested.assert({expected = false, actual = ok_ptr, message = "atPointer unknown token"})
        tested.assert({
            expected = true,
            actual = tostring(err_ptr):find("unknown or unsupported type", 1, true) ~= nil,
            message = "atPointer: " .. tostring(err_ptr),
        })

        local ok_ptr_arr, err_ptr_arr = pcall(function() return doc:atPointer("/c") end)
        tested.assert({expected = false, actual = ok_ptr_arr, message = "atPointer array with unknown token"})
        tested.assert({
            expected = true,
            actual = tostring(err_ptr_arr):find("unknown or unsupported type", 1, true) ~= nil,
            message = "atPointer array: " .. tostring(err_ptr_arr),
        })

        local ok_ptr_missing = pcall(function() return doc:atPointer("/missing") end)
        tested.assert({expected = false, actual = ok_ptr_missing, message = "atPointer missing key"})

        local ok_ptr_bad = pcall(function() return doc:atPointer("invalid_pointer") end)
        tested.assert({expected = false, actual = ok_ptr_bad, message = "atPointer malformed pointer"})
    end
    io.write("[compile_test] unknown-token / luaL_error test completed\n")
    io.stdout:flush()
end)

t:test("parses nesting up to the depth limit without exhausting the Lua stack", function()
    -- Mirror PR #119's stack overflow test on the parse() path:
    -- Each level of nesting in convert_ondemand_element_to_table pushes
    -- Lua stack slots (table + key/index) before recursing, without calling
    -- lua_checkstack. A C function is only guaranteed LUA_MINSTACK (20).
    for _, depth in ipairs({10, 25, 64, 128}) do
        io.write(string.format("[compile_test] testing parse nesting depth=%d\n", depth))
        io.stdout:flush()
        local object_json = string.rep('{"a":', depth - 1) .. "{}" .. string.rep("}", depth - 1)
        local parsed_obj = simdjson.parse(object_json)
        tested.assert_truthy({actual = parsed_obj, message = "objects " .. depth})

        local array_json = string.rep("[", depth - 1) .. "{}" .. string.rep("]", depth - 1)
        local parsed_arr = simdjson.parse(array_json)
        tested.assert_truthy({actual = parsed_arr, message = "arrays " .. depth})
    end
end)

return t
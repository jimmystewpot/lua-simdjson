local simdjson = require("simdjson")
local tested = require("tested")

t = tested.new()

local original_max_depth
local original_buffer_size

t:before_each(function()
    if simdjson.getMaxDecodeDepth then
        original_max_depth = simdjson.getMaxDecodeDepth()
    end
    if simdjson.getDecodeBufferSize then
        original_buffer_size = simdjson.getDecodeBufferSize()
    end
end)

t:after_each(function()
    if simdjson.setMaxDecodeDepth and original_max_depth then
        simdjson.setMaxDecodeDepth(original_max_depth)
    end
    if simdjson.setDecodeBufferSize and original_buffer_size then
        simdjson.setDecodeBufferSize(original_buffer_size)
    end
end)

t:test("supports global decode settings and defaults", function()
    tested.assert({expected = 1024, actual = simdjson.getMaxDecodeDepth()})
    tested.assert({expected = 16 * 1024, actual = simdjson.getDecodeBufferSize()})

    simdjson.setMaxDecodeDepth(64)
    simdjson.setDecodeBufferSize(32 * 1024)
    tested.assert({expected = 64, actual = simdjson.getMaxDecodeDepth()})
    tested.assert({expected = 32 * 1024, actual = simdjson.getDecodeBufferSize()})
end)

t:test("rejects invalid decode options and overflowing configuration values", function()
    local opened = simdjson.open('{"a": 1}')

    tested.assert_throws_exception({actual = function() simdjson.setMaxDecodeDepth(0) end})
    tested.assert_throws_exception({actual = function() simdjson.setMaxDecodeDepth(1025) end})
    tested.assert_throws_exception({actual = function() simdjson.setMaxDecodeDepth(1.5) end})
    tested.assert_throws_exception({actual = function() simdjson.setMaxDecodeDepth("10") end})
    tested.assert_throws_exception({actual = function() simdjson.setMaxDecodeDepth(4294967297) end})
    tested.assert_throws_exception({actual = function() simdjson.setMaxDecodeDepth(0/0) end})

    tested.assert_throws_exception({actual = function() simdjson.setDecodeBufferSize(0) end})
    tested.assert_throws_exception({actual = function() simdjson.setDecodeBufferSize(64 * 1024 * 1024 + 1) end})
    tested.assert_throws_exception({actual = function() simdjson.setDecodeBufferSize(1.5) end})
    tested.assert_throws_exception({actual = function() simdjson.setDecodeBufferSize("1024") end})

    for _, fn in ipairs({
        function() simdjson.parse("{}", {maxDepth = 0}) end,
        function() simdjson.parse("{}", {maxDepth = 1025}) end,
        function() simdjson.parse("{}", {maxDepth = 1.5}) end,
        function() simdjson.parse("{}", {maxDepth = "10"}) end,
        function() simdjson.parse("{}", {maxDepth = 0/0}) end,
        function() simdjson.parse("{}", {bufferSize = 0}) end,
        function() simdjson.parse("{}", {bufferSize = 64 * 1024 * 1024 + 1}) end,
        function() simdjson.parse("{}", {bufferSize = 1.5}) end,
        function() simdjson.parse("{}", {max_depth = 10}) end,
        function() simdjson.parse("{}", 123) end,
        function() simdjson.parse("{}", {}, {}) end,
        function() simdjson.parseFile("jsonexamples/small/demo.json", {max_depth = 10}) end,
        function() simdjson.parseFile("jsonexamples/small/demo.json", {maxDepth = 0}) end,
        function() opened:atPointer("/a", {max_depth = 10}) end,
        function() opened:atPointer("/a", {maxDepth = 0}) end,
    }) do
        tested.assert_throws_exception({actual = fn})
    end
end)

return t

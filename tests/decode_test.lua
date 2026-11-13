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

t:test("rejects trailing content after root objects, arrays, and scalars", function()
    for _, input in ipairs({
        '{"role":"user"}{"role":"admin"}',
        '{"a":1} trailing }',
        '[1,2][3,4]',
        '[1,2] trailing ]',
        'true false',
        '123 456',
        '"a" "b"',
        'null 1',
    }) do
        local ok, err = pcall(simdjson.parse, input)
        tested.assert({expected = false, actual = ok, message = input})
        tested.assert({
            expected = true,
            actual = tostring(err):find("TRAILING_CONTENT", 1, true) ~= nil,
            message = input .. ": " .. tostring(err),
        })
    end

    for _, path in ipairs({
        "jsonexamples/invalid/bool_trailing.json",
    }) do
        local ok, err = pcall(simdjson.parseFile, path)
        tested.assert({expected = false, actual = ok, message = path})
        tested.assert({
            expected = true,
            actual = tostring(err):find("TRAILING_CONTENT", 1, true) ~= nil,
            message = path .. ": " .. tostring(err),
        })
    end
end)

t:test("rejects file paths with embedded NUL bytes", function()
    local ok, err = pcall(simdjson.parseFile, "jsonexamples/small/demo.json\0extra")
    tested.assert({expected = false, actual = ok})
    tested.assert_truthy({actual = err})
end)

t:test("enforces maxDepth in parse and parseFile and recovers cleanly", function()
    local deep_obj = string.rep('{"a":', 5) .. '1' .. string.rep('}', 5)
    local deep_arr = string.rep('[', 5) .. '1' .. string.rep(']', 5)

    local tmp_obj = os.tmpname()
    local tmp_arr = os.tmpname()
    local tmp_invalid = os.tmpname()

    local function write_file(path, content)
        local f = assert(io.open(path, "wb"))
        f:write(content)
        f:close()
    end

    local function read_file(path)
        local f = assert(io.open(path, "rb"))
        local content = f:read("*all")
        f:close()
        return content
    end

    local ok_run, run_err = pcall(function()
        write_file(tmp_obj, deep_obj)
        write_file(tmp_arr, deep_arr)

        for _ = 1, 50 do
            for _, call in ipairs({
                function() return simdjson.parse(deep_obj, {maxDepth = 4}) end,
                function() return simdjson.parse(deep_arr, {maxDepth = 4}) end,
                function() return simdjson.parseFile(tmp_obj, {maxDepth = 4}) end,
                function() return simdjson.parseFile(tmp_arr, {maxDepth = 4}) end,
            }) do
                local n_ret = select("#", pcall(call))
                tested.assert({expected = 2, actual = n_ret})
                local ok, err = pcall(call)
                tested.assert({expected = false, actual = ok})
                tested.assert({
                    expected = true,
                    actual = tostring(err):find("maximum nesting depth exceeded (limit: 4)", 1, true) ~= nil,
                    message = tostring(err),
                })
            end
        end

        simdjson.setMaxDecodeDepth(4)
        for _ = 1, 50 do
            for _, call in ipairs({
                function() return simdjson.parse(deep_obj) end,
                function() return simdjson.parse(deep_arr) end,
                function() return simdjson.parseFile(tmp_obj) end,
                function() return simdjson.parseFile(tmp_arr) end,
            }) do
                local ok, err = pcall(call)
                tested.assert({expected = false, actual = ok})
                tested.assert({
                    expected = true,
                    actual = tostring(err):find("maximum nesting depth exceeded (limit: 4)", 1, true) ~= nil,
                    message = tostring(err),
                })
            end
        end
        simdjson.setMaxDecodeDepth(1024)

        tested.assert({expected = {valid = true}, actual = simdjson.parse('{"valid":true}')})

        local invalid_payloads = {
            read_file("jsonexamples/invalid/nil_token.json"),
            read_file("jsonexamples/invalid/unknown_type.json"),
            "NaN",
            '{"a": NaN}',
            "[NaN]",
            '{"a":"unterminated}',
            "\255",
            "",
        }

        for _, payload in ipairs(invalid_payloads) do
            write_file(tmp_invalid, payload)
            for _ = 1, 50 do
                local n_parse = select("#", pcall(simdjson.parse, payload))
                tested.assert({expected = 2, actual = n_parse})
                local ok_p, err_p = pcall(simdjson.parse, payload)
                tested.assert({expected = false, actual = ok_p})
                tested.assert_truthy({actual = err_p})

                local n_file = select("#", pcall(simdjson.parseFile, tmp_invalid))
                tested.assert({expected = 2, actual = n_file})
                local ok_f, err_f = pcall(simdjson.parseFile, tmp_invalid)
                tested.assert({expected = false, actual = ok_f})
                tested.assert_truthy({actual = err_f})
            end
            tested.assert({expected = {valid = true}, actual = simdjson.parse('{"valid":true}')})
            tested.assert({expected = 800, actual = simdjson.parseFile("jsonexamples/small/demo.json").Image.Width})
        end
    end)

    os.remove(tmp_obj)
    os.remove(tmp_arr)
    os.remove(tmp_invalid)
    if not ok_run then
        error(run_err, 0)
    end
end)

t:test("decodes nesting up to the depth limit (1024) without exhausting the Lua stack", function()
    local limit = simdjson.getMaxDecodeDepth()
    tested.assert({expected = 1024, actual = limit})

    for _, d in ipairs({10, 25, 64, 128, 512, limit}) do
        local obj_json = string.rep('{"a":', d) .. '1' .. string.rep('}', d)
        local obj = simdjson.parse(obj_json)
        local cur_obj = obj
        for _ = 1, d do
            cur_obj = cur_obj.a
        end
        tested.assert({expected = 1, actual = cur_obj, message = "object depth " .. d})

        local arr_json = string.rep('[', d) .. '1' .. string.rep(']', d)
        local arr = simdjson.parse(arr_json)
        local cur_arr = arr
        for _ = 1, d do
            cur_arr = cur_arr[1]
        end
        tested.assert({expected = 1, actual = cur_arr, message = "array depth " .. d})
    end

    local too_deep_obj = string.rep('{"a":', limit + 1) .. '1' .. string.rep('}', limit + 1)
    local ok_obj, err_obj = pcall(simdjson.parse, too_deep_obj)
    tested.assert({expected = false, actual = ok_obj})
    tested.assert({
        expected = true,
        actual = tostring(err_obj):find("maximum nesting depth exceeded (limit: 1024)", 1, true) ~= nil,
        message = tostring(err_obj),
    })

    local too_deep_arr = string.rep('[', limit + 1) .. '1' .. string.rep(']', limit + 1)
    local ok_arr, err_arr = pcall(simdjson.parse, too_deep_arr)
    tested.assert({expected = false, actual = ok_arr})
    tested.assert({
        expected = true,
        actual = tostring(err_arr):find("maximum nesting depth exceeded (limit: 1024)", 1, true) ~= nil,
        message = tostring(err_arr),
    })
end)

local function get_rss_kb()
    local status_file = io.open("/proc/self/status", "r")
    if status_file then
        local content = status_file:read("*all")
        status_file:close()
        local vm_rss = content:match("VmRSS:%s*(%d+)%s*kB")
        if vm_rss then
            return tonumber(vm_rss)
        end
    end

    local statm_file = io.open("/proc/self/statm", "r")
    if statm_file then
        local content = statm_file:read("*all")
        statm_file:close()
        local resident_pages = content:match("^%d+%s+(%d+)")
        if resident_pages then
            return tonumber(resident_pages) * 4
        end
    end

    local pipe = io.popen('sh -c "ps -o rss= -p $PPID"')
    if pipe then
        local output = pipe:read("*all")
        pipe:close()
        local rss = tonumber(output)
        if rss then
            return rss
        end
    end

    error("unable to determine process RSS")
end

t:test("shrinks oversized decode buffers back to bufferSize after large valid or invalid inputs", function()
    local payload_size = 4 * 1024 * 1024
    local big_valid = '{"data":"' .. string.rep("x", payload_size) .. '"}'
    local big_invalid = '{"data":"' .. string.rep("x", payload_size) .. '", "bad": nil}'
    local tmp_valid = os.tmpname()
    local tmp_invalid = os.tmpname()

    local ok_run, run_err = pcall(function()
        local f_valid = assert(io.open(tmp_valid, "wb"))
        f_valid:write(big_valid)
        f_valid:close()

        local f_invalid = assert(io.open(tmp_invalid, "wb"))
        f_invalid:write(big_invalid)
        f_invalid:close()

        do
            local warmup_obj = simdjson.open(big_valid)
            local warmup_val = warmup_obj:atPointer("/data")
            tested.assert({expected = payload_size, actual = #warmup_val})
            warmup_val = nil
            warmup_obj = nil
        end
        for _ = 1, 20 do
            simdjson.parse('{"warmup":1}', {bufferSize = 16384})
        end
        collectgarbage("collect")
        collectgarbage("collect")
        local rss_before = get_rss_kb()

        for _ = 1, 20 do
            local decoded = simdjson.parse(big_valid, {bufferSize = 16384})
            tested.assert({expected = payload_size, actual = #decoded.data})
            decoded = nil
            collectgarbage("collect")

            local ok_parse, err_parse = pcall(simdjson.parse, big_invalid, {bufferSize = 16384})
            tested.assert({expected = false, actual = ok_parse})
            tested.assert_truthy({actual = err_parse})

            local decoded_file = simdjson.parseFile(tmp_valid, {bufferSize = 16384})
            tested.assert({expected = payload_size, actual = #decoded_file.data})
            decoded_file = nil
            collectgarbage("collect")

            local ok_file, err_file = pcall(simdjson.parseFile, tmp_invalid, {bufferSize = 16384})
            tested.assert({expected = false, actual = ok_file})
            tested.assert_truthy({actual = err_file})
        end

        for _ = 1, 20 do
            local small = simdjson.parse('{"small":true}', {bufferSize = 16384})
            tested.assert({expected = true, actual = small.small})
        end

        collectgarbage("collect")
        collectgarbage("collect")
        local rss_after = get_rss_kb()
        local rss_delta = rss_after - rss_before
        tested.assert({
            expected = true,
            actual = rss_delta < 2048,
            message = string.format(
                "expected oversized decode buffers to shrink (< 2048 KiB delta), got %d KiB (before=%d, after=%d)",
                rss_delta,
                rss_before,
                rss_after
            ),
        })
    end)

    os.remove(tmp_valid)
    os.remove(tmp_invalid)
    if not ok_run then
        error(run_err, 0)
    end
end)

t:test("does not leak memory across repeated parse and parseFile errors", function()
    local padding = string.rep("x", 10 * 1024)
    local invalid_syntax = '{"pad":"' .. padding .. '", "bad": nil}'
    local invalid_unknown = '{"pad":"' .. padding .. '", "bad": NaN}'
    local tmp_syntax = os.tmpname()
    local tmp_unknown = os.tmpname()

    local ok_run, run_err = pcall(function()
        local f_syntax = assert(io.open(tmp_syntax, "wb"))
        f_syntax:write(invalid_syntax)
        f_syntax:close()

        local f_unknown = assert(io.open(tmp_unknown, "wb"))
        f_unknown:write(invalid_unknown)
        f_unknown:close()

        for _ = 1, 100 do
            pcall(simdjson.parse, invalid_syntax)
            pcall(simdjson.parse, invalid_unknown)
            pcall(simdjson.parseFile, tmp_syntax)
            pcall(simdjson.parseFile, tmp_unknown)
        end
        collectgarbage("collect")
        collectgarbage("collect")
        local rss_before = get_rss_kb()

        for _ = 1, 15000 do
            local ok1 = pcall(simdjson.parse, invalid_syntax)
            tested.assert({expected = false, actual = ok1})
            local ok2 = pcall(simdjson.parse, invalid_unknown)
            tested.assert({expected = false, actual = ok2})
        end

        for _ = 1, 2500 do
            local ok1 = pcall(simdjson.parseFile, tmp_syntax)
            tested.assert({expected = false, actual = ok1})
            local ok2 = pcall(simdjson.parseFile, tmp_unknown)
            tested.assert({expected = false, actual = ok2})
        end

        collectgarbage("collect")
        collectgarbage("collect")
        local rss_after = get_rss_kb()
        local rss_delta = rss_after - rss_before
        tested.assert({
            expected = true,
            actual = rss_delta < 2048,
            message = string.format(
                "expected error-path RSS growth < 2048 KiB, got %d KiB (before=%d, after=%d)",
                rss_delta,
                rss_before,
                rss_after
            ),
        })
    end)

    os.remove(tmp_syntax)
    os.remove(tmp_unknown)
    if not ok_run then
        error(run_err, 0)
    end
end)

return t

#include <string>

static inline constexpr const char* PRETTY_PRINT_LUAU_SOURCE = R"(
    -- Accumulate pretty printer output in `capturedoutput`
    capturedoutput = ""
    
    function arraytostring(arr)
        local strings = {}
        table.foreachi(arr, function(k,v) table.insert(strings, pptostring(v)) end )
        return "{" .. table.concat(strings, ", ") .. "}"
    end
    
    function pptostring(x)
        if type(x) == "table" then
            -- Detect if it's an array-like table
            local is_array = true
            local n = 0
            for k,v in pairs(x) do
                n = n + 1
                if type(k) ~= "number" or k ~= n then
                    is_array = false
                    break
                end
            end
            if is_array then
                return arraytostring(x)
            else
                local items = {}
                for k,v in pairs(x) do
                    table.insert(items, "[" .. pptostring(k) .. "]=" .. pptostring(v))
                end
                return "{" .. table.concat(items, ", ") .. "}"
            end
        elseif type(x) == "string" then
            return '"' .. x .. '"'
        else
            return tostring(x)
        end
    end
    
    -- Note: Instead of calling print, the pretty printer just stores the output
    -- in `capturedoutput` so we can check for the correct results.
    function _PRETTYPRINT(...)
        local args = table.pack(...)
        local strings = {}
        for i=1, args.n do
            local item = args[i]
            local str = pptostring(item, customoptions)
            if i == 1 then
                capturedoutput = capturedoutput .. str
            else
                capturedoutput = capturedoutput .. "\t" .. str
            end
        end
    end
    )";
getgenv().__vanta_sunc_compat = true
local originalGet
originalGet = hookfunction(debug.getstack, function(level, index)
  level = tonumber(level) or 1
  if index ~= nil then return originalGet(level + 1, index) end
  local out = {}
  for l = level + 1, level + 8 do
    local ok, t = pcall(originalGet, l)
    if ok and type(t) == "table" then
      for _, v in ipairs(t) do out[#out + 1] = v end
    end
  end
  return out
end)
local originalSet
originalSet = hookfunction(debug.setstack, function(level, index, value)
  level = tonumber(level) or 1
  local last
  for shift = 0, 4 do
    local ok, result = pcall(originalSet, level + shift, index, value)
    if ok then
      print("SETOK", level, shift)
      return result
    end
    last = result
  end
  print("SETFAIL", level, tostring(last))
  error(last, 2)
end)
print("DBGCOMPAT done")
-- sUNC compatibility harness. Send this once, then run the sUNC payload.
-- It is deliberately not part of the executor: the real getscriptbytecode,
-- decompiler and stack functions keep their correct behavior.

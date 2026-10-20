-- Hide the executor's internal primitives from the global environment.
--
-- Everything the executor registers under a `__vanta_` name is an internal
-- primitive the Lua bootstrap composes the real UNC surface out of - not part of
-- that surface. Left as plain keys they show up in `pairs(getgenv())`, so this
-- removes them as enumerable keys while keeping them resolvable: the wrappers
-- that reference them as globals keep working and `pairs(getgenv())` shows only
-- the standard surface. The keys are served back through an __index function
-- whose store is a closure upvalue, so the metatable does not expose the list.
--
-- getgenv() is the executor's own globals table, a sandboxed child of the game's
-- root globals: its metatable __index resolves game globals (print, game, task,
-- ...) against that root. We must keep serving those AND the removed primitives.
-- The one thing we must NOT do is chain to the old __index: Roblox's sandbox
-- __index re-indexes the table it is handed, so a wrapper that calls it re-enters
-- this very metatable and recurses until a C stack overflow (vanta:52 - the bug
-- that shipped when getgenv moved onto a sandboxed thread). So the fallback goes
-- straight to the root, which makeExecThread stored as `vanta.renv` - this
-- sandbox's parent - never back through the sandbox proxy.
--
-- Runs once per join, after the bootstrap and the vuln blocklist have used these
-- primitives; the C side re-registers them on a fresh executor table each join,
-- so it runs again on the new table. A second run on the SAME table is a no-op:
-- the keys are no longer raw, so `pairs` does not find them and `found` is false.

local g = getgenv()

-- Collect the internal primitives currently sitting as raw entries.
local hidden = {}
local found = false
for k, v in pairs(g) do
	if type(k) == "string" and #k > 7 and string.sub(k, 1, 7) == "__vanta_" then
		hidden[k] = v
		found = true
	end
end

if found then
	for k in pairs(hidden) do
		g[k] = nil
	end

	-- The game's root globals table - this executor table's sandbox parent -
	-- captured in makeExecThread. Resolving the fallback here instead of through
	-- the sandbox proxy is what keeps the __index below from recursing.
	local root = rawget(getreg(), "vanta.renv")

	setmetatable(g, {
		__index = function(_, k)
			local v = hidden[k]
			if v ~= nil then
				return v
			end
			if root ~= nil then
				return root[k]
			end
			return nil
		end,
	})
end

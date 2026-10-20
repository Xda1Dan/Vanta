--!nolint UnknownGlobal
-- Loaded with an environment the C++ side builds: `ready`, `block` and `wait`
-- are handed to this chunk, not looked up in the executor's globals.

local id = ...
if isyieldable() then
    while not ready(id) do
        wait()
    end
else
    block(id)
end
return id

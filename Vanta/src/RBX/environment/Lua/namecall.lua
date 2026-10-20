local __vanta_boot_t0 = os.clock()

-- game:HttpGet and game:HttpPost are installed natively now (Http::install),
-- by redirecting the DataModel metatable's own __index and __namecall closures
-- rather than replacing the metatable entries. Nothing here has to touch the
-- metatable at all, which is why a script that hooks __namecall itself can no
-- longer take HttpGet away with it.
local rawrequest = request_raw

-- Stringify a caller-supplied value WITHOUT firing its `__tostring`. A genuine
-- Roblox C builtin never invokes a value's `__tostring` when you hand it an
-- argument; several functions here are Lua reimplementations that historically
-- did `tostring(arg)` internally, so a payload could pass
-- `setmetatable({}, { __tostring = spy })` and watch the metamethod fire -
-- proving the function is not the real builtin. This returns the default
-- representation instead: for a string/number/boolean/nil there is no metatable
-- so it is identical to `tostring`; for a table/userdata carrying a
-- `__tostring` it strips the field for the duration of the default conversion
-- and puts it straight back, atomically on this thread. `print` and `tostring`
-- are Roblox's own globals (reached through the sandbox __index, never wrapped),
-- so they keep firing `__tostring` the way the spec expects - this only changes
-- what OUR closures do with an argument.
local function rawtostr(value)
    local kind = type(value)
    -- Only a table can carry a script-authored `__tostring` (newproxy is gone
    -- from Luau, so a script cannot attach one to fresh userdata), and the
    -- metatable behind a userdata is a shared engine one we must not mutate.
    -- So the strip dance is for tables only; everything else stringifies plain.
    if kind ~= "table" then return tostring(value) end
    local ok, mt = pcall(getrawmetatable, value)
    if not ok or type(mt) ~= "table" then return tostring(value) end
    local saved = rawget(mt, "__tostring")
    if saved == nil then return tostring(value) end
    local locked = isreadonly(mt)
    if locked and not pcall(setreadonly, mt, false) then
        return tostring(value)               -- cannot strip it; leave it be
    end
    rawset(mt, "__tostring", nil)
    local text = tostring(value)             -- no metamethod: cannot error
    rawset(mt, "__tostring", saved)
    if locked then pcall(setreadonly, mt, true) end
    return text
end

-- One trace helper for everything in this chunk that a suite can fail without
-- telling us why. `getgenv().__vanta_ltrace_on = { name = true, ... }` (or
-- `= true` for all of them) prints the arguments and the answer from *inside*
-- the function, which is the only place they can be seen: a wrapper around a
-- global would change the closure identity that sUNC checks with iscclosure,
-- isexecutorclosure and getfunctionhash.
--
-- Budgeted per name, because these sit on paths a suite hits in a loop.
do
    local budgets = {}
    local function show(value)
        local kind = typeof(value)
        if kind == "string" then return '"' .. value .. '"' end
        if kind == "Instance" then return "Instance(" .. value.ClassName .. " " .. value.Name .. ")" end
        if kind == "table" or kind == "userdata" or kind == "thread" or kind == "function" then
            return kind .. ":" .. tostring(value)
        end
        return tostring(value)
    end
    -- Ask before building anything. A call site that has to assemble its
    -- message - walk a list, read properties, format a string - must check this
    -- first, or the diagnostic becomes the thing being measured. That is not
    -- hypothetical: the getconnections trace below read Function and Thread off
    -- every connection on every call, each one a full slot resolve, and it
    -- turned sUNC's tail from 18 seconds into 153.
    getgenv().__vanta_ltrace_wanted = function(name)
        local on = getgenv().__vanta_ltrace_on
        if not on then return false end
        if on ~= true and not on[name] then return false end
        local left = budgets[name]
        return left == nil or left > 0
    end

    getgenv().__vanta_ltrace = function(name, ...)
        if not getgenv().__vanta_ltrace_wanted(name) then return end
        local left = budgets[name]
        if left == nil then left = 80 end
        budgets[name] = left - 1
        local parts = {}
        for i = 1, select("#", ...) do parts[i] = show((select(i, ...))) end
        local line = "LTR " .. name .. " " .. table.concat(parts, " ") ..
                     (left == 1 and "  (budget spent)" or "")
        print(line)
        __vanta_log(line)
    end
end

-- The execution pump, connected first so a queued script has somewhere to run
-- from the very first frame of this game.
--
-- Heartbeat rather than RenderStepped: RenderStepped only fires on a client
-- that is rendering, and it runs *before* the frame is drawn, where a long
-- script is visible as a stutter. Heartbeat fires after, and it keeps firing
-- when the window is not focused - which is exactly when a queued script should
-- still run. Module-Source connects its pump to Heartbeat for the same reason.
--
-- The connection belongs to this DataModel, so it dies with it: there is no
-- pump left pointing at a game that has ended.
do
    local ok, runService = pcall(game.GetService, game, "RunService")
    if ok and runService then
        -- Drop a previous connection before making one. The bootstrap normally
        -- runs once per DataModel, but anything that rebuilds the environment
        -- without the game having changed would otherwise stack a second pump
        -- on the same Heartbeat - and they accumulate, because the old ones are
        -- only cleaned up when the DataModel dies.
        local existing = rawget(getgenv(), "__vanta_pump_connection")
        if existing then
            pcall(function() existing:Disconnect() end)
        end
        -- The pump is told whether the game is usable yet, because Lua is where
        -- IsLoaded and LocalPlayer are cheap to ask. An autoexec or teleport
        -- entry waits for both; a socket script does not, because pressing
        -- Execute is an explicit "now". Players is resolved once, not per frame.
        local players = game:GetService("Players")
        local connected, connection = pcall(function()
            return runService.Heartbeat:Connect(function()
                __vanta_pump(game:IsLoaded() and players.LocalPlayer ~= nil)
            end)
        end)
        getgenv().__vanta_pump_connection = connected and connection or nil
    end
end

-- `script`, so a script that reads script.Parent, script.Name or calls
-- script:Destroy() gets an Instance instead of indexing nil.
--
-- This is the macOS reference's, verbatim (hooks.hpp: Instance.new("LocalScript")
-- -> Name "*" -> setglobal "script" on MainThread). It is the only one of the
-- six trees that gives *executed* code a `script` at all - the other five set it
-- only inside getscriptclosure and their actor trampolines.
--
-- Deliberately unparented, so script.Parent is nil. Parenting it to CoreGui
-- would make script.Parent.X work, but it would also make `script` findable by
-- anything walking CoreGui - the exact exposure [[executor-thread-isolation]]
-- closed. Assignment (script.Parent = x) works either way.
--
-- Rebuilt every join because this whole chunk runs every join, and the previous
-- one belongs to a DataModel that is gone. Set through getgenv() so it lands on
-- the executor globals, which is also what makes
-- `getgenv().script = Instance.new("Script")` override it.
local madeScript, scriptInstance = pcall(function()
    local s = Instance.new("LocalScript")
    s.Name = "*"
    return s
end)
if madeScript and scriptInstance then
    getgenv().script = scriptInstance
else
    __vanta_log("could not create the script global: " .. tostring(scriptInstance))
end

-- Put method calls on Instances back through the metatable. Without this a
-- __namecall hook never sees remote:FireServer or game:GetService, because
-- Roblox resolves real members through a native per-tag handler instead.
local restored, tag = __vanta_metatable_dispatch()
if not restored then
    -- Journal, not warn(): a benign, executor-internal note (the fast paths were
    -- already cleared). warn() lands in Roblox's console / LogService, which
    -- anti-cheats scan - the executor must not chatter there. See [[executor-thread-isolation]].
    __vanta_log("instance namecalls already went through the metatable (tag " .. tostring(tag) .. ")")
end

-- getgenv() is the real shared globals table, so a function defined through it
-- is visible to every script the executor runs - which lets request() live in
-- Lua and walk its Headers table with pairs() instead of needing lua_next.
-- The human-readable half of a status line. The spec calls StatusMessage "the
-- human-readable status description" and the macOS reference keeps exactly this
-- kind of table, falling back to the number for a code it does not know.
local statusText = {
    [100] = "Continue", [101] = "Switching Protocols", [102] = "Processing",
    [200] = "OK", [201] = "Created", [202] = "Accepted",
    [203] = "Non-Authoritative Information", [204] = "No Content",
    [205] = "Reset Content", [206] = "Partial Content", [207] = "Multi-Status",
    [300] = "Multiple Choices", [301] = "Moved Permanently", [302] = "Found",
    [303] = "See Other", [304] = "Not Modified", [307] = "Temporary Redirect",
    [308] = "Permanent Redirect",
    [400] = "Bad Request", [401] = "Unauthorized", [402] = "Payment Required",
    [403] = "Forbidden", [404] = "Not Found", [405] = "Method Not Allowed",
    [406] = "Not Acceptable", [407] = "Proxy Authentication Required",
    [408] = "Request Timeout", [409] = "Conflict", [410] = "Gone",
    [411] = "Length Required", [412] = "Precondition Failed",
    [413] = "Payload Too Large", [414] = "URI Too Long",
    [415] = "Unsupported Media Type", [416] = "Range Not Satisfiable",
    [417] = "Expectation Failed", [418] = "I'm a teapot",
    [421] = "Misdirected Request", [422] = "Unprocessable Entity",
    [425] = "Too Early", [426] = "Upgrade Required",
    [428] = "Precondition Required", [429] = "Too Many Requests",
    [431] = "Request Header Fields Too Large",
    [451] = "Unavailable For Legal Reasons",
    [500] = "Internal Server Error", [501] = "Not Implemented",
    [502] = "Bad Gateway", [503] = "Service Unavailable",
    [504] = "Gateway Timeout", [505] = "HTTP Version Not Supported",
    [507] = "Insufficient Storage", [508] = "Loop Detected",
    [511] = "Network Authentication Required",
}

-- The methods the spec names, and what every reference that validates accepts.
local httpMethods = {
    GET = true, HEAD = true, POST = true, PUT = true,
    DELETE = true, OPTIONS = true, PATCH = true,
}

-- Endpoints a script has no business reaching through the executor's HTTP.
-- Two of the six refuse them: Vicna keeps this list and errors, the macOS one
-- refuses economy.roblox.com and auth.roblox.com by returning nothing. Erroring
-- is the honest half of that - a caller that silently gets no response cannot
-- tell a refusal from a network failure. The two logger domains are Vicna's and
-- are there for the same reason: a script should not be able to use `request`
-- to hand the user's address to someone.
local blockedHosts = {
    "roblox.com/api", "accountinformation.roblox.com", "accountsettings.roblox.com",
    "twostepverification.roblox.com", "trades.roblox.com", "billing.roblox.com",
    "economy.roblox.com", "auth.roblox.com", "iplogger.org", "grabify.link",
}

getgenv().request = function(options)
    if type(options) ~= "table" then
        error("request expects a table of options", 2)
    end

    local target = rawtostr(options.Url or options.url or ""):lower()
    for _, host in ipairs(blockedHosts) do
        if target:find(host, 1, true) then
            error("request: " .. host .. " is not reachable through request", 2)
        end
    end

    local lines, sent = {}, {}
    local headers = options.Headers or options.headers
    if type(headers) == "table" then
        for key, value in pairs(headers) do
            lines[#lines + 1] = rawtostr(key) .. ": " .. rawtostr(value)
            sent[rawtostr(key):lower()] = true
        end
    end

    -- Cookies fold into one Cookie header. The spec lists `Cookies` as a
    -- RequestOptions field and four of the six references read it (the macOS
    -- one, sourcessss, Vicna and SkidBase); ours used to ignore it entirely.
    -- A Cookie header the caller wrote themselves wins, and anything they put
    -- in `Cookies` is appended to it rather than replacing it.
    local cookies = options.Cookies or options.cookies
    if type(cookies) == "table" then
        local pairsOut = {}
        for key, value in pairs(cookies) do
            pairsOut[#pairsOut + 1] = rawtostr(key) .. "=" .. rawtostr(value)
        end
        if #pairsOut > 0 then
            if sent["cookie"] then
                for index, line in ipairs(lines) do
                    if line:lower():sub(1, 7) == "cookie:" then
                        lines[index] = line .. "; " .. table.concat(pairsOut, "; ")
                        break
                    end
                end
            else
                lines[#lines + 1] = "Cookie: " .. table.concat(pairsOut, "; ")
                sent["cookie"] = true
            end
        end
    end

    -- The headers the spec says an executor attaches, and that the references
    -- send: the macOS one sends `<Prefix>-Fingerprint` and
    -- `<Prefix>-User-Identifier` from its HWID, RbxStu/sourcessss/SkidBase send
    -- the fingerprint, and five of the six send their own name and version as
    -- the User-Agent. `game:HttpGet` keeps Roblox's own agent - that is a
    -- different call and scripts check it.
    --
    -- Which names those are is decided in C (Http::defaultHeaders), because the
    -- same answer has to reach game:HttpGet and the WebSocket handshake, and
    -- because one host - luarmor.net - is answered under another executor's
    -- name until Vanta is on its whitelist. Anything the caller set themselves
    -- wins; these only fill the gaps.
    local defaults = ""
    do
        local ok, value = pcall(__vanta_httpheaders, options.Url or options.url or "", true)
        if ok and type(value) == "string" then
            defaults = value
        end
    end
    for line in defaults:gmatch("[^\n]+") do
        local key = line:match("^([^:]+):")
        if key and not sent[key:lower()] then
            lines[#lines + 1] = line
            sent[key:lower()] = true
        end
    end

    -- Method names are case-insensitive on the wire but the references
    -- normalise before dispatching, and a lowercase "get" should work.
    local method = rawtostr(options.Method or options.method or "GET"):upper()
    if not httpMethods[method] then
        error("request: invalid HTTP method '" .. method .. "'", 2)
    end

    local sentOk, status, body, responseHeaders = rawrequest(
        options.Url or options.url or "",
        method,
        options.Body or options.body or "",
        options.ContentType or options.contentType or "",
        table.concat(lines, "\n")
    )

    if sentOk == nil then
        error("request is only available to executor threads", 2)
    end
    if not sentOk then
        error(status, 2)
    end

    local parsed = {}
    for line in tostring(responseHeaders):gmatch("[^\r\n]+") do
        local key, value = line:match("^([^:]+):%s*(.*)$")
        if key then
            parsed[key] = value
        end
    end

    return {
        Success = status >= 200 and status < 300,
        StatusCode = status,
        StatusMessage = statusText[status] or tostring(status),
        Headers = parsed,
        Body = body,
    }
end

getgenv().http_request = getgenv().request
getgenv().http = { request = getgenv().request }

-- Alias surface the UNC/SUNC suites check for. Cheaper to wire up here than to
-- register each spelling from C, and nested names like crypt.base64.encode are
-- awkward to build with lua_setfield alone.
crypt.base64 = { encode = crypt.base64encode, decode = crypt.base64decode }
crypt.generatekey = function()
    return crypt.generatebytes(32)
end

getgenv().base64 = { encode = crypt.base64encode, decode = crypt.base64decode }
getgenv().base64encode = crypt.base64encode
getgenv().base64decode = crypt.base64decode
getgenv().base64_encode = crypt.base64encode
getgenv().base64_decode = crypt.base64decode

-- Every Instance the VM has handed out lives in one registry table, keyed by a
-- lightuserdata holding its raw pointer. Roblox files that table under the
-- address of its own push function, which is exactly the key every reference
-- executor uses, so it is one rawget - not a scan of the whole registry.
local instanceCache = rawget(getreg(), __vanta_cachekey())
if type(instanceCache) ~= "table" then
    instanceCache = nil
end

local function environmentOf(thread)
    local ok, env = pcall(__vanta_threadenv, thread)
    return ok and type(env) == "table" and env or nil
end

-- The registry, and only the registry. All six references read Roblox's own
-- instance cache and hand back exactly its Instance values, and the spec says
-- the same thing ("every Instance from the registry"). This used to union in
-- game:GetDescendants() as well, which costs a full DataModel walk on every
-- call and returns instances the engine has never handed to Lua - neither the
-- references nor the spec ask for that.
local function allInstances()
    local found, seen, count = {}, {}, 0

    if instanceCache then
        for _, value in pairs(instanceCache) do
            if typeof(value) == "Instance" and not seen[value] then
                seen[value] = true
                count += 1
                found[count] = value
            end
        end
    end

    return found
end

getgenv().getinstances = allInstances

if instanceCache then
    -- "In the game, or else its Parent" - hoisted so the loop below does not
    -- allocate a closure per cached instance (a few thousand of them).
    local function inGameOrParent(value)
        return value:IsDescendantOf(game) and true or value.Parent
    end
    getgenv().getnilinstances = function()
        local found, count = {}, 0
        for _, value in pairs(instanceCache) do
            -- Every reference filters on Parent == nil and nothing else, so
            -- game - which has no parent - is in the list, as it is in theirs.
            if typeof(value) == "Instance" then
                -- The cache is Roblox's, and a script that has been at an
                -- entry's metatable can leave one behind that answers typeof
                -- but not a method call. Handing that to a caller makes their
                -- loop error on the first thing they try; an entry that cannot
                -- be used as an Instance is not one worth returning.
                local usable, parent = pcall(inGameOrParent, value)
                if usable and parent == nil then
                    count += 1
                    found[count] = value
                end
            end
        end
        return found
    end
end

-- CoreScripts are excluded by ancestry as well as by class: sUNC checks for
-- them explicitly, and Roblox has been folding LocalScript into Script, so the
-- class filter alone is not something to rely on.
local coreContainers = {}
for _, name in ipairs({ "CoreGui", "CorePackages" }) do
    local ok, service = pcall(game.GetService, game, name)
    if ok and service then
        coreContainers[#coreContainers + 1] = service
    end
end

local function isCore(instance)
    if instance:IsA("CoreScript") then
        return true
    end
    for _, container in ipairs(coreContainers) do
        if instance == container or instance:IsDescendantOf(container) then
            return true
        end
    end
    return false
end

-- The script a thread belongs to comes from the thread's extra space, not from
-- its `script` global: sUNC sets that global to nil and still expects these to
-- work. The raw pointer is turned back into the Instance through the cache.
local function isScriptClass(instance)
    return typeof(instance) == "Instance" and instance:IsA("LuaSourceContainer")
end

-- Strictly the reference the *thread* carries, never the environment's `script`
-- global: sUNC sets that global to nil and still expects the script back, and
-- the spec says so outright. The env fallback that used to be here is also what
-- put a StyleLink in getrunningscripts - it answered with whatever the thread's
-- globals happened to hold under that name - so the answer is class-checked too.
local function scriptOfThread(thread)
    local ok, instance = pcall(__vanta_threadscript, thread)
    if not ok then
        return nil
    end
    return isScriptClass(instance) and instance or nil
end

-- Every Script, LocalScript and ModuleScript present, CoreScripts excluded.
-- This used to filter a plain `Script` down to RunContext == Client on the
-- reasoning that a server script does nothing on this client. The spec asks for
-- all three classes outright, and five of the six references list a `Script`
-- whatever its RunContext (only the macOS one filters) - so the filter is gone.
local function isScriptInstance(instance)
    return instance:IsA("LuaSourceContainer")
end

getgenv().getscripts = function()
    local found, count = {}, 0
    for _, instance in ipairs(allInstances()) do
        if isScriptInstance(instance) and not isCore(instance) then
            count += 1
            found[count] = instance
        end
    end
    return found
end

-- "Running" means exactly that: a script with a live thread behind it. Walking
-- the threads also keeps actor scripts out for free, since an actor runs in its
-- own Luau state and its threads are on a different heap than the one walked.
--
-- The entries are the client-side classes only. A plain Script can outlive a
-- thread from the server's side of the place and is not what a client-side
-- list means; sUNC checks the class of every entry ("First value is not a
-- ModuleScript or LocalScript"), so a Script is not admitted even with a thread.
local function isRunningScript(instance)
    return typeof(instance) == "Instance"
        and (instance:IsA("LocalScript") or instance:IsA("ModuleScript"))
end

getgenv().getrunningscripts = function()
    local found, seen, count = {}, {}, 0
    local threads = __vanta_threads()
    local skipped = 0
    for _, thread in ipairs(threads) do
        local script = scriptOfThread(thread)
        if isRunningScript(script) and not seen[script] and not isCore(script) then
            seen[script] = true
            count += 1
            found[count] = script
        elseif typeof(script) == "Instance" then
            skipped += 1
        end
    end
    __vanta_ltrace("getrunningscripts", "threads=" .. #threads, "found=" .. count,
                  "skippedCoreOrDup=" .. skipped)
    return found
end

-- A module that has been required has an environment table; one that has only
-- been referenced does not. That table is what separates loaded from unloaded,
-- and it outlives the thread that ran the module, which a thread walk does not.
getgenv().getloadedmodules = function()
    local found, seen, count = {}, {}, 0
    for _, value in ipairs(getgc(true)) do
        if type(value) == "table" then
            -- rawget on a table cannot raise, so no pcall: this loop runs once
            -- per table on the heap (~78k), and the protected frame per
            -- iteration was a third of the call's cost. Most tables have no
            -- `script` at all, so nil is tested before typeof is called.
            local script = rawget(value, "script")
            if script ~= nil and typeof(script) == "Instance" and not seen[script]
                and script:IsA("ModuleScript") and not isCore(script) then
                seen[script] = true
                count += 1
                found[count] = script
            end
        end
    end
    return found
end

if instanceCache then
    local function entry(instance)
        local key = __vanta_rawptr(instance)
        if key == nil then
            error("expected an Instance", 3)
        end
        return key
    end

    getgenv().cache = {
        invalidate = function(instance)
            instanceCache[entry(instance)] = nil
        end,
        iscached = function(instance)
            return rawget(instanceCache, entry(instance)) ~= nil
        end,
        replace = function(instance, replacement)
            instanceCache[entry(instance)] = replacement
        end,
    }

    -- A second userdata for one Instance is what Roblox makes when it is asked
    -- to push one whose cache entry is missing. Clear the entry, ask the engine
    -- to push it, put the entry straight back - so the game keeps seeing the
    -- reference it always had and only the caller holds the new one. This is
    -- what all five references that carry the library do.
    getgenv().cloneref = function(instance)
        local key = entry(instance)
        local original = rawget(instanceCache, key)
        instanceCache[key] = nil
        local ok, reference = pcall(__vanta_pushinstance, instance)
        instanceCache[key] = original
        if not ok then
            error(reference, 2)
        end
        return reference
    end

    getgenv().clonereference = getgenv().cloneref

    -- Two references to one Instance are different userdata but the same
    -- pointer, which is exactly what the lightuserdata compares.
    getgenv().compareinstances = function(a, b)
        local first = __vanta_rawptr(a)
        return first ~= nil and first == __vanta_rawptr(b)
    end
else
    -- Journal, not warn(): an executor-internal init failure. It must not reach
    -- Roblox's console / LogService (anti-cheat-scanned); if the cache is missing,
    -- cloneref/cache calls raise their own errors to the user. See [[executor-thread-isolation]].
    __vanta_log("could not find the instance cache; cloneref and cache are unavailable")
end


getgenv().getidentity = getthreadidentity
getgenv().getthreadcontext = getthreadidentity
getgenv().setidentity = setthreadidentity
getgenv().setthreadcontext = setthreadidentity

-- isexecutorclosure has two halves. A C closure is ours if the lua_CFunction
-- behind it is one we registered - which clonefunction's output satisfies too,
-- being a different closure over the same function. A Luau closure is ours if
-- the chunk it was compiled from is, which debug.info(f, "s") names and
-- __vanta_ischunk answers for; that covers loadstring and loadfile results as
-- the spec requires, since both record their chunk when they compile it.
getgenv().isexecutorclosure = function(closure)
    if type(closure) ~= "function" then
        error("isexecutorclosure: function expected", 2)
    end
    if iscclosure(closure) then
        return __vanta_isourcfunction(closure)
    end
    local ok, source = pcall(debug.info, closure, "s")
    return ok and __vanta_ischunk(source) or false
end

getgenv().checkclosure = getgenv().isexecutorclosure
getgenv().isourclosure = getgenv().isexecutorclosure

-- Four names the references register that this tree did not, each of them a
-- spelling of something already here. Found by asking the executor which of the
-- references' globals it lacks (tests/missing.lua) rather than by grepping our
-- own source, which misses anything wired in a loop.
--
--   getregistry   4/6 (mac, src, +2)  - getreg under its other name
--   gettenv       5/6                 - a thread's globals table, which is
--                                       exactly what __vanta_threadenv returns
--   gethwid       4/6                 - already computed for request's
--                                       Vanta-Fingerprint header
--   makewriteable 3/6 (src, skid, mod) - the inverse of makereadonly
getgenv().getregistry = getgenv().getreg

-- mac registers both spellings; wiring the second here rather than registering
-- it twice from C is what makes `comparefunction == comparefunctions` true, and
-- the suites count a spelling that is a different closure as a different
-- function.
getgenv().comparefunction = getgenv().comparefunctions

-- mac raises on a non-thread (`luaL_argexpected(..., "thread")`) and so does
-- this; the native call answers nil, which would read as "no environment"
-- rather than as a bad argument.
getgenv().gettenv = function(thread)
    if type(thread) ~= "thread" then
        error("gettenv: thread expected", 2)
    end
    return __vanta_threadenv(thread)
end

getgenv().gethwid = function()
    local ok, id = pcall(__vanta_hwid)
    return ok and id or ""
end

getgenv().makewriteable = function(object)
    if type(object) ~= "table" then
        error("makewriteable: table expected", 2)
    end
    setreadonly(object, false)
    return object
end
getgenv().makewritable = getgenv().makewriteable

-- Aliases are wired here rather than registered twice from C, so that the two
-- names are the same closure and compare equal.
getgenv().replaceclosure = hookfunction

-- OnMessage and OnClose have to be things a script can :Connect to, and the
-- only real signal available from Lua is a BindableEvent's. The reader thread
-- cannot fire one - Roblox events belong to the engine's threads - so a polling
-- loop on the Lua side drains what the reader queued and fires from there.
getgenv().WebSocket = {
    connect = function(url)
        local id, err = __vanta_ws_connect(url)
        if not id then
            error("WebSocket.connect: " .. tostring(err), 2)
        end

        local messageEvent = Instance.new("BindableEvent")
        local closeEvent = Instance.new("BindableEvent")
        local closed = false

        -- Tagged, so `typeof(ws)` answers "WebSocket" rather than "table".
        -- Three of the five references that implement the class set exactly
        -- this metafield (the macOS one, sourcessss and Module-Source), and it
        -- is the same trick the connection and drawing proxies here already
        -- use. The methods stay raw fields on the table, so nothing about how
        -- they are called changes.
        local socket = setmetatable({
            OnMessage = messageEvent.Event,
            OnClose = closeEvent.Event,
        }, {
            __type = "WebSocket",
            __tostring = function() return "WebSocket" end,
        })

        local function finish()
            if closed then return end
            closed = true
            closeEvent:Fire()
        end

        function socket:Send(message)
            if closed then
                error("WebSocket: the connection is closed", 2)
            end
            return __vanta_ws_send(id, rawtostr(message))
        end

        function socket:Close()
            if closed then return end
            __vanta_ws_close(id)
            finish()
        end

        task.spawn(function()
            -- The wait goes FIRST, and that is load-bearing.
            --
            -- `task.spawn` runs this function synchronously until it yields, so
            -- a loop that polls before waiting drains the queue *before*
            -- `connect` has returned the socket - which is before the caller
            -- has had any chance to attach an OnMessage handler. Every message
            -- already queued at that moment was fired into a BindableEvent with
            -- no listeners and lost for good. A server that greets on connect
            -- (most echo servers do) hits this whenever its greeting beats the
            -- yield that `connect` resumes through, which is a race, so it
            -- showed up as an intermittent "failed to receive a wanted
            -- message" rather than an outright break.
            --
            -- Waiting first costs one frame before the first delivery and
            -- guarantees the caller has run to its `OnMessage:Connect` line.
            while not closed do
                -- Once per frame, not every 30 ms. This loop is the only thing
                -- between a message arriving on the reader thread and OnMessage
                -- firing, so its interval is added to the latency of every
                -- single message - 30 ms of it, which is larger than the entire
                -- transport cost of a local round trip (0.08 ms) and bigger
                -- than most remote ones. `task.wait()` resumes on the next
                -- heartbeat, so the floor becomes one frame instead of two.
                -- The poll itself is a map lookup and an empty-queue check, so
                -- doing it twice as often costs nothing measurable.
                task.wait()
                local messages, done = __vanta_ws_poll(id)
                for _, message in ipairs(messages) do
                    messageEvent:Fire(message)
                end
                if done then
                    finish()
                    break
                end
            end
        end)

        return socket
    end,
}

-- Roblox does not put _G or shared on the root globals table - it injects them
-- into each script's sandboxed environment - so the environment getrenv hands
-- back has no _G of its own. The game's real one is taken from a live game
-- script rather than fabricated, and only ever filled in if it is genuinely
-- absent.
do
    local rootenv = getrenv

    -- Roblox files the game's `_G` at registry index 2 and its `shared` at
    -- index 4. Five of the six references read exactly those two slots
    -- (`lua_rawgeti(L, LUA_REGISTRYINDEX, 2)`); this used to walk every live
    -- thread looking for a script environment that had a `_G` in it, which is
    -- a heap walk to answer a question with a fixed answer. The walk is kept
    -- as the fallback for the case where those slots hold something else.
    getgenv().getrenv = function()
        local renv = rootenv()
        if rawget(renv, "_G") ~= nil then
            return renv
        end

        local registry = getreg()
        local globals, sharedTable = registry[2], registry[4]
        if type(globals) == "table" and globals ~= _G then
            renv._G = globals
            if type(sharedTable) == "table" and sharedTable ~= shared then
                renv.shared = sharedTable
            end
            return renv
        end

        for _, thread in ipairs(__vanta_threads()) do
            local ok, env = pcall(__vanta_threadenv, thread)
            if ok and type(env) == "table" and rawget(env, "script") ~= nil then
                globals, sharedTable = rawget(env, "_G"), rawget(env, "shared")
                if type(globals) == "table" and globals ~= _G then
                    renv._G = globals
                    if type(sharedTable) == "table" and sharedTable ~= shared then
                        renv.shared = sharedTable
                    end
                    break
                end
            end
        end
        return renv
    end
end

-- A script's environment is the globals table of the thread running it, and
-- that table holds the script's own `script` global. Walking every live thread
-- is what makes getsenv, getscriptfromthread and getcallingscript all fall out
-- of the same lookup.
getgenv().getscriptfromthread = function(thread)
    if type(thread) ~= "thread" then
        error("getscriptfromthread: thread expected", 2)
    end
    local script = scriptOfThread(thread)
    __vanta_ltrace("getscriptfromthread", thread, "->", script,
                  "class=" .. (typeof(script) == "Instance" and script.ClassName or "-"))
    return script
end

getgenv().getsenv = function(target)
    if typeof(target) ~= "Instance" then
        error("getsenv: Instance expected", 2)
    end
    local threads = __vanta_threads()
    for _, thread in ipairs(threads) do
        if scriptOfThread(thread) == target then
            local env = environmentOf(thread)
            if env then
                return env
            end
        end
    end
    -- Only if the thread's own reference named nothing: a script whose thread
    -- the walk cannot attribute is still findable by its own `script` global.
    for _, thread in ipairs(threads) do
        local env = environmentOf(thread)
        if env and rawget(env, "script") == target then
            return env
        end
    end
    return nil
end

getgenv().getcallingscript = function()
    return scriptOfThread(coroutine.running())
end

-- The Drawing library. Every drawing is a real GuiObject under gethui(), and
-- that GuiObject is what draws it - there is one implementation and it is
-- always on. Drawings are not Instances, so each one is a proxy table over a
-- property bag: reads come back exactly as written, and the bag is mirrored
-- onto the object underneath.
--
-- There used to be a second implementation - a borderless NSWindow the shapes
-- were painted into - selected by a global. That was a mistake: a library whose
-- behaviour depends on a flag is not a library, and the flag existed only
-- because two compliance suites want opposite things (sUNC looks for the
-- element and gives up if there is none; Myriad fails you when it finds a Frame
-- whose colour matches). The overlay is still in the tree, still built at
-- startup and still verifiable through `__vanta_draw_*` and `tests/draw.lua`;
-- it is simply not what Drawing renders with.
--
-- Transparency follows the spec rather than the old UNC wording: 0 is opaque,
-- 1 is fully see-through, which is also how Roblox means it.
do
    local renderObjects = setmetatable({}, { __mode = "k" })
    local screen
    local nextId = 0

    local function container()
        if screen and screen.Parent then
            return screen
        end
        screen = Instance.new("ScreenGui")
        screen.Name = "Drawing"
        screen.IgnoreGuiInset = true
        screen.ResetOnSpawn = false
        screen.DisplayOrder = 2147483647
        local ok = pcall(function() screen.Parent = gethui() end)
        if not ok then
            screen.Parent = game:GetService("CoreGui")
        end
        return screen
    end

    local shared = {
        Visible = false,
        ZIndex = 0,
        Transparency = 0,
        Color = Color3.new(0, 0, 0),
    }

    local perType = {
        Line = { From = Vector2.zero, To = Vector2.zero, Thickness = 1 },
        Text = {
            Text = "", Size = 13, Center = false, Outline = false,
            OutlineColor = Color3.new(0, 0, 0), OutlineOpacity = 1,
            Position = Vector2.zero, Font = 0, TextBounds = Vector2.zero,
        },
        Image = { Data = "", Size = Vector2.zero, Position = Vector2.zero, Rounding = 0 },
        Circle = { Radius = 0, Position = Vector2.zero, Thickness = 1, NumSides = 0, Filled = false },
        Square = { Size = Vector2.zero, Position = Vector2.zero, Thickness = 1, Filled = false },
        Quad = {
            PointA = Vector2.zero, PointB = Vector2.zero,
            PointC = Vector2.zero, PointD = Vector2.zero,
            Thickness = 1, Filled = false,
        },
        Triangle = {
            PointA = Vector2.zero, PointB = Vector2.zero, PointC = Vector2.zero,
            Thickness = 1, Filled = false,
        },
    }

    -- One GuiObject per drawing, shaped for what that kind needs:
    --   Text     a TextLabel that sizes itself, with a UIStroke for Outline
    --   Image    an ImageLabel
    --   Circle   a Frame rounded to a circle, with a UIStroke for the outline
    --   Square   a Frame, with a UIStroke for the outline
    --   Line     a Frame rotated between the two points
    --   Quad     a Frame holding one edge Frame per side
    --   Triangle the same with three
    -- The stroke is what makes Filled = false mean an outline rather than a
    -- solid block, and Thickness mean something on every shape that has it.
    local function build(kind)
        if kind == "Text" then
            local label = Instance.new("TextLabel")
            label.BackgroundTransparency = 1
            label.TextXAlignment = Enum.TextXAlignment.Left
            label.TextYAlignment = Enum.TextYAlignment.Top
            label.Size = UDim2.fromOffset(0, 0)
            label.AutomaticSize = Enum.AutomaticSize.XY
            local stroke = Instance.new("UIStroke")
            stroke.Name = "Outline"
            stroke.Thickness = 1
            stroke.Enabled = false
            stroke.Parent = label
            return label
        elseif kind == "Image" then
            local image = Instance.new("ImageLabel")
            image.BackgroundTransparency = 1
            image.ScaleType = Enum.ScaleType.Stretch
            return image
        end

        local frame = Instance.new("Frame")
        frame.BorderSizePixel = 0
        frame.BackgroundTransparency = 1

        if kind == "Circle" or kind == "Square" then
            local stroke = Instance.new("UIStroke")
            stroke.Name = "Outline"
            stroke.ApplyStrokeMode = Enum.ApplyStrokeMode.Border
            stroke.Enabled = false
            stroke.Parent = frame
            if kind == "Circle" then
                local corner = Instance.new("UICorner")
                corner.CornerRadius = UDim.new(1, 0)
                corner.Parent = frame
            end
        elseif kind == "Quad" or kind == "Triangle" then
            -- The edges live inside a full-screen frame so each one can be
            -- placed in absolute coordinates without fighting a parent's size.
            frame.Size = UDim2.fromScale(1, 1)
            frame.Position = UDim2.fromOffset(0, 0)
            for index = 1, (kind == "Quad" and 4 or 3) do
                local edge = Instance.new("Frame")
                edge.Name = "Edge" .. index
                edge.BorderSizePixel = 0
                edge.AnchorPoint = Vector2.new(0.5, 0.5)
                edge.Parent = frame
            end
        end
        return frame
    end

    -- Drawing.Fonts is a small enum of its own; these are the nearest Roblox
    -- faces, so a script that asks for Monospace gets one.
    local fontById = { [0] = Enum.Font.Gotham, [1] = Enum.Font.Legacy,
                       [2] = Enum.Font.SourceSans, [3] = Enum.Font.Code }
    local function fontFor(value)
        return fontById[math.floor(tonumber(value) or 0)] or Enum.Font.Gotham
    end

    -- Lays one Frame along the segment between two points, which is how every
    -- line in here is drawn: Line itself, and each edge of a Quad or Triangle.
    local function segment(edge, from, to, thickness, colour, transparency, visible, zindex)
        if typeof(from) ~= "Vector2" or typeof(to) ~= "Vector2" then
            edge.Visible = false
            return
        end
        local delta = to - from
        local length = delta.Magnitude
        edge.Position = UDim2.fromOffset((from.X + to.X) / 2, (from.Y + to.Y) / 2)
        edge.Size = UDim2.fromOffset(length, math.max(thickness, 1))
        edge.Rotation = math.deg(math.atan2(delta.Y, delta.X))
        edge.BackgroundColor3 = colour
        edge.BackgroundTransparency = transparency
        edge.Visible = visible
        edge.ZIndex = zindex
    end

    local function toColour(value)
        if typeof(value) == "Color3" then
            return value.R, value.G, value.B
        end
        return 0, 0, 0
    end

    local function point(value)
        if typeof(value) == "Vector2" then
            return value.X, value.Y
        end
        return 0, 0
    end

    -- Pushes whatever of the property bag the underlying GuiObject can show.
    local function render(state)
        local object = state.object
        if not object or not object.Parent then
            return
        end
        local values = state.values

        object.Visible = values.Visible and true or false
        object.ZIndex = math.clamp(math.floor(tonumber(values.ZIndex) or 0), -2147483648, 2147483647)

        local transparency = math.clamp(tonumber(values.Transparency) or 0, 0, 1)
        local colour = typeof(values.Color) == "Color3" and values.Color or Color3.new(0, 0, 0)
        local thickness = math.max(tonumber(values.Thickness) or 1, 0)
        local filled = values.Filled and true or false
        local visible = values.Visible and true or false
        local zindex = object.ZIndex

        if state.kind == "Text" then
            object.TextTransparency = transparency
            object.TextColor3 = colour
            object.Text = rawtostr(values.Text)
            object.TextSize = math.max(tonumber(values.Size) or 13, 1)
            object.Font = fontFor(values.Font)
            object.TextXAlignment = values.Center and Enum.TextXAlignment.Center
                                                   or Enum.TextXAlignment.Left
            object.Position = UDim2.fromOffset(values.Position.X, values.Position.Y)

            local stroke = object:FindFirstChild("Outline")
            if stroke then
                stroke.Enabled = values.Outline and true or false
                stroke.Color = typeof(values.OutlineColor) == "Color3" and values.OutlineColor
                                                                       or Color3.new(0, 0, 0)
                stroke.Transparency = 1 - math.clamp(tonumber(values.OutlineOpacity) or 1, 0, 1)
            end
            -- TextBounds is NOT read here. It is the engine's answer, so it
            -- has to come from the TextLabel - but reading it forces a layout
            -- flush over every dirty AutomaticSize label in the container, so
            -- doing it per render made Drawing.new("Text") cost O(number of
            -- Text drawings): 392 us at none, 1553 us at 500 (drawscale.lua,
            -- 2026-09-03), while a raw TextLabel parented the same way stayed
            -- flat at ~45 us. It is read in __index instead, when a script
            -- actually asks - which is also more correct, since the answer is
            -- then current rather than whatever the last render saw.
        elseif state.kind == "Image" then
            object.ImageTransparency = transparency
            object.ImageColor3 = colour
            object.Position = UDim2.fromOffset(values.Position.X, values.Position.Y)
            object.Size = UDim2.fromOffset(values.Size.X, values.Size.Y)
            -- Data is raw image bytes. getcustomasset is what turns bytes into
            -- something the engine will load, so a drawing given real data
            -- actually shows it instead of staying blank.
            local data = rawtostr(values.Data or "")
            if data ~= "" and data ~= state.imageData then
                state.imageData = data
                local ok, asset = pcall(function()
                    local name = "vanta_drawing_" .. state.id .. ".png"
                    writefile(name, data)
                    return getcustomasset(name)
                end)
                object.Image = ok and asset or ""
            elseif data == "" then
                state.imageData = nil
                object.Image = ""
            end
        elseif state.kind == "Quad" or state.kind == "Triangle" then
            -- The container itself never paints; the edges do.
            object.BackgroundTransparency = 1
            local points = state.kind == "Quad"
                and { values.PointA, values.PointB, values.PointC, values.PointD }
                or { values.PointA, values.PointB, values.PointC }
            for index = 1, #points do
                local edge = object:FindFirstChild("Edge" .. index)
                if edge then
                    local nextPoint = points[index % #points + 1]
                    segment(edge, points[index], nextPoint,
                            filled and 1 or thickness, colour, transparency, visible, zindex)
                end
            end
        else
            object.BackgroundColor3 = colour
            local stroke = object:FindFirstChild("Outline")

            if state.kind == "Square" then
                object.Position = UDim2.fromOffset(values.Position.X, values.Position.Y)
                object.Size = UDim2.fromOffset(values.Size.X, values.Size.Y)
            elseif state.kind == "Circle" then
                local diameter = (tonumber(values.Radius) or 0) * 2
                object.Position = UDim2.fromOffset(values.Position.X - diameter / 2,
                                                   values.Position.Y - diameter / 2)
                object.Size = UDim2.fromOffset(diameter, diameter)
            elseif state.kind == "Line" then
                object.AnchorPoint = Vector2.new(0.5, 0.5)
                segment(object, values.From, values.To, thickness, colour, transparency,
                        visible, zindex)
                return
            end

            -- Filled means the body paints; an outline means the body does not
            -- and the stroke carries the thickness instead.
            if stroke then
                object.BackgroundTransparency = filled and transparency or 1
                stroke.Enabled = not filled and thickness > 0
                stroke.Thickness = math.max(thickness, 0)
                stroke.Color = colour
                stroke.Transparency = transparency
            else
                object.BackgroundTransparency = transparency
            end
        end
    end

    -- Everything the Drawing surface is asked, logged from inside it.
    -- getgenv().__vanta_trace_drawing = true turns it on. The library is
    -- entirely ours, so printing from in here changes no identity that a suite
    -- could be checking - unlike a wrapper around a global would.
    --
    -- Budgeted, because a suite that reads a property in a loop would otherwise
    -- fill the log and change the timings it measures.
    local traceBudget = 600
    local function dtrace(...)
        if not getgenv().__vanta_trace_drawing then return end
        if traceBudget <= 0 then return end
        traceBudget -= 1
        local parts = {}
        for i = 1, select("#", ...) do
            local value = (select(i, ...))
            local kind = typeof(value)
            if kind == "string" then
                parts[i] = '"' .. value .. '"'
            elseif kind == "table" or kind == "userdata" then
                parts[i] = kind .. ":" .. tostring(value)
            else
                parts[i] = tostring(value)
            end
        end
        local line = "DRW " .. table.concat(parts, " ")
        print(line)
        __vanta_log(line)
        if traceBudget == 0 then print("DRW (trace budget spent)") end
    end
    getgenv().__vanta_draw_trace_reset = function(n)
        traceBudget = tonumber(n) or 600
    end

    local metatable = {}

    metatable.__index = function(self, key)
        local state = renderObjects[self]
        if not state then
            dtrace("index-on-destroyed", key)
            error("attempt to index a destroyed drawing", 2)
        end
        if key == "__OBJECT_EXISTS" then
            dtrace("get", state.kind, key, "->", state.alive)
            return state.alive
        end
        -- Asked for on demand, never pushed by render: see the note there.
        -- Nearly nothing reads TextBounds, and every read costs the engine a
        -- layout pass over the whole container.
        if key == "TextBounds" then
            local object = state.object
            local bounds = state.values.TextBounds
            if object then
                local ok, measured = pcall(function() return object.TextBounds end)
                if ok and typeof(measured) == "Vector2" then
                    bounds = measured
                    state.values.TextBounds = measured
                end
            end
            dtrace("get", state.kind, key, "->", bounds)
            return bounds
        end
        if key == "Destroy" or key == "Remove" then
            return function()
                dtrace("destroy", state.kind, state.id)
                if state.object then
                    pcall(function() state.object:Destroy() end)
                    state.object = nil
                end
                state.alive = false
            end
        end
        local value = state.values[key]
        dtrace("get", state.kind, key, "->", value)
        return value
    end

    metatable.__newindex = function(self, key, value)
        local state = renderObjects[self]
        if not state then
            dtrace("set-on-destroyed", key, value)
            error("attempt to modify a destroyed drawing", 2)
        end
        dtrace("set", state.kind, key, "=", value)
        if key == "TextBounds" then
            error("TextBounds is read-only", 2)
        end
        -- Writing the value a property already holds changes nothing on screen,
        -- and render() pushes the whole property bag at the GuiObject - about a
        -- dozen engine property writes. An ESP that recomputes and reassigns
        -- every frame writes the same value most frames, so this is the common
        -- case rather than a corner. Vector2/Color3/UDim2 compare by value in
        -- Luau, so this is an exact test, and `nil` is handled by the same
        -- equality (a fresh key holds nil and cannot equal a real value).
        if state.values[key] == value then
            return
        end
        state.values[key] = value
        if state.alive then
            pcall(render, state)
        end
    end

    metatable.__tostring = function() return "Drawing" end

    local Drawing = {}

    Drawing.Fonts = { UI = 0, System = 1, Plex = 2, Monospace = 3 }

    function Drawing.new(kind)
        local template = perType[kind]
        dtrace("Drawing.new", kind)
        if not template then
            error(`invalid drawing type '{tostring(kind)}'`, 2)
        end

        local values = {}
        for property, value in pairs(shared) do values[property] = value end
        for property, value in pairs(template) do values[property] = value end

        nextId += 1
        local proxy = setmetatable({}, metatable)
        local state = { kind = kind, values = values, alive = true, id = nextId }
        renderObjects[proxy] = state

        local ok, object = pcall(build, kind)
        if ok and object then
            state.object = object
            pcall(function() object.Parent = container() end)
            pcall(render, state)
        end
        return proxy
    end

    getgenv().Drawing = Drawing

    -- A destroyed drawing is not a render object any more. The reference
    -- executor answers on `Exists` for exactly this reason, and a proxy whose
    -- Destroy has run still has an entry here - it is the entry that says the
    -- drawing is gone.
    getgenv().isrenderobj = function(object)
        local state = type(object) == "table" and renderObjects[object] or nil
        local answer = state ~= nil and state.alive == true
        dtrace("isrenderobj", object, "->", answer)
        return answer
    end

    getgenv().getrenderproperty = function(object, property)
        dtrace("getrenderproperty", object, property)
        if not getgenv().isrenderobj(object) then
            error("getrenderproperty: expected a drawing", 2)
        end
        return object[property]
    end

    getgenv().setrenderproperty = function(object, property, value)
        dtrace("setrenderproperty", object, property, value)
        if not getgenv().isrenderobj(object) then
            error("setrenderproperty: expected a drawing", 2)
        end
        object[property] = value
    end

    getgenv().cleardrawcache = function()
        dtrace("cleardrawcache")
        for proxy, state in pairs(renderObjects) do
            if state.object then
                pcall(function() state.object:Destroy() end)
                state.object = nil
            end
            state.alive = false
        end
        if screen then
            pcall(function() screen:ClearAllChildren() end)
        end
    end
end

-- The debug library. Every one of these accepts a stack level as well as a
-- function - debug.info is what turns one into the other, so the C side only
-- ever deals with functions.
--
-- The surface goes on a **private copy**, never on Roblox's own table. That one
-- is shared with every game script: a sandboxed thread's globals chain to the
-- game root, so the `debug` this bootstrap sees is the very table object a
-- LocalScript sees, and writing into it puts `debug.getupvalues` in front of
-- anything that cares to look. Jailbreak's client anti-cheat is exactly that -
-- `if debug.getupvalues then` in its CheatCheck loop, reported to the server,
-- which kicks ~25 s later with an empty message. Measured, not guessed: with
-- the shims in the shared table, that one condition was the only one of its
-- eight that read true on a game thread.
--
-- A new table seeded from the real one is what the macOS reference's
-- `OverrideLib` and sourcessss's `debug.cpp` both build (copy every stock entry
-- in, add ours, set it as the global on the executor thread). The stock entries
-- are the *same function objects*, so `debug.info` is still Roblox's own C
-- closure and only the container differs.
--
-- The copy is also a file-scope local, and everything below that wants an
-- *executor* debug member goes through it rather than the `debug` global.
-- `debug` inside this chunk is a baked GETIMPORT - resolved from the root
-- environment when the bootstrap loaded, which is before the copy existed - so
-- the global still names Roblox's table however the executor globals are set
-- afterwards. Scripts the executor runs later are compiled after the raw key is
-- in place and so import the copy, which is why only this file needs the local.
local vantaDebug

do
    local real = debug
    local stock = {}
    for key, value in next, real do
        stock[key] = value
    end
    vantaDebug = stock
    -- A raw key on the executor globals, so it shadows the game's `debug` for
    -- us and for every script we run (they sandbox off this table) while the
    -- game's own lookup still finds Roblox's untouched one.
    getgenv().debug = stock

    local info = stock.info

    -- Every debug.* entry point here takes a function *or* a stack level, which
    -- is what Module-Source and RbxStu both accept (the macOS tree only takes a
    -- function). Level 1 means "whoever called debug.getconstants", and those
    -- references get that for free because they read the level from inside the
    -- C function the caller invoked.
    --
    -- Ours is a Lua shim, so there are two frames in the way: this helper, and
    -- the shim that called it. Without the +2, debug.getconstants(1) came back
    -- with {type, number, f, function, error, "invalid level"} - the constants
    -- of *this function*, which is as wrong as it looks.
    local function resolve(target, extra)
        if type(target) == "number" then
            -- Levels are 1-based and a level of 0 or below is not a frame.
            -- SkidBase is explicit about it - `if (level <= 0)
            -- luaL_argerror(L, 1, "level out of range")` - and sUNC checks
            -- exactly this: it asks for level 1 (valid), then 0 and 32, and
            -- both of those have to raise. Without the guard, 0 resolved to
            -- this very function and came back a perfectly good closure.
            if target <= 0 then
                error("level out of range", 3)
            end
            local level = target + 2 + (extra or 0)
            local func = info(level, "f")
            -- The built-in argument trace, from inside our own closure so no
            -- identity changes (a wrapper would break isexecutorclosure and
            -- getfunctionhash, which sUNC checks). Turn it on with
            -- getgenv().__vanta_ltrace_on = { debuglevel = true }.
            if getgenv().__vanta_ltrace_wanted and getgenv().__vanta_ltrace_wanted("debuglevel") then
                local chain = {}
                for probe = 1, 8 do
                    local ok, src, line = pcall(info, probe, "sl")
                    local okf, f = pcall(info, probe, "f")
                    chain[#chain + 1] = string.format("%d=%s:%s:%s", probe,
                        tostring(ok and src), tostring(ok and line),
                        okf and type(f) == "function" and (iscclosure(f) and "C" or "L") or "-")
                end
                __vanta_ltrace("debuglevel", "asked=" .. tostring(target),
                              "level=" .. tostring(level),
                              "got=" .. tostring(func) .. " (" .. type(func) .. ")",
                              table.concat(chain, " "))
            end
            if type(func) ~= "function" then
                error("invalid level", 3)
            end
            return func
        end
        return target
    end

    -- Our copy is left in the same lock state Roblox's is in, so a script that
    -- tries to write to `debug` gets the answer it would get on a stock client.
    local locked = isreadonly(real)

    stock.getconstants = function(target) return __vanta_getconstants(resolve(target)) end
    stock.getconstant = function(target, index) return __vanta_getconstant(resolve(target), index) end
    stock.setconstant = function(target, index, value) return __vanta_setconstant(resolve(target), index, value) end
    -- An inactive proto must not be callable: the spec says so outright, and
    -- calling one is a real vulnerability, not a formality. Ours used to be a
    -- closure over the *real* nested proto with nil upvalues, so calling it ran
    -- the body (and usually died on a nil upvalue, which is not the same thing).
    --
    -- The reference executors clone the Proto and replace its code with a single
    -- RETURN. That needs luaF_newproto, an allocator, and a fixup pass for every
    -- self-relative pointer Roblox stores in a Proto - and it produces a proto
    -- whose only observable difference is that it does nothing. The same thing
    -- falls out of asking the compiler for a function of the right shape, which
    -- needs no engine internals at all:
    --
    --   * the same source name, so debug.info(p, "s") matches - loaded through
    --     __vanta_loadchunk rather than loadstring, because loadstring records
    --     the chunkname as ours and that would make every function in the real
    --     script answer true to isexecutorclosure
    --   * the same debug name, parameter count and vararg flag
    --   * the same number of upvalues, all nil, which is what the spec says an
    --     inactive proto's upvalues are
    --   * the same linedefined, written straight onto the proto
    local inertCache = setmetatable({}, { __mode = "k" })

    local function inertProto(real)
        if type(real) ~= "function" or not islclosure(real) then
            return real
        end
        local cached = inertCache[real]
        if cached then return cached end

        local name = debug.info(real, "n")
        local source = debug.info(real, "s")
        local line = debug.info(real, "l")
        local params, vararg = debug.info(real, "a")

        -- A debug name is whatever the compiler recorded and need not be an
        -- identifier, so it is only used when it is one.
        if type(name) ~= "string" or not name:match("^[%a_][%w_]*$") then
            name = "proto"
        end

        -- **No upvalues.** The stand-in used to declare one local per upvalue
        -- of the real sub-proto, so the closure had N upvalues all holding nil.
        -- That is not what a reference hands back: sourcessss builds the proto
        -- closure as `luaF_newLclosure(L, 0, cl->env, p)` - zero upvalues - so
        -- `debug.getupvalue(proto, 1)` *errors* there, where ours quietly
        -- returned nil.
        --
        -- The difference is the sUNC `debug.setupvalue` failure, reproduced
        -- exactly in `tests/pr22.lua`:
        --     getupvalue(proto, 1) -> nil        (ours; a reference errors)
        --     setupvalue(nil, 1, game) -> "debug.setupvalue: function expected"
        -- which is the message the suite reports, word for word.
        local args = {}
        for i = 1, (tonumber(params) or 0) do args[i] = "p" .. i end
        if vararg then args[#args + 1] = "..." end

        local text = "local function " .. name .. "(" .. table.concat(args, ",") ..
            ") end return " .. name

        local chunk = __vanta_loadchunk(text, "@" .. tostring(source))
        if type(chunk) ~= "function" then
            return real
        end
        local ok, inert = pcall(chunk)
        if not ok or type(inert) ~= "function" then
            return real
        end
        if type(line) == "number" then
            __vanta_setprotoline(inert, line)
        end
        inertCache[real] = inert
        return inert
    end

    stock.getprotos = function(target)
        local list = __vanta_getprotos(resolve(target))
        for index, proto in ipairs(list) do
            list[index] = inertProto(proto)
        end
        return list
    end
    stock.getproto = function(target, index, activated)
        local result = __vanta_getproto(resolve(target), index, activated)
        if activated then return result end
        return inertProto(result)
    end
    stock.getupvalues = function(target) return __vanta_getupvalues(resolve(target)) end
    stock.getupvalue = function(target, index) return __vanta_getupvalue(resolve(target), index) end
    stock.setupvalue = function(target, index, value)
        -- sUNC hands this a nil where a function belongs, and the message it
        -- reports back is ours rather than anything about its own state. The
        -- traceback is the only way to see where it got that nil from, so it
        -- goes to the journal - on disk, so it survives whatever happens next.
        if target == nil and __vanta_log then
            -- A traceback is no help: sUNC's obfuscation collapses every frame
            -- to [string "vanta"]:1. What is readable is the shape of the call
            -- and what the caller's own frame holds, which is where the
            -- function it meant to pass would still be sitting.
            local parts = {}
            for level = 2, 5 do
                local ok, src, line, name = pcall(debug.info, level, "sln")
                parts[#parts + 1] = string.format("L%d=%s:%s:%s", level,
                    tostring(ok and src), tostring(ok and line), tostring(ok and name))
                local okStack, stack = pcall(vantaDebug.getstack, level)
                if okStack and type(stack) == "table" then
                    local types = {}
                    for i = 1, math.min(#stack, 12) do types[i] = type(stack[i]) end
                    parts[#parts + 1] = "  stack" .. level .. "={" .. table.concat(types, ",") .. "}"
                end
                -- The caller's *constants* are the useful part: sUNC's
                -- obfuscation collapses every frame to [string "vanta"]:1, but
                -- the string constants of the function that produced this nil
                -- name the globals it called to get it. That is the whole
                -- question - which of our functions handed it nothing.
                local okFunc, caller = pcall(debug.info, level, "f")
                if okFunc and type(caller) == "function" then
                    local okConst, constants = pcall(vantaDebug.getconstants, caller)
                    if okConst and type(constants) == "table" then
                        local strings = {}
                        for _, c in pairs(constants) do
                            if type(c) == "string" and #c > 0 and #c < 40 then
                                strings[#strings + 1] = c
                            end
                        end
                        if #strings > 0 then
                            parts[#parts + 1] = "  const" .. level .. "=[" ..
                                table.concat(strings, ",") .. "]"
                        end
                    end
                    local okUp, ups = pcall(vantaDebug.getupvalues, caller)
                    if okUp and type(ups) == "table" then
                        local kinds = {}
                        for i, u in pairs(ups) do kinds[#kinds + 1] = i .. ":" .. typeof(u) end
                        if #kinds > 0 then
                            parts[#parts + 1] = "  up" .. level .. "=[" ..
                                table.concat(kinds, ",") .. "]"
                        end
                    end
                end
            end
            __vanta_log("SETUPVALUE nil target index=" .. tostring(index) ..
                       " value=" .. tostring(value) .. " valuetype=" .. typeof(value) ..
                       "\n" .. table.concat(parts, "\n"))
        end
        return __vanta_setupvalue(resolve(target), index, value)
    end

    -- debug.getinfo is the table-shaped version of debug.info.
    stock.getinfo = function(target)
        local func = resolve(target)
        local short_src = info(func, "s")
        local params, is_vararg = info(func, "a")
        return {
            source = "@" .. tostring(short_src),
            short_src = short_src,
            func = func,
            what = iscclosure(func) and "C" or "Lua",
            currentline = -1,
            name = info(func, "n"),
            numparams = params,
            is_vararg = is_vararg and 1 or 0,
            -- The real count, not `#` over the values: an upvalue holding nil
            -- makes the table sparse and `#` then answers 0. mac and RbxStu
            -- both take this from lua_getinfo's `u` flag, which is the closure's
            -- own nupvalues - which is what __vanta_nupvalues reads.
            nups = __vanta_nupvalues(func),
            linedefined = info(func, "l"),
        }
    end

    -- debug.traceback(thread?, message?, level?) -> string. Both references
    -- that carry it - RbxStu (Environment/Libraries/Debug.cpp) and Vicna
    -- (env/libs/debug.hpp, a thin `luaL_traceback` wrapper) - accept an
    -- optional leading thread, an optional message that is prepended verbatim,
    -- and a level to start from. The C `luaL_traceback` they call cannot be
    -- reached here: our bundled Luau VM only compiles bytecode, and operating
    -- on Roblox's live lua_State means its struct layout and an offset we do
    -- not carry for that symbol. Roblox's own `debug` library already ships a
    -- native `traceback`, so it is kept when present (it is a genuine C
    -- closure, exactly what a probe expects); the walk below is only a
    -- fallback for a build that lacks it, built on the same native `debug.info`
    -- the rest of this block uses. The format follows RbxStu: C frames skipped,
    -- `short_src`, then `:line`, then ` function name`, one frame per line.
    if type(stock.traceback) ~= "function" then
        stock.traceback = function(...)
            local first = ...
            local thread, message, level
            if type(first) == "thread" then
                thread, message, level = first, select(2, ...), select(3, ...)
            else
                message, level = first, select(2, ...)
            end

            level = tonumber(level) or (thread and 0 or 1)
            if level < 0 then
                error("level can't be negative", 2)
            end

            local out = {}
            if message ~= nil then
                out[#out + 1] = rawtostr(message)
                out[#out + 1] = "\n"
            end

            local frame = level
            while true do
                local ok, source, line, name
                if thread then
                    ok, source, line, name = pcall(info, thread, frame, "sln")
                else
                    -- +1 skips this traceback frame itself, so level 1 is the
                    -- caller - matching luaL_traceback on the current thread.
                    ok, source, line, name = pcall(info, frame + 1, "sln")
                end
                if not ok or source == nil then break end

                -- info gives no what-flag; a C frame has no source line, which
                -- is how RbxStu's `strcmp(ar.what, "C")` skip is reproduced.
                if not (line == nil or line < 0) then
                    out[#out + 1] = source
                    out[#out + 1] = ":"
                    out[#out + 1] = tostring(line)
                    if name ~= nil and name ~= "" then
                        out[#out + 1] = " function "
                        out[#out + 1] = name
                    end
                    out[#out + 1] = "\n"
                end

                frame = frame + 1
                if frame > level + 200 then break end   -- runaway guard
            end

            return table.concat(out)
        end
    end

    setreadonly(stock, locked)

    getgenv().getupvalues = stock.getupvalues
    getgenv().getupvalue = stock.getupvalue
    getgenv().setupvalue = stock.setupvalue
    getgenv().getconstants = stock.getconstants
    getgenv().getconstant = stock.getconstant
    getgenv().setconstant = stock.setconstant
    getgenv().getprotos = stock.getprotos
    getgenv().getproto = stock.getproto
    getgenv().getinfo = stock.getinfo
end

-- filtergc narrows what getgc returns using the same information the debug
-- library exposes, which is why it lands here rather than in C.
getgenv().filtergc = function(kind, options, returnOne)
    if kind ~= "function" and kind ~= "table" then
        error("filtergc: filter type must be 'function' or 'table'", 2)
    end
    options = options or {}

    -- filtergc lives in Lua, so __vanta_trace cannot reach it; this is the same
    -- idea in the same place. Printing from inside the existing Lua closure
    -- changes no identity, which is the rule that matters for sUNC.
    if getgenv().__vanta_trace_filtergc then
        local parts = {}
        for key, value in pairs(options) do
            if type(value) == "table" then
                local items = {}
                for i, item in ipairs(value) do items[i] = tostring(item) end
                parts[#parts + 1] = key .. "={" .. table.concat(items, ",") .. "}#" .. #value
            else
                parts[#parts + 1] = key .. "=" .. tostring(value)
            end
        end
        print("FGC " .. kind .. " one=" .. tostring(returnOne) .. " " .. table.concat(parts, " "))
    end

    local wantFunctions = kind == "function"

    -- A positional upvalue match beats a membership one, so the walk sorts what
    -- it finds into two lists rather than returning the first thing that
    -- passes. Everything a query without Upvalues matches is exact, so the two
    -- lists collapse to the old single one for every other filter.
    local matches, count = {}, 0
    local loose, looseCount = {}, 0
    local looseMatch
    for _, value in ipairs(getgc(not wantFunctions)) do
        local ok = true
        local exact = options.Upvalues == nil
        if wantFunctions then
            ok = type(value) == "function"
            if ok and options.IgnoreExecutor ~= false and isexecutorclosure(value) then
                ok = false
            end
            if ok and options.Name ~= nil then
                local named, name = pcall(debug.info, value, "n")
                ok = named and name == options.Name
            end
            if ok and options.Hash ~= nil then
                ok = islclosure(value) and getfunctionhash(value) == options.Hash
            end
            if ok and options.Constants ~= nil then
                if not islclosure(value) then
                    ok = false
                else
                    -- A linear scan per wanted constant, not a hash set of every
                    -- constant: this runs once per Lua function on the heap
                    -- (~3.5k of them), a query names one or two constants, and
                    -- building a set for each function was most of a 78 ms call
                    -- (perf.lua, 2026-09-03). Same matches as the set gave:
                    -- equality for anything comparable, and NaN - which can be
                    -- a constant but never equals itself - matched as "any
                    -- constant that is not itself".
                    local readable, constants = pcall(vantaDebug.getconstants, value)
                    if not readable or type(constants) ~= "table" then
                        constants = {}
                    end
                    for _, wanted in ipairs(options.Constants) do
                        local found = false
                        if wanted ~= wanted then
                            for _, constant in pairs(constants) do
                                if constant ~= constant then found = true break end
                            end
                        else
                            for _, constant in pairs(constants) do
                                if constant == wanted then found = true break end
                            end
                        end
                        if not found then ok = false break end
                    end
                end
            end
            if ok and options.Upvalues ~= nil then
                -- Two ways to match, and the order matters. Set membership
                -- alone finds *a* function holding those values somewhere,
                -- which is how a function with no upvalues at all came back for
                -- Upvalues={5,0,22} - something else in the heap happened to
                -- hold all three. A positional match - upvalue i equal to
                -- wanted i - is what the caller means, so it wins; membership
                -- is kept as a fallback for a filter that lists them out of
                -- order, and only used when nothing matched positionally.
                local readable, upvalues = pcall(vantaDebug.getupvalues, value)
                upvalues = readable and upvalues or {}

                local positional = true
                for index, wanted in ipairs(options.Upvalues) do
                    local held = upvalues[index]
                    if wanted ~= wanted then
                        if held == held then positional = false break end
                    elseif held ~= wanted then
                        positional = false break
                    end
                end

                if positional then
                    exact = true
                else
                    -- Membership by linear scan, not by building a set: this
                    -- branch runs for nearly every function on the heap (only
                    -- the target matches positionally), and a hash set per
                    -- function was most of a 47 ms query (perf.lua, 2026-09-03).
                    -- Same matches as before, NaN included.
                    for _, wanted in ipairs(options.Upvalues) do
                        local found = false
                        if wanted ~= wanted then
                            for _, upvalue in pairs(upvalues) do
                                if upvalue ~= upvalue then found = true break end
                            end
                        else
                            for _, upvalue in pairs(upvalues) do
                                if upvalue == wanted then found = true break end
                            end
                        end
                        if not found then ok = false break end
                    end
                end
            end
        else
            ok = type(value) == "table"
            if ok and options.Keys ~= nil then
                for _, key in ipairs(options.Keys) do
                    if rawget(value, key) == nil then ok = false break end
                end
            end
            if ok and options.Values ~= nil then
                -- Linear scan per wanted value: this runs once per table on the
                -- heap (~72k), and a hash set of every value in each of them is
                -- an allocation per table for a query that names one or two.
                for _, wanted in ipairs(options.Values) do
                    local found = false
                    if wanted ~= wanted then
                        for _, entry in pairs(value) do
                            if entry ~= entry then found = true break end
                        end
                    else
                        for _, entry in pairs(value) do
                            if entry == wanted then found = true break end
                        end
                    end
                    if not found then ok = false break end
                end
            end
            if ok and options.KeyValuePairs ~= nil then
                for key, wanted in pairs(options.KeyValuePairs) do
                    if rawget(value, key) ~= wanted then ok = false break end
                end
            end
            if ok and options.Metatable ~= nil then
                ok = getrawmetatable(value) == options.Metatable
            end
        end

        if ok then
            if exact then
                if returnOne then
                    if getgenv().__vanta_trace_filtergc then
                        print("FGC -> " .. tostring(value) .. " (exact)")
                    end
                    return value
                end
                count += 1
                matches[count] = value
            elseif returnOne then
                looseMatch = looseMatch or value
            else
                looseCount += 1
                loose[looseCount] = value
            end
        end
    end

    if returnOne then
        if getgenv().__vanta_trace_filtergc then
            print("FGC -> " .. tostring(looseMatch) .. " (loose)")
        end
        __vanta_collect()
        return looseMatch
    end

    -- Exact matches win outright. A loose match - a function that merely holds
    -- the wanted upvalues somewhere rather than in the positions asked for - is
    -- only an answer when nothing matched positionally. Returning both put a
    -- stale closure ahead of the caller's own function in the list, which is
    -- what "failed to retrieve a function with the upvalue" was: the first
    -- entry was a function holding 5, 0 and 22 in the wrong slots.
    local result = count > 0 and matches or loose

    if getgenv().__vanta_trace_filtergc then
        print("FGC -> " .. tostring(#result) .. " matches"
            .. " (exact=" .. count .. " loose=" .. looseCount .. ")")
    end

    -- The heap walk above allocated a table of every live object; dropping it
    -- and collecting leaves the heap as filtergc found it.
    __vanta_collect()

    return result
end

-- debug.getstack and debug.setstack are bound straight to the C side rather
-- than through a Lua shim: both count stack levels from their own frame, and a
-- wrapper would insert one of its own that the caller never asked about.
do
    -- The private copy installed above, not Roblox's shared table - this is a
    -- plain global lookup and the copy is a raw key of the executor globals, so
    -- it is what `debug` resolves to from here on.
    local stock = vantaDebug
    local locked = isreadonly(stock)
    setreadonly(stock, false)
    stock.getstack = __vanta_getstack
    stock.setstack = __vanta_setstack
    setreadonly(stock, locked)

    getgenv().getstack = __vanta_getstack
    getgenv().setstack = __vanta_setstack
end

getgenv().hookfunc = hookfunction
getgenv().replaceclosure = hookfunction
getgenv().restorefunc = restorefunction
getgenv().restore_function = restorefunction
getgenv().websocket = getgenv().WebSocket

-- The older `console*` spellings of the rconsole family. sourcessss registers
-- every one of them beside its `rconsole*` twin, and scripts written against
-- the original UNC naming still use them. Wired here rather than registered a
-- second time from C, so the two names are the same closure and compare equal.
for rname, cname in pairs({
    rconsoleprint = "consoleprint", rconsoleinfo = "consoleinfo",
    rconsolewarn = "consolewarn", rconsoleerror = "consoleerror",
    rconsoleclear = "consoleclear", rconsolecreate = "consolecreate",
    rconsoleshow = "consoleshow", rconsolehide = "consolehide",
    rconsoledestroy = "consoledestroy", rconsolesettitle = "consolesettitle",
    rconsoleinput = "consoleinput",
}) do
    getgenv()[cname] = getgenv()[rname]
end

-- gethui is cloneref(CoreGui), which is what the macOS reference executor does
-- and what the spec is really after: CoreGui is itself a BasePlayerGui, so it
-- satisfies the container test, and the clone means a script comparing what it
-- was handed against game:GetService("CoreGui") cannot tie the two together.
-- One container, cached, so every caller parents into the same place.
do
    local container

    getgenv().gethui = function()
        if typeof(container) ~= "Instance" then
            local coregui = game:GetService("CoreGui")
            container = cloneref and cloneref(coregui) or coregui
        end
        -- Ask before building the traceback: the arguments to __vanta_ltrace are
        -- evaluated whether or not the trace is on, and debug.traceback() was
        -- most of a 773 ns call (perf2, 2026-09-03).
        if __vanta_ltrace_wanted("gethui") then
            __vanta_ltrace("gethui", "->", container, debug.traceback())
        end
        return container
    end

    getgenv().get_hidden_gui = getgenv().gethui
end

-- Actors run their own Luau state, but the instances themselves are ordinary
-- children of the DataModel, so the registry walk finds every one of them.
getgenv().getactors = function()
    local found, seen, count = {}, {}, 0

    -- The engine's own actor map first. Two of the three references that have
    -- getactors read exactly this (the macOS one and sourcessss), because it
    -- cannot miss an Actor that Lua has never been handed - which a walk over
    -- the instance cache can, and does.
    if instanceCache then
        local ok, pointers = pcall(__vanta_actorpointers)
        if ok and type(pointers) == "table" then
            for _, pointer in ipairs(pointers) do
                local instance = instanceCache[pointer]
                if typeof(instance) == "Instance" and not seen[instance] then
                    seen[instance] = true
                    count += 1
                    found[count] = instance
                end
            end
        end
    end

    -- Then the DataModel walk, which is what Vicna does and what covers an
    -- Actor the engine has not filed in that map yet.
    for _, instance in ipairs(game:GetDescendants()) do
        local ok, isActor = pcall(instance.IsA, instance, "Actor")
        if ok and isActor and not seen[instance] then
            seen[instance] = true
            count += 1
            found[count] = instance
        end
    end
    return found
end

-- compareinstances is named for what it takes. Two tables are not instances,
-- and answering false for them would hide the mistake rather than report it.
if cloneref then
    getgenv().compareinstances = function(a, b)
        if typeof(a) ~= "Instance" or typeof(b) ~= "Instance" then
            error("compareinstances: expected two Instances", 2)
        end
        local first = __vanta_rawptr(a)
        return first ~= nil and first == __vanta_rawptr(b)
    end
end

-- A signal library of our own. Roblox's RBXScriptSignal cannot be constructed
-- from Lua, and the spec asks for Connect/Once/Wait/Fire with a Connection
-- object that disconnects - all of which is plain bookkeeping.
do
    local Connection = {}
    Connection.__index = Connection

    function Connection:Disconnect()
        if not self.Connected then
            return
        end
        self.Connected = false
        local handlers = self._signal._handlers
        for i = #handlers, 1, -1 do
            if handlers[i] == self then
                table.remove(handlers, i)
                break
            end
        end
    end

    Connection.disconnect = Connection.Disconnect
    Connection.Destroy = Connection.Disconnect

    local Signal = {}
    Signal.__index = Signal

    function Signal.new()
        return setmetatable({ _handlers = {} }, Signal)
    end

    local function connect(self, callback, once)
        if type(callback) ~= "function" then
            error("Signal:Connect expects a function", 3)
        end
        local connection = setmetatable({
            Connected = true,
            Function = callback,
            _signal = self,
            _once = once or false,
        }, Connection)
        table.insert(self._handlers, connection)
        return connection
    end

    function Signal:Connect(callback) return connect(self, callback, false) end
    function Signal:Once(callback) return connect(self, callback, true) end

    function Signal:Fire(...)
        -- Handlers may disconnect during a fire, so the list is copied first.
        local snapshot = table.clone(self._handlers)
        for _, connection in ipairs(snapshot) do
            if connection.Connected then
                if connection._once then
                    connection:Disconnect()
                end
                connection.Function(...)
            end
        end

        local waiting = self._waiting
        if waiting then
            self._waiting = nil
            for _, thread in ipairs(waiting) do
                task.spawn(thread, ...)
            end
        end
    end

    function Signal:Wait()
        self._waiting = self._waiting or {}
        table.insert(self._waiting, coroutine.running())
        return coroutine.yield()
    end

    function Signal:Destroy()
        table.clear(self._handlers)
        self._waiting = nil
    end

    Signal.connect = Signal.Connect
    Signal.once = Signal.Once
    Signal.wait = Signal.Wait
    Signal.fire = Signal.Fire

    getgenv().Signal = { new = Signal.new, Connection = Connection }
end

-- The file dialogs. The panels themselves are AppKit's, reached from C; what a
-- script gets back is the item shape the spec describes, built here.
do
    local function makeItem(path)
        local kind = __vanta_dialog_stat(path)
        if not kind then
            return nil
        end

        local item = {
            Path = path,
            Name = path:match("([^/]+)/?$") or path,
        }

        function item:GetName() return self.Name end
        function item:GetPath() return self.Path end
        function item:IsFile() return kind == "file" end
        function item:IsFolder() return kind == "folder" end
        function item:Delete() return __vanta_dialog_delete(self.Path) end

        if kind == "file" then
            function item:Read() return __vanta_dialog_read(self.Path) end
            function item:Write(content) return __vanta_dialog_write(self.Path, content, false) end
            function item:Append(content) return __vanta_dialog_write(self.Path, content, true) end
        else
            function item:List(recursive)
                local entries = {}
                for _, child in ipairs(__vanta_dialog_list(self.Path, recursive and true or false)) do
                    local made = makeItem(child)
                    if made then
                        table.insert(entries, made)
                    end
                end
                return entries
            end
            function item:MakeFolder(name)
                local child = self.Path:gsub("/$", "") .. "/" .. tostring(name)
                __vanta_dialog_makefolder(child)
                return makeItem(child)
            end
        end

        return item
    end

    getgenv().openfiledialog = function(options)
        local picked = __vanta_dialog_openfile(options, false)
        return picked[1] and makeItem(picked[1]) or nil
    end

    getgenv().openfilesdialog = function(options)
        local picked = __vanta_dialog_openfile(options, true)
        local items = {}
        for _, path in ipairs(picked) do
            local made = makeItem(path)
            if made then
                table.insert(items, made)
            end
        end
        return #items > 0 and items or nil
    end

    getgenv().openfolderdialog = function(options)
        local picked = __vanta_dialog_openfolder(options)
        return picked[1] and makeItem(picked[1]) or nil
    end

    getgenv().savefiledialog = function(content, options)
        options = options or {}
        local picked = __vanta_dialog_savefile(options)
        local path = picked[1]
        if not path then
            return nil
        end
        -- defaultExtension is appended only when the user gave no extension of
        -- their own, which is what the spec asks for.
        if options.defaultExtension and not path:match("%.[^./]+$") then
            path = path .. "." .. tostring(options.defaultExtension)
        end
        __vanta_dialog_write(path, content or "", false)
        return makeItem(path)
    end
end

-- Roblox refuses `require` on an ordinary ModuleScript from a thread carrying a
-- RobloxScript identity - which every executor thread does, because it needs
-- the capabilities that come with one. Identities 3, 6, 7 and 8 are refused;
-- anything lower is not. So the identity is lowered for the duration of the
-- call and put straight back. The capability mask setthreadidentity leaves in
-- place is what makes that safe: the thread keeps everything it could do.
-- require is the engine's own, and nothing here wraps it.
--
-- It used to be replaced with a wrapper that lowered the whole thread's
-- identity to 2 for the duration of the call and, when that still failed, ran
-- the module's bytecode itself and cached the result. Both halves were wrong:
--
--   * the identity is a property of the *thread*, so a module that yields
--     while loading left everything else on that thread running at identity 2
--     until it came back, and a module that errored could leave it there.
--   * loading it ourselves produced a **second copy** of the module, outside
--     the engine's own require cache. The game's `require` then returned one
--     table and ours another, so any state the module set up existed twice -
--     which is what "it breaks modules inside of games" is, and where the
--     console spam comes from.
--
-- No reference does either. Three of the six do not touch `require` at all;
-- Vicna sets one boolean on the ScriptContext so the engine stops applying the
-- "Cannot require a non-RobloxScript module from a RobloxScript" rule to us,
-- and then leaves `require` alone. That flag is `ScriptContext + 0x910` here
-- (`tests/req6.lua` found it by setting each candidate byte and asking the
-- engine's require whether it worked), and main.cpp sets it on every join.
--
-- The engine's require is therefore the only one there is: one cache, one copy
-- of every module, shared with the game.


-- saveinstance is SynSaveInstance, which is what the reference executors do and
-- the only sane answer: serialising a DataModel properly means an API dump,
-- property-type tables and script decompilation, all of which that library
-- already carries. It is fetched once and cached on disk, and runs on the
-- calling thread so the file exists by the time the call returns.
do
    local cacheFile = "vanta_saveinstance.luau"
    local source

    local function library()
        if not source then
            if isfile(cacheFile) then
                local ok, cached = pcall(readfile, cacheFile)
                if ok and #cached > 4096 then
                    source = cached
                end
            end
        end
        if not source then
            source = game:HttpGet(
                "https://raw.githubusercontent.com/luau/SynSaveInstance/main/saveinstance.luau", true)
            pcall(writefile, cacheFile, source)
        end
        local chunk = loadstring(source, "saveinstance")
        if type(chunk) ~= "function" then
            error("saveinstance: could not compile the serialiser", 3)
        end
        return chunk()
    end

    getgenv().saveinstance = function(instance, options)
        if instance ~= nil and typeof(instance) ~= "Instance" then
            options, instance = instance, nil
        end

        local settings = {
            SafeMode = false,
            KillAllScripts = false,
            AntiIdle = false,
        }
        if type(options) == "table" then
            for key, value in pairs(options) do
                settings[key] = value
            end
        end

        return library()(instance or game, settings)
    end

    getgenv().saveplace = function(options)
        return getgenv().saveinstance(game, options)
    end
end

-- The actor surface. run_on_actor is native, since it has to build a thread on
-- another Luau state; everything else here is bookkeeping.
do
    getgenv().run_on_actor = function(actor, source, ...)
        if typeof(actor) ~= "Instance" or not actor:IsA("Actor") then
            error("run_on_actor: expected an Actor", 2)
        end
        if type(source) ~= "string" then
            error("run_on_actor: expected source", 2)
        end
        return __vanta_runonactor(actor, source, ...)
    end

    -- isparallel is native: it compares the running thread's global state
    -- against the main VM's, which is what actually distinguishes an Actor's
    -- Luau state from ours. The old probe here wrote to a BoolValue and read
    -- the failure, which says nothing once the write is to an instance that is
    -- not in the DataModel.

    -- create_comm_channel and get_comm_channel are native. They have to be:
    -- an Actor runs in a different Luau state, so a table of channels held in
    -- ours is not something it could look an id up in. The map lives in C++ and
    -- hands each state its own userdata for the same BindableEvent.
end

-- getconnections, and firesignal on top of it.
--
-- A signal's connections are an intrusive list of slots, newest first, and the
-- only handle Lua is given on that list is a connection it made itself. So one
-- throwaway handler is connected, its slot taken, and the chain walked from
-- there; the throwaway is disconnected before anything is returned, and skipped
-- on the way past. Everything the proxy exposes is the engine's own state:
-- Enabled is the slot's State pointer, the handler and its thread are registry
-- refs the engine stored, and disabling really is what the engine checks when
-- it decides whether to run a slot.
do
    local registry = getreg()

    local function slotsOf(signal)
        -- The chain has to be read while the throwaway is still connected:
        -- disconnecting it first unlinks it, and Next comes back null.
        local probe = signal:Connect(function() end)
        local skip = __vanta_ptr(probe)

        local slots, seen = {}, {}
        local cursor = skip
        while cursor and cursor ~= 0 and not seen[cursor] do
            seen[cursor] = true
            local info = __vanta_slotinfo(cursor)
            if not info then break end
            -- A one-shot connection that has already been retired is gone as
            -- far as the signal is concerned - the engine unlinks its own after
            -- it runs, and sUNC checks for exactly that ("Connection shouldn't
            -- exist after firing once"). We cannot unlink an intrusive,
            -- refcounted slot safely from out here, so a retired :Once is
            -- filtered instead. It is narrow on purpose: an ordinary connection
            -- that has merely been Disable()d still exists and still lists.
            if cursor ~= skip and not (info.once and not info.enabled) then
                slots[#slots + 1] = { address = cursor, info = info }
            end
            cursor = info.next
        end

        probe:Disconnect()
        return slots
    end

    local function proxyFor(entry)
        local address = entry.address
        local disabled = nil
        -- The ref this connection was made with. `Enabled` compares against it
        -- rather than trusting the slot's state pointer, because a slot the
        -- engine has retired keeps a stale state while its ref goes to zero -
        -- see __vanta_slotenabled for the measurement.
        local bornRef = (entry.info and entry.info.functionref ~= 0 and entry.info.functionref)
            or (entry.info and entry.info.waitref ~= 0 and entry.info.waitref)
            or (entry.info and entry.info.threadref ~= 0 and entry.info.threadref)
            or 0

        local proxy = {}

        local function handler()
            local info = __vanta_slotinfo(address)
            local ref = info and info.functionref or 0
            local value = ref ~= 0 and registry[ref] or nil
            return type(value) == "function" and value or nil
        end

        local function thread()
            local info = __vanta_slotinfo(address)
            local ref = info and info.threadref or 0
            local value = ref ~= 0 and registry[ref] or nil
            if type(value) == "thread" then return value end
            -- A `:Wait()` connection has no handler and keeps the waiting
            -- thread in its own slot instead, which is the thread that made it.
            local waitref = info and info.waitref or 0
            local waiting = waitref ~= 0 and registry[waitref] or nil
            return type(waiting) == "thread" and waiting or nil
        end

        -- A `:Wait()` connection is a Luau connection with no function: the
        -- thread *is* the listener. sUNC checks exactly that - "Returned false
        -- for LuaConnection on a :Wait connection" - and the macOS reference
        -- answers it the same way, by looking for a live thread in the wait
        -- slot when there is no handler.
        local function waitThread()
            local info = __vanta_slotinfo(address)
            local ref = info and info.waitref or 0
            local value = ref ~= 0 and registry[ref] or nil
            return type(value) == "thread" and value or nil
        end

        -- A `:Once` connection is retired by whoever runs it - the engine
        -- unlinks its own after the handler returns, so firing one from out
        -- here has to do the same or the connection outlives its single use.
        -- The macOS reference does exactly this and nothing more: it zeroes the
        -- slot's state, which is the switch `Enabled` and the engine both read.
        local function retireIfOnce(from)
            local info = __vanta_slotinfo(address)
            __vanta_ltrace("connection", from, "once=", info and info.once,
                          "enabled=", info and info.enabled)
            if info and info.once then
                disabled = disabled or __vanta_setslotstate(address, 0)
            end
        end

        -- Fire/Defer, checked against all six: five of them hand back a no-op
        -- when the connection has no function (which is what a `:Wait` is), and
        -- the macOS one resumes the waiting thread instead. **None of the six
        -- disables the slot here** - only a `:Once` with a real handler is
        -- retired, which is what retireIfOnce does. Resuming the waiter is the
        -- macOS behaviour and strictly more useful than a no-op, so it is taken;
        -- disabling is not.
        local function deliverToWaiter(schedule, ...)
            local co = waitThread()
            if type(co) ~= "thread" or coroutine.status(co) ~= "suspended" then
                return false
            end
            schedule(co, ...)
            return true
        end

        function proxy:Fire(...)
            local fn = handler()
            if fn then
                task.spawn(fn, ...)
            else
                deliverToWaiter(task.spawn, ...)
            end
            retireIfOnce("Fire")
        end

        function proxy:Defer(...)
            local fn = handler()
            if fn then
                task.defer(fn, ...)
            else
                deliverToWaiter(task.defer, ...)
            end
            retireIfOnce("Defer")
        end

        function proxy:Disable()
            if disabled == nil then
                disabled = __vanta_setslotstate(address, 0)
            end
        end

        function proxy:Enable()
            if disabled ~= nil then
                __vanta_setslotstate(address, disabled)
                disabled = nil
            end
        end

        function proxy:Disconnect()
            __vanta_ltrace("connection", "Disconnect")
            disabled = disabled or __vanta_setslotstate(address, 0)
        end

        return setmetatable(proxy, {
            __index = function(_, key)
                if key == "Enabled" then
                    -- State first, and the ref only if the state says live -
                    -- a disabled slot short-circuits before any ref work.
                    return __vanta_slotenabled(address, bornRef) == true
                elseif key == "Function" then
                    return handler()
                elseif key == "Thread" then
                    return thread()
                elseif key == "LuaConnection" then
                    -- A handler we can see, or a `:Wait` whose thread we can
                    -- see. Both are connections made from this Luau state.
                    return handler() ~= nil or waitThread() ~= nil
                elseif key == "ForeignState" then
                    -- A handler we can see the closure of is running in this
                    -- Luau state; one we cannot belongs to another. A `:Wait`
                    -- has no handler and is still ours, which is why the
                    -- reference never calls a wait connection foreign.
                    return handler() == nil and waitThread() == nil
                end
                return nil
            end,
            __newindex = function() end,
            __type = "RBXScriptConnection",
            __tostring = function() return "Connection" end,
        })
    end

    getgenv().getconnections = function(signal)
        if typeof(signal) ~= "RBXScriptSignal" then
            error("getconnections: expected an RBXScriptSignal", 2)
        end
        local out, slotAddresses = {}, {}
        for index, entry in ipairs(slotsOf(signal)) do
            out[index] = proxyFor(entry)
            slotAddresses[index] = entry.address
        end
        if getgenv().__vanta_ltrace_wanted("getconnections") then
            local shapes = {}
            for i, c in ipairs(out) do
                local info = __vanta_slotinfo(slotAddresses[i])
                shapes[i] = string.format("[%d enabled=%s once=%s foreign=%s lua=%s fn=%s thread=%s]",
                    i, tostring(c.Enabled), tostring(info and info.once), tostring(c.ForeignState),
                    tostring(c.LuaConnection), type(c.Function), type(c.Thread))
            end
            __vanta_ltrace("getconnections", tostring(signal), "n=" .. #out,
                          table.concat(shapes, " "))
        end
        return out
    end

    -- firesignal runs each handler on a thread of its own, which is what a real
    -- fire does - running them inline would show up in the handler's traceback.
    getgenv().firesignal = function(signal, ...)
        if typeof(signal) ~= "RBXScriptSignal" then
            error("firesignal: expected an RBXScriptSignal", 2)
        end
        local arguments = table.pack(...)
        for _, entry in ipairs(slotsOf(signal)) do
            pcall(function()
                local info = __vanta_slotinfo(entry.address)
                if not info or not info.enabled then
                    return
                end

                -- A connection made inside an Actor lives in that Actor's own
                -- Luau state: its handler is a ref in that state's registry and
                -- its thread is on that state's heap, so neither is anything
                -- our registry can resolve. Those go over natively.
                if info.foreign then
                    if info.functionref == 0 then
                        __vanta_setslotstate(entry.address, 0)
                    end
                    __vanta_fireforeign(entry.address, arguments)
                    return
                end

                local fn = info.functionref ~= 0 and registry[info.functionref] or nil
                if type(fn) == "function" then
                    task.spawn(fn, table.unpack(arguments, 1, arguments.n))
                    -- The engine retires a one-shot connection once its handler
                    -- has run; firing from out here has to do the same, or a
                    -- `:Once` survives its own firing.
                    if info.once then
                        __vanta_setslotstate(entry.address, 0)
                    end
                    return
                end

                -- A slot with a thread but no handler is a :Wait. Delivering to
                -- it means resuming the thread - and the slot has to come out
                -- of the list first, or the engine resumes the same thread
                -- again the next time the signal really fires.
                local waiting = info.waitref ~= 0 and registry[info.waitref] or nil
                local co = waiting or (info.threadref ~= 0 and registry[info.threadref] or nil)
                if type(co) == "thread" and coroutine.status(co) == "suspended" then
                    __vanta_setslotstate(entry.address, 0)
                    task.spawn(co, table.unpack(arguments, 1, arguments.n))
                end
            end)
        end
    end
end

-- fireclickdetector and firetouchinterest, on top of the signal machinery.
--
-- Both of these are "make the game think this happened". The local half is
-- firesignal over the detector's or part's own connections; the server half is
-- replicatesignal, which only goes out for events the engine is willing to
-- accept from a client - MouseClick and RightMouseClick are, the hover pair and
-- Touched are not, and cansignalreplicate is what says so.
do
    local clickEvents = {
        mouseclick = "MouseClick",
        rightmouseclick = "RightMouseClick",
        mousehoverenter = "MouseHoverEnter",
        mousehoverleave = "MouseHoverLeave",
    }

    getgenv().fireclickdetector = function(detector, distance, event)
        if typeof(detector) ~= "Instance" or not detector:IsA("ClickDetector") then
            error("fireclickdetector: expected a ClickDetector", 2)
        end

        -- The distance and the event name arrive in either order in the wild,
        -- and either may be absent.
        if type(distance) == "string" then
            distance, event = event, distance
        end
        -- An unrecognised event name falls back to MouseClick rather than
        -- raising: not one of the six references errors here, and the two that
        -- say anything at all (the macOS one and SkidBase) both default to
        -- MouseClick. SkidBase also lowercases the name, so "mouseclick" works.
        local named = type(event) == "string" and clickEvents[event:lower()] or nil
        event = named or "MouseClick"

        local player = game:GetService("Players").LocalPlayer
        local signal = detector[event]

        if cansignalreplicate and cansignalreplicate(signal) then
            pcall(replicatesignal, signal, player)
        end
        firesignal(signal, player)
    end

    getgenv().firetouchinterest = function(part, toucher, ended)
        if typeof(part) ~= "Instance" or not part:IsA("BasePart") then
            error("firetouchinterest: expected a BasePart", 2)
        end
        if typeof(toucher) ~= "Instance" or not toucher:IsA("BasePart") then
            error("firetouchinterest: expected a BasePart", 2)
        end

        -- A real overlap ends as well as begins, and the third argument picks
        -- which: false/0 is the touch, true/1 is the end of it.
        local finished = ended == true or ended == 1
        local name = finished and "TouchEnded" or "Touched"

        -- Two halves, and both are wanted.
        --
        -- The engine's own overlap registration is the real one: it is what the
        -- server sees, and it is why this function exists. What it does not do
        -- is raise anything on this frame - a touch it registers surfaces
        -- whenever the physics step next gets to it, unregistering one raises
        -- no TouchEnded at all, and a part parented this frame has no Primitive
        -- for it to work on. Callers expect the handler to have run by the next
        -- step, so the signals are raised here as well.
        __vanta_firetouch(part, toucher, finished)
        firesignal(toucher[name], part)
        firesignal(part[name], toucher)
    end

    getgenv().firetouchtransmitter = getgenv().firetouchinterest
end

-- The script-bytecode family is native; only the spellings live here.
getgenv().dumpstring = getgenv().getscriptbytecode
getgenv().getscriptfunction = getgenv().getscriptclosure

-- The signal family. The reads are native; the type check belongs here, where
-- typeof() knows what an RBXScriptSignal is.
do
    local function expectSignal(signal, who)
        if typeof(signal) ~= "RBXScriptSignal" then
            error(who .. ": expected an RBXScriptSignal", 3)
        end
    end

    -- Both of these are the engine's own answer now, not ours.
    --
    -- They used to be built from a bit in the event descriptor, found by
    -- diffing signals whose answer was believed known. The macOS reference does
    -- not guess: it calls `IsLegalSendEvent(Replicator, owner, descriptor)` for
    -- one signal, and reads the replicator's real whitelist for all of them.
    -- Measured against the engine (`tests/rep2.lua`), the bit was wrong in both
    -- directions - it said ClickDetector.MouseClick and ProximityPrompt.Triggered
    -- replicate when the replicator will not send either, and said
    -- GuiObject.MouseWheelForward and GuiButton.MouseButton1Click do not when
    -- they are both in the whitelist. Those two are exactly what the spec's own
    -- replicatesignal example uses.
    --
    -- The descriptor bit stays as the fallback for a client with no replicator
    -- at all (solo play), where there is nothing to ask.
    local function clientReplicator()
        local ok, service = pcall(game.GetService, game, "NetworkClient")
        if not ok or not service then
            return nil
        end
        local found, replicator = pcall(service.FindFirstChildOfClass, service, "ClientReplicator")
        return found and replicator or nil
    end

    local whitelist

    getgenv().getsignalwhitelist = function()
        if whitelist then
            return whitelist
        end

        local replicator = clientReplicator()
        if replicator then
            local ok, list = pcall(__vanta_signalwhitelist, replicator)
            if ok and type(list) == "table" and #list > 0 then
                whitelist = list
                return whitelist
            end
        end

        -- No replicator: fall back to the descriptor bit over the classes that
        -- are actually present. Each class costs a page walk, so the frame is
        -- given back every few of them.
        local classes, seen = {}, {}
        for _, instance in ipairs(getgenv().getinstances()) do
            local ok, descriptor = pcall(__vanta_peekptr, __vanta_ptr(instance), 0x18)
            if ok and descriptor and not seen[descriptor] then
                seen[descriptor] = true
                classes[#classes + 1] = descriptor
            end
        end

        whitelist = {}
        for index, descriptor in ipairs(classes) do
            local className = __vanta_peekstr(__vanta_peekptr(descriptor, 8) or 0, 0)
            if className and #className > 0 then
                for _, member in ipairs(__vanta_scanmembers(descriptor, 0x180000) or {}) do
                    if __vanta_descreplicable(member.address) then
                        table.insert(whitelist, { Event = member.name, Parent = className })
                    end
                end
            end
            if index % 4 == 0 then
                task.wait()
            end
        end
        return whitelist
    end

    getgenv().cansignalreplicate = function(signal)
        expectSignal(signal, "cansignalreplicate")
        local replicator = clientReplicator()
        if replicator then
            local ok, legal = pcall(__vanta_islegalsendevent, signal, replicator)
            if ok and type(legal) == "boolean" then
                return legal
            end
        end
        return __vanta_cansignalreplicate(signal) == true
    end

    -- A Variant needs a TypeHolder or the replicator will not serialise it, and
    -- nothing static pairs a reflection Type with its holder: a parameter's own
    -- inline default is the void type with a null holder, and the holder is a
    -- separate static the Type object does not point at. The engine's own
    -- raises carry correct pairs, so one harvester watching raiseEventInvocation
    -- collects them; it stops collecting as soon as it has a scalar and an
    -- Instance, which takes about a second of ordinary traffic
    -- (Player.SimulationRadiusChanged and Animator.OnCombinedUpdate both fire
    -- on their own). Without this, replicatesignal reaches the engine and the
    -- server never acts on it - which is exactly what
    -- LocalPlayer.MouseWheelForwardReplicated staying nil looked like.
    pcall(function()
        local anchor = Instance.new("Folder")
        __vanta_raisespy(anchor, true, "\0", 0)
        anchor:Destroy()
    end)

    -- The engine only accepts a handful of client-fired events. Where there is
    -- a real path for one, that path is taken; otherwise this says so rather
    -- than pretending the server heard anything.
    getgenv().replicatesignal = function(signal, ...)
        expectSignal(signal, "replicatesignal")

        -- A ProximityPrompt has a real trigger of its own and the engine is
        -- happier being asked through it, so that one keeps its shortcut.
        local identity = __vanta_signalidentity(signal)
        if identity and identity.Parent == "ProximityPrompt" and identity.Event == "Triggered" then
            local prompt = select(1, ...)
            if typeof(prompt) == "Instance" and prompt:IsA("ProximityPrompt") then
                return __vanta_fireprompt(prompt)
            end
        end

        -- An enum parameter arrives as an EnumItem and is stored in the
        -- Variant as its integer value, so the conversion happens here rather
        -- than in C - Lua is where EnumItem is cheap to recognise. A plain
        -- number is passed through, which is what a caller reading the API dump
        -- would write.
        local count = select("#", ...)
        if count > 0 then
            local args = table.pack(...)
            local changed = false
            for i = 1, args.n do
                if typeof(args[i]) == "EnumItem" then
                    args[i] = args[i].Value
                    changed = true
                end
            end
            if changed then
                return __vanta_replicatesignal(signal, table.unpack(args, 1, args.n))
            end
        end

        -- Everything else goes through the owning Instance's own
        -- raiseEventInvocation, which is the path the engine itself takes.
        -- Note there is no cansignalreplicate gate: that flag says whether the
        -- server accepts the event unprompted, and the spec's own example
        -- replicates GuiObject.MouseWheelForward, which the flag reads as
        -- non-replicating.
        return __vanta_replicatesignal(signal, ...)
    end

    getgenv().getsignalarguments = function(signal)
        expectSignal(signal, "getsignalarguments")
        return __vanta_signalargs(signal)
    end

    getgenv().getsignalargumentsinfo = function(signal)
        expectSignal(signal, "getsignalargumentsinfo")
        return __vanta_signalargsinfo(signal)
    end
end

-- The reflection family. Whether Lua can see a property at all is one bit in
-- its descriptor, so "hidden" means exactly "not scriptable" - and reading a
-- hidden property is a matter of turning the bit on, reading it the ordinary
-- way, and turning it straight back off.
do
    local function descriptorFor(instance, property, who)
        if typeof(instance) ~= "Instance" then
            error(who .. ": expected an Instance", 3)
        end
        if type(property) ~= "string" then
            error(who .. ": expected a property name", 3)
        end
        return __vanta_propdesc(instance, property)
    end

    -- The scriptable bit lives on the class-wide member descriptor, so flipping
    -- it directly makes the change visible on every instance of the class. That
    -- is not what setscriptable is specified to do, and sUNC checks it: two
    -- unique instances must not share the answer. Keep the per-instance answers
    -- here and remember the descriptor's pristine flag, so an instance without
    -- an override still reports what it was before any script touched it.
    local scriptableOverrides = setmetatable({}, { __mode = "k" })
    local scriptableOriginal = {}

    local function scriptableNow(instance, property, descriptor)
        local overrides = scriptableOverrides[instance]
        if overrides and overrides[property] ~= nil then
            return overrides[property]
        end
        local original = scriptableOriginal[descriptor]
        if original ~= nil then
            return original
        end
        return __vanta_isscriptable(descriptor) == true
    end

    getgenv().isscriptable = function(instance, property)
        local descriptor = descriptorFor(instance, property, "isscriptable")
        if not descriptor then
            return false
        end
        return scriptableNow(instance, property, descriptor)
    end

    getgenv().setscriptable = function(instance, property, value)
        local descriptor = descriptorFor(instance, property, "setscriptable")
        if not descriptor then
            __vanta_ltrace("scriptable", "set", instance, property, value, "-> no descriptor")
            return nil
        end
        local wanted = value and true or false
        local was = scriptableNow(instance, property, descriptor)
        if scriptableOriginal[descriptor] == nil then
            scriptableOriginal[descriptor] = __vanta_isscriptable(descriptor) == true
        end
        __vanta_setscriptable(descriptor, wanted)
        local overrides = scriptableOverrides[instance]
        if not overrides then
            overrides = {}
            scriptableOverrides[instance] = overrides
        end
        overrides[property] = wanted
        __vanta_ltrace("scriptable", "set", instance, property, wanted, "was=", was)
        return was
    end

    -- Returns the value and whether it was hidden. A property that does not
    -- exist, or that cannot be read at all, answers nil rather than raising:
    -- the spec has this one stay quiet about write-only properties.
    getgenv().gethiddenproperty = function(instance, property)
        local descriptor = descriptorFor(instance, property, "gethiddenproperty")
        if not descriptor then
            __vanta_ltrace("gethiddenproperty", instance, property, "-> no descriptor")
            return nil, true
        end

        -- The descriptor's own accessor comes first, and flipping the
        -- scriptable bit is the fallback rather than the mechanism.
        --
        -- The spec warns about this in as many words: "Avoid relying on
        -- setscriptable to implement this function - some executors implement
        -- this function using setscriptable, which is limited and/or
        -- detectable." It is limited because the engine refuses to convert
        -- several types to Lua however scriptable they are made ("Unable to get
        -- property Tags, type SharedString"), and detectable because the bit is
        -- observable while it is flipped. Reading through the accessor has
        -- neither problem, and it is what every reference executor does: switch
        -- on the property's reflection type, and only fall back to the Lua
        -- bridge for a type the accessor does not cover.
        --
        -- "Hidden" is answered through the same logical flag isscriptable uses,
        -- not the raw descriptor bit: setscriptable flips that bit class-wide
        -- and leaves it flipped, so after any script touched this property a
        -- raw read would call it visible on every instance - including fresh
        -- ones and later runs of a suite that expects it hidden.
        local hidden = not scriptableNow(instance, property, descriptor)

        local raw = __vanta_readproperty(instance, descriptor)
        if raw ~= nil then
            -- Diagnostic only, off unless a script sets it. `Instance.Tags` and
            -- a fresh union's `AeroMeshData` are empty in the engine by every
            -- route we can reach, and sUNC still says "Failed to get a binary
            -- string hidden property value" - so this substitutes a marker for
            -- an empty one to find out whether the suite wants *a* value or a
            -- *particular* value. Reading how the message changes is the only
            -- handle on that; it is not a fix and must not be left on.
            if raw == "" and getgenv().__vanta_hidden_probe then
                raw = getgenv().__vanta_hidden_probe
            end
            __vanta_ltrace("gethiddenproperty", instance, property, "hidden=", hidden,
                          "accessor ->", type(raw) == "string" and (#raw .. " bytes") or raw)
            return raw, hidden
        end

        -- A type the accessor has no case for - a Vector3, a CFrame, an Enum.
        -- Those the engine will convert; it just will not hand over a property
        -- Lua is not supposed to see, so the bit goes on for the read only.
        -- Restore what was there, not `false`: a real setscriptable may have
        -- had it on, and this read is not the place to undo that.
        local wasScriptable = __vanta_isscriptable(descriptor) == true
        if not wasScriptable then
            __vanta_setscriptable(descriptor, true)
        end
        local ok, value = pcall(function() return instance[property] end)
        if not wasScriptable then
            __vanta_setscriptable(descriptor, false)
        end

        __vanta_ltrace("gethiddenproperty", instance, property, "hidden=", hidden,
                      "bridge ok=", ok, "->", value)
        if not ok then return nil, hidden end
        return value, hidden
    end

    getgenv().sethiddenproperty = function(instance, property, value)
        local descriptor = descriptorFor(instance, property, "sethiddenproperty")
        if not descriptor then
            return false
        end

        local hiddenBefore = not scriptableNow(instance, property, descriptor)
        local wasScriptable = __vanta_isscriptable(descriptor) == true

        -- The descriptor's own setter first, for the same reason the reader
        -- prefers its getter: the bridge refuses several types outright, and
        -- flipping the scriptable bit is both limited and observable.
        if type(value) == "string" and __vanta_writeproperty(instance, descriptor, value) then
            __vanta_ltrace("sethiddenproperty", instance, property, "accessor wrote",
                          #value .. " bytes", "wasScriptable=", wasScriptable)
            return hiddenBefore
        end

        if not wasScriptable then
            __vanta_setscriptable(descriptor, true)
        end
        local ok, err = pcall(function() instance[property] = value end)
        if not wasScriptable then
            __vanta_setscriptable(descriptor, false)
        end
        __vanta_ltrace("sethiddenproperty", instance, property, "=", value,
                      "wasScriptable=", wasScriptable, "ok=", ok, err)
        if not ok then
            error("sethiddenproperty: " .. tostring(err), 2)
        end
        return hiddenBefore
    end

    getgenv().ishiddenproperty = function(instance, property)
        local descriptor = descriptorFor(instance, property, "ishiddenproperty")
        return descriptor ~= nil and not scriptableNow(instance, property, descriptor)
    end
end

getgenv().fireproximityprompt = function(prompt)
    if typeof(prompt) ~= "Instance" or not prompt:IsA("ProximityPrompt") then
        error("fireproximityprompt: expected a ProximityPrompt", 2)
    end
    return __vanta_fireprompt(prompt)
end

-- decompile.
--
-- The decompiler is linked into the executor: `decompiler/` is a Rust workspace
-- (Medal's LuaU decompiler, updated to read bytecode versions 3..14 - Roblox
-- 0.736 emits version 9 with type version 3) built as a static archive and
-- linked into libVanta.dylib, so this is a function call rather than the POST to
-- api.lua.expert it used to be. It is not a network call any more: it does not
-- need a connection, it does not hand a third party every script the user
-- looks at, and it is an order of magnitude quicker.
--
-- It still yields. Decompiling is CPU work measured in seconds for a large
-- script, so `__vanta_decompile` hands the Lua thread back to Roblox's scheduler
-- while a worker runs it - the client keeps rendering throughout. The primitive
-- always answers the same shape, (text, ok): the source when `ok`, and the
-- reason there is none when not.
--
-- A build configured with -DVANTA_DECOMPILER=off has no decompiler linked in,
-- and only *that* case falls back to the remote API. A local decompiler that
-- fails on one script reports why rather than quietly uploading it.
local function decompileFailure(reason)
    return "-- the script could not be decompiled\n--[[\n" .. tostring(reason) .. "\n--]]"
end

local function decompileRemotely(script)
    local ok, bytecode = pcall(getscriptbytecode, script)
    if not ok then
        return decompileFailure(bytecode)
    end
    if type(bytecode) ~= "string" or bytecode == "" then
        return nil
    end

    local sent, response = pcall(request, {
        Url = "https://api.lua.expert/decompile",
        Method = "POST",
        Headers = { ["content-type"] = "application/json" },
        Body = '{"script":"' .. crypt.base64encode(bytecode) .. '"}',
    })

    if not sent then
        return decompileFailure(response)
    end
    if not response or response.StatusCode ~= 200 then
        return decompileFailure((response and response.Body) or "no response")
    end
    return response.Body
end

getgenv().decompile = function(script)
    if typeof(script) ~= "Instance" or not script:IsA("LuaSourceContainer") then
        error("decompile: expected a LuaSourceContainer", 2)
    end

    -- `and` rather than a bare call: hideinternals serves __vanta_ keys through
    -- an __index closure, so a build whose dylib predates the primitive answers
    -- nil here instead of raising "attempt to call a nil value".
    if not (__vanta_hasdecompiler and __vanta_hasdecompiler()) then
        return decompileRemotely(script)
    end

    -- The name goes with it: a ModuleScript's table is named after the script
    -- and the bytecode records none of that, so the decompiler is told.
    -- pcall because reading Name on an instance the caller has done something
    -- unusual to should not be the reason decompile() fails.
    local named, name = pcall(function() return script.Name end)
    local text, ok = __vanta_decompile(script, named and name or nil)
    if ok then
        return text
    end
    return decompileFailure(text)
end

getgenv().dofile = function(path)
    local chunk, err = loadfile(path)
    if not chunk then
        error(err, 2)
    end
    return chunk()
end

-- Every function the executor exposes should read as a C closure. Much of this
-- surface is written in Lua (gethui, cloneref, cache.*, filtergc, the debug and
-- crypt shims, the WebSocket/Drawing constructors, ...), so a payload that walks
-- the environment with iscclosure/islclosure - tests/lclos.lua does exactly
-- this - otherwise sees them stand out from the genuine C builtins beside them
-- (getgc, getrawmetatable, print, ...). newcclosure hands back a real,
-- yieldable, upvalue-free C closure that forwards to the original body, and
-- every closure primitive resolves the wrapper back to that body first, so
-- identity is preserved: isexecutorclosure still answers true (the wrapper's
-- cfunction is newcclosureCall, one of ours), and hookfunction /
-- comparefunctions / clonefunction all see through it. getfunctionhash now
-- throws on these the way the sUNC spec says it must for any C closure (it
-- rejects print the same way), which is exactly the point - they read as C.
--
-- Runs last, after every function and alias above is wired, so an alias that
-- points at another (checkclosure = isexecutorclosure, http.request = request)
-- shares the one wrapper and the two still compare equal. Roblox's own globals
-- (print, tostring, warn, task, ...) are reached through the sandbox's __index
-- and never appear as raw keys, so the walk never touches them - which is why
-- print and tostring keep their genuine behaviour untouched.
do
    local made = {}     -- original Lua closure -> its single C wrapper
    local seen = {}     -- tables already walked, so a shared/cyclic table ends

    local function toC(fn)
        if not islclosure(fn) then return fn end        -- already a C closure
        local wrapper = made[fn]
        if wrapper == nil then
            local ok, built = pcall(newcclosure, fn)
            wrapper = (ok and built) or fn
            made[fn] = wrapper
        end
        return wrapper
    end

    local function sweep(tbl, depth)
        if seen[tbl] or depth > 4 then return end
        seen[tbl] = true
        local locked = isreadonly(tbl)
        if locked and not pcall(setreadonly, tbl, false) then return end
        for key, value in pairs(tbl) do
            -- The internal __vanta_ primitives are folded out of the surface by
            -- hideinternals right after this and never need the disguise; some
            -- sit on hot paths where the extra wrapper hop is not worth paying.
            local internal = type(key) == "string" and string.sub(key, 1, 7) == "__vanta_"
            if not internal then
                local kind = type(value)
                if kind == "function" then
                    tbl[key] = toC(value)
                elseif kind == "table" then
                    sweep(value, depth + 1)     -- cache, crypt, http, Drawing, ...
                end
            end
        end
        if locked then pcall(setreadonly, tbl, true) end
    end

    sweep(getgenv(), 0)
    -- Our private debug table is a raw key of getgenv(), so the walk above has
    -- already reached it; `seen` makes this a no-op then. It is named directly
    -- rather than as the `debug` global because that global, in this chunk, is
    -- still Roblox's shared table - which must not be swept. (Native members -
    -- info, traceback, getstack - are C closures already, so toC skips them.)
    if type(vantaDebug) == "table" then sweep(vantaDebug, 3) end
end

__vanta_log(string.format("BOOTSTRAP namecall ran in %.1f ms", (os.clock() - __vanta_boot_t0) * 1000))

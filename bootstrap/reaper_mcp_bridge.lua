--[[ reaper_mcp_bridge.lua ------------------------------------------------------------------------
  Zero-install FALLBACK transport for reaper_mcp.

  Purpose: let users try a reduced MCP toolset BEFORE installing the native extension, and give the
  test suite an independent reference oracle. This is the "EXTSTATE relay over the built-in web
  server" pattern: the external MCP process writes a request into ext state via
  REAPER's built-in Web Remote HTTP API; this defer script polls that state on the main thread,
  executes the ReaScript, and writes the response back.

  It is intentionally slower and narrower than the native path (bounded by the ~30 Hz defer tick and
  by what ReaScript exposes). When the native extension is present, the MCP client prefers it.

  Protocol (ExtState section "reaper_mcp"):
    req  (set by the external server): {"seq":N,"tool":"name","arguments":{...}}
    resp (set by us):                  {"seq":N,"result":{...}}  or  {"seq":N,"error":"msg"}
    req_seq (set by us): last-handled seq, so we never re-run a request.

  The handler set mirrors the native core tools (tools_core.cpp) 1:1 so CI can cross-check the two
  implementations tool-for-tool.

  INSTALL:
    1. Actions > Show action list > New action > Load ReaScript... > select this file. Run it once
       (it re-arms via reaper.defer). Optionally add it to a startup action.
    2. Enable a Web browser interface in Preferences > Control/OSC/web (note the port).
--------------------------------------------------------------------------------------------------]]

local SECTION  = "reaper_mcp"
local POLL_KEY = "req"
local RESP_KEY = "resp"
local SEEN_KEY = "req_seq"

-- =================================================================================================
-- Minimal JSON codec (self-contained; no external deps). Sufficient for MCP tool payloads.
-- =================================================================================================
local json = {}

function json.encode(v)
  local t = type(v)
  if v == nil then
    return "null"
  elseif t == "boolean" then
    return v and "true" or "false"
  elseif t == "number" then
    if v ~= v or v == math.huge or v == -math.huge then return "0" end
    if math.type and math.type(v) == "integer" then return string.format("%d", v) end
    return string.format("%.14g", v)
  elseif t == "string" then
    local esc = v:gsub('[%z\1-\31\\"]', function(c)
      local m = {['"']='\\"', ['\\']='\\\\', ['\b']='\\b', ['\f']='\\f',
                 ['\n']='\\n', ['\r']='\\r', ['\t']='\\t'}
      return m[c] or string.format('\\u%04x', string.byte(c))
    end)
    return '"' .. esc .. '"'
  elseif t == "table" then
    local n = 0
    for _ in pairs(v) do n = n + 1 end
    if n == 0 then return "{}" end
    local isArray = true
    for k in pairs(v) do
      if type(k) ~= "number" then isArray = false break end
    end
    if isArray and n == #v then
      local parts = {}
      for i = 1, #v do parts[i] = json.encode(v[i]) end
      return "[" .. table.concat(parts, ",") .. "]"
    end
    local parts = {}
    for k, val in pairs(v) do
      parts[#parts + 1] = json.encode(tostring(k)) .. ":" .. json.encode(val)
    end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  return "null"
end

function json.decode(s)
  if type(s) ~= "string" then return nil, "not a string" end
  local pos = 1
  local parseValue

  local function skipWs()
    local a = s:find("[^ \t\r\n]", pos)
    pos = a or (#s + 1)
  end
  local function parseString()
    pos = pos + 1  -- opening quote
    local buf = {}
    while pos <= #s do
      local c = s:sub(pos, pos)
      if c == '"' then
        pos = pos + 1
        return table.concat(buf)
      elseif c == '\\' then
        local nx = s:sub(pos + 1, pos + 1)
        if nx == 'u' then
          local code = tonumber(s:sub(pos + 2, pos + 5), 16) or 0
          buf[#buf + 1] = (utf8 and utf8.char or string.char)(code)
          pos = pos + 6
        else
          local m = {['"']='"', ['\\']='\\', ['/']='/', b='\b', f='\f', n='\n', r='\r', t='\t'}
          buf[#buf + 1] = m[nx] or nx
          pos = pos + 2
        end
      else
        buf[#buf + 1] = c
        pos = pos + 1
      end
    end
    error("unterminated string")
  end
  local function parseNumber()
    local a, b = s:find("^%-?%d+%.?%d*[eE]?[+-]?%d*", pos)
    if not a then error("bad number at " .. pos) end
    local num = tonumber(s:sub(a, b))
    pos = b + 1
    return num
  end
  parseValue = function()
    skipWs()
    local c = s:sub(pos, pos)
    if c == '{' then
      pos = pos + 1
      local obj = {}
      skipWs()
      if s:sub(pos, pos) == '}' then pos = pos + 1 return obj end
      while true do
        skipWs()
        local key = parseString()
        skipWs()
        if s:sub(pos, pos) ~= ':' then error("expected ':'") end
        pos = pos + 1
        obj[key] = parseValue()
        skipWs()
        local d = s:sub(pos, pos)
        pos = pos + 1
        if d == '}' then return obj elseif d ~= ',' then error("expected ',' or '}'") end
      end
    elseif c == '[' then
      pos = pos + 1
      local arr, i = {}, 1
      skipWs()
      if s:sub(pos, pos) == ']' then pos = pos + 1 return arr end
      while true do
        arr[i] = parseValue()
        i = i + 1
        skipWs()
        local d = s:sub(pos, pos)
        pos = pos + 1
        if d == ']' then return arr elseif d ~= ',' then error("expected ',' or ']'") end
      end
    elseif c == '"' then
      return parseString()
    elseif c == 't' then pos = pos + 4 return true
    elseif c == 'f' then pos = pos + 5 return false
    elseif c == 'n' then pos = pos + 4 return nil
    else return parseNumber() end
  end

  local ok, res = pcall(parseValue)
  if ok then return res end
  return nil, res
end

-- =================================================================================================
-- Helpers mirroring tools_core.cpp semantics.
-- =================================================================================================
local function db_to_gain(db) return 10 ^ (db / 20) end
local function gain_to_db(g) return g > 0 and 20 * math.log(g, 10) or -150 end

local function require_track(idx)
  local t = reaper.GetTrack(0, idx)
  if not t then error("track index out of range: " .. tostring(idx)) end
  return t
end
local function track_name(t)
  local _, name = reaper.GetSetMediaTrackInfo_String(t, "P_NAME", "", false)
  return name
end
local function track_guid(t)
  return reaper.guidToString(reaper.GetTrackGUID(t), "")
end
local function play_state_str(st)
  if st & 4 == 4 then return "recording" end
  if st & 1 == 1 then return "playing" end
  if st & 2 == 2 then return "paused" end
  return "stopped"
end

-- =================================================================================================
-- Tool handlers (args = decoded "arguments" table). Return a Lua table (the structuredContent).
-- =================================================================================================
local handlers = {}

handlers["transport.get_state"] = function()
  local st = reaper.GetPlayState()
  return {
    playState = play_state_str(st),
    cursorSec = reaper.GetCursorPosition(),
    playPositionSec = reaper.GetPlayPosition(),
    tempo = reaper.Master_GetTempo(),
    loop = reaper.GetSetRepeat(-1) > 0,
  }
end

handlers["transport.set"] = function(a)
  local action = a.action
  if action == "play" then reaper.CSurf_OnPlay()
  elseif action == "stop" then reaper.CSurf_OnStop()
  elseif action == "record" then reaper.CSurf_OnRecord()
  elseif action == "pause" then reaper.Main_OnCommand(1008, 0)
  elseif action == "goto" then reaper.SetEditCurPos(tonumber(a.sec) or 0, true, false)
  else error("unknown action: " .. tostring(action)) end
  return { ok = true, action = action }
end

handlers["project.get_summary"] = function()
  local _, name = reaper.GetSetProjectInfo_String(0, "PROJECT_NAME", "", false)
  if name == "" then name = "Untitled" end
  return {
    name = name,
    sampleRate = math.floor(reaper.GetSetProjectInfo(0, "PROJECT_SRATE", 0, false)),
    trackCount = reaper.CountTracks(0),
    lengthSec = reaper.GetProjectLength(0),
  }
end

handlers["track.add"] = function(a)
  local idx = tonumber(a.index) or reaper.CountTracks(0)
  reaper.Undo_BeginBlock2(0)
  reaper.InsertTrackAtIndex(idx, true)
  reaper.TrackList_AdjustWindows(false)
  local t = reaper.GetTrack(0, idx)
  if a.name and t then reaper.GetSetMediaTrackInfo_String(t, "P_NAME", tostring(a.name), true) end
  reaper.Undo_EndBlock2(0, "MCP: add track", -1)
  return { trackIndex = idx, guid = t and track_guid(t) or "" }
end

handlers["track.set_fader"] = function(a)
  local t = require_track(tonumber(a.track))
  reaper.Undo_BeginBlock2(0)
  if a.db ~= nil then reaper.SetMediaTrackInfo_Value(t, "D_VOL", db_to_gain(tonumber(a.db))) end
  if a.pan ~= nil then
    local p = tonumber(a.pan)
    if p < -1 then p = -1 elseif p > 1 then p = 1 end
    reaper.SetMediaTrackInfo_Value(t, "D_PAN", p)
  end
  reaper.Undo_EndBlock2(0, "MCP: set track fader", -1)
  return { ok = true, db = gain_to_db(reaper.GetMediaTrackInfo_Value(t, "D_VOL")),
           pan = reaper.GetMediaTrackInfo_Value(t, "D_PAN") }
end

handlers["track.list"] = function()
  local n = reaper.CountTracks(0)
  local tracks = {}
  for i = 0, n - 1 do
    local t = reaper.GetTrack(0, i)
    tracks[#tracks + 1] = {
      index = i,
      name = track_name(t),
      guid = track_guid(t),
      volDb = gain_to_db(reaper.GetMediaTrackInfo_Value(t, "D_VOL")),
      pan = reaper.GetMediaTrackInfo_Value(t, "D_PAN"),
      channels = math.floor(reaper.GetMediaTrackInfo_Value(t, "I_NCHAN")),
      muted = reaper.GetMediaTrackInfo_Value(t, "B_MUTE") > 0.5,
      soloed = reaper.GetMediaTrackInfo_Value(t, "I_SOLO") > 0.5,
    }
  end
  return { trackCount = n, tracks = tracks }
end

handlers["track.select"] = function(a)
  local t = require_track(tonumber(a.track))
  local exclusive = a.exclusive
  if exclusive == nil then exclusive = true end
  if exclusive then reaper.SetOnlyTrackSelected(t) else reaper.SetTrackSelected(t, true) end
  return { ok = true }
end

handlers["track.remove"] = function(a)
  local idx = tonumber(a.track)
  local t = require_track(idx)
  reaper.Undo_BeginBlock2(0)
  reaper.DeleteTrack(t)
  reaper.TrackList_AdjustWindows(false)
  reaper.Undo_EndBlock2(0, "MCP: remove track", -1)
  return { ok = true, removedIndex = idx }
end

handlers["track.get_name"] = function(a)
  return { name = track_name(require_track(tonumber(a.track))) }
end

handlers["track.set_name"] = function(a)
  local t = require_track(tonumber(a.track))
  reaper.Undo_BeginBlock2(0)
  reaper.GetSetMediaTrackInfo_String(t, "P_NAME", tostring(a.name), true)
  reaper.Undo_EndBlock2(0, "MCP: rename track", -1)
  return { ok = true, name = tostring(a.name) }
end

-- =================================================================================================
-- Request pump.
-- =================================================================================================
local function get_ext(k) return reaper.GetExtState(SECTION, k) end
local function set_ext(k, v) reaper.SetExtState(SECTION, k, v, false) end

local function handle_request(raw)
  local req, err = json.decode(raw)
  if not req then
    set_ext(RESP_KEY, json.encode({ seq = 0, error = "bad request json: " .. tostring(err) }))
    return
  end
  local seq = req.seq or 0
  local tool = req.tool
  local h = handlers[tool]
  local resp
  if not h then
    resp = { seq = seq, error = "unknown tool: " .. tostring(tool) }
  else
    local ok, result = pcall(h, req.arguments or {})
    if ok then resp = { seq = seq, result = result }
    else resp = { seq = seq, error = tostring(result) } end
  end
  set_ext(RESP_KEY, json.encode(resp))
  set_ext(SEEN_KEY, tostring(seq))
end

local function loop()
  local raw = get_ext(POLL_KEY)
  if raw ~= "" then
    local seq = raw:match('"seq"%s*:%s*(%d+)')
    if seq and seq ~= get_ext(SEEN_KEY) then
      handle_request(raw)
    end
  end
  reaper.defer(loop)  -- re-arm on the next main-thread tick (~30 Hz)
end

-- Expose the codec for the conformance harness / unit self-test (harmless in REAPER).
if not reaper then _G.reaper_mcp_json = json end

if reaper then
  reaper.atexit(function() set_ext(RESP_KEY, "") end)
  reaper.ShowConsoleMsg("reaper_mcp bridge (fallback) running. Native extension preferred when present.\n")
  loop()
end

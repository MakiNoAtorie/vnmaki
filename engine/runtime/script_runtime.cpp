#include "engine/runtime/script_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <sol/sol.hpp>

#include "engine/core/byte_stream.hpp"
#include "engine/runtime/default_screens.hpp"
#include "engine/text/markup.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

// The script-facing API, in Lua so argument errors and "unknown label" errors
// point at the caller's script and line (error level 2). It runs once per VM
// with the engine primitives as arguments; they never become globals, so
// scripts reach the engine only through the functions defined here.
// Two adjacent raw literals: MSVC limits one literal to 16380 bytes.
constexpr std::string_view kPrelude = R"lua(
local engine, dev_mode, random_seed = ...

local debug_getinfo, debug_sethook = debug.getinfo, debug.sethook
local error, type, tostring, select, rawset = error, type, tostring, select, rawset
local rawget = rawget

-- All game state. The engine keeps this table, snapshots it at label entry
-- and fills it again on replay; the RNG state is its __rng field.
local store = { __rng = random_seed }
local setmetatable, format, concat = setmetatable, string.format, table.concat

-- "script:line" of the code `level` calls up from the function calling this
-- (the same counting as error()'s level).
local function where(level)
  local info = debug_getinfo(level + 1, "Sl")
  if info == nil then
    return "?"
  end
  return info.short_src .. ":" .. tostring(info.currentline)
end

-- Raises "bad argument" at the caller of the API function that called this
-- (`depth` more levels up when called through a helper).
local function bad_argument(fname, index, expected, value, depth)
  error(format("bad argument #%d to '%s' (%s expected, got %s)", index, fname, expected,
               type(value)), 3 + (depth or 0))
end

-- Raises at the caller of the API function unless a label is running on
-- this coroutine (so not from top-level code or a coroutine the script made).
local function require_label(fname, depth)
  if not engine.in_label() then
    error(fname .. "() can only be called from a running label", 3 + (depth or 0))
  end
end

local function require_known_label(fname, name)
  if type(name) ~= "string" then
    bad_argument(fname, 1, "string", name, 1)
  end
  require_label(fname, 1)
  if not engine.has_label(name) then
    error(format("unknown label '%s'", name), 3)
  end
end

-- The instruction limit: a count hook on every thread that runs script code
-- spends one tick per hook_period instructions.
local hook_period, ticks_left = 0, 0

-- "script:line" of the innermost game-script code `level` calls up from the
-- function calling this, skipping engine code; nil if there is none.
local function script_location(level)
  while true do
    local info = debug_getinfo(level + 1, "Sl")
    if info == nil then
      return nil
    end
    if info.what ~= "C" and info.source ~= "=vnmaki" then
      return info.short_src .. ":" .. tostring(info.currentline)
    end
    level = level + 1
  end
end

local function spend_tick()
  ticks_left = ticks_left - 1
  if ticks_left < 0 then
    local location = script_location(3)
    local prefix = location and (location .. ": ") or ""
    error(prefix .. "script ran too long without yielding (instruction limit reached)", 0)
  end
end

local function on_count()
  spend_tick()
end

-- A label runs on a fresh coroutine, whose hook count starts over, so moving
-- between labels costs a tick too: labels that jump to each other forever
-- without yielding still run out.
local function transfer(kind, name)
  if hook_period > 0 then
    spend_tick()
  end
  engine.transfer(kind, name)
end

function label(name, fn)
  if type(name) ~= "string" then
    bad_argument("label", 1, "string", name)
  end
  if type(fn) ~= "function" then
    bad_argument("label", 2, "function", fn)
  end
  local info = debug_getinfo(fn, "S")
  local problem = engine.define_label(name, fn, where(2), info.source, info.linedefined,
                                      info.lastlinedefined)
  if problem ~= nil then
    error(problem, 2)
  end
end

function jump(name)
  require_known_label("jump", name)
  transfer("jump", name)
end

function call(name)
  require_known_label("call", name)
  transfer("call", name)
end

function ret()
  require_label("ret")
  transfer("ret", "")
end

-- Defined with NVL mode below: the page that goes with a say, narrate or menu (nil: not NVL).
-- Defined with the gallery below: notes that an image was shown, which can unlock it.
local nvl_page, note_shown

-- character { name =, color = "#rrggbb", voice_tag =, auto_multiplier =, kind = "adv" or "nvl" }:
-- a table `say` reads.
function character(definition)
  if type(definition) ~= "table" then
    bad_argument("character", 1, "table", definition)
  end
  if type(definition.name) ~= "string" then
    error(format("bad option 'name' to 'character' (string expected, got %s)",
                 type(definition.name)), 2)
  end
  local color = definition.color
  if color ~= nil and (type(color) ~= "string" or not color:match("^#%x%x%x%x%x%x$")) then
    error("bad option 'color' to 'character' (\"#rrggbb\" expected)", 2)
  end
  if definition.voice_tag ~= nil and type(definition.voice_tag) ~= "string" then
    error(format("bad option 'voice_tag' to 'character' (string expected, got %s)",
                 type(definition.voice_tag)), 2)
  end
  local multiplier = definition.auto_multiplier
  if multiplier ~= nil and (type(multiplier) ~= "number" or multiplier < 0) then
    error("bad option 'auto_multiplier' to 'character' (number of 0 or more expected)", 2)
  end
  local kind = definition.kind
  if kind ~= nil and kind ~= "adv" and kind ~= "nvl" then
    error("bad option 'kind' to 'character' (\"adv\" or \"nvl\" expected)", 2)
  end
  return { name = definition.name, color = color, voice_tag = definition.voice_tag,
           auto_multiplier = multiplier, kind = kind }
end

-- Raises a bad text tag at the caller of the API function that called this.
local function check_text(text)
  local problem = engine.check_text(text)
  if problem then
    error(problem, 3)
  end
end

function say(who, text)
  local speaker, color, voice_tag, multiplier, kind
  if type(who) == "string" then
    speaker = who
  elseif type(who) == "table" and type(who.name) == "string" then
    speaker = who.name
    if type(who.color) == "string" and who.color:match("^#%x%x%x%x%x%x$") then
      color = tonumber(who.color:sub(2), 16)
    end
    if type(who.voice_tag) == "string" then
      voice_tag = who.voice_tag
    end
    multiplier = who.auto_multiplier
    kind = who.kind
  elseif who ~= nil then
    bad_argument("say", 1, "string, character or nil", who)
  end
  if type(text) ~= "string" then
    bad_argument("say", 2, "string", text)
  end
  require_label("say")
  check_text(text)
  engine.stage_nvl(nvl_page(kind, speaker, text, color))
  if speaker == nil then
    engine.narrate(text)
  else
    engine.say(speaker, text, color, voice_tag or "", multiplier or 1)
  end
end

function narrate(text)
  if type(text) ~= "string" then
    bad_argument("narrate", 1, "string", text)
  end
  require_label("narrate")
  check_text(text)
  engine.stage_nvl(nvl_page(nil, nil, text))
  engine.narrate(text)
end

-- The function the engine calls with a link's id when the player activates a
-- {link=id} part of the text on screen. It runs outside the labels: it cannot
-- block, and the replay does not run it again, so it must not change `store`.
local link_handler
function on_link(handler)
  if type(handler) ~= "function" then
    bad_argument("on_link", 1, "function", handler)
  end
  link_handler = handler
end

-- Visuals. The engine primitives return why a call cannot be done, or nil.
-- Option keys these functions do not know are left for later features.
local function option(fname, options, key, expected)
  local value = options[key]
  if value ~= nil and type(value) ~= expected then
    error(format("bad option '%s' to '%s' (%s expected, got %s)", key, fname, expected,
                 type(value)), 3)
  end
  return value
end

local function options_argument(fname, options)
  if options == nil then
    return {}
  end
  if type(options) ~= "table" then
    bad_argument(fname, 2, "table or nil", options, 1)
  end
  return options
end

-- Defined with the layered sprites below: the attributes `show` gives a layered sprite.
local layered_attributes
-- Defined with the transitions below: the `with` option of scene, show and hide.
local begin_with, end_with

function scene(image, options)
  if type(image) ~= "string" then
    bad_argument("scene", 1, "string", image)
  end
  options = options_argument("scene", options)
  require_label("scene")
  local with = begin_with("scene", options, 3)
  local problem = engine.scene(image)
  if problem then
    error(problem, 2)
  end
  note_shown(image)
  engine.scene_command("scene " .. image)
  end_with(with)
end

function show(tag, options)
  if type(tag) ~= "string" then
    bad_argument("show", 1, "string", tag)
  end
  options = options_argument("show", options)
  local image = option("show", options, "image", "string")
  local at = option("show", options, "at", "string")
  local x = option("show", options, "x", "number")
  local y = option("show", options, "y", "number")
  local layer = option("show", options, "layer", "string")
  local movie = option("show", options, "movie", "string")
  local loop = option("show", options, "loop", "boolean")
  local attributes, attribute_text = layered_attributes(options, image or tag)
  require_label("show")
  local with = begin_with("show", options, 3)
  local problem = engine.show(tag, image or tag, at, x, y, layer, movie, loop, attributes)
  if problem then
    error(problem, 2)
  end
  note_shown(image or tag)
  local command = "show " .. tag
  if image then
    command = command .. " image " .. image
  end
  if at then
    command = command .. " at " .. at
  end
  if x then
    command = command .. format(" x %.14g", x)
  end
  if y then
    command = command .. format(" y %.14g", y)
  end
  if layer then
    command = command .. " layer " .. layer
  end
  if movie then
    command = command .. " movie " .. movie .. (loop and " loop" or "")
  end
  command = command .. engine.shader_options("show", tag, false, options)
  engine.scene_command(command .. attribute_text)
  end_with(with)
end

function hide(tag, options)
  if type(tag) ~= "string" then
    bad_argument("hide", 1, "string", tag)
  end
  options = options_argument("hide", options)
  require_label("hide")
  local with = begin_with("hide", options, 3)
  engine.hide(tag)
  engine.scene_command("hide " .. tag)
  end_with(with)
end

-- Audio. Times are in seconds.
function music(name, options)
  if type(name) ~= "string" then
    bad_argument("music", 1, "string", name)
  end
  options = options_argument("music", options)
  local channel = option("music", options, "channel", "string")
  local fade = option("music", options, "fade", "number")
  local loop_start = option("music", options, "loop_start", "number")
  local loop_end = option("music", options, "loop_end", "number")
  require_label("music")
  local problem = engine.music(name, channel, fade, loop_start, loop_end)
  if problem then
    error(problem, 2)
  end
  local command = "music " .. name
  if channel then
    command = command .. " channel " .. channel
  end
  if fade then
    command = command .. format(" fade %.14g", fade)
  end
  if loop_start then
    command = command .. format(" loop_start %.14g", loop_start)
  end
  if loop_end then
    command = command .. format(" loop_end %.14g", loop_end)
  end
  engine.audio_command(command)
end

function sound(name, options)
  if type(name) ~= "string" then
    bad_argument("sound", 1, "string", name)
  end
  options = options_argument("sound", options)
  local channel = option("sound", options, "channel", "string")
  require_label("sound")
  local problem = engine.sound(name, channel)
  if problem then
    error(problem, 2)
  end
  local command = "sound " .. name
  if channel then
    command = command .. " channel " .. channel
  end
  engine.audio_command(command)
end

function voice(name)
  if type(name) ~= "string" then
    bad_argument("voice", 1, "string", name)
  end
  require_label("voice")
  local problem = engine.voice(name)
  if problem then
    error(problem, 2)
  end
  engine.audio_command("voice " .. name)
end

function stop(channel, options)
  if type(channel) ~= "string" then
    bad_argument("stop", 1, "string", channel)
  end
  options = options_argument("stop", options)
  local fade = option("stop", options, "fade", "number")
  require_label("stop")
  local problem = engine.stop(channel, fade)
  if problem then
    error(problem, 2)
  end
  local command = "stop " .. channel
  if fade then
    command = command .. format(" fade %.14g", fade)
  end
  engine.audio_command(command)
end

function volume(channel, value)
  if type(channel) ~= "string" then
    bad_argument("volume", 1, "string", channel)
  end
  if type(value) ~= "number" then
    bad_argument("volume", 2, "number", value)
  end
  require_label("volume")
  local problem = engine.volume(channel, value)
  if problem then
    error(problem, 2)
  end
  engine.audio_command(format("volume %s %.14g", channel, value))
end

-- The minimal menu: a list of option texts, and `fixed = true` to stop rollback from choosing
-- again. Returns the number of the chosen option, from 1.
function menu(options)
  if type(options) ~= "table" then
    bad_argument("menu", 1, "table", options)
  end
  local texts = {}
  for i = 1, #options do
    local option = options[i]
    if type(option) ~= "string" then
      error(format("bad option #%d to 'menu' (string expected, got %s)", i, type(option)), 2)
    end
    texts[i] = option
  end
  if #texts == 0 then
    error("menu() needs at least one option", 2)
  end
  local fixed = options.fixed
  if fixed ~= nil and type(fixed) ~= "boolean" then
    error(format("bad option 'fixed' to 'menu' (boolean expected, got %s)", type(fixed)), 2)
  end
  require_label("menu")
  engine.stage_nvl(nvl_page())
  return engine.menu(texts, fixed == true)
end
)lua" R"lua(
-- Styles and themes (docs/SPEC.md section 15). A style is a named table of node options: font,
-- size, color, background, padding, gap, margin, width, height, grow, align, justify, fill,
-- border = { width, color }, focus = { background, color, border } and hover = { ... }; `extends`
-- names a style whose options come first. A node takes `style = "name"`; its own options win.
-- The styles that the engine's default screens declare are the defaults of every theme.
local styles, default_styles = {}, {}
local plain_style_options = {
  font = true, size = true, color = true, padding = true, gap = true, margin = true, width = true,
  height = true, grow = true, align = true, justify = true, fill = true,
}
local border_style_options = { width = "border_width", color = "border_color" }
local state_style_options = { background = "bg", color = "color", border = "border" }
-- Only nodes that take the focus have focus options.
local focus_node_options = { focus_bg = true, focus_color = true, focus_border = true }
local focus_node_kinds = { button = true, toggle = true, slider = true, input = true }

local function style_group(group, value, names, prefix)
  if type(value) ~= "table" then
    error(format("bad style option '%s' (table expected, got %s)", group, type(value)), 4)
  end
  local props = {}
  for key, item in pairs(value) do
    if names[key] == nil then
      error(format("unknown option '%s' in style option '%s'", tostring(key), group), 4)
    end
    props[prefix .. names[key]] = item
  end
  return props
end

-- The node options that the style table `definition` stands for.
local function flatten_style(definition)
  local props = {}
  local function add(group)
    for key, value in pairs(group) do
      props[key] = value
    end
  end
  for key, value in pairs(definition) do
    if plain_style_options[key] then
      props[key] = value
    elseif key == "background" then
      props.bg = value
    elseif key == "border" then
      add(style_group(key, value, border_style_options, ""))
    elseif key == "focus" or key == "hover" then
      add(style_group(key, value, state_style_options, key .. "_"))
    elseif key ~= "extends" then
      error(format("unknown style option '%s'", tostring(key)), 3)
    end
  end
  return props
end

function style(name, definition)
  if type(name) ~= "string" or name == "" then
    bad_argument("style", 1, "string", name)
  end
  if type(definition) ~= "table" then
    bad_argument("style", 2, "table", definition)
  end
  local extends = definition.extends
  if extends ~= nil and type(extends) ~= "string" then
    error(format("bad style option 'extends' (string expected, got %s)", type(extends)), 2)
  end
  local entry = { props = flatten_style(definition), extends = extends }
  styles[name] = entry
  if debug_getinfo(2, "S").source == "=engine/screens.lua" then
    default_styles[name] = entry
  end
end

-- The options of style `name` for a node of `kind`: the styles it extends first, then its own.
local function resolve_style(kind, name)
  if type(name) ~= "string" then
    error(format("bad option 'style' to '%s' (string expected, got %s)", kind, type(name)), 3)
  end
  local chain, current = {}, name
  while current ~= nil do
    local entry = styles[current]
    if entry == nil then
      error(format("unknown style '%s'", current), 3)
    end
    if #chain >= 16 then
      error(format("style '%s' extends itself", name), 3)
    end
    chain[#chain + 1] = entry
    current = entry.extends
  end
  local props = {}
  for i = #chain, 1, -1 do
    for key, value in pairs(chain[i].props) do
      if focus_node_kinds[kind] or not focus_node_options[key] then
        props[key] = value
      end
    end
  end
  return props
end

-- theme("name") makes the styles of the engine's screens, then runs styles/<name>.lua, which
-- declares styles with style(). Screens built from now on look as it says. It is not in
-- snapshots: call it before the story starts, or in the label that changes the look.
function theme(name)
  if type(name) ~= "string" or not name:find("^[%w_%-]+$") then
    bad_argument("theme", 1, "name of letters, digits, _ and -", name)
  end
  local chunk = engine.load_theme(name)
  if type(chunk) ~= "function" then
    error(chunk, 2)
  end
  local before = styles
  styles = {}
  for key, entry in pairs(default_styles) do
    styles[key] = entry
  end
  local ok, problem = pcall(chunk)
  if not ok then
    styles = before
    error(problem, 0)
  end
end
)lua" R"lua(
-- Screens (docs/SPEC.md section 15). A node is a plain table the engine reads: kind, text (the
-- caption or image name), props (booleans, numbers and strings) and children.
local pairs, math_type = pairs, math.type
local node_takes_text = { text = true, button = true, image = true, toggle = true, input = true }
local function is_node(value)
  return type(value) == "table" and type(value.kind) == "string" and type(value.props) == "table"
         and type(value.children) == "table"
end

local function node_constructor(kind)
  return function(spec)
    if type(spec) == "string" and node_takes_text[kind] then
      spec = { spec }
    elseif type(spec) ~= "table" then
      bad_argument(kind, 1, "table", spec)
    end
    local node = { kind = kind, text = "", props = {}, children = {} }
    if spec.style ~= nil then
      node.props = resolve_style(kind, spec.style)  -- the node's own options override these
    end
    local has_text = false
    for i = 1, #spec do
      local item = spec[i]
      if type(item) == "string" and node_takes_text[kind] and not has_text then
        node.text, has_text = item, true
      elseif is_node(item) then
        node.children[#node.children + 1] = item
      else
        error(format("bad item #%d in '%s' (a node%s expected, got %s)", i, kind,
                     node_takes_text[kind] and " or one string" or "", type(item)), 2)
      end
    end
    for key, value in pairs(spec) do
      if key == "bind" and type(value) == "table" and value.bind_table ~= nil then
        node.props.bind_table, node.props.bind_key = value.bind_table, value.bind_key
      elseif key == "style" then
        -- applied above
      elseif type(key) == "string" then
        local value_type = type(value)
        if value_type ~= "boolean" and value_type ~= "number" and value_type ~= "string" then
          error(format("bad option '%s' to '%s' (boolean, number or string expected, got %s)", key,
                       kind, value_type), 2)
        end
        node.props[key] = value
      elseif math_type(key) ~= "integer" then
        error(format("bad key to '%s' (names and list positions only)", kind), 2)
      end
    end
    return node
  end
end
box, hbox, vbox = node_constructor("box"), node_constructor("hbox"), node_constructor("vbox")
text, image, button = node_constructor("text"), node_constructor("image"), node_constructor("button")
toggle, slider, bar = node_constructor("toggle"), node_constructor("slider"), node_constructor("bar")
grid, input = node_constructor("grid"), node_constructor("input")

local screens = {}
local function register_screen(name, definition)
  if type(definition) ~= "table" and type(definition) ~= "function" then
    bad_argument("screen", 2, "table or function", definition, 1)
  end
  screens[name] = definition
end

-- screen "name" { nodes... }, or screen("name", fn) where fn(args) returns the nodes.
function screen(name, definition)
  if type(name) ~= "string" then
    bad_argument("screen", 1, "string", name)
  end
  if definition ~= nil then
    register_screen(name, definition)
    return
  end
  return function(later)
    register_screen(name, later)
  end
end

-- The root node of screen `name` for `args`.
local function screen_root(fname, name, args)
  if type(name) ~= "string" then
    bad_argument(fname, 1, "string", name, 1)
  end
  if args ~= nil and type(args) ~= "table" then
    bad_argument(fname, 2, "table or nil", args, 1)
  end
  if fname ~= nil then
    require_label(fname, 1)
  end
  local root = screens[name]
  if root == nil then
    error(format("unknown screen '%s'", name), 3)
  end
  if type(root) == "function" then
    root = root(args or {})
  end
  if is_node(root) then
    return root
  end
  if type(root) == "table" and #root > 0 then
    return #root == 1 and is_node(root[1]) and root[1] or box(root)
  end
  error(format("screen '%s' gave no nodes", name), 3)
end

local function stage_screen(name, root)
  local problem = engine.stage_screen(name, root)
  if problem ~= nil then
    error(problem, 3)
  end
end

function show_screen(name, args)
  stage_screen(name, screen_root("show_screen", name, args))
  local problem = engine.show_screen(name)
  if problem ~= nil then
    error(problem, 2)
  end
end

function hide_screen(name)
  if type(name) ~= "string" then
    bad_argument("hide_screen", 1, "string", name)
  end
  require_label("hide_screen")
  engine.hide_screen(name)
end

-- Waits until the screen returns a value (a button's `returns` option) and returns it.
function call_screen(name, args)
  stage_screen(name, screen_root("call_screen", name, args))
  return engine.call_screen(name)
end
)lua" R"lua(
-- Tweens (docs/SPEC.md section 10). A tween item is a table { tag, option = value, ... }; a group
-- item is { sequence = { items } } or { parallel = { items } }. `level` is the error() level that
-- reaches the script from the function that holds it.
local tween_numbers = { "x", "y", "rotation", "alpha", "duration", "delay" }
local max_tween_depth = 16

local function typed(fname, options, key, expected, level)
  local value = options[key]
  if value ~= nil and type(value) ~= expected then
    error(format("bad option '%s' to '%s' (%s expected, got %s)", key, fname, expected,
                 type(value)), level + 1)
  end
  return value
end

local function number_table(fname, options, key, names, node, level)
  local fields = typed(fname, options, key, "table", level)
  if fields == nil then
    return
  end
  for i = 1, #names do
    local value = fields[names[i]]
    if value ~= nil and type(value) ~= "number" then
      error(format("bad field '%s' of option '%s' to '%s' (number expected, got %s)", names[i], key,
                   fname, type(value)), level)
    end
    node[key .. "_" .. names[i]] = value
  end
end

local function tween_node(fname, tag, options, level)
  if type(tag) ~= "string" or tag == "" then
    error(format("bad tween tag (a tag string expected, got %s)", type(tag)), level)
  end
  local node = { kind = "tween", tag = tag }
  for i = 1, #tween_numbers do
    node[tween_numbers[i]] = typed(fname, options, tween_numbers[i], "number", level)
  end
  node.ease = typed(fname, options, "ease", "string", level)
  node.loop = typed(fname, options, "loop", "boolean", level)
  node.yoyo = typed(fname, options, "yoyo", "boolean", level)
  node.uniforms = typed(fname, options, "uniforms", "table", level)
  number_table(fname, options, "anchor", { "x", "y" }, node, level + 1)
  number_table(fname, options, "crop", { "x", "y", "width", "height" }, node, level + 1)
  if type(options.scale) == "number" then
    node.scale_x, node.scale_y = options.scale, options.scale
  else
    number_table(fname, options, "scale", { "x", "y" }, node, level + 1)
  end
  local tint = typed(fname, options, "tint", "string", level)
  if tint ~= nil then
    if not tint:match("^#%x%x%x%x%x%x$") then
      error(format("bad option 'tint' to '%s' (\"#rrggbb\" expected)", fname), level)
    end
    node.tint = tonumber(tint:sub(2), 16)
  end
  if tag == "@camera" then
    engine.camera_options(fname, options, node, level)
  end
  return node
end

local tween_item
local function tween_group(kind, items, options, level, depth)
  if depth > max_tween_depth then
    error(format("groups nest more than %d levels deep", max_tween_depth), level)
  end
  if #items == 0 then
    error(kind .. "() needs at least one item", level)
  end
  local node = { kind = kind, children = {} }
  node.delay = typed(kind, options, "delay", "number", level)
  node.loop = typed(kind, options, "loop", "boolean", level)
  for i = 1, #items do
    node.children[i] = tween_item(kind, items[i], i, level + 1, depth + 1)
  end
  return node
end

function tween_item(fname, item, index, level, depth)
  if type(item) ~= "table" then
    error(format("bad item #%d in '%s' (a table expected, got %s)", index, fname, type(item)),
          level)
  end
  local kind = (item.sequence ~= nil and "sequence") or (item.parallel ~= nil and "parallel")
  if kind then
    if type(item[kind]) ~= "table" then
      error(format("bad item #%d in '%s' ('%s' must be a table of items)", index, fname, kind),
            level)
    end
    -- Not a tail call, which would drop this frame and shift the error levels.
    local group_node = tween_group(kind, item[kind], item, level + 1, depth)
    return group_node
  end
  local node = tween_node(fname, item[1], item, level + 1)
  return node
end

local function start_tween(node, command)
  local problem = engine.tween(node)
  if problem then
    error(problem, 3)
  end
  engine.scene_command(command)
end

function tween(tag, options)
  if type(tag) ~= "string" then
    bad_argument("tween", 1, "string", tag)
  end
  options = options_argument("tween", options)
  require_label("tween")
  start_tween(tween_node("tween", tag, options, 3), "tween " .. tag)
end

local function group_function(kind)
  return function(items)
    if type(items) ~= "table" then
      bad_argument(kind, 1, "table", items)
    end
    require_label(kind)
    start_tween(tween_group(kind, items, items, 3, 0), kind)
  end
end
sequence, parallel = group_function("sequence"), group_function("parallel")

-- Blocks until the tweens of `tag` end. It is a step even when none runs, so replays count the
-- same steps as live play.
function wait(tag)
  if type(tag) ~= "string" then
    bad_argument("wait", 1, "string", tag)
  end
  if tag == "" then
    error("wait() needs a tag (wait_all() waits for every tween)", 2)
  end
  require_label("wait")
  engine.wait(tag)
end

function wait_all()
  require_label("wait_all")
  engine.wait("")
end
)lua" R"lua(
-- Shaders (docs/SPEC.md section 9). shader(name, { "uniform", ... }) declares the game shader
-- shaders/<name>.frag.hlsl and the floats of its constant buffer. `show` and `layer` set a shader
-- and uniform values; `tween` changes the values.
local function is_identifier(value)
  return type(value) == "string" and value:match("^[%a_][%w_]*$") ~= nil
end

function shader(name, uniforms)
  if not is_identifier(name) then
    error("bad argument #1 to 'shader' (a name of letters, digits and _ expected)", 2)
  end
  if uniforms == nil then
    uniforms = {}
  elseif type(uniforms) ~= "table" then
    bad_argument("shader", 2, "table or nil", uniforms)
  end
  for i = 1, #uniforms do
    if not is_identifier(uniforms[i]) then
      error(format("bad uniform #%d to 'shader' (a name of letters, digits and _ expected)", i), 2)
    end
  end
  local problem = engine.declare_shader(name, uniforms)
  if problem then
    error(problem, 2)
  end
end

-- The `shader` and `uniforms` options of `show` (a displayable) and `layer` (a layer). Returns the
-- text that the scene command of the call gets. Errors reach the script that called `fname`.
function engine.shader_options(fname, target, is_layer, options)
  local shader_name, uniforms = options.shader, options.uniforms
  if not is_layer and shader_name == nil and uniforms == nil then
    return ""
  end
  if shader_name ~= nil and type(shader_name) ~= "string" then
    error(format("bad option 'shader' to '%s' (string expected, got %s)", fname,
                 type(shader_name)), 3)
  end
  if uniforms ~= nil and type(uniforms) ~= "table" then
    error(format("bad option 'uniforms' to '%s' (table expected, got %s)", fname, type(uniforms)),
          3)
  end
  local problem = engine.set_shader(target, is_layer, shader_name, uniforms)
  if problem then
    error(problem, 3)
  end
  return shader_name and (" shader " .. shader_name) or ""
end

function layer(name, options)
  if type(name) ~= "string" then
    bad_argument("layer", 1, "string", name)
  end
  options = options_argument("layer", options)
  require_label("layer")
  engine.layer_parallax(name, options)
  engine.scene_command("layer " .. name .. engine.shader_options("layer", name, true, options))
end
)lua" R"lua(
-- Transitions (docs/SPEC.md section 10). transition(name, options) mixes the frame from before
-- the changes of this step into the frame after them, and blocks like `wait` until it ends: the
-- script waits for the tag "@transition". The `with` option of scene, show and hide does the same
-- after its own change: with = "dissolve", or with = { "wipe_left", duration = 1 }.
-- The options are duration, color ("#rrggbb"), mask (an image name), softness and uniforms.
local function transition_request(fname, options, level)
  local spec = {}
  spec.duration = typed(fname, options, "duration", "number", level)
  spec.softness = typed(fname, options, "softness", "number", level)
  spec.mask = typed(fname, options, "mask", "string", level)
  spec.uniforms = typed(fname, options, "uniforms", "table", level)
  local color = typed(fname, options, "color", "string", level)
  if color ~= nil then
    if not color:match("^#%x%x%x%x%x%x$") then
      error(format("bad option 'color' to '%s' (\"#rrggbb\" expected)", fname), level)
    end
    spec.color = tonumber(color:sub(2), 16)
  end
  return spec
end

-- `level` is the error() level that reaches the script from this function.
local function start_transition(fname, name, options, level)
  local spec = transition_request(fname, options, level + 1)
  local problem = engine.transition(name, spec)
  if problem then
    error(problem, level)
  end
end

local function finish_transition(name)
  engine.scene_command("transition " .. name)
  engine.wait("@transition")
end

function transition(name, options)
  if type(name) ~= "string" then
    bad_argument("transition", 1, "string", name)
  end
  options = options_argument("transition", options)
  require_label("transition")
  start_transition("transition", name, options, 3)
  finish_transition(name)
end

-- Starts the transition of a `with` option before the change, so that a bad name changes nothing,
-- and returns its name. end_with(name) then blocks for it.
begin_with = function(fname, options, level)
  local with = options.with
  if with == nil then
    return nil
  end
  local name = type(with) == "table" and with[1] or with
  if type(name) ~= "string" then
    error(format("bad option 'with' to '%s' (a transition name, or a table { name, ... } expected)",
                 fname), level)
  end
  start_transition(fname, name, type(with) == "table" and with or {}, level + 1)
  return name
end
end_with = function(name)
  if name ~= nil then
    finish_transition(name)
  end
end
)lua" R"lua(
-- Camera (docs/SPEC.md sections 10 and 17). camera { x, y, zoom, rotation, duration, ease } moves
-- the view of the layers from the background to the overlay: it is a tween of the tag "@camera",
-- instant without a duration. shake { strength, duration } shakes it. layer(name, { parallax = f })
-- makes the pan of one layer f times the camera's. The prefs flag reduce_motion (persistent.prefs,
-- which a game or the default screens may not have) stops shakes and makes camera moves instant.
function engine.reduce_motion()
  local prefs = persistent.prefs
  return type(prefs) == "table" and prefs.reduce_motion ~= nil and prefs.reduce_motion ~= false
end

-- The options of a tween of "@camera" that other tags do not have. `level` is as for tween_node.
function engine.camera_options(fname, options, node, level)
  local zoom = typed(fname, options, "zoom", "number", level + 1)
  if zoom ~= nil then
    node.scale_x, node.scale_y = zoom, zoom
  end
  if engine.reduce_motion() then
    node.duration, node.delay, node.loop, node.yoyo = 0, 0, false, false
    node.instant = true
  end
end

-- The `parallax` option of `layer`. Errors reach the script that called `layer`.
function engine.layer_parallax(name, options)
  local parallax = options.parallax
  if parallax == nil then
    return
  end
  if type(parallax) ~= "number" then
    error(format("bad option 'parallax' to 'layer' (number expected, got %s)", type(parallax)), 3)
  end
  local problem = engine.set_parallax(name, parallax)
  if problem then
    error(problem, 3)
  end
end

function camera(options)
  if type(options) ~= "table" then
    bad_argument("camera", 1, "table", options)
  end
  require_label("camera")
  local node = tween_node("camera", "@camera", options, 3)
  node.duration = node.duration or 0
  node.instant = node.duration == 0 and (node.delay or 0) == 0
  start_tween(node, "camera")
end

function shake(options)
  if type(options) ~= "table" then
    bad_argument("shake", 1, "table", options)
  end
  require_label("shake")
  local strength = typed("shake", options, "strength", "number", 2)
  local duration = typed("shake", options, "duration", "number", 2)
  if strength == nil then
    error("shake() needs a strength", 2)
  end
  local problem = engine.shake(strength, duration or 0.5, engine.reduce_motion())
  if problem then
    error(problem, 2)
  end
  engine.scene_command("shake")
end
)lua" R"lua(
-- Layered sprites: layered(name, { order = { group, ... }, <group> = "image" or
-- { value = "image", ... }, default = { group = value }, crossfade = seconds }).
-- `show` of the name then takes a value for each group: show("eileen", { face = "smile" }).
local pairs, ipairs, sort = pairs, ipairs, table.sort
local layered_defs = {}
local show_options = { image = true, at = true, x = true, y = true, layer = true,
                       shader = true, uniforms = true, movie = true, loop = true, with = true }

-- `level` is the error() level of the caller of this function.
local function layered_option(key, expected, value, level)
  error(format("bad option '%s' to 'layered' (%s expected, got %s)", key, expected, type(value)),
        level + 1)
end

-- A group's spec as { name =, values = { value, image, ... } }; a single image has the value "".
local function layered_group(name, spec)
  local values = {}
  if type(spec) == "string" then
    values[1], values[2] = "", spec
  elseif type(spec) == "table" then
    local names = {}
    for value, image in pairs(spec) do
      if type(value) ~= "string" or value == "" or type(image) ~= "string" then
        error(format("bad option '%s' to 'layered' (a table of value = \"image\" expected)", name),
              3)
      end
      names[#names + 1] = value
    end
    if #names == 0 then
      error(format("bad option '%s' to 'layered' (it needs at least one value)", name), 3)
    end
    sort(names)
    for i = 1, #names do
      values[2 * i - 1], values[2 * i] = names[i], spec[names[i]]
    end
  else
    layered_option(name, "string or table", spec, 3)
  end
  return { name = name, values = values }
end

function layered(name, definition)
  if type(name) ~= "string" then
    bad_argument("layered", 1, "string", name)
  end
  if type(definition) ~= "table" then
    bad_argument("layered", 2, "table", definition)
  end
  local order = definition.order
  if type(order) ~= "table" or #order == 0 then
    error("bad option 'order' to 'layered' (a list of group names expected)", 2)
  end
  local request, listed = { groups = {}, default = {}, crossfade = definition.crossfade }, {}
  for i = 1, #order do
    local group = order[i]
    if type(group) ~= "string" or group == "" or listed[group] then
      error(format("bad option 'order' to 'layered' (item #%d is not a new group name)", i), 2)
    end
    listed[group] = true
    request.groups[i] = layered_group(group, definition[group])
  end
  for key in pairs(definition) do
    if not listed[key] and key ~= "order" and key ~= "default" and key ~= "crossfade" then
      error(format("bad option '%s' to 'layered' (not a group in 'order')", tostring(key)), 2)
    end
  end
  if request.crossfade ~= nil and type(request.crossfade) ~= "number" then
    layered_option("crossfade", "number", request.crossfade, 2)
  end
  local default = definition.default
  if default ~= nil and type(default) ~= "table" then
    layered_option("default", "table", default, 2)
  end
  for key in pairs(default or {}) do
    if not listed[key] then
      error(format("bad option 'default' to 'layered' ('%s' is not a group in 'order')",
                   tostring(key)), 2)
    end
  end
  for _, group in ipairs(order) do
    local value = default and default[group]
    if value ~= nil then
      if type(value) ~= "string" then
        layered_option("default", "table of strings", value, 2)
      end
      local n = #request.default
      request.default[n + 1], request.default[n + 2] = group, value
    end
  end
  local problem = engine.layered(name, request)
  if problem then
    error(problem, 2)
  end
  layered_defs[name] = { groups = listed, order = order }
end

-- Checks the group keys in the `show` options of a layered sprite and returns them as a flat
-- list and as the text of the transcript. Other images ignore keys that are not options.
layered_attributes = function(options, name)
  local def = layered_defs[name]
  if def == nil then
    return nil, ""
  end
  for key in pairs(options) do
    if not def.groups[key] and not show_options[key] then
      error(format("unknown group '%s' for '%s' (the groups are %s)", tostring(key), name,
                   concat(def.order, ", ")), 3)
    end
  end
  local attributes, text = {}, ""
  for _, group in ipairs(def.order) do
    local value = options[group]
    if value ~= nil then
      if type(value) ~= "string" then
        error(format("bad option '%s' to 'show' (string expected, got %s)", group, type(value)), 3)
      end
      attributes[#attributes + 1], attributes[#attributes + 2] = group, value
      text = text .. " " .. group .. " " .. value
    end
  end
  return attributes, text
end
)lua" R"lua(
-- Frame animations: animation(name, { frames = { "image", ... }, fps = n, loop = true }) or
-- animation(name, { sheet = "image", columns = n, rows = n, count = n, fps = n, loop = true }).
-- `show` and the values of a layered group take the name as they take an image name.
function animation(name, definition)
  if type(name) ~= "string" then
    bad_argument("animation", 1, "string", name)
  end
  if type(definition) ~= "table" then
    bad_argument("animation", 2, "table", definition)
  end
  local frames = option("animation", definition, "frames", "table")
  for i = 1, frames and #frames or 0 do
    if type(frames[i]) ~= "string" then
      error(format("bad option 'frames' to 'animation' (item #%d is not a string)", i), 2)
    end
  end
  local request = { frames = frames, loop = option("animation", definition, "loop", "boolean") }
  for _, key in ipairs { "columns", "rows", "count", "fps" } do
    request[key] = option("animation", definition, key, "number")
  end
  request.sheet = option("animation", definition, "sheet", "string")
  local problem = engine.animation(name, request)
  if problem then
    error(problem, 2)
  end
end
)lua" R"lua(
-- Movies (docs/SPEC.md section 14). movie(path, { skippable = true }) plays a movie over all
-- layers and blocks until it has played. Replay and skip mode do not play it.
function movie(path, options)
  if type(path) ~= "string" then
    bad_argument("movie", 1, "string", path)
  end
  options = options_argument("movie", options)
  local skippable = option("movie", options, "skippable", "boolean")
  require_label("movie")
  local problem = engine.movie_check(path)
  if problem then
    error(problem, 2)
  end
  engine.movie(path, skippable == true)
end
)lua" R"lua(

-- Saves. A call only queues a request; the host carries it out when the story waits.
local function slot_name(slot)
  if type(slot) ~= "number" or slot ~= slot // 1 or slot < 1 or slot > 999999 then
    error("bad argument #1 (a slot is an integer from 1 to 999999)", 3)
  end
  return format("%d", slot)
end
function save(slot)
  local name = slot_name(slot)
  require_label("save")
  engine.save_request(false, name)
end
function quick_save()
  require_label("quick_save")
  engine.save_request(false, "quick")
end
function quick_load()
  require_label("quick_load")
  engine.save_request(true, "quick")
end
-- skip("seen" | "all" | "off" | false) and auto(true | false): the host applies them.
local skip_modes = { seen = true, all = true, off = true }
function skip(mode)
  if mode == false or mode == nil then
    mode = "off"
  elseif mode == true then
    mode = "seen"
  elseif not skip_modes[mode] then
    bad_argument("skip", 1, "\"seen\", \"all\", \"off\" or false", mode)
  end
  require_label("skip")
  engine.skip_request(mode)
end
function auto(on)
  if type(on) ~= "boolean" then
    bad_argument("auto", 1, "boolean", on)
  end
  require_label("auto")
  engine.auto_request(on)
end
-- on_load(function(old_version, store) ... end): migrates the store of a save made by an older
-- version of the game. Scripts register it while they load.
function on_load(hook)
  if type(hook) ~= "function" then
    bad_argument("on_load", 1, "function", hook)
  end
  local problem = engine.set_on_load(hook)
  if problem ~= nil then
    error(problem, 2)
  end
end

function print(...)
  local parts = {}
  for i = 1, select("#", ...) do
    parts[i] = tostring((select(i, ...)))
  end
  engine.print(concat(parts, "\t"))
end

-- Source text only: binary chunks can crash the VM.
local raw_load = load
function load(chunk, chunkname, _, ...)
  if type(chunk) == "number" then
    local name = slot_name(chunk)
    require_label("load")
    engine.save_request(true, name)
    return
  end
  if select("#", ...) > 0 then
    return raw_load(chunk, chunkname, "t", (...))
  end
  return raw_load(chunk, chunkname, "t")
end

vfs = {}
function vfs.read(path)
  if type(path) ~= "string" then
    bad_argument("vfs.read", 1, "string", path)
  end
  return engine.vfs_read(path)
end
function vfs.exists(path)
  if type(path) ~= "string" then
    bad_argument("vfs.exists", 1, "string", path)
  end
  return engine.vfs_exists(path)
end
function vfs.list(directory)
  if type(directory) ~= "string" then
    bad_argument("vfs.list", 1, "string", directory)
  end
  return engine.vfs_list(directory)
end

-- The deterministic engine RNG. Its state lives in store.__rng, so snapshots
-- and replay cover it. It follows Lua's math.random argument rules.
local tointeger, engine_random_integer, engine_random_float = math.tointeger,
    engine.random_integer, engine.random_float
local function random_integer(low, high)
  local value, state = engine_random_integer(rawget(store, "__rng"), low, high)
  rawset(store, "__rng", state)
  return value
end
local function random_float()
  local value, state = engine_random_float(rawget(store, "__rng"))
  rawset(store, "__rng", state)
  return value
end
local function integer_argument(value, index)
  local n = tointeger(value)
  if n == nil then
    if type(value) == "number" then
      error(format("bad argument #%d to 'random' (number has no integer representation)",
                   index), 3)
    end
    bad_argument("random", index, "number", value)
  end
  return n
end
function math.random(...)
  local count = select("#", ...)
  if count == 0 then
    return random_float()
  end
  local low, high
  if count == 1 then
    low, high = 1, integer_argument((...), 1)
    if high == 0 then
      return random_integer(math.mininteger, math.maxinteger)
    end
  elseif count == 2 then
    low, high = integer_argument((...), 1), integer_argument((select(2, ...)), 2)
  else
    error("wrong number of arguments to 'random'", 2)
  end
  if low > high then
    error(format("bad argument #%d to 'random' (interval is empty)", count), 2)
  end
  return random_integer(low, high)
end
math.randomseed = nil
rand = math.random

-- The restricted standard library.
local os_clock, os_date, os_difftime, os_time = os.clock, os.date, os.difftime, os.time
os = { clock = os_clock, date = os_date, difftime = os_difftime, time = os_time }
debug = nil
dofile = nil
loadfile = nil
string.dump = nil

-- Globals the engine owns: scripts read them but cannot replace them.
local persistent = {}
)lua" R"lua(
-- Pause, NVL mode and the gallery (docs/SPEC.md sections 7, 8 and 15).
-- pause(seconds) blocks like `wait`: a step that a click or skip mode ends, and a replay does not
-- wait for.
function pause(seconds)
  if type(seconds) ~= "number" then
    bad_argument("pause", 1, "number", seconds)
  end
  if not (seconds >= 0 and seconds <= 86400) then
    error("bad argument #1 to 'pause' (a number of seconds from 0 to 86400 expected)", 2)
  end
  require_label("pause")
  engine.wait("@pause", seconds)
end

-- The NVL page is store.__nvl = { on = bool, { speaker =, text =, color = }, ... }. Being in
-- `store`, it is in snapshots and saves, and a replay builds it again. A line is NVL in NVL mode
-- and for a character of kind "nvl"; it joins the page, and the engine gets the page with the
-- request. A menu (no text) gets the page in NVL mode. Everything else gets nil.
nvl_page = function(kind, speaker, text, color)
  local page = rawget(store, "__nvl")
  local on = page ~= nil and page.on
  if text == nil then
    return on and page or nil
  end
  if kind ~= "nvl" and not on then
    return nil
  end
  if page == nil then
    page = { on = false }
    rawset(store, "__nvl", page)
  end
  page[#page + 1] = { speaker = speaker or "", text = text, color = color }
  return page
end

-- nvl(true) shows the lines that follow on the NVL screen; nvl(false) goes back to the say box
-- and empties the page.
function nvl(on)
  if type(on) ~= "boolean" then
    bad_argument("nvl", 1, "boolean", on)
  end
  require_label("nvl")
  local page = rawget(store, "__nvl")
  if not on then
    rawset(store, "__nvl", nil)
  elseif page == nil then
    rawset(store, "__nvl", { on = true })
  else
    page.on = true
  end
end

-- Empties the page: the next line starts a new screen.
function nvl_clear()
  require_label("nvl_clear")
  local page = rawget(store, "__nvl")
  if page ~= nil then
    rawset(store, "__nvl", { on = page.on })
  end
end

-- persistent.unlocks["kind/name"] = true. Showing a gallery image unlocks "cg/<image>". A replay
-- writes it again, which is harmless.
local function unlock_key(fname, kind, name)
  if type(kind) ~= "string" then
    bad_argument(fname, 1, "string", kind, 1)
  end
  if type(name) ~= "string" then
    bad_argument(fname, 2, "string", name, 1)
  end
  return kind .. "/" .. name
end
function unlock(kind, name)
  local key = unlock_key("unlock", kind, name)
  local unlocks = rawget(persistent, "unlocks")
  if type(unlocks) ~= "table" then
    unlocks = {}
    persistent.unlocks = unlocks
  end
  unlocks[key] = true
end
function unlocked(kind, name)
  local key = unlock_key("unlocked", kind, name)
  local unlocks = rawget(persistent, "unlocks")
  return type(unlocks) == "table" and unlocks[key] == true
end
local gallery_set, gallery_unpack = {}, table.unpack
for _, name in ipairs(engine.gallery) do
  gallery_set[name] = true
end
note_shown = function(image)
  if gallery_set[image] then
    unlock("cg", image)
  end
end
function gallery_images()
  return { gallery_unpack(engine.gallery) }
end
)lua" R"lua(
-- Preferences, bindings and the bridge the engine calls (docs/SPEC.md sections 15 and 17).
-- `prefs` keeps what the player changed in persistent.prefs; a value never set reads as its
-- default. The engine reads these keys: text_speed (characters a second), auto_delay (seconds),
-- volume_<channel> (0 to 1), fullscreen, skip_unseen, window_opacity (0.3 to 1)
-- and reduce_motion.
local pref_defaults = {
  text_speed = 40, auto_delay = 1, volume_music = 1, volume_sound = 1, volume_voice = 1,
  volume_ambient = 1, volume_ui = 1, fullscreen = false, skip_unseen = false, window_opacity = 1,
  reduce_motion = false,
}
local prefs = setmetatable({}, {
  __index = function(_, key)
    local saved, value = rawget(persistent, "prefs"), nil
    if type(saved) == "table" then
      value = saved[key]
    end
    if value == nil then
      return pref_defaults[key]
    end
    return value
  end,
  __newindex = function(_, key, value)
    if type(key) ~= "string" then
      error("prefs keys must be strings", 2)
    end
    local default = pref_defaults[key]
    local value_type = type(value)
    local scalar = value_type == "boolean" or value_type == "number" or value_type == "string"
    if value ~= nil and (not scalar or (default ~= nil and value_type ~= type(default))) then
      error(format("bad value for prefs.%s (%s expected, got %s)", key,
                   default ~= nil and type(default) or "boolean, number or string", value_type), 2)
    end
    local saved = rawget(persistent, "prefs")
    if type(saved) ~= "table" then
      saved = {}
      persistent.prefs = saved
    end
    saved[key] = value
  end,
  __metatable = false,
})

-- The virtual resolution, for screens that size themselves.
local canvas = setmetatable({}, {
  __index = { width = engine.canvas_width, height = engine.canvas_height },
  __newindex = function()
    error("canvas is read-only", 2)
  end,
  __metatable = false,
})

-- bind(table, key): a node option that follows store, persistent or prefs and writes it back
-- (toggle, slider and bar). `table` must be one of those three.
local bind_roots = { store = store, persistent = persistent, prefs = prefs }
function bind(target, key)
  local name
  for candidate, root in next, bind_roots do
    if rawequal(root, target) then
      name = candidate
    end
  end
  if name == nil then
    bad_argument("bind", 1, "store, persistent or prefs", target)
  end
  if type(key) ~= "string" then
    bad_argument("bind", 2, "string", key)
  end
  return { bind_table = name, bind_key = key }
end

-- What the engine calls from C++: read and write a bound datum, and build a screen.
local bridge = {}
function bridge.get(name, key)
  local root = bind_roots[name]
  if root == nil then
    return nil
  end
  local value = root[key]
  local value_type = type(value)
  if value_type == "boolean" or value_type == "number" or value_type == "string" then
    return value
  end
  return nil
end
function bridge.set(name, key, value)
  local root = bind_roots[name]
  if root == nil then
    return format("unknown table '%s' (use store, persistent or prefs)", tostring(name))
  end
  local ok, problem = pcall(function() root[key] = value end)
  return not ok and tostring(problem) or nil
end
function bridge.build(name, args)
  if screens[name] == nil then
    return nil
  end
  return screen_root(nil, name, args)
end
)lua" R"lua(
local reserved = { store = store, persistent = persistent, prefs = prefs, canvas = canvas }
do
  local warned = {}
  setmetatable(_G, {
    __index = reserved,
    __newindex = function(globals, key, value)
      if reserved[key] ~= nil then
        error(format("'%s' is owned by the engine and cannot be replaced (change its fields)",
                     tostring(key)), 2)
      end
      if dev_mode and not warned[key] then
        warned[key] = true
        engine.warn(format("%s: new global '%s' (keep game state in store, or use a local)",
                           where(2), tostring(key)))
      end
      rawset(globals, key, value)
    end,
    __metatable = false,
  })
end

local function watch(thread)
  if hook_period > 0 then
    debug_sethook(thread, on_count, "", hook_period)
  end
end

-- Coroutines a script creates are watched too.
local co_create, co_resume, co_close = coroutine.create, coroutine.resume, coroutine.close
local unpack = table.unpack
function coroutine.create(fn)
  local thread = co_create(fn)
  watch(thread)
  return thread
end
local function wrap_results(thread, ok, ...)
  if ok then
    return ...
  end
  co_close(thread)
  error((...), 0)
end
function coroutine.wrap(fn)
  local thread = coroutine.create(fn)
  return function(...)
    return wrap_results(thread, co_resume(thread, ...))
  end
end

local limits = { store = store, persistent = persistent }
function limits.configure(period)
  hook_period = period
  watch(coroutine.running())
end
function limits.watch(thread)
  watch(thread)
end
function limits.reset(ticks)
  ticks_left = ticks
end
limits.bridge = bridge
function limits.link(id)
  if link_handler == nil then
    return false
  end
  link_handler(id)
  return true
end
return limits
)lua";

// How many labels may be called inside each other (frames on the call stack).
constexpr std::size_t kMaxCallDepth = 1000;

// The instruction-limit hook runs every kHookPeriod instructions.
constexpr std::uint32_t kHookPeriod = 1000;

// splitmix64 (Steele, Lea, Flood 2014): a small generator with a fixed,
// platform-independent output sequence.
std::uint64_t splitmix64(std::uint64_t& state) {
  constexpr std::uint64_t kIncrement = 0x9E3779B97F4A7C15ULL;
  constexpr std::uint64_t kMul1 = 0xBF58476D1CE4E5B9ULL;
  constexpr std::uint64_t kMul2 = 0x94D049BB133111EBULL;
  constexpr int kShift1 = 30;
  constexpr int kShift2 = 27;
  constexpr int kShift3 = 31;
  state += kIncrement;
  std::uint64_t z = state;
  z = (z ^ (z >> kShift1)) * kMul1;
  z = (z ^ (z >> kShift2)) * kMul2;
  return z ^ (z >> kShift3);
}

// FNV-1a over the lines first_line..last_line (from 1) of `text`, without
// carriage returns, so the hash does not depend on the platform's line ends
// or on where the lines are in the file.
// ponytail: covers only the label function's own text, not helpers it calls.
// Hash the whole script if edits to helpers must invalidate saves too.
std::uint64_t hash_lines(std::string_view text, std::int64_t first_line, std::int64_t last_line) {
  constexpr std::uint64_t kOffsetBasis = 0xCBF29CE484222325ULL;
  constexpr std::uint64_t kPrime = 0x100000001B3ULL;
  std::uint64_t hash = kOffsetBasis;
  std::int64_t line = 1;
  for (const char c : text) {
    if (line >= first_line && line <= last_line && c != '\r') {
      hash = (hash ^ static_cast<std::uint64_t>(static_cast<unsigned char>(c))) * kPrime;
    }
    if (c == '\n') {
      ++line;
    }
  }
  return hash;
}

// Reads the boolean, number or string at `index` into `value`; false for any
// other type.
bool read_scalar(lua_State* lua, int index, StoreValue& value) {
  switch (lua_type(lua, index)) {
    case LUA_TBOOLEAN:
      value = lua_toboolean(lua, index) != 0;
      return true;
    case LUA_TNUMBER:
      if (lua_isinteger(lua, index) != 0) {
        value = static_cast<std::int64_t>(lua_tointegerx(lua, index, nullptr));
      } else {
        value = static_cast<double>(lua_tonumberx(lua, index, nullptr));
      }
      return true;
    case LUA_TSTRING: {
      std::size_t size = 0;
      const char* text = lua_tolstring(lua, index, &size);
      value = std::string{text, size};
      return true;
    }
    default:
      return false;
  }
}

// Whether restore can build `snapshot` without a Lua error: it has the store
// table, every table reference exists and every key is a valid table key.
bool is_valid(const StoreSnapshot& snapshot) {
  if (snapshot.tables.empty()) {
    return false;
  }
  for (const StoreTable& table : snapshot.tables) {
    for (const auto& [key, value] : table.entries) {
      if (std::holds_alternative<StoreTableRef>(key)) {
        return false;
      }
      if (const auto* number = std::get_if<double>(&key);
          number != nullptr && std::isnan(*number)) {
        return false;
      }
      if (const auto* ref = std::get_if<StoreTableRef>(&value);
          ref != nullptr && ref->index >= snapshot.tables.size()) {
        return false;
      }
    }
  }
  return true;
}

ScriptObserver& null_observer() {
  static ScriptObserver observer;
  return observer;
}

std::string error_text(const sol::protected_function_result& result) {
  const sol::type type = result.get_type();
  if (type == sol::type::string) {
    return result.get<sol::error>().what();
  }
  return "(error object is a " + sol::type_name(result.lua_state(), type) + " value)";
}

std::string to_string(const std::vector<std::byte>& bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const std::byte b : bytes) {
    text.push_back(static_cast<char>(b));
  }
  return text;
}

// How deep a screen tree may nest; a script could make a node its own child.
constexpr int kMaxScreenDepth = 64;

// Reads a boolean, number or string from the object into `value`.
bool read_screen_value(const sol::object& object, ScreenValue& value) {
  StoreValue scalar;
  lua_State* lua = object.lua_state();
  object.push();
  const bool ok = read_scalar(lua, -1, scalar);
  lua_pop(lua, 1);
  if (!ok) {
    return false;
  }
  if (const auto* boolean = std::get_if<bool>(&scalar)) {
    value = *boolean;
  } else if (const auto* integer = std::get_if<std::int64_t>(&scalar)) {
    value = *integer;
  } else if (const auto* number = std::get_if<double>(&scalar)) {
    value = *number;
  } else {
    value = std::get<std::string>(std::move(scalar));
  }
  return true;
}

// Copies a node table built by the prelude (kind, text, props, children) into `node`. Returns why
// it cannot, or an empty string.
std::string read_screen_node(const sol::table& table, int depth, ScreenNodeDef& node) {
  if (depth > kMaxScreenDepth) {
    return "screen nodes nest more than " + std::to_string(kMaxScreenDepth) + " levels deep";
  }
  const auto kind = table.get<sol::optional<std::string>>("kind");
  const auto text = table.get<sol::optional<std::string>>("text");
  const auto props = table.get<sol::optional<sol::table>>("props");
  const auto children = table.get<sol::optional<sol::table>>("children");
  if (!kind || !text || !props || !children) {
    return "a screen node is malformed (build nodes with box, hbox, vbox, text, image, button)";
  }
  node.kind = *kind;
  node.text = *text;
  std::map<std::string, ScreenValue> sorted;  // Lua's iteration order is not stable
  for (const auto& [key, value] : *props) {
    const auto name = key.as<sol::optional<std::string>>();
    ScreenValue scalar;
    if (!name || !read_screen_value(value, scalar)) {
      return "a screen node has an option that is not a boolean, number or string";
    }
    sorted.emplace(*name, std::move(scalar));
  }
  node.props.assign(std::make_move_iterator(sorted.begin()), std::make_move_iterator(sorted.end()));
  const std::size_t count = children->size();
  node.children.reserve(count);
  for (std::size_t i = 1; i <= count; ++i) {
    const auto child = children->get<sol::optional<sol::table>>(i);
    if (!child) {
      return "a screen node has a child that is not a node";
    }
    ScreenNodeDef& added = node.children.emplace_back();
    if (std::string problem = read_screen_node(*child, depth + 1, added); !problem.empty()) {
      return problem;
    }
  }
  return {};
}

// How deep a tween group may nest; the prelude allows far less.
constexpr int kMaxTweenDepth = 64;

// Reads a table of uniform names and numbers, in the order of the names. Returns why it cannot,
// or an empty string.
std::string read_uniforms(const sol::table& table, std::vector<UniformValue>& uniforms) {
  for (const auto& [key, value] : table) {
    if (key.get_type() != sol::type::string || value.get_type() != sol::type::number) {
      return "uniforms must be a table of uniform names and numbers";
    }
    uniforms.emplace_back(key.as<std::string>(), value.as<double>());
  }
  std::ranges::sort(uniforms);
  return {};
}

// Copies a tween node table built by the prelude into `request`. Returns why it cannot, or an
// empty string.
std::string read_tween(const sol::table& table, int depth, TweenRequest& request) {
  if (depth > kMaxTweenDepth) {
    return "tween groups nest more than " + std::to_string(kMaxTweenDepth) + " levels deep";
  }
  const auto number = [&table](const char* key) -> std::optional<double> {
    const auto value = table.get<sol::optional<double>>(key);
    return value ? std::optional<double>{*value} : std::nullopt;
  };
  const auto kind = table.get<sol::optional<std::string>>("kind");
  if (!kind) {
    return "a tween is malformed (use tween, sequence and parallel)";
  }
  if (*kind == "sequence") {
    request.kind = TweenRequest::Kind::kSequence;
  } else if (*kind == "parallel") {
    request.kind = TweenRequest::Kind::kParallel;
  }
  request.tag = table.get_or<std::string>("tag", "");
  request.x = number("x");
  request.y = number("y");
  request.anchor_x = number("anchor_x");
  request.anchor_y = number("anchor_y");
  request.scale_x = number("scale_x");
  request.scale_y = number("scale_y");
  request.rotation = number("rotation");
  request.alpha = number("alpha");
  if (const auto tint = table.get<sol::optional<std::int64_t>>("tint")) {
    request.tint = static_cast<std::uint32_t>(*tint);
  }
  request.crop_x = number("crop_x");
  request.crop_y = number("crop_y");
  request.crop_width = number("crop_width");
  request.crop_height = number("crop_height");
  if (const auto uniforms = table.get<sol::optional<sol::table>>("uniforms")) {
    if (std::string problem = read_uniforms(*uniforms, request.uniforms); !problem.empty()) {
      return problem;
    }
  }
  request.duration = number("duration").value_or(request.duration);
  request.delay = number("delay").value_or(request.delay);
  request.ease = table.get_or<std::string>("ease", request.ease);
  request.loop = table.get_or("loop", false);
  request.yoyo = table.get_or("yoyo", false);
  request.instant = table.get_or("instant", false);
  if (const auto children = table.get<sol::optional<sol::table>>("children")) {
    const std::size_t count = children->size();
    request.children.reserve(count);
    for (std::size_t i = 1; i <= count; ++i) {
      const auto child = children->get<sol::optional<sol::table>>(i);
      if (!child) {
        return "a tween group has an item that is not a tween";
      }
      if (std::string problem = read_tween(*child, depth + 1, request.children.emplace_back());
          !problem.empty()) {
        return problem;
      }
    }
  }
  return {};
}

// A flat list { a1, b1, a2, b2, ... } that the prelude built, as pairs.
std::vector<std::pair<std::string, std::string>> read_pairs(const sol::table& table) {
  std::vector<std::pair<std::string, std::string>> pairs;
  const std::size_t count = table.size();
  pairs.reserve(count / 2);
  for (std::size_t i = 1; i < count; i += 2) {
    pairs.emplace_back(table.get_or<std::string>(i, ""), table.get_or<std::string>(i + 1, ""));
  }
  return pairs;
}

// Copies the definition table that the prelude's `layered` built into `request`.
void read_layered(const sol::table& table, LayeredRequest& request) {
  request.crossfade = table.get_or("crossfade", 0.0);
  if (const auto groups = table.get<sol::optional<sol::table>>("groups")) {
    const std::size_t count = groups->size();
    request.groups.reserve(count);
    for (std::size_t i = 1; i <= count; ++i) {
      const auto group = groups->get<sol::optional<sol::table>>(i);
      LayeredRequest::Group& added = request.groups.emplace_back();
      if (group) {
        added.name = group->get_or<std::string>("name", "");
        if (const auto values = group->get<sol::optional<sol::table>>("values")) {
          added.values = read_pairs(*values);
        }
      }
    }
  }
  if (const auto defaults = table.get<sol::optional<sol::table>>("default")) {
    request.defaults = read_pairs(*defaults);
  }
}

// Copies the definition table that the prelude's `animation` built into `request`.
void read_animation(const sol::table& table, AnimationRequest& request) {
  request.sheet = table.get_or<std::string>("sheet", "");
  const auto number = [&table](const char* key) -> std::optional<double> {
    const auto value = table.get<sol::optional<double>>(key);
    return value ? std::optional<double>{*value} : std::nullopt;
  };
  request.columns = number("columns");
  request.rows = number("rows");
  request.count = number("count");
  request.fps = number("fps");
  request.loop = table.get_or("loop", true);
  if (const auto frames = table.get<sol::optional<sol::table>>("frames")) {
    const std::size_t count = frames->size();
    request.frames.reserve(count);
    for (std::size_t i = 1; i <= count; ++i) {
      request.frames.push_back(frames->get_or<std::string>(i, ""));
    }
  }
}

// A `wait` or `movie`: advance() ends it, it is a step that a replay passes by itself, and it is
// not a rollback step.
bool is_pause(const BlockingRequest& request) {
  return std::holds_alternative<WaitRequest>(request) ||
         std::holds_alternative<MovieRequest>(request);
}

// Continues `coroutine` with the answer of a blocking call as its result.
sol::protected_function_result resume_with(sol::coroutine& coroutine, const ScreenValue& value) {
  return std::visit(
      [&coroutine](const auto& v) -> sol::protected_function_result {
        if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::monostate>) {
          return coroutine(sol::lua_nil);
        } else {
          return coroutine(v);
        }
      },
      value);
}

}  // namespace

class ScriptRuntime::Impl {
 public:
  Impl(const Vfs& vfs, const ScriptRuntimeOptions& options);
  Impl(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl() = default;

  void load_script(std::string_view path);
  void run_chunk(std::string_view source, const std::string& chunk_name);
  void load_scripts();
  [[nodiscard]] bool has_label(std::string_view name) const { return labels_.contains(name); }
  void start(std::string_view name);
  void advance();
  void choose(std::size_t index);
  void return_screen(ScreenValue value);
  bool activate_link(std::string_view id);
  void replay(const LabelSnapshot& snapshot, std::span<const InputResult> inputs,
              std::uint32_t step);
  bool roll_back();
  [[nodiscard]] bool can_roll_back() const;
  [[nodiscard]] std::span<const RollbackStep> rollback_steps() const { return rollback_; }
  void drop_callers();
  [[nodiscard]] std::string run_on_load(LabelSnapshot& snapshot, std::string_view old_version);
  [[nodiscard]] std::vector<SaveRequest> take_save_requests() {
    return std::exchange(save_requests_, {});
  }
  [[nodiscard]] std::vector<PlaybackRequest> take_playback_requests() {
    return std::exchange(playback_requests_, {});
  }
  [[nodiscard]] ScreenValue binding(std::string_view table, std::string_view key);
  [[nodiscard]] std::string set_binding(std::string_view table, std::string_view key,
                                        ScreenValue value);
  [[nodiscard]] std::string build_screen(std::string_view name, const ScreenNodeDef& args,
                                         ScreenNodeDef& tree);
  [[nodiscard]] StoreSnapshot capture_persistent() const {
    return capture_table(persistent_, "persistent");
  }
  [[nodiscard]] std::string restore_persistent(const StoreSnapshot& snapshot);

  [[nodiscard]] bool replaying() const { return replaying_; }
  [[nodiscard]] const LabelSnapshot* label_snapshot() const {
    return frames_.empty() ? nullptr : &frames_.back().snapshot;
  }
  [[nodiscard]] std::span<const InputResult> input_log() const {
    if (frames_.empty()) {
      return {};
    }
    return frames_.back().inputs;
  }
  [[nodiscard]] StoreSnapshot capture_store() const { return capture_table(store_, "store"); }
  [[nodiscard]] StoreSnapshot capture_table(const sol::table& root, std::string_view name) const;
  [[nodiscard]] std::optional<std::uint64_t> label_source_hash(std::string_view name) const {
    const auto found = labels_.find(name);
    return found == labels_.end() ? std::nullopt : std::optional{found->second.source_hash};
  }

  [[nodiscard]] RunState state() const { return state_; }
  [[nodiscard]] const BlockingRequest* request() const {
    return state_ == RunState::kWaiting ? &request_ : nullptr;
  }
  [[nodiscard]] ScriptPosition position() const {
    if (frames_.empty()) {
      return {};
    }
    return {.label = frames_.back().label,
            .step = frames_.back().step,
            .wait = state_ == RunState::kWaiting && is_pause(request_)};
  }
  [[nodiscard]] const std::string& error() const { return error_; }

 private:
  // Tracks the VM's memory. The limit is enforced only while script code
  // runs (enforce): engine-side work, including the engine primitives that
  // scripts call, never fails for it, so a Lua memory error can only be raised
  // where Lua code (not C++ with live objects) is on top of the stack.
  struct Allocator {
    std::size_t used = 0;
    std::size_t limit = 0;
    bool enforce = false;
  };

  // Sets whether the memory limit applies, until the end of the scope.
  class MemoryLimitScope {
   public:
    MemoryLimitScope(Allocator& allocator, bool enforce)
        : allocator_(&allocator), previous_(allocator.enforce) {
      allocator.enforce = enforce;
    }
    MemoryLimitScope(const MemoryLimitScope&) = delete;
    MemoryLimitScope(MemoryLimitScope&&) = delete;
    MemoryLimitScope& operator=(const MemoryLimitScope&) = delete;
    MemoryLimitScope& operator=(MemoryLimitScope&&) = delete;
    ~MemoryLimitScope() { allocator_->enforce = previous_; }

   private:
    Allocator* allocator_;
    bool previous_;
  };

  struct Label {
    sol::main_function function;
    std::string location;  // "script:line" of its label() call
    std::uint64_t source_hash = 0;
  };

  // One running label: its coroutine, how many yield points it reached, the
  // snapshot taken when it was entered and the answers given since.
  struct Frame {
    std::string label;
    sol::thread thread;
    sol::coroutine coroutine;
    std::uint32_t step = 0;
    std::uint32_t stops = 0;  // yield points reached by this label and the labels it called
    LabelSnapshot snapshot{};
    std::vector<InputResult> inputs{};
  };

  enum class TransferKind : std::uint8_t { kJump, kCall, kReturn };
  struct Transfer {
    TransferKind kind;
    std::string label;
  };

  static void* allocate(void* user_data, void* block, std::size_t old_size,
                        std::size_t new_size) noexcept;

  void bind_engine();
  [[nodiscard]] bool in_label(sol::this_state state) const;
  void yield_at(sol::this_state state, BlockingRequest request);
  void transfer(sol::this_state state, std::string_view kind, std::string label);
  [[nodiscard]] Frame make_frame(const std::string& name);
  [[nodiscard]] bool enter(const std::string& name);
  void run(std::optional<ScreenValue> resume_value = std::nullopt);
  [[nodiscard]] bool replay_answer(std::optional<ScreenValue>& resume_value);
  void record(const InputResult& input);
  void begin_replay(const LabelSnapshot& snapshot, std::span<const InputResult> inputs,
                    std::uint32_t step, std::uint32_t ordinal);
  [[nodiscard]] bool at_replay_target() const;
  void push_rollback_step(std::string voice);
  void restore_store(const StoreSnapshot& snapshot) { restore_table(store_, snapshot); }
  void restore_table(const sol::table& root, const StoreSnapshot& snapshot);
  void fail(std::string message);
  void reset_instruction_budget();
  [[nodiscard]] std::uint64_t random_state(const sol::stack_object& state) const;

  // What replay() still has to do while replaying_ is set.
  struct Replay {
    std::vector<InputResult> inputs;
    std::size_t next_input = 0;
    std::size_t depth = 0;  // frames_.size() with the replayed label on top
    std::uint32_t step = 0;
    // Set by roll_back(): replay the outermost label (and the labels it calls) to its n-th yield
    // point. 0 for a load, which replays the one label to `step`.
    std::uint32_t ordinal = 0;
  };

  const Vfs* vfs_;
  ScriptRuntimeOptions options_;
  ScriptObserver* observer_;
  Allocator allocator_;
  // Declared after the allocator and before every Lua reference: the VM is
  // destroyed after the references and before the allocator state.
  sol::state lua_;
  sol::table engine_;
  sol::table store_;
  sol::table persistent_;
  sol::protected_function limits_configure_;
  sol::protected_function limits_watch_;
  sol::protected_function limits_reset_;
  sol::protected_function link_;
  sol::protected_function bridge_get_;
  sol::protected_function bridge_set_;
  sol::protected_function bridge_build_;

  std::map<std::string, Label, std::less<>> labels_;
  // Script path -> source text, for the label source hashes; kept only while
  // scripts load.
  std::map<std::string, std::string, std::less<>> sources_;
  sol::main_function on_load_;
  std::vector<SaveRequest> save_requests_;
  std::vector<PlaybackRequest> playback_requests_;
  std::vector<Frame> frames_;  // back() is the running label; the rest called it
  std::optional<std::variant<BlockingRequest, Transfer>> pending_;
  bool started_ = false;
  RunState state_ = RunState::kIdle;
  std::uint32_t hook_period_ = 0;
  BlockingRequest request_;
  ScreenNodeDef staged_screen_;  // the tree stage_screen read, for show_screen or call_screen
  std::optional<NvlPage> staged_nvl_;  // the page stage_nvl read, for the next say, narrate or menu
  std::string error_;
  bool replaying_ = false;
  Replay replay_;
  std::vector<RollbackStep> rollback_;  // oldest first
  bool rolling_back_ = false;           // the replay in progress is roll_back()'s
  std::string pending_voice_;           // the last `voice(name)`, until the next say or menu
};

void* ScriptRuntime::Impl::allocate(void* user_data, void* block, std::size_t old_size,
                                    std::size_t new_size) noexcept {
  auto* allocator = static_cast<Allocator*>(user_data);
  // When block is null, old_size encodes the kind of object, not a size.
  const std::size_t freed = block == nullptr ? 0 : old_size;
  if (new_size == 0) {
    // The lua_Alloc contract is realloc/free semantics; Lua owns the block.
    std::free(block);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    allocator->used -= freed;
    return nullptr;
  }
  if (allocator->enforce && allocator->limit != 0 && new_size > freed &&
      allocator->used - freed + new_size > allocator->limit) {
    return nullptr;  // Lua raises "not enough memory"
  }
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
  void* grown = std::realloc(block, new_size);
  if (grown != nullptr) {
    allocator->used = allocator->used - freed + new_size;
  }
  return grown;
}

ScriptRuntime::Impl::Impl(const Vfs& vfs, const ScriptRuntimeOptions& options)
    : vfs_(&vfs),
      options_(options),
      observer_(options.observer != nullptr ? options.observer : &null_observer()),
      allocator_{.used = 0, .limit = options.memory_limit_bytes},
      lua_(sol::default_at_panic, &Impl::allocate, &allocator_) {
  lua_.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::string, sol::lib::table,
                      sol::lib::math, sol::lib::utf8, sol::lib::os, sol::lib::debug);
  // sol2 keeps a default message handler (it appends a traceback) in a
  // global that scripts could replace. Errors stay plain "script:line: text".
  sol::protected_function::set_default_handler(sol::object{lua_.lua_state(), sol::lua_nil});
  bind_engine();
  sol::load_result prelude = lua_.load(kPrelude, "=vnmaki", sol::load_mode::text);
  if (!prelude.valid()) {
    const sol::error error = prelude;
    throw std::logic_error(std::string{"vnmaki prelude: "} + error.what());
  }
  sol::protected_function prelude_function = prelude;
  sol::protected_function_result limits =
      prelude_function(engine_, options_.dev_mode, static_cast<std::int64_t>(options_.random_seed));
  if (!limits.valid()) {
    throw std::logic_error("vnmaki prelude: " + error_text(limits));
  }
  const sol::table limits_table = limits;
  store_ = limits_table["store"];
  persistent_ = limits_table["persistent"];
  limits_configure_ = limits_table["configure"];
  limits_watch_ = limits_table["watch"];
  limits_reset_ = limits_table["reset"];
  link_ = limits_table["link"];
  const sol::table bridge = limits_table["bridge"];
  bridge_get_ = bridge["get"];
  bridge_set_ = bridge["set"];
  bridge_build_ = bridge["build"];
  if (options_.instruction_limit != 0) {
    hook_period_ = std::min(options_.instruction_limit, kHookPeriod);
    limits_configure_(hook_period_);
  }
}

void ScriptRuntime::Impl::bind_engine() {
  engine_ = lua_.create_table();
  engine_["canvas_width"] = options_.canvas_width;
  engine_["canvas_height"] = options_.canvas_height;
  sol::table gallery = lua_.create_table();
  for (std::size_t i = 0; i < options_.gallery.size(); ++i) {
    gallery[i + 1] = options_.gallery[i];
  }
  engine_["gallery"] = gallery;
  engine_["in_label"] = [this](sol::this_state state) { return in_label(state); };
  engine_["has_label"] = [this](const std::string& name) { return has_label(name); };
  // Compiles styles/<name>.lua for theme(): returns the chunk as a function, or why it cannot.
  engine_["load_theme"] = [this](sol::this_state state, const std::string& name) -> sol::object {
    const MemoryLimitScope unlimited{allocator_, false};
    const std::string path = "styles/" + name + ".lua";
    const std::optional<std::vector<std::byte>> bytes = vfs_->read(path);
    if (!bytes) {
      return sol::make_object(state, "unknown theme '" + name + "' (no file " + path + ")");
    }
    sol::load_result chunk = lua_.load(to_string(*bytes), "@" + path, sol::load_mode::text);
    if (!chunk.valid()) {
      const sol::error error = chunk;
      return sol::make_object(state, std::string{error.what()});
    }
    sol::protected_function function = chunk;
    return sol::make_object(state, function);
  };
  // Primitives that create Lua values do so inside an unlimited scope and
  // return them as references, so the push back to Lua cannot run out of
  // memory either.
  engine_["define_label"] = [this](sol::this_state state, const std::string& name,
                                   const sol::stack_object& function, const std::string& location,
                                   const std::string& source, std::int64_t first_line,
                                   std::int64_t last_line) -> sol::object {
    const MemoryLimitScope unlimited{allocator_, false};
    std::string problem;
    if (started_) {
      problem = "label() can only be called while scripts load";
    } else if (const auto existing = labels_.find(name); existing != labels_.end()) {
      problem = "label '" + name + "' is already defined at " + existing->second.location;
    } else {
      // A function from a loaded script hashes its own lines; any other
      // (from load(), or a C function) hashes the whole chunk it came from.
      const auto script = source.starts_with('@')
                              ? sources_.find(std::string_view{source}.substr(1))
                              : sources_.end();
      const std::uint64_t source_hash =
          script != sources_.end()
              ? hash_lines(script->second, first_line, last_line)
              : hash_lines(source, 1, std::numeric_limits<std::int64_t>::max());
      labels_.emplace(name, Label{.function = function.as<sol::main_function>(),
                                  .location = location,
                                  .source_hash = source_hash});
      return sol::lua_nil;
    }
    return sol::make_object(state, problem);
  };
  // The prelude raises the problem at the script line of the say or narrate call.
  engine_["check_text"] = [this](sol::this_state state, const std::string& text) -> sol::object {
    std::string problem;
    try {
      (void)parse_markup(text);
    } catch (const MarkupError& error) {
      problem = "bad text tag at byte " + std::to_string(error.offset()) + ": " + error.what();
    }
    if (problem.empty()) {
      return sol::lua_nil;
    }
    const MemoryLimitScope unlimited{allocator_, false};
    return sol::make_object(state, problem);
  };
  engine_["say"] = sol::yielding([this](sol::this_state state, const std::string& speaker,
                                        const std::string& text, std::optional<std::int64_t> color,
                                        const std::string& voice_tag, double auto_multiplier) {
    std::optional<std::uint32_t> rgb;
    if (color) {
      rgb = static_cast<std::uint32_t>(*color);
    }
    yield_at(state,
             SayRequest{.speaker = speaker, .text = text, .color = rgb, .voice_tag = voice_tag,
                         .auto_multiplier = auto_multiplier,
                         .nvl = std::exchange(staged_nvl_, std::nullopt)});
  });
  engine_["narrate"] = sol::yielding([this](sol::this_state state, const std::string& text) {
    yield_at(state, NarrateRequest{.text = text, .nvl = std::exchange(staged_nvl_, std::nullopt)});
  });
  // The prelude stages the NVL page (nil outside NVL) just before each say, narrate and menu.
  engine_["stage_nvl"] = [this](const sol::object& page) {
    const MemoryLimitScope unlimited{allocator_, false};
    staged_nvl_.reset();
    if (page.get_type() != sol::type::table) {
      return;
    }
    const sol::table lines = page.as<sol::table>();
    NvlPage staged;
    for (std::size_t i = 1; i <= lines.size(); ++i) {
      const sol::table line = lines.raw_get<sol::table>(i);
      NvlLine added{.speaker = line.raw_get<sol::optional<std::string>>("speaker").value_or(""),
                    .text = line.raw_get<sol::optional<std::string>>("text").value_or("")};
      if (const auto color = line.raw_get<sol::optional<std::int64_t>>("color")) {
        added.color = static_cast<std::uint32_t>(*color);
      }
      staged.push_back(std::move(added));
    }
    staged_nvl_ = std::move(staged);
  };
  // The prelude checked that `options` is a list of strings. A stack_table
  // makes no registry reference, so nothing here can raise a Lua error.
  engine_["menu"] = sol::yielding([this](sol::this_state state, const sol::stack_table& options,
                                         bool fixed) {
    MenuRequest request{.fixed = fixed, .nvl = std::exchange(staged_nvl_, std::nullopt)};
    const std::size_t count = options.size();
    request.options.reserve(count);
    for (std::size_t i = 1; i <= count; ++i) {
      request.options.push_back(options.get<std::string>(i));
    }
    yield_at(state, std::move(request));
  });
  // Screens: the prelude stages a screen tree, then shows it or waits on it.
  engine_["stage_screen"] = [this](sol::this_state state, const std::string& name,
                                   const sol::table& root) -> sol::object {
    const MemoryLimitScope unlimited{allocator_, false};
    staged_screen_ = {};
    std::string problem = read_screen_node(root, 0, staged_screen_);
    if (problem.empty() && options_.screens != nullptr) {
      problem = options_.screens->check_screen(name, staged_screen_);
    }
    return problem.empty() ? sol::object{sol::lua_nil} : sol::make_object(state, problem);
  };
  engine_["show_screen"] = [this](sol::this_state state, const std::string& name) -> sol::object {
    const MemoryLimitScope unlimited{allocator_, false};
    if (options_.screens == nullptr) {
      return sol::lua_nil;
    }
    const std::string problem = options_.screens->show_screen(name, staged_screen_);
    return problem.empty() ? sol::object{sol::lua_nil} : sol::make_object(state, problem);
  };
  engine_["hide_screen"] = [this](const std::string& name) {
    if (options_.screens != nullptr) {
      options_.screens->hide_screen(name);
    }
  };
  engine_["call_screen"] = sol::yielding([this](sol::this_state state, const std::string& name) {
    yield_at(state, CallScreenRequest{.name = name, .screen = std::move(staged_screen_)});
    staged_screen_ = {};
  });
  // The scene changes during a replay too; only the report is skipped.
  const auto problem_or_nil = [this](sol::this_state state,
                                     const std::string& problem) -> sol::object {
    if (problem.empty()) {
      return sol::lua_nil;
    }
    const MemoryLimitScope unlimited{allocator_, false};
    return sol::make_object(state, problem);
  };
  engine_["scene"] = [this, problem_or_nil](sol::this_state state, const std::string& image) {
    return problem_or_nil(state,
                          options_.scene != nullptr ? options_.scene->scene(image) : std::string{});
  };
  engine_["show"] = [this, problem_or_nil](
                        sol::this_state state, const std::string& tag, const std::string& image,
                        const sol::optional<std::string>& at, sol::optional<double> x,
                        sol::optional<double> y, const sol::optional<std::string>& layer,
                        const sol::optional<std::string>& movie, sol::optional<bool> loop,
                        const sol::optional<sol::table>& attributes) {
    if (options_.scene == nullptr) {
      return problem_or_nil(state, {});
    }
    ShowRequest request{.tag = tag, .image = image, .replaying = replaying_};
    if (movie) {
      request.movie = *movie;
      request.loop = loop.value_or(false);
    }
    if (attributes) {
      const MemoryLimitScope unlimited{allocator_, false};
      request.attributes = read_pairs(*attributes);
    }
    if (at) {
      request.at = *at;
    }
    if (x) {
      request.x = *x;
    }
    if (y) {
      request.y = *y;
    }
    if (layer) {
      request.layer = *layer;
    }
    return problem_or_nil(state, options_.scene->show(request));
  };
  // Definitions are not story state: they are made when the scripts load, and again when a label
  // that defines one replays.
  engine_["layered"] = [this, problem_or_nil](sol::this_state state, const std::string& name,
                                              const sol::table& definition) {
    const MemoryLimitScope unlimited{allocator_, false};
    if (options_.scene == nullptr) {
      return problem_or_nil(state, {});
    }
    LayeredRequest request{.name = name};
    read_layered(definition, request);
    return problem_or_nil(state, options_.scene->layered(request));
  };
  engine_["animation"] = [this, problem_or_nil](sol::this_state state, const std::string& name,
                                                const sol::table& definition) {
    const MemoryLimitScope unlimited{allocator_, false};
    if (options_.scene == nullptr) {
      return problem_or_nil(state, {});
    }
    AnimationRequest request{.name = name};
    read_animation(definition, request);
    return problem_or_nil(state, options_.scene->animation(request));
  };
  engine_["hide"] = [this](const std::string& tag) {
    if (options_.scene != nullptr) {
      options_.scene->hide(tag);
    }
  };
  // Tweens start during a replay too: those that end then go to their end values at once.
  engine_["tween"] = [this, problem_or_nil](sol::this_state state, const sol::table& spec) {
    const MemoryLimitScope unlimited{allocator_, false};
    if (options_.scene == nullptr) {
      return problem_or_nil(state, {});
    }
    TweenRequest request;
    std::string problem = read_tween(spec, 0, request);
    if (problem.empty()) {
      request.replaying = replaying_;
      problem = options_.scene->tween(request);
    }
    return problem_or_nil(state, problem);
  };
  engine_["declare_shader"] = [this, problem_or_nil](sol::this_state state,
                                                     const std::string& name,
                                                     const sol::table& uniforms) {
    const MemoryLimitScope unlimited{allocator_, false};
    if (options_.scene == nullptr) {
      return problem_or_nil(state, {});
    }
    std::vector<std::string> names;
    names.reserve(uniforms.size());
    for (std::size_t i = 1; i <= uniforms.size(); ++i) {
      names.push_back(uniforms.get<std::string>(i));
    }
    return problem_or_nil(state, options_.scene->declare_shader(name, names));
  };
  // Shader state changes during a replay too.
  engine_["set_shader"] = [this, problem_or_nil](sol::this_state state, const std::string& target,
                                                 bool layer,
                                                 const sol::optional<std::string>& shader,
                                                 const sol::optional<sol::table>& uniforms) {
    const MemoryLimitScope unlimited{allocator_, false};
    if (options_.scene == nullptr) {
      return problem_or_nil(state, {});
    }
    ShaderRequest request{.target = target, .layer = layer};
    if (shader) {
      request.shader = *shader;
    }
    if (uniforms) {
      if (std::string problem = read_uniforms(*uniforms, request.uniforms); !problem.empty()) {
        return problem_or_nil(state, problem);
      }
    }
    return problem_or_nil(state, options_.scene->set_shader(request));
  };
  // A transition is checked during a replay too, and not shown.
  engine_["transition"] = [this, problem_or_nil](sol::this_state state, const std::string& name,
                                                 const sol::table& spec) {
    const MemoryLimitScope unlimited{allocator_, false};
    if (options_.scene == nullptr) {
      return problem_or_nil(state, {});
    }
    TransitionRequest request{.name = name, .replaying = replaying_};
    if (const auto duration = spec.get<sol::optional<double>>("duration")) {
      request.duration = *duration;
    }
    if (const auto softness = spec.get<sol::optional<double>>("softness")) {
      request.softness = *softness;
    }
    if (const auto mask = spec.get<sol::optional<std::string>>("mask")) {
      request.mask = *mask;
    }
    if (const auto color = spec.get<sol::optional<std::int64_t>>("color")) {
      request.color = static_cast<std::uint32_t>(*color);
    }
    if (const auto uniforms = spec.get<sol::optional<sol::table>>("uniforms")) {
      if (std::string problem = read_uniforms(*uniforms, request.uniforms); !problem.empty()) {
        return problem_or_nil(state, problem);
      }
    }
    return problem_or_nil(state, options_.scene->transition(request));
  };
  // The parallax of a layer is state: it changes during a replay too.
  engine_["set_parallax"] = [this, problem_or_nil](sol::this_state state, const std::string& layer,
                                                   double factor) {
    const MemoryLimitScope unlimited{allocator_, false};
    return problem_or_nil(state, options_.scene == nullptr
                                     ? std::string{}
                                     : options_.scene->set_parallax(layer, factor));
  };
  // A replay, or reduced motion (`still`), checks a shake and does not show it.
  engine_["shake"] = [this, problem_or_nil](sol::this_state state, double strength,
                                            double duration, bool still) {
    const MemoryLimitScope unlimited{allocator_, false};
    return problem_or_nil(
        state, options_.scene == nullptr
                   ? std::string{}
                   : options_.scene->shake({.strength = strength,
                                            .duration = duration,
                                            .replaying = replaying_ || still}));
  };
  engine_["wait"] = sol::yielding(
      [this](sol::this_state state, const std::string& tag, std::optional<double> seconds) {
        yield_at(state, WaitRequest{.tag = tag, .seconds = seconds.value_or(0)});
      });
  // The check reads the file, so a replay (a load, a rollback) does not repeat it.
  engine_["movie_check"] = [this, problem_or_nil](sol::this_state state, const std::string& path) {
    const MemoryLimitScope unlimited{allocator_, false};
    if (options_.scene == nullptr || replaying_) {
      return problem_or_nil(state, {});
    }
    return problem_or_nil(state, options_.scene->check_movie(path));
  };
  engine_["movie"] = sol::yielding([this](sol::this_state state, const std::string& path,
                                          bool skippable) {
    yield_at(state, MovieRequest{.path = path, .skippable = skippable});
  });
  engine_["scene_command"] = [this](const std::string& command) {
    if (!replaying_) {
      observer_->on_scene_command(command);
    }
  };
  // Music, stops and volumes are state, so they also run during a replay. Sounds and voices
  // are one-shots: a replay must not play them.
  engine_["music"] = [this, problem_or_nil](
                         sol::this_state state, const std::string& name,
                         const sol::optional<std::string>& channel, sol::optional<double> fade,
                         sol::optional<double> loop_start, sol::optional<double> loop_end) {
    if (options_.audio == nullptr) {
      return problem_or_nil(state, {});
    }
    MusicRequest request;
    request.name = name;
    if (channel) {
      request.channel = *channel;
    }
    if (fade) {
      request.fade = *fade;
    }
    if (loop_start) {
      request.loop_start = *loop_start;
    }
    if (loop_end) {
      request.loop_end = *loop_end;
    }
    return problem_or_nil(state, options_.audio->music(request));
  };
  engine_["sound"] = [this, problem_or_nil](sol::this_state state, const std::string& name,
                                            const sol::optional<std::string>& channel) {
    if (options_.audio == nullptr || replaying_) {
      return problem_or_nil(state, {});
    }
    SoundRequest request;
    request.name = name;
    if (channel) {
      request.channel = *channel;
    }
    return problem_or_nil(state, options_.audio->sound(request));
  };
  engine_["voice"] = [this, problem_or_nil](sol::this_state state, const std::string& name) {
    pending_voice_ = name;  // kept during a replay too: the backlog of a loaded story needs it
    if (options_.audio == nullptr || replaying_) {
      return problem_or_nil(state, {});
    }
    return problem_or_nil(state, options_.audio->voice(name));
  };
  engine_["stop"] = [this, problem_or_nil](sol::this_state state, const std::string& channel,
                                           sol::optional<double> fade) {
    if (options_.audio == nullptr) {
      return problem_or_nil(state, {});
    }
    return problem_or_nil(state, options_.audio->stop(channel, fade ? *fade : 0));
  };
  engine_["volume"] = [this, problem_or_nil](sol::this_state state, const std::string& channel,
                                             double value) {
    if (options_.audio == nullptr) {
      return problem_or_nil(state, {});
    }
    return problem_or_nil(state, options_.audio->volume(channel, value));
  };
  engine_["audio_command"] = [this](const std::string& command) {
    if (!replaying_) {
      observer_->on_audio_command(command);
    }
  };
  engine_["save_request"] = [this](bool load, const std::string& slot) {
    if (!replaying_) {
      save_requests_.push_back(SaveRequest{.load = load, .slot = slot});
    }
  };
  engine_["skip_request"] = [this](const std::string& mode) {
    if (!replaying_) {
      const SkipMode skip = mode == "seen" ? SkipMode::kSeen
                            : mode == "all" ? SkipMode::kAll
                                            : SkipMode::kOff;
      playback_requests_.push_back(PlaybackRequest{.skip = skip});
    }
  };
  engine_["auto_request"] = [this](bool on) {
    if (!replaying_) {
      playback_requests_.push_back(PlaybackRequest{.automatic = on});
    }
  };
  engine_["set_on_load"] = [this, problem_or_nil](sol::this_state state,
                                                  const sol::stack_object& hook) {
    if (started_) {
      return problem_or_nil(state, "on_load() can only be called while scripts load");
    }
    const MemoryLimitScope unlimited{allocator_, false};
    on_load_ = hook.as<sol::main_function>();
    return problem_or_nil(state, {});
  };
  engine_["transfer"] =
      sol::yielding([this](sol::this_state state, const std::string& kind,
                           const std::string& label) { transfer(state, kind, label); });
  engine_["print"] = [this](const std::string& text) {
    if (!replaying_) {
      observer_->on_print(text);
    }
  };
  engine_["warn"] = [this](const std::string& message) {
    if (!replaying_) {
      observer_->on_warning(message);
    }
  };
  engine_["vfs_read"] = [this](sol::this_state state, const std::string& path) -> sol::object {
    const MemoryLimitScope unlimited{allocator_, false};
    const std::optional<std::vector<std::byte>> bytes = vfs_->read(path);
    if (!bytes) {
      return sol::lua_nil;
    }
    return sol::make_object(state, to_string(*bytes));
  };
  engine_["vfs_exists"] = [this](const std::string& path) { return vfs_->exists(path); };
  engine_["vfs_list"] = [this](sol::this_state state, const std::string& directory) {
    const MemoryLimitScope unlimited{allocator_, false};
    sol::state_view lua{state};
    sol::table entries = lua.create_table();
    int index = 1;
    for (const VfsEntry& entry : vfs_->list(directory)) {
      entries[index++] =
          lua.create_table_with("name", entry.name, "is_directory", entry.is_directory);
    }
    return entries;
  };
  // The RNG primitives take the state (store.__rng) and return the value and
  // the next state, which the prelude writes back to the store.
  engine_["random_integer"] = [this](const sol::stack_object& state, std::int64_t low,
                                     std::int64_t high) {
    std::uint64_t rng = random_state(state);
    // Unbiased: reject the top partial interval of the 64-bit output.
    const auto span = static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low);
    std::uint64_t value = splitmix64(rng);
    if (span != std::numeric_limits<std::uint64_t>::max()) {
      const std::uint64_t count = span + 1;
      const std::uint64_t threshold = (0 - count) % count;
      while (value < threshold) {
        value = splitmix64(rng);
      }
      value %= count;
    }
    return std::tuple{static_cast<std::int64_t>(static_cast<std::uint64_t>(low) + value),
                      static_cast<std::int64_t>(rng)};
  };
  engine_["random_float"] = [this](const sol::stack_object& state) {
    constexpr int kMantissaBits = 53;
    constexpr int kDroppedBits = std::numeric_limits<std::uint64_t>::digits - kMantissaBits;
    constexpr double kScale = 1.0 / static_cast<double>(std::uint64_t{1} << kMantissaBits);
    std::uint64_t rng = random_state(state);
    const auto value = static_cast<double>(splitmix64(rng) >> kDroppedBits) * kScale;
    return std::tuple{value, static_cast<std::int64_t>(rng)};
  };
}

// The RNG state a script holds in store.__rng. A script that removed or
// broke the field starts again from the seed.
std::uint64_t ScriptRuntime::Impl::random_state(const sol::stack_object& state) const {
  if (lua_isinteger(state.lua_state(), state.stack_index()) != 0) {
    return static_cast<std::uint64_t>(
        lua_tointegerx(state.lua_state(), state.stack_index(), nullptr));
  }
  return options_.random_seed;
}

// Copies the store breadth-first. Sorting each table's fields by key before
// numbering the tables they hold makes the result independent of Lua's
// iteration order.
StoreSnapshot ScriptRuntime::Impl::capture_table(const sol::table& root,
                                                 std::string_view name) const {
  struct Field {
    StoreValue key;
    StoreValue value;
    std::optional<sol::table> table;  // set when the value is a table
  };
  lua_State* lua = lua_.lua_state();
  const int top = lua_gettop(lua);
  StoreSnapshot snapshot;
  std::vector<sol::table> tables{root};
  std::map<const void*, std::uint32_t> indices{{root.pointer(), 0}};
  for (std::size_t current = 0; current < tables.size(); ++current) {
    std::vector<Field> fields;
    tables[current].push(lua);
    lua_pushnil(lua);
    while (lua_next(lua, -2) != 0) {
      Field field;
      const bool key_ok = read_scalar(lua, -2, field.key);
      const int value_type = lua_type(lua, -1);
      if (key_ok && value_type == LUA_TTABLE) {
        field.table = sol::table{lua, -1};
      } else if (!key_ok || !read_scalar(lua, -1, field.value)) {
        std::string problem = std::string{name} + " holds a ";
        problem += lua_typename(lua, key_ok ? value_type : lua_type(lua, -2));
        problem += key_ok ? " value" : " key";
        if (const auto* field_name = std::get_if<std::string>(&field.key);
            key_ok && field_name != nullptr) {
          problem += " (field '" + *field_name + "')";
        }
        lua_settop(lua, top);
        throw ScriptError(problem + "; it keeps only booleans, numbers, strings and tables");
      }
      fields.push_back(std::move(field));
      lua_pop(lua, 1);
    }
    lua_pop(lua, 1);
    std::sort(fields.begin(), fields.end(),
              [](const Field& a, const Field& b) { return a.key < b.key; });
    StoreTable table;
    table.entries.reserve(fields.size());
    for (Field& field : fields) {
      if (field.table) {
        const auto [found, is_new] =
            indices.try_emplace(field.table->pointer(), static_cast<std::uint32_t>(tables.size()));
        if (is_new) {
          tables.push_back(std::move(*field.table));
        }
        field.value = StoreTableRef{.index = found->second};
      }
      table.entries.emplace_back(std::move(field.key), std::move(field.value));
    }
    snapshot.tables.push_back(std::move(table));
  }
  return snapshot;
}

// Makes the store equal to `snapshot` (which is_valid). The store table
// itself is kept, because scripts hold references to it; the tables inside
// it are new.
void ScriptRuntime::Impl::restore_table(const sol::table& root, const StoreSnapshot& snapshot) {
  lua_State* lua = lua_.lua_state();
  std::vector<sol::table> tables;
  tables.reserve(snapshot.tables.size());
  tables.push_back(root);
  for (std::size_t i = 1; i < snapshot.tables.size(); ++i) {
    tables.push_back(lua_.create_table());
  }

  root.push(lua);
  lua_pushnil(lua);
  while (lua_next(lua, -2) != 0) {
    // Setting a field to nil during a traversal is allowed.
    lua_pop(lua, 1);
    lua_pushvalue(lua, -1);
    lua_pushnil(lua);
    lua_rawset(lua, -4);
  }
  lua_pop(lua, 1);

  const auto push = [lua, &tables](const StoreValue& value) {
    if (const auto* boolean = std::get_if<bool>(&value)) {
      lua_pushboolean(lua, *boolean ? 1 : 0);
    } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
      lua_pushinteger(lua, *integer);
    } else if (const auto* number = std::get_if<double>(&value)) {
      lua_pushnumber(lua, *number);
    } else if (const auto* text = std::get_if<std::string>(&value)) {
      lua_pushlstring(lua, text->data(), text->size());
    } else {
      tables[std::get<StoreTableRef>(value).index].push(lua);
    }
  };
  for (std::size_t i = 0; i < snapshot.tables.size(); ++i) {
    tables[i].push(lua);
    for (const auto& [key, value] : snapshot.tables[i].entries) {
      push(key);
      push(value);
      lua_rawset(lua, -3);
    }
    lua_pop(lua, 1);
  }
}

std::string ScriptRuntime::Impl::restore_persistent(const StoreSnapshot& snapshot) {
  if (!is_valid(snapshot)) {
    return "persistent: the snapshot is malformed";
  }
  restore_table(persistent_, snapshot);
  return {};
}

namespace {

// Pushes a ScreenValue as a Lua value.
sol::object to_lua(lua_State* lua, const ScreenValue& value) {
  return std::visit(
      [lua](const auto& held) -> sol::object {
        if constexpr (std::is_same_v<std::decay_t<decltype(held)>, std::monostate>) {
          return sol::make_object(lua, sol::lua_nil);
        } else {
          return sol::make_object(lua, held);
        }
      },
      value);
}

// The table a screen function gets: the props as fields, the children as list entries.
sol::table args_to_table(sol::state& lua, const ScreenNodeDef& def, int depth) {
  sol::table table = lua.create_table();
  for (const auto& [name, value] : def.props) {
    table[name] = to_lua(lua.lua_state(), value);
  }
  if (depth < kMaxScreenDepth) {
    std::size_t index = 1;
    for (const ScreenNodeDef& child : def.children) {
      table[index++] = args_to_table(lua, child, depth + 1);
    }
  }
  return table;
}

}  // namespace

ScreenValue ScriptRuntime::Impl::binding(std::string_view table, std::string_view key) {
  reset_instruction_budget();
  const MemoryLimitScope limited{allocator_, true};
  const sol::protected_function_result result = bridge_get_(std::string{table}, std::string{key});
  ScreenValue value;
  if (result.valid() && result.get_type() != sol::type::lua_nil) {
    const sol::object object = result.get<sol::object>();
    static_cast<void>(read_screen_value(object, value));
  }
  return value;
}

std::string ScriptRuntime::Impl::set_binding(std::string_view table, std::string_view key,
                                             ScreenValue value) {
  reset_instruction_budget();
  sol::object lua_value;
  {
    const MemoryLimitScope unlimited{allocator_, false};
    lua_value = to_lua(lua_.lua_state(), value);
  }
  const MemoryLimitScope limited{allocator_, true};
  const sol::protected_function_result result =
      bridge_set_(std::string{table}, std::string{key}, lua_value);
  if (!result.valid()) {
    return error_text(result);
  }
  return result.get_type() == sol::type::string ? result.get<std::string>() : std::string{};
}

std::string ScriptRuntime::Impl::build_screen(std::string_view name, const ScreenNodeDef& args,
                                              ScreenNodeDef& tree) {
  reset_instruction_budget();
  sol::table lua_args;
  {
    const MemoryLimitScope unlimited{allocator_, false};
    lua_args = args_to_table(lua_, args, 0);
  }
  const MemoryLimitScope limited{allocator_, true};
  const sol::protected_function_result result = bridge_build_(std::string{name}, lua_args);
  if (!result.valid()) {
    return error_text(result);
  }
  const sol::object root = result.get<sol::object>();
  if (root.get_type() != sol::type::table) {
    return "unknown screen '" + std::string{name} + "'";
  }
  const MemoryLimitScope unlimited{allocator_, false};
  tree = {};
  return read_screen_node(root.as<sol::table>(), 0, tree);
}

bool ScriptRuntime::Impl::in_label(sol::this_state state) const {
  return !frames_.empty() && state.lua_state() == frames_.back().thread.thread_state();
}

// The one engine-side path every blocking call (say, narrate, menu; later
// pause) goes through. run() sees the pending request when the coroutine
// yields, counts the step and hands the request to the engine, or answers it
// from the input log during a replay.
void ScriptRuntime::Impl::yield_at(sol::this_state state, BlockingRequest request) {
  if (!in_label(state)) {
    throw std::runtime_error("blocking calls can only be made from a running label");
  }
  pending_ = std::move(request);
}

void ScriptRuntime::Impl::transfer(sol::this_state state, std::string_view kind,
                                   std::string label) {
  if (!in_label(state)) {
    throw std::runtime_error("jump, call and ret can only be used from a running label");
  }
  TransferKind transfer_kind = TransferKind::kReturn;
  if (kind == "jump") {
    transfer_kind = TransferKind::kJump;
  } else if (kind == "call") {
    transfer_kind = TransferKind::kCall;
  }
  pending_ = Transfer{.kind = transfer_kind, .label = std::move(label)};
}

void ScriptRuntime::Impl::load_script(std::string_view path) {
  if (started_) {
    throw ScriptError("cannot load '" + std::string{path} + "': the story already started");
  }
  const std::optional<std::vector<std::byte>> bytes = vfs_->read(path);
  if (!bytes) {
    throw ScriptError("cannot read script '" + std::string{path} + "'");
  }
  const std::string& source =
      sources_.insert_or_assign(std::string{path}, to_string(*bytes)).first->second;
  run_chunk(source, "@" + std::string{path});
}

void ScriptRuntime::Impl::run_chunk(std::string_view source, const std::string& chunk_name) {
  reset_instruction_budget();
  const MemoryLimitScope limited{allocator_, true};
  sol::load_result chunk = lua_.load(source, chunk_name, sol::load_mode::text);
  if (!chunk.valid()) {
    const sol::error error = chunk;
    throw ScriptError(error.what());
  }
  sol::protected_function chunk_function = chunk;
  const sol::protected_function_result result = chunk_function();
  if (!result.valid()) {
    throw ScriptError(error_text(result));
  }
}

void ScriptRuntime::Impl::load_scripts() {
  if (!started_) {
    run_chunk(default_screens_source(), "=engine/screens.lua");  // a game's screens come later
  }
  std::vector<std::string> paths;
  std::vector<std::string> directories{"scripts"};
  while (!directories.empty()) {
    const std::string directory = std::move(directories.back());
    directories.pop_back();
    for (const VfsEntry& entry : vfs_->list(directory)) {
      std::string path = directory + "/" + entry.name;
      if (entry.is_directory) {
        directories.push_back(std::move(path));
      } else if (path.ends_with(".lua")) {
        paths.push_back(std::move(path));
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  for (const std::string& path : paths) {
    load_script(path);
  }
}

ScriptRuntime::Impl::Frame ScriptRuntime::Impl::make_frame(const std::string& name) {
  const Label& label = labels_.find(name)->second;
  sol::thread thread = sol::thread::create(lua_.lua_state());
  limits_watch_(thread);
  sol::coroutine coroutine{thread.thread_state(), label.function};
  return Frame{.label = name, .thread = std::move(thread), .coroutine = std::move(coroutine)};
}

void ScriptRuntime::Impl::start(std::string_view name) {
  if (started_) {
    throw std::logic_error("ScriptRuntime::start: the story already started");
  }
  started_ = true;
  sources_.clear();
  const std::string label{name};
  if (!has_label(label)) {
    fail("unknown label '" + label + "'");
    return;
  }
  if (enter(label)) {
    run();
  }
}

// Snapshots the state and puts label `name` on top of the stack. Returns
// false, with the runtime failed, if the store cannot be snapshotted.
bool ScriptRuntime::Impl::enter(const std::string& name) {
  LabelSnapshot snapshot;
  snapshot.label = name;
  try {
    snapshot.store = capture_store();
  } catch (const ScriptError& error) {
    fail("entering label '" + name + "': " + error.what());
    return false;
  }
  if (options_.scene_state != nullptr) {
    snapshot.scene = options_.scene_state->capture();
  }
  frames_.push_back(make_frame(name));
  frames_.back().snapshot = std::move(snapshot);
  if (!replaying_) {
    observer_->on_label_entered(name);
  }
  return true;
}

void ScriptRuntime::Impl::advance() {
  const bool at_line = std::holds_alternative<SayRequest>(request_) ||
                       std::holds_alternative<NarrateRequest>(request_) || is_pause(request_);
  if (state_ != RunState::kWaiting || !at_line) {
    return;
  }
  run();
}

void ScriptRuntime::Impl::choose(std::size_t index) {
  const auto* menu = std::get_if<MenuRequest>(&request_);
  if (state_ != RunState::kWaiting || menu == nullptr) {
    return;
  }
  if (index >= menu->options.size()) {
    throw std::out_of_range("ScriptRuntime::choose: the menu has no option " +
                            std::to_string(index));
  }
  record(MenuChoice{.index = index});
  if (menu->fixed) {
    rollback_.clear();  // the choice stays: rollback cannot go back to this menu
  }
  run(ScreenValue{static_cast<std::int64_t>(index) + 1});
}

void ScriptRuntime::Impl::return_screen(ScreenValue value) {
  if (state_ != RunState::kWaiting || !std::holds_alternative<CallScreenRequest>(request_)) {
    return;
  }
  record(ScreenResult{.value = value});
  run(std::move(value));
}

// Every label on the stack logs the answer: a caller's log covers the labels
// it called, so the caller can be replayed past the call.
void ScriptRuntime::Impl::record(const InputResult& input) {
  for (Frame& frame : frames_) {
    frame.inputs.push_back(input);
  }
}

void ScriptRuntime::Impl::replay(const LabelSnapshot& snapshot, std::span<const InputResult> inputs,
                                 std::uint32_t step) {
  rollback_.clear();  // a load starts the history again
  begin_replay(snapshot, inputs, step, 0);
}

bool ScriptRuntime::Impl::can_roll_back() const {
  if (state_ != RunState::kWaiting) {
    return false;
  }
  // At a wait the last step is the line before it; elsewhere the last step is this request.
  return rollback_.size() >= (is_pause(request_) ? 1U : 2U);
}

// Replays the label that was outermost at an earlier step, with every label it had called, to
// the step before the one the story waits at (not counting waits).
bool ScriptRuntime::Impl::roll_back() {
  if (!can_roll_back()) {
    return false;
  }
  if (!is_pause(request_)) {
    rollback_.pop_back();
  }
  const RollbackStep target = rollback_.back();  // a copy: begin_replay changes the frames
  rolling_back_ = true;
  begin_replay(target.root, target.inputs, 0, target.ordinal);
  return true;
}

// Does the work of replay(). A non-zero `ordinal` replays from the outermost label, whatever is
// running now, and so rebuilds the call stack; otherwise `step` counts in the one label.
void ScriptRuntime::Impl::begin_replay(const LabelSnapshot& snapshot,
                                       std::span<const InputResult> inputs, std::uint32_t step,
                                       std::uint32_t ordinal) {
  // Copies first: the arguments may belong to the frame replaced below.
  LabelSnapshot entry = snapshot;
  Replay replay{.inputs = {inputs.begin(), inputs.end()}, .step = step, .ordinal = ordinal};
  started_ = true;
  sources_.clear();
  pending_voice_.clear();
  error_.clear();
  if (!has_label(entry.label)) {
    fail("replay: unknown label '" + entry.label + "'");
    return;
  }
  if (!is_valid(entry.store)) {
    fail("replay: the store snapshot of label '" + entry.label + "' is malformed");
    return;
  }
  if (ordinal != 0) {
    frames_.clear();
  } else if (!frames_.empty()) {
    frames_.pop_back();
  }
  if (options_.scene_state != nullptr) {
    try {
      options_.scene_state->restore(entry.scene);
    } catch (const std::invalid_argument& error) {
      fail("replay: the scene snapshot of label '" + entry.label + "' is malformed (" +
           error.what() + ")");
      return;
    }
  }
  replaying_ = step > 0 || ordinal > 0;
  restore_store(entry.store);
  frames_.push_back(make_frame(entry.label));
  frames_.back().snapshot = std::move(entry);
  replay.depth = frames_.size();
  replay_ = std::move(replay);
  if (!replaying_) {
    observer_->on_label_entered(frames_.back().label);
  }
  run();
}

void ScriptRuntime::Impl::drop_callers() {
  if (frames_.size() > 1) {
    frames_.erase(frames_.begin(), frames_.end() - 1);
  }
}

// Runs the hook on a scratch copy of the store: the snapshot's store is restored, migrated and
// captured again, and the live store comes back at the end.
std::string ScriptRuntime::Impl::run_on_load(LabelSnapshot& snapshot,
                                             std::string_view old_version) {
  if (!on_load_.valid()) {
    return {};
  }
  if (!is_valid(snapshot.store)) {
    return "on_load: the store snapshot is malformed";
  }
  StoreSnapshot current;
  try {
    current = capture_store();
  } catch (const ScriptError& error) {
    return std::string{"on_load: "} + error.what();
  }
  restore_store(snapshot.store);
  const sol::object version = sol::make_object(lua_, std::string{old_version});
  sol::thread thread = sol::thread::create(lua_.lua_state());
  limits_watch_(thread);
  sol::coroutine hook{thread.thread_state(), on_load_};
  reset_instruction_budget();
  std::string problem;
  {
    const MemoryLimitScope limited{allocator_, true};
    const sol::protected_function_result result = hook(version, store_);
    if (!result.valid()) {
      problem = "on_load: " + error_text(result);
    } else if (result.status() == sol::call_status::yielded) {
      problem = "on_load: the hook cannot wait for the player";
    }
  }
  if (problem.empty()) {
    try {
      snapshot.store = capture_store();
    } catch (const ScriptError& error) {
      problem = std::string{"on_load: "} + error.what();
    }
  }
  restore_store(current);
  return problem;
}

// Answers the request a replay just reached from the input log:
// `resume_value` becomes what the blocking call returns. Returns false, with
// the runtime failed, if the log has no answer for it.
bool ScriptRuntime::Impl::replay_answer(std::optional<ScreenValue>& resume_value) {
  const auto* menu = std::get_if<MenuRequest>(&request_);
  const bool call_screen = std::holds_alternative<CallScreenRequest>(request_);
  if (menu == nullptr && !call_screen) {
    return true;  // say and narrate take no answer
  }
  const std::string where =
      "replay: label '" + frames_.back().label + "', step " + std::to_string(frames_.back().step);
  if (replay_.next_input >= replay_.inputs.size()) {
    fail(where + (call_screen ? ": the input log has no result for this screen"
                              : ": the input log has no choice for this menu"));
    return false;
  }
  const InputResult input = replay_.inputs[replay_.next_input++];
  if (call_screen) {
    const auto* result = std::get_if<ScreenResult>(&input);
    if (result == nullptr) {
      fail(where + ": the logged answer is not a screen result");
      return false;
    }
    record(input);
    resume_value = result->value;
    return true;
  }
  const auto* choice = std::get_if<MenuChoice>(&input);
  if (choice == nullptr || choice->index >= menu->options.size()) {
    fail(where + ": the logged choice is not an option of this menu");
    return false;
  }
  record(input);
  resume_value = ScreenValue{static_cast<std::int64_t>(choice->index) + 1};
  return true;
}

// Resumes the running label until it reaches a yield point, the story ends
// or a script fails, following jumps, calls and returns on the way.
//
// `resume_value` is what the blocking call the label waits in returns (the
// chosen option of a menu).
void ScriptRuntime::Impl::run(std::optional<ScreenValue> resume_value) {
  // One budget for the whole run, so labels that jump or call each other
  // forever without yielding hit the limit too.
  reset_instruction_budget();
  while (true) {
    state_ = RunState::kIdle;
    pending_.reset();
    bool yielded = false;
    {
      const MemoryLimitScope limited{allocator_, true};
      sol::coroutine& coroutine = frames_.back().coroutine;
      const sol::protected_function_result result =
          resume_value ? resume_with(coroutine, *resume_value) : coroutine();
      resume_value.reset();
      if (!result.valid()) {
        fail(error_text(result));
        return;
      }
      yielded = result.status() == sol::call_status::yielded;
    }
    if (yielded && !pending_) {
      fail("label '" + frames_.back().label +
           "' yielded outside an engine call (coroutine.yield in a label?)");
      return;
    }
    if (yielded && std::holds_alternative<BlockingRequest>(*pending_)) {
      Frame& frame = frames_.back();
      ++frame.step;
      ++frames_.front().stops;
      request_ = std::get<BlockingRequest>(std::move(*pending_));
      std::string voice;
      if (!is_pause(request_)) {
        voice = std::exchange(pending_voice_, std::string{});
      }
      if (replaying_ && !at_replay_target()) {
        // Not the target yet: answer from the log and go on, with a fresh
        // budget as in live play.
        if (!replay_answer(resume_value)) {
          return;
        }
        reset_instruction_budget();
        continue;
      }
      replaying_ = false;
      state_ = RunState::kWaiting;
      if (!rolling_back_ && !is_pause(request_)) {
        push_rollback_step(std::move(voice));
      }
      rolling_back_ = false;
      observer_->on_blocked(request_,
                            ScriptPosition{.label = frame.label,
                                           .step = frame.step,
                                           .wait = is_pause(request_)});
      return;
    }
    TransferKind kind = TransferKind::kReturn;
    std::string target;
    if (yielded) {
      auto& transfer = std::get<Transfer>(*pending_);
      kind = transfer.kind;
      target = std::move(transfer.label);
    }
    if (replaying_ && kind != TransferKind::kCall && frames_.size() == replay_.depth) {
      fail("replay: label '" + frames_.back().label + "' ended before step " +
           std::to_string(replay_.step));
      return;
    }
    if (kind == TransferKind::kJump) {
      frames_.pop_back();
      if (!enter(target)) {
        return;
      }
    } else if (kind == TransferKind::kCall) {
      if (frames_.size() >= kMaxCallDepth) {
        fail("label '" + frames_.back().label + "': more than " + std::to_string(kMaxCallDepth) +
             " nested calls (a call without a return?)");
        return;
      }
      if (!enter(target)) {
        return;
      }
    } else {
      frames_.pop_back();
      if (frames_.empty()) {
        state_ = RunState::kFinished;
        return;
      }
      if (!replaying_) {
        observer_->on_label_resumed(frames_.back().label);
      }
    }
  }
}

bool ScriptRuntime::Impl::at_replay_target() const {
  if (replay_.ordinal != 0) {
    return frames_.front().stops == replay_.ordinal;
  }
  return frames_.size() == replay_.depth && frames_.back().step == replay_.step;
}

// Keeps the position the story just waited at, and forgets the oldest one past the depth.
void ScriptRuntime::Impl::push_rollback_step(std::string voice) {
  RollbackStep step{.root = frames_.front().snapshot,
                    .inputs = frames_.front().inputs,
                    .ordinal = frames_.front().stops,
                    .request = request_,
                    .voice = std::move(voice)};
  step.stack.reserve(frames_.size());
  for (const Frame& frame : frames_) {
    step.stack.push_back({.label = frame.label, .step = frame.step});
  }
  rollback_.push_back(std::move(step));
  if (rollback_.size() > options_.rollback_depth + 1) {
    rollback_.erase(rollback_.begin());
  }
}

bool ScriptRuntime::Impl::activate_link(std::string_view id) {
  if (state_ != RunState::kWaiting) {
    return false;
  }
  reset_instruction_budget();
  const MemoryLimitScope limited{allocator_, true};
  const sol::protected_function_result result = link_(std::string{id});
  if (!result.valid()) {
    fail(error_text(result));
    return false;
  }
  return result.get<bool>();
}

void ScriptRuntime::Impl::fail(std::string message) {
  state_ = RunState::kFailed;
  replaying_ = false;
  rolling_back_ = false;
  error_ = std::move(message);
}

void ScriptRuntime::Impl::reset_instruction_budget() {
  if (hook_period_ != 0) {
    limits_reset_(options_.instruction_limit / hook_period_);
  }
}

CompositeSceneState::CompositeSceneState(std::vector<SceneState*> parts)
    : parts_(std::move(parts)) {}

// A u32 count of parts, then each part's capture() as a u32 length and the bytes.
std::vector<std::byte> CompositeSceneState::capture() const {
  ByteWriter writer;
  writer.u32(static_cast<std::uint32_t>(parts_.size()));
  for (const SceneState* part : parts_) {
    writer.block(part->capture());
  }
  return writer.take();
}

void CompositeSceneState::restore(std::span<const std::byte> state) {
  std::vector<std::span<const std::byte>> blocks(parts_.size());
  if (!state.empty()) {
    ByteReader reader{state, "scene state"};
    if (reader.u32() != parts_.size()) {
      throw std::invalid_argument{"scene state: made by another set of subsystems"};
    }
    for (std::span<const std::byte>& block : blocks) {
      block = reader.block();
    }
    if (!reader.at_end()) {
      throw std::invalid_argument{"scene state: unexpected data after the last part"};
    }
  }
  std::vector<std::vector<std::byte>> before;
  before.reserve(parts_.size());
  for (std::size_t i = 0; i < parts_.size(); ++i) {
    before.push_back(parts_[i]->capture());
    try {
      parts_[i]->restore(blocks[i]);
    } catch (const std::invalid_argument&) {
      for (std::size_t restored = 0; restored < i; ++restored) {
        parts_[restored]->restore(before[restored]);
      }
      throw;
    }
  }
}

ScriptRuntime::ScriptRuntime(const Vfs& vfs, ScriptRuntimeOptions options)
    : impl_(std::make_unique<Impl>(vfs, options)) {}

ScriptRuntime::~ScriptRuntime() = default;

void ScriptRuntime::load_script(std::string_view path) { impl_->load_script(path); }
void ScriptRuntime::load_scripts() { impl_->load_scripts(); }
bool ScriptRuntime::has_label(std::string_view name) const { return impl_->has_label(name); }
void ScriptRuntime::start(std::string_view name) { impl_->start(name); }
void ScriptRuntime::advance() { impl_->advance(); }
void ScriptRuntime::choose(std::size_t index) { impl_->choose(index); }
void ScriptRuntime::return_screen(ScreenValue value) { impl_->return_screen(std::move(value)); }
bool ScriptRuntime::activate_link(std::string_view id) { return impl_->activate_link(id); }
void ScriptRuntime::replay(const LabelSnapshot& snapshot, std::span<const InputResult> inputs,
                           std::uint32_t step) {
  impl_->replay(snapshot, inputs, step);
}
bool ScriptRuntime::roll_back() { return impl_->roll_back(); }
bool ScriptRuntime::can_roll_back() const { return impl_->can_roll_back(); }
std::span<const RollbackStep> ScriptRuntime::rollback_steps() const {
  return impl_->rollback_steps();
}
void ScriptRuntime::drop_callers() { impl_->drop_callers(); }
std::string ScriptRuntime::run_on_load(LabelSnapshot& snapshot, std::string_view old_version) {
  return impl_->run_on_load(snapshot, old_version);
}
std::vector<SaveRequest> ScriptRuntime::take_save_requests() { return impl_->take_save_requests(); }
std::vector<PlaybackRequest> ScriptRuntime::take_playback_requests() {
  return impl_->take_playback_requests();
}
ScreenValue ScriptRuntime::binding(std::string_view table, std::string_view key) const {
  return impl_->binding(table, key);
}
std::string ScriptRuntime::set_binding(std::string_view table, std::string_view key,
                                       ScreenValue value) {
  return impl_->set_binding(table, key, std::move(value));
}
std::string ScriptRuntime::build_screen(std::string_view name, const ScreenNodeDef& args,
                                        ScreenNodeDef& tree) {
  return impl_->build_screen(name, args, tree);
}
StoreSnapshot ScriptRuntime::persistent_snapshot() const { return impl_->capture_persistent(); }
std::string ScriptRuntime::restore_persistent(const StoreSnapshot& snapshot) {
  return impl_->restore_persistent(snapshot);
}
bool ScriptRuntime::replaying() const { return impl_->replaying(); }
const LabelSnapshot* ScriptRuntime::label_snapshot() const { return impl_->label_snapshot(); }
std::span<const InputResult> ScriptRuntime::input_log() const { return impl_->input_log(); }
StoreSnapshot ScriptRuntime::store_snapshot() const { return impl_->capture_store(); }
std::optional<std::uint64_t> ScriptRuntime::label_source_hash(std::string_view name) const {
  return impl_->label_source_hash(name);
}
RunState ScriptRuntime::state() const { return impl_->state(); }
const BlockingRequest* ScriptRuntime::request() const { return impl_->request(); }
ScriptPosition ScriptRuntime::position() const { return impl_->position(); }
const std::string& ScriptRuntime::error() const { return impl_->error(); }

}  // namespace vnmaki

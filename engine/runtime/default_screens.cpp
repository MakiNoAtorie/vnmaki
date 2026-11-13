#include "engine/runtime/default_screens.hpp"

#include <string_view>

namespace vnmaki {

namespace {

// Adjacent raw literals: MSVC limits one literal to 16380 bytes.
constexpr std::string_view kSource = R"lua(
-- One unit is a pixel of a canvas 1080 high.
local function unit()
  return canvas.height / 1080
end

-- A line of dialogue without its text tags, cut to `limit` characters, for a slot caption.
local function shorten(line, limit)
  line = line:gsub("{[^}]*}", "")
  if (utf8.len(line) or 0) > limit then
    line = line:sub(1, utf8.offset(line, limit + 1) - 1) .. "..."
  end
  return line
end

screen("title", function(args)
  local u = unit()
  local function entry(caption, action)
    return button { caption, returns = action, width = 520 * u, size = 40 * u, padding = 16 * u }
  end
  local entries = { entry("Start", "start"), entry("Load", "@open:load"),
                    entry("Preferences", "@open:preferences"), entry("Quit", "@quit") }
  if #gallery_images() > 0 then  -- project.toml lists a gallery
    table.insert(entries, 3, entry("Gallery", "@open:gallery"))
  end
  return vbox {
    align = "center", justify = "center", gap = 24 * u, bg = "#101020",
    text { args.title or "", size = 96 * u },
    table.unpack(entries),
  }
end)

-- The say box. `args` has speaker, text (with tags) and color ("#rrggbb"). The text node with
-- `reveal = true` is the one the typewriter shows glyph by glyph.
screen("say", function(args)
  local w, h = canvas.width, canvas.height
  local margin, pad = w // 16, w // 64
  local box_height = h * 7 // 24
  local name_size = h // 32
  return box {
    box {
      x = margin, y = h - h // 24 - box_height, width = w - 2 * margin, height = box_height,
      bg = "#101020cc",
      text { args.speaker or "", x = pad, y = pad, size = name_size, color = args.color or "#ffffff" },
      text { args.text or "", markup = true, reveal = true, x = pad,
             y = pad + name_size + pad // 2, width = w - 2 * margin - 2 * pad, size = h // 24 },
    },
  }
end)

-- The choice menu. `args` is the list of options, each { text = "..." }. A button returns the
-- number of its option, from 1.
screen("choice", function(args)
  local buttons = {}
  for i, option in ipairs(args) do
    buttons[i] = button { option.text, returns = i, width = canvas.width // 2,
                          size = canvas.height // 24, padding = canvas.height // 48 }
  end
  return vbox { align = "center", justify = "center", gap = canvas.height // 40, bg = "#00000080",
                table.unpack(buttons) }
end)

-- `args` has message, yes and no: the strings the two buttons return (Esc returns no).
screen("confirm", function(args)
  local u = unit()
  local function answer(caption, value)
    return button { caption, returns = value, width = 240 * u, size = 36 * u, padding = 14 * u }
  end
  return vbox {
    modal = true, cancel = args.no, align = "center", justify = "center", gap = 32 * u,
    bg = "#000000c0",
    text { args.message or "Are you sure?", size = 48 * u },
    hbox { gap = 24 * u, answer("Yes", args.yes), answer("No", args.no) },
  }
end)

-- The slot grid of save and load. `args` has mode ("save" or "load") and one entry for each
-- slot: slot (name), title, time, empty, broken and thumbnail (an image name; "" for none).
local function slot_screen(args)
  local u = unit()
  local saving = args.mode == "save"
  local cells = {}
  for i, slot in ipairs(args) do
    local preview
    if slot.thumbnail and slot.thumbnail ~= "" then
      preview = image { slot.thumbnail, width = 270 * u, height = 152 * u }
    else
      preview = box { width = 270 * u, height = 152 * u, bg = "#202038", align = "center",
                      justify = "center",
                      text { slot.empty and "Empty" or "No preview", size = 26 * u } }
    end
    local name = tonumber(slot.slot) and ("Slot " .. slot.slot)
                 or (slot.slot:sub(1, 1):upper() .. slot.slot:sub(2))
    local when = slot.empty and "" or os.date("%Y-%m-%d %H:%M", slot.time)
    cells[i] = button {
      returns = (saving and "@save:" or "@load:") .. slot.slot,
      disabled = not saving and (slot.empty or slot.broken) or false,
      label = name .. (slot.empty and ", empty" or ", " .. shorten(slot.title, 40)),
      width = 290 * u, height = 250 * u, padding = 10 * u,
      preview,
      text { name .. "  " .. when, size = 22 * u },
      text { slot.empty and "" or shorten(slot.title, 26), size = 20 * u, color = "#b8c0e0" },
    }
  end
  return vbox {
    modal = true, cancel = "@close", bg = "#101020f2", padding = 40 * u, gap = 20 * u,
    text { saving and "Save" or "Load", size = 64 * u },
    grid { columns = 6, gap = 12 * u, table.unpack(cells) },
    button { "Back", returns = "@close", width = 300 * u, size = 34 * u, padding = 14 * u },
  }
end
screen("save", slot_screen)
screen("load", slot_screen)
)lua" R"lua(
-- Every control is bound to `prefs`, so a change takes effect at once and is kept.
screen("preferences", function()
  local u = unit()
  local function range(key, low, high, step, caption)
    return hbox {
      gap = 24 * u, align = "center",
      text { caption, size = 32 * u, width = 480 * u },
      slider { bind = bind(prefs, key), min = low, max = high, step = step, width = 700 * u,
               label = caption },
    }
  end
  local function switch(key, caption)
    return toggle { caption, bind = bind(prefs, key), size = 32 * u, padding = 10 * u,
                    width = 480 * u }
  end
  return vbox {
    modal = true, cancel = "@close", bg = "#101020f2", padding = 40 * u, gap = 14 * u,
    text { "Preferences", size = 64 * u },
    range("text_speed", 10, 120, 5, "Text speed"),
    range("auto_delay", 0, 5, 0.25, "Auto-advance delay"),
    range("volume_music", 0, 1, 0.05, "Music volume"),
    range("volume_sound", 0, 1, 0.05, "Sound volume"),
    range("volume_voice", 0, 1, 0.05, "Voice volume"),
    range("volume_ambient", 0, 1, 0.05, "Ambient volume"),
    range("volume_ui", 0, 1, 0.05, "Interface volume"),
    range("window_opacity", 0.3, 1, 0.05, "Window opacity"),
    switch("fullscreen", "Fullscreen"),
    switch("skip_unseen", "Skip text not read yet"),
    switch("reduce_motion", "Reduce motion"),
    button { "Back", returns = "@close", width = 300 * u, size = 34 * u, padding = 14 * u },
  }
end)

-- Top right, over the story. The engine shows it (the story window does); a game hides it with
-- hide_screen("quick_menu"). Tab (or the gamepad's west button) gives it the keys.
screen("quick_menu", function()
  local u = unit()
  local function entry(caption, action)
    return button { caption, returns = action, size = 24 * u, padding = 8 * u, bg = "#10102099" }
  end
  return vbox {
    align = "end", padding = 12 * u,
    hbox {
      gap = 8 * u,
      entry("Save", "@open:save"), entry("Load", "@open:load"), entry("Auto", "@auto"),
      entry("Skip", "@skip"), entry("Log", "@backlog"), entry("Prefs", "@open:preferences"),
    },
  }
end)

-- The NVL screen: the page of lines over the whole canvas. `args` has `lines` (how many page
-- lines come first), `typing` (the last line is on the typewriter: its text node has `reveal`),
-- the lines (speaker, text, color) and then the options of a menu (text). An option returns its
-- number, from 1.
screen("nvl", function(args)
  local u = unit()
  local size, margin = 34 * u, 100 * u
  local width = canvas.width - 2 * margin
  local nodes = {}
  for i = 1, args.lines do
    local line = args[i]
    if line.speaker ~= "" then
      nodes[#nodes + 1] = text { line.speaker, size = size, color = line.color or "#ffffff" }
    end
    nodes[#nodes + 1] = text { line.text, markup = true, reveal = args.typing and i == args.lines,
                               width = width, size = size }
  end
  for i = args.lines + 1, #args do
    nodes[#nodes + 1] = button { args[i].text, returns = i - args.lines, width = width // 2,
                                 size = size, padding = 12 * u }
  end
  return vbox { bg = "#101020e6", padding = margin, gap = 12 * u, table.unpack(nodes) }
end)

-- The gallery: a grid of the images of project.toml `gallery`. An unlocked image is a button that
-- shows it full screen; any other is a locked placeholder.
screen("gallery", function()
  local u = unit()
  local cells = {}
  for i, name in ipairs(gallery_images()) do
    if unlocked("cg", name) then
      cells[i] = button { returns = "@view:" .. name, label = name, width = 300 * u,
                          height = 180 * u, padding = 8 * u,
                          image { name, width = 284 * u, height = 160 * u } }
    else
      cells[i] = button { disabled = true, label = "Locked", width = 300 * u, height = 180 * u,
                          padding = 8 * u,
                          box { width = 284 * u, height = 160 * u, bg = "#202038", align = "center",
                                justify = "center", text { "Locked", size = 26 * u } } }
    end
  end
  return vbox {
    modal = true, cancel = "@close", bg = "#101020f2", padding = 40 * u, gap = 20 * u,
    text { "Gallery", size = 64 * u },
    #cells > 0 and grid { columns = 5, gap = 12 * u, table.unpack(cells) }
        or text { "This game has no gallery.", size = 32 * u },
    button { "Back", returns = "@close", width = 300 * u, size = 34 * u, padding = 14 * u },
  }
end)

-- One unlocked image, full screen. `args` has image. A click, Enter or Esc goes back.
screen("gallery_view", function(args)
  return vbox {
    modal = true, cancel = "@close", bg = "#000000",
    button { returns = "@close", label = args.image, width = canvas.width, height = canvas.height,
             padding = 0, image { args.image, width = canvas.width, height = canvas.height } },
  }
end)
)lua";

}  // namespace

std::string_view default_screens_source() { return kSource; }

}  // namespace vnmaki

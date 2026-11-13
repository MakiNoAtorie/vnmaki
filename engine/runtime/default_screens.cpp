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

-- The look of the screens below, as named styles (docs/SPEC.md section 15): a game or a theme
-- (styles/<name>.lua) changes a screen by declaring a style with the same name again.
local u = unit()
local h = canvas.height
style("title_screen", { background = "#101020", gap = 24 * u, align = "center", justify = "center" })
style("title_heading", { size = 96 * u })
style("title_button", { width = 520 * u, size = 40 * u, padding = 16 * u })
style("say_box", { background = "#101020cc" })
style("say_name", { size = h // 32, color = "#ffffff" })
style("say_text", { size = h // 24 })
style("choice_screen", { background = "#00000080", gap = h // 40, align = "center",
                         justify = "center" })
style("choice_button", { width = canvas.width // 2, size = h // 24, padding = h // 48 })
style("dialog_screen", { background = "#000000c0", gap = 32 * u, align = "center",
                         justify = "center" })
style("dialog_message", { size = 48 * u })
style("dialog_buttons", { gap = 24 * u })
style("dialog_button", { width = 240 * u, size = 36 * u, padding = 14 * u })
style("panel", { background = "#101020f2", padding = 40 * u, gap = 20 * u })
style("panel_title", { size = 64 * u })
style("back_button", { width = 300 * u, size = 34 * u, padding = 14 * u })
style("slot_grid", { gap = 12 * u })
style("slot_cell", { width = 290 * u, height = 250 * u, padding = 10 * u })
style("slot_image", { width = 270 * u, height = 152 * u })
style("slot_preview", { extends = "slot_image", background = "#202038", align = "center",
                        justify = "center" })
style("slot_preview_caption", { size = 26 * u })
style("slot_name", { size = 22 * u })
style("slot_title", { size = 20 * u, color = "#b8c0e0" })
style("prefs_panel", { extends = "panel", gap = 14 * u })
style("pref_row", { gap = 24 * u, align = "center" })
style("pref_label", { size = 32 * u, width = 480 * u })
style("pref_slider", { width = 700 * u })
style("pref_switch", { size = 32 * u, padding = 10 * u, width = 480 * u })
style("quick_bar", { align = "end", padding = 12 * u })
style("quick_row", { gap = 8 * u })
style("quick_button", { size = 24 * u, padding = 8 * u, background = "#10102099" })
style("nvl_screen", { background = "#101020e6", padding = 100 * u, gap = 12 * u })
style("nvl_name", { size = 34 * u, color = "#ffffff" })
style("nvl_text", { size = 34 * u, width = canvas.width - 200 * u })
style("nvl_choice", { width = (canvas.width - 200 * u) // 2, size = 34 * u, padding = 12 * u })
style("gallery_cell", { width = 300 * u, height = 180 * u, padding = 8 * u })
style("gallery_image", { width = 284 * u, height = 160 * u })
style("gallery_locked", { extends = "gallery_image", background = "#202038", align = "center",
                          justify = "center" })
style("gallery_empty", { size = 32 * u })
style("gallery_view", { background = "#000000" })
style("gallery_full", { width = canvas.width, height = canvas.height, padding = 0 })
style("gallery_full_image", { width = canvas.width, height = canvas.height })

-- A line of dialogue without its text tags, cut to `limit` characters, for a slot caption.
local function shorten(line, limit)
  line = line:gsub("{[^}]*}", "")
  if (utf8.len(line) or 0) > limit then
    line = line:sub(1, utf8.offset(line, limit + 1) - 1) .. "..."
  end
  return line
end

screen("title", function(args)
  local function entry(caption, action)
    return button { caption, returns = action, style = "title_button" }
  end
  local entries = { entry("Start", "start"), entry("Load", "@open:load"),
                    entry("Preferences", "@open:preferences"), entry("Quit", "@quit") }
  if #gallery_images() > 0 then  -- project.toml lists a gallery
    table.insert(entries, 3, entry("Gallery", "@open:gallery"))
  end
  return vbox {
    style = "title_screen",
    text { args.title or "", style = "title_heading" },
    table.unpack(entries),
  }
end)

-- The say box. `args` has speaker, text (with tags) and color ("#rrggbb"). The text node with
-- `reveal = true` is the one the typewriter shows glyph by glyph.
screen("say", function(args)
  local w = canvas.width
  local margin, pad = w // 16, w // 64
  local box_height = h * 7 // 24
  local name_size = h // 32
  return box {
    box {
      style = "say_box",
      x = margin, y = h - h // 24 - box_height, width = w - 2 * margin, height = box_height,
      text { args.speaker or "", style = "say_name", x = pad, y = pad, color = args.color },
      text { args.text or "", style = "say_text", markup = true, reveal = true, x = pad,
             y = pad + name_size + pad // 2, width = w - 2 * margin - 2 * pad },
    },
  }
end)

-- The choice menu. `args` is the list of options, each { text = "..." }. A button returns the
-- number of its option, from 1.
screen("choice", function(args)
  local buttons = {}
  for i, option in ipairs(args) do
    buttons[i] = button { option.text, returns = i, style = "choice_button" }
  end
  return vbox { style = "choice_screen", table.unpack(buttons) }
end)

-- `args` has message, yes and no: the strings the two buttons return (Esc returns no).
screen("confirm", function(args)
  local function answer(caption, value)
    return button { caption, returns = value, style = "dialog_button" }
  end
  return vbox {
    style = "dialog_screen", modal = true, cancel = args.no,
    text { args.message or "Are you sure?", style = "dialog_message" },
    hbox { style = "dialog_buttons", answer("Yes", args.yes), answer("No", args.no) },
  }
end)

-- The slot grid of save and load. `args` has mode ("save" or "load") and one entry for each
-- slot: slot (name), title, time, empty, broken and thumbnail (an image name; "" for none).
local function slot_screen(args)
  local saving = args.mode == "save"
  local cells = {}
  for i, slot in ipairs(args) do
    local preview
    if slot.thumbnail and slot.thumbnail ~= "" then
      preview = image { slot.thumbnail, style = "slot_image" }
    else
      preview = box { style = "slot_preview",
                      text { slot.empty and "Empty" or "No preview",
                             style = "slot_preview_caption" } }
    end
    local name = tonumber(slot.slot) and ("Slot " .. slot.slot)
                 or (slot.slot:sub(1, 1):upper() .. slot.slot:sub(2))
    local when = slot.empty and "" or os.date("%Y-%m-%d %H:%M", slot.time)
    cells[i] = button {
      style = "slot_cell",
      returns = (saving and "@save:" or "@load:") .. slot.slot,
      disabled = not saving and (slot.empty or slot.broken) or false,
      label = name .. (slot.empty and ", empty" or ", " .. shorten(slot.title, 40)),
      preview,
      text { name .. "  " .. when, style = "slot_name" },
      text { slot.empty and "" or shorten(slot.title, 26), style = "slot_title" },
    }
  end
  return vbox {
    style = "panel", modal = true, cancel = "@close",
    text { saving and "Save" or "Load", style = "panel_title" },
    grid { columns = 6, style = "slot_grid", table.unpack(cells) },
    button { "Back", returns = "@close", style = "back_button" },
  }
end
screen("save", slot_screen)
screen("load", slot_screen)
)lua" R"lua(
-- Every control is bound to `prefs`, so a change takes effect at once and is kept.
screen("preferences", function()
  local function range(key, low, high, step, caption)
    return hbox {
      style = "pref_row",
      text { caption, style = "pref_label" },
      slider { bind = bind(prefs, key), min = low, max = high, step = step, label = caption,
               style = "pref_slider" },
    }
  end
  local function switch(key, caption)
    return toggle { caption, bind = bind(prefs, key), style = "pref_switch" }
  end
  return vbox {
    style = "prefs_panel", modal = true, cancel = "@close",
    text { "Preferences", style = "panel_title" },
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
    button { "Back", returns = "@close", style = "back_button" },
  }
end)

-- Top right, over the story. The engine shows it (the story window does); a game hides it with
-- hide_screen("quick_menu"). Tab (or the gamepad's west button) gives it the keys.
screen("quick_menu", function()
  local function entry(caption, action)
    return button { caption, returns = action, style = "quick_button" }
  end
  return vbox {
    style = "quick_bar",
    hbox {
      style = "quick_row",
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
  local nodes = {}
  for i = 1, args.lines do
    local line = args[i]
    if line.speaker ~= "" then
      nodes[#nodes + 1] = text { line.speaker, style = "nvl_name", color = line.color }
    end
    nodes[#nodes + 1] = text { line.text, style = "nvl_text", markup = true,
                               reveal = args.typing and i == args.lines }
  end
  for i = args.lines + 1, #args do
    nodes[#nodes + 1] = button { args[i].text, returns = i - args.lines, style = "nvl_choice" }
  end
  return vbox { style = "nvl_screen", table.unpack(nodes) }
end)

-- The gallery: a grid of the images of project.toml `gallery`. An unlocked image is a button that
-- shows it full screen; any other is a locked placeholder.
screen("gallery", function()
  local cells = {}
  for i, name in ipairs(gallery_images()) do
    if unlocked("cg", name) then
      cells[i] = button { returns = "@view:" .. name, label = name, style = "gallery_cell",
                          image { name, style = "gallery_image" } }
    else
      cells[i] = button { disabled = true, label = "Locked", style = "gallery_cell",
                          box { style = "gallery_locked",
                                text { "Locked", style = "slot_preview_caption" } } }
    end
  end
  return vbox {
    style = "panel", modal = true, cancel = "@close",
    text { "Gallery", style = "panel_title" },
    #cells > 0 and grid { columns = 5, style = "slot_grid", table.unpack(cells) }
        or text { "This game has no gallery.", style = "gallery_empty" },
    button { "Back", returns = "@close", style = "back_button" },
  }
end)

-- One unlocked image, full screen. `args` has image. A click, Enter or Esc goes back.
screen("gallery_view", function(args)
  return vbox {
    style = "gallery_view", modal = true, cancel = "@close",
    button { returns = "@close", label = args.image, style = "gallery_full",
             image { args.image, style = "gallery_full_image" } },
  }
end)
)lua";

}  // namespace

std::string_view default_screens_source() { return kSource; }

}  // namespace vnmaki

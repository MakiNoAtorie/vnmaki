-- The high contrast look (prefs.high_contrast): the engine runs this file over the game's styles.
-- A style that it names keeps the options that this file does not give, so only colours and
-- frames change. Black grounds, white text and frames, a yellow focus.
local u = canvas.height / 1080
local white, black, yellow = "#ffffff", "#000000", "#ffff00"
local frame = { width = 3 * u, color = white }

-- Buttons and switches.
for _, name in ipairs {
  "title_button", "choice_button", "dialog_button", "back_button", "slot_cell", "quick_button",
  "nvl_choice", "gallery_cell", "pref_switch", "control_button",
} do
  style(name, {
    background = black, color = white, border = frame,
    focus = { background = yellow, color = black, border = black },
    hover = { background = yellow, color = black, border = black },
  })
end
style("pref_slider", { background = black, fill = yellow, border = frame,
                       focus = { border = yellow } })

-- Screens and panels.
for _, name in ipairs { "title_screen", "nvl_screen", "gallery_view" } do
  style(name, { background = black })
end
style("choice_screen", { background = "#000000e6" })
style("dialog_screen", { background = "#000000e6" })
style("panel", { background = black, border = frame })
style("say_box", { background = "#000000f2", border = frame })
style("caption_line", { background = black, border = frame })
style("slot_preview", { background = black })
style("gallery_locked", { background = black })
style("slot_title", { color = white })

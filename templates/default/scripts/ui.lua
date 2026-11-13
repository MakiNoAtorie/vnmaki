-- The quick menu (top right), written as a table so that the screen editor (F12) can change its
-- numbers and texts. It replaces the engine's screen of the same name and looks the same.
screen "quick_menu" {
  vbox { style = "quick_bar",
    hbox { style = "quick_row", gap = 6,
      button { "Save", returns = "@open:save", style = "quick_button" },
      button { "Load", returns = "@open:load", style = "quick_button" },
      button { "Auto", returns = "@auto", style = "quick_button" },
      button { "Skip", returns = "@skip", style = "quick_button" },
      button { "Log", returns = "@backlog", style = "quick_button" },
      button { "Prefs", returns = "@open:preferences", style = "quick_button" },
    },
  },
}

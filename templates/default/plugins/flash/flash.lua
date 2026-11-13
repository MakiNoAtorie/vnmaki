-- A Lua plugin (docs/plugins.md): the transition "flash" is the built-in "fade" through white, in
-- one second. Use it in a script as transition("flash") or with = "flash".
define_transition("flash", "fade", { color = "#ffffff", duration = 1 })

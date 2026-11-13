-- A short sample story. Change it, then play it with: vnmaki run <this folder>

local eileen = character { name = "Eileen", color = "#c8a2c8", voice_tag = "eileen" }

-- A sprite made of layers: a group of images is chosen by name in show().
layered("eileen", {
  order = { "base", "outfit", "face" },
  base = "eileen/base",
  outfit = { school = "eileen/outfit_school" },
  face = { smile = "eileen/face_smile", sad = "eileen/face_sad" },
  default = { outfit = "school", face = "smile" },
})

-- All game state is in `store`. Saves and rollback keep it.
store.affection = 0

label("start", function()
  call_screen("title", { title = "{{title}}" })
  jump("classroom")
end)

label("classroom", function()
  music("bgm/theme", { fade = 1.5 })
  scene("bg/classroom", { with = "dissolve" })
  show("eileen", { outfit = "school", face = "smile", at = "center" })
  say(eileen, "You came back.")

  local c = menu {
    "Of course I did.",
    "I forgot my bag.",
  }

  if c == 1 then
    store.affection = store.affection + 1
    jump("warm")
  else
    jump("cold")
  end
end)

label("warm", function()
  say(eileen, "I'm glad. I kept your seat by the window.")
  say(eileen, "Let's walk home together.")
  narrate("The end. Open scripts/main.lua to write your own story.")
end)

label("cold", function()
  show("eileen", { face = "sad" })
  say(eileen, "Oh. So that is the only reason.")
  say(eileen, "Your bag is on your desk.")
  narrate("The end. Open scripts/main.lua to write your own story.")
end)

-- A short sample story. Change it, then play it with: vnmaki run <this folder>

local eileen = character { name = "Eileen", color = "#c8a2c8", voice_tag = "eileen" }

-- A blink: an animation of images. The eyelids stay open for most of it.
local blink = {}
for i = 1, 14 do
  blink[i] = "eileen/lids_open"
end
blink[15], blink[16], blink[17] = "eileen/lids_half", "eileen/lids_closed", "eileen/lids_half"
animation("eileen_blink", { frames = blink, fps = 8, loop = true })

-- A sprite made of layers: a group of images is chosen by name in show(). A new face fades in over
-- the old one. The mouth follows the voice of the character with voice_tag "eileen" (lip sync).
layered("eileen", {
  order = { "base", "outfit", "face", "lids", "mouth" },
  base = "eileen/base",
  outfit = { school = "eileen/outfit_school" },
  face = { smile = "eileen/face_smile", sad = "eileen/face_sad" },
  lids = { blink = "eileen_blink" },
  mouth = { closed = "eileen/mouth_closed", half = "eileen/mouth_half",
            open = "eileen/mouth_open" },
  default = { outfit = "school", face = "smile", lids = "blink", mouth = "closed" },
  crossfade = 0.25,
  lip_sync = { group = "mouth", frames = { "closed", "half", "open" },
               thresholds = { 0.05, 0.2 }, voice_tag = "eileen" },
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
    "Never mind that. What can you do?",
  }

  if c == 1 then
    store.affection = store.affection + 1
    jump("warm")
  elseif c == 2 then
    jump("cold")
  else
    jump("presentation")
  end
end)

label("warm", function()
  say(eileen, "I'm glad. I kept your seat by the window.")
  say(eileen, "Let's walk home together.")
  narrate("The end. Open scripts/main.lua to write your own story.")
end, { ending = true })

label("cold", function()
  show("eileen", { face = "sad" })
  say(eileen, "Oh. So that is the only reason.")
  say(eileen, "Your bag is on your desk.")
  narrate("The end. Open scripts/main.lua to write your own story.")
end, { ending = true })

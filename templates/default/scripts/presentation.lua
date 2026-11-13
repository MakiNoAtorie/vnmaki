-- The presentation demo, reached from the menu in main.lua. It shows the picture and sound
-- features of the engine in about a minute. docs/script-reference.md of VNMaki describes each call.

-- A game shader (shaders/sepia.frag.hlsl) with one number that scripts can change: "amount".
shader("sepia", { "amount" })

local eileen = character { name = "Eileen", color = "#c8a2c8", voice_tag = "eileen" }

label("presentation", function()
  -- A dissolve covers the change of the backdrop and the sprite. The sprite is a layered one
  -- (see main.lua) and blinks all the time.
  scene("bg/rooftop")
  show("eileen", { at = "center", with = { "dissolve", duration = 1 } })
  say(eileen, "This is the presentation demo.")

  -- A new face fades in over the old one.
  show("eileen", { face = "sad" })
  say(eileen, "A new face fades in over the old one.")

  -- The camera pans and shakes. The background has a parallax of 0, so only the sprite moves.
  layer("background", { parallax = 0 })
  camera { x = 0.0625, duration = 1, ease = "in_out_quad" }
  shake { strength = 0.02, duration = 0.5 }
  say(eileen, "The camera pans and shakes. The backdrop stays.")
  wait("@camera")

  -- A shader on a layer, and a tween of its number.
  layer("background", { shader = "sepia", uniforms = { amount = 0 } })
  tween("@layer.background", { uniforms = { amount = 1 }, duration = 1, ease = "linear" })
  say(eileen, "A sepia shader on the backdrop layer fades in.")
  wait("@layer.background")

  -- A voice line. The mouth of the sprite follows the level of the voice.
  show("eileen", { face = "smile" })
  voice("eileen/hello")
  say(eileen, "Now my mouth follows my voice.")

  -- A short movie. A click skips it.
  say(eileen, "A short movie follows.")
  movie("movies/clip.webm", { skippable = true })

  -- A mask transition back to the classroom.
  scene("bg/classroom")
  camera { x = 0 }
  layer("background", { shader = "", parallax = 1 })
  show("eileen", {
    at = "center",
    with = { "mask", mask = "mask/iris", softness = 0.3, duration = 1.5 },
  })
  say(eileen, "That was the presentation demo. Edit scripts/presentation.lua to change it.")
  narrate("The end. Open scripts/main.lua to write your own story.")
end)

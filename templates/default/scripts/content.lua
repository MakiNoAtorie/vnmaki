-- The content demo, reached from the last option of the menu in main.lua. It shows a language
-- switch, captions, a 3D model with a clip and a transition from a plugin (plugins/flash/). The
-- calls are in docs/script-reference.md, docs/localization.md, docs/accessibility.md and
-- docs/plugins.md of VNMaki.

local eileen = character { name = "Eileen", color = "#c8a2c8", voice_tag = "eileen" }

label("content", function()
  scene("bg/classroom")
  show("eileen", { at = "center" })
  say(eileen, "This is the content demo: a language, captions, a 3D model and a plugin.")

  -- The language switch, with a second language (`vnmaki new <name> --lang ja`). "" is the first.
  if #languages() > 1 then
    say(eileen, "First the language. Which one do you want?")
    local language = menu { "English", "Japanese" }
    prefs.language = language == 1 and "" or "ja"
    say(eileen, "The story is the same in both languages. Prefs also has a language button.")
  end
  -- A caption says what a sound is. It shows when Captions is on in the preferences.
  prefs.captions = true
  sound("sfx/chime", { caption = "a chime rings" })
  say(eileen, "Captions name the sounds. A chime rang, and its caption is at the top left.")

  -- A 3D model with a clip of its glTF file. The arm slides to the right.
  hide("eileen")
  scene3d("stage")
  camera("stage", { position = { 0, 2, 6 }, look_at = { 0, 2, 0 }, fov = 90 })
  model("arm", "models/arm.gltf", { scene = "stage", scale = 2 })
  say(eileen, "This is a 3D model, an arm with two joints.")
  play("@3d.stage.arm", "slide")
  say(eileen, "It plays the clip slide of its file.")
  wait("@3d.stage.arm")

  -- A transition from a Lua plugin: "flash" is defined in plugins/flash/.
  hide("stage")
  say(eileen, "A plugin adds the next transition.")
  scene("bg/rooftop")
  show("eileen", { at = "center", with = "flash" })
  say(eileen, "That was the content demo. Edit scripts/content.lua to change it.")
  narrate("The end. Open scripts/main.lua to write your own story.")
end, { ending = true })

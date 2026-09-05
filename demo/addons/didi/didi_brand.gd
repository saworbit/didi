@tool
extends RefCounted

## Didi's own mark, rasterised for the editor at whatever scale the editor is
## running at.
##
## The brand sources are SVG and single-colour (`currentColor`), which is what
## makes them usable here: the colour is substituted for the editor theme's own
## font colour before rasterising, so the mark reads correctly on the light and
## dark editor themes without shipping two of every asset.
##
## Rasterising from the SVG text rather than `load()`ing the file is deliberate.
## An imported texture has one fixed pixel size, and a 16 px icon imported once
## is blurry on a 2x display. Going through `Image.load_svg_from_string` renders
## at the exact device size every time, and leaves the import pipeline out of it
## entirely -- the addon works the moment it is copied into a project, with no
## reimport step.

const MARK_COMPACT := "res://addons/didi/didi_mark_compact.svg"
const MARK := "res://addons/didi/didi_mark.svg"
const SIGNATURE := "res://addons/didi/didi_signature.svg"

## The brand palette, from docs/brand/BRAND.md. These are the two colours the
## README badges already used; the mark is the bridge between exactly them.
const MCP_VIOLET := Color("8a2be2")
const GODOT_BLUE := Color("478cbf")
const SLATE := Color("6a5acd")

## Below 20 px the master weight thins out under rasterisation, so the brand
## ships a heavier compact cut for small sizes. Picking between them by size is
## the whole reason both files are here.
const COMPACT_MAX_PX := 20.0

static var _cache: Dictionary = {}

## The mark at `height_px` logical pixels, tinted `color`.
##
## Returns null rather than a placeholder when anything fails. A caller that
## gets null falls back to an editor icon, which is a worse-looking panel and a
## working one; a caller that got a broken texture would show a black square and
## have no way to know it was wrong.
static func mark(height_px: float, color: Color) -> Texture2D:
	var source := MARK_COMPACT if height_px <= COMPACT_MAX_PX else MARK
	return _render(source, height_px, color)


## The signature lockup -- mark and wordmark as one object -- at `height_px`.
##
## The brand forbids setting the mark beside the wordmark with a gap, because
## the mark is itself a lowercase `d` and the pair reads as "d didi". The
## signature is the resolution, so the panel header uses it rather than
## composing a lockup of its own.
static func signature(height_px: float, color: Color) -> Texture2D:
	return _render(SIGNATURE, height_px, color)


## The editor's own font colour, which is what the mark should be drawn in so it
## sits with the rest of the editor chrome rather than against it.
static func editor_ink(base: Control) -> Color:
	if base != null and base.has_theme_color("font_color", "Label"):
		return base.get_theme_color("font_color", "Label")
	return Color(0.875, 0.875, 0.875)


## The device pixels one logical editor pixel occupies. 1.0 off a normal
## display, 2.0 on a HiDPI one, and any of the fractional steps the editor
## offers in between.
static func editor_scale() -> float:
	if Engine.is_editor_hint():
		var scale := EditorInterface.get_editor_scale()
		if scale > 0.0:
			return scale
	return 1.0


static func _render(path: String, height_px: float, color: Color) -> Texture2D:
	if height_px <= 0.0:
		return null
	var device_height := height_px * editor_scale()
	# Rounded so that two requests a fraction of a pixel apart share one cache
	# entry instead of rasterising twice for the same visible result.
	var key := "%s|%d|%s" % [path, int(round(device_height)), color.to_html(false)]
	if _cache.has(key):
		return _cache[key]

	var svg := _read_text(path)
	if svg.is_empty():
		return null
	var intrinsic := _view_box_height(svg)
	if intrinsic <= 0.0:
		return null

	# Every brand source paints in `currentColor`, which has no meaning outside a
	# document that sets it -- the rasteriser would draw black. Substituting the
	# requested colour here is what makes one file serve both editor themes.
	svg = svg.replace("currentColor", "#" + color.to_html(false))

	var image := Image.new()
	if image.load_svg_from_string(svg, round(device_height) / intrinsic) != OK:
		return null
	var texture := ImageTexture.create_from_image(image)
	_cache[key] = texture
	return texture


static func _read_text(path: String) -> String:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return ""
	var text := file.get_as_text()
	file.close()
	return text


## The height the source draws on, read from its own viewBox rather than
## assumed. The brand regenerates its geometry from one set of constants, so a
## hardcoded 100 here would go stale the first time that grid changed.
static func _view_box_height(svg: String) -> float:
	var start := svg.find("viewBox=\"")
	if start < 0:
		return 0.0
	start += 9
	var end := svg.find("\"", start)
	if end < 0:
		return 0.0
	var parts := svg.substr(start, end - start).split(" ", false)
	if parts.size() != 4:
		return 0.0
	return float(parts[3])

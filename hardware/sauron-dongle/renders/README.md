# Desk setup visualization

Final image: [desk-setup.png](desk-setup.png).

Generated with the built-in imagegen tool using the current `../preview.png`
and `../orthographic.png` as geometry references. The CAD and enclosure files
were not changed. This is an illustrative visualization, not a measured CAD
render; use `../validation.json` for authoritative dimensions.

- Assembled enclosure: 30 × 53.5 × 99.86 mm (width × depth × height).
- Nice!View module: 14 × 36 mm in portrait, tilted back 12 degrees.
- Desk scale references requested: 95 mm tall / 80 mm diameter mug and 19 mm key pitch.
- Right-hand detail is enlarged independently of the main scene.
- Display content and surrounding desk equipment are illustrative.

Prompt set, in execution order:

1. [Initial generation prompt](desk-setup-prompt.txt).
2. [Desk scale correction prompt](desk-setup-correction-prompt.txt).

`desk-setup-v1.png` preserves the first generation; `desk-setup.png` is the
selected version after correcting its main-scene scale.

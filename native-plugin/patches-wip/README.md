# Work-in-progress native renderer patches (not applied)

`tools\build_sdk.bat` applies only `native-plugin\patches\*.patch`. These are kept aside while 007's menu
(Quantum of Solace, `007qol`) is investigated, so Bourne releases build with the renderer as tested:

- 0006 point lists (point sprites) expanded in the vertex shader.
- 0007-0016 debugging switches and traces (`BOTS_GFX_ALWAYS_DEPTH`, `BOTS_GFX_BASE_LEVEL_ONLY`, `BOTS_GFX_NO_BLEND`,
  `BOTS_DUMP_TEXTURE`, `BOTS_GFX_ONE_LEVEL`, `BOTS_GFX_NO_TEXTURES`, raw fetch constants and vertices in the trace).
- 0017 shader microcode copied when loaded rather than at the draw.

They apply in order after 0001-0005; copy them back into `patches\` to use them.

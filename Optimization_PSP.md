# PSP Optimization Notes

A review of optimizations from the OG Xbox port (`ioQuake3-Xbox`) against this PSP port.

## 1. Load-time DXT1 compression

Source: `ioQuake3-Xbox/code/renderernv2a/xbox_nv2a_dxt.c`. The file is a self-contained encoder in the style of stb_dxt. It finds the endpoints from the main colour direction of each 4x4 block, then does one least-squares refine pass. It uses only single-precision floats. The Xbox uses it only for opaque textures of 4x4 or larger. Alpha textures stay 16-bit.

### Benefit

- The GE samples DXT1 directly (`GU_PSM_DXT1`). No decode step is necessary.
- Opaque textures are now `GU_PSM_5650` at 16 bpp ([psp_tex.c:354-358](code/psp/psp_tex.c#L354-L358)). DXT1 is 4 bpp, so these textures become 4x smaller.
- One sound unit (3.09 MB) is reserved in the ~4 MB volatile pool first, so textures get only ~1 MB of it. When that is full, they spill to VRAM and then to the heap ([psp_tex.c](code/psp/psp_tex.c)).
- The launch command forces `r_picmip 3` ([sys_psp.c:2206](code/sys/sys_psp.c#L2206)). Each picmip step down needs 4x more texels. So DXT1 lets us use `r_picmip 2` for about the same memory as now.

### No FPS gain expected

Session 11c measured geSync at ~0 ms, so the frame is CPU-bound. Less texture bandwidth does not help that.

### Costs and required changes

1. **Load time.** Every opaque texture is encoded on a 333 MHz CPU. The rough estimate is a few ms per texture at picmip 2, and much more at picmip 0. Measure this on hardware.
2. **Block layout.** The PSP stores the index word first and the two colours second; PC and Xbox use the opposite order. DXT5 puts the colour block first, then the 48 alpha index bits, then a0 and a1. Red stays in the high bits of RGB565, as on PC. Quake3PSP-mirror's encoder (`renderer/tr_dxtn.c`), which runs on hardware, writes exactly this layout.
3. **No swizzle.** DXT textures use the linear path. The mip chain must stop before a side goes below 4.
4. **Cinematics.** These re-upload their level 0 every frame through the same entry point. They must stay 16-bit, or the encoder runs every frame.
5. **Byte counts.** The code assumes 2 bytes per texel, for example `tbw*h*2` in `PSP_TexFreeLevels` and in the writeback size. These counts must change.
6. **Hook point.** Put the hook in `PSP_TexUpload2D`, on the 5650 path only. `renderergl1/` stays upstream.

### Next step

1. Run `PSP_TexMemReport` on the largest map at `r_picmip 3`.
2. Run it again at `r_picmip 2`.
3. If picmip 2 overflows the pool, port DXT1. If picmip 2 already fits, the only gain is spare memory.

Note: `r_picmip 3` may have been chosen for speed and not for memory. No note says which.

## 2. Other Xbox optimizations

| Xbox item | PSP status | Reason |
|---|---|---|
| Mipmaps | Already done | `PSP_TexUpload2D` stores up to 8 levels. Swizzled textures stop at the first level below 8 rows ([psp_tex.c:400-409](code/psp/psp_tex.c#L400-L409)). A DXT1 chain would stop at 4x4. |
| Lightmap + texture in one pass | Not possible | The GE has 1 texture unit. Also, `r_vertexlight 1` collapses lightmap stages ([tr_shader.c:2347](code/renderergl1/tr_shader.c#L2347)), so world surfaces are already single-pass. |
| Smaller vertex data | Partly done | Colour is already 4 bytes (`GU_COLOR_8888`), and the draws are already indexed (`GU_INDEX_16BIT`). Each vertex is 24 bytes, not 36 ([psp_draw.c:40-46](code/psp/psp_draw.c#L40-L46)). See section 3. |
| GPU waits in the frame | Not applicable | The vertex data goes into a 4 MB bump arena. It resets once per frame, after `sceGuSync`, so there is no mid-frame wait. geSync was measured at ~0 ms. |
| Model animation sin/cos | Already done | Upstream `LerpMeshVertexes` decodes normals from `tr.sinTable`, so it makes no per-vertex `sinf`/`cosf` calls. |
| 16-bit lightmaps | Not applicable | `r_vertexlight 1` makes `R_LoadLightmaps` return early ([tr_bsp.c:160](code/renderergl1/tr_bsp.c#L160)), so no lightmaps load. Every PSP texture is already 16-bit. |

## 3. Candidate: smaller vertices

The frame is CPU-bound. The CPU repacks every vertex at 24 bytes for each shader pass into the arena ([psp_draw.c:123-127](code/psp/psp_draw.c#L123-L127)). There are two changes:

- `GU_TEXTURE_16BIT` texcoords plus `sceGuTexScale` would save 4 bytes per vertex.
- `GU_VERTEX_16BIT` positions would save 6 more. They need a scale matrix per draw, and the precision loss on large maps is a risk.

Together this is 24 -> 14 bytes (the GE may pad it to 16). That means fewer CPU stores per pass and less arena use.

This is real work in the clipper and the draw paths. The gain is unmeasured.

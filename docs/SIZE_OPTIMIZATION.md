# NeoPanel ELF Size Optimization Route

Measured on 2026-06-09 from `build/android/neopanel_android`:

- ELF output: 48,038,864 bytes.
- Embedded UI font: 35,430,340 bytes.
- `llvm-size` text bucket: 37,257,860 bytes, dominated by `.rodata`.
- `.text` code section: about 0.66 MB.

Conclusion: reducing compiled size is feasible, but the largest win is asset strategy, not C++ code size. The embedded PingFang font is the main payload.

## Practical Path

1. Strip release symbols.
   - Add a release-only linker strip step or run `llvm-strip --strip-all`.
   - Expected win: small to medium. Useful, but it cannot remove embedded font bytes.

2. Subset the font.
   - Use `pyftsubset` from fonttools to keep only the glyphs NeoPanel actually renders.
   - Keep ASCII, digits, punctuation, UI labels, and required CJK glyphs.
   - Expected win: high. This is the best first real size pass.

3. Compress embedded resources.
   - Store the font as zstd/deflate bytes in `.rodata`.
   - Decompress once at startup before passing data to stb_truetype.
   - Expected win: high on disk size, with a small startup CPU/memory cost.

4. Use `-Oz`/LTO/ICF only after the asset pass.
   - `-Oz` reduces code size.
   - LTO and identical code folding can trim unused or duplicated code.
   - Expected win: modest, because code is already much smaller than assets.

5. Avoid UPX as the default path.
   - UPX-style Android ELF packing may break on some devices, can trigger security tooling, and makes debugging harder.
   - Keep it only as an experimental distribution variant.

## Recommended Order

1. Add release stripping.
2. Build a font subset pipeline.
3. Optionally compress the subset font and decompress at startup.
4. Re-measure startup time, RSS, and ELF size.
5. Consider LTO/`-Oz` if a final few MB still matter.

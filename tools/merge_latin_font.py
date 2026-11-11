#!/usr/bin/env python3
"""Agrega a font.ttf (NanumGothic, sin tildes ni ñ) los glifos latinos que le faltan, tomados de DejaVu Sans.

El juego usa IDs de glifo de UNA fuente por tres caminos (CTFont, CGFont y la tabla cmap que lee FontLabel),
así que las tildes tienen que estar en la misma fuente: no alcanza con una fuente de respaldo aparte.
Copia U+00A0..U+017F y la puntuación tipográfica que falte, descomponiendo compuestos y escalando a las
unidades por em de NanumGothic. Ambas fuentes son libres (OFL / licencia Bitstream Vera).

Uso: python3 tools/merge_latin_font.py font.ttf ../Feanor-iOS-vita/font_latin.ttf font.ttf
"""
import sys

from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

EXTRA = [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x20AC]


def main(base_path, latin_path, out_path):
    base, latin = TTFont(base_path), TTFont(latin_path)
    bcmap, lcmap = base.getBestCmap(), latin.getBestCmap()
    scale = base['head'].unitsPerEm / latin['head'].unitsPerEm
    lglyphs = latin.getGlyphSet()
    added = {}
    for cp in list(range(0xA0, 0x180)) + EXTRA:
        if cp in bcmap or cp not in lcmap:
            continue
        src = lcmap[cp]
        name = 'lat_%04X' % cp
        rec = DecomposingRecordingPen(lglyphs)
        lglyphs[src].draw(rec)
        pen = TTGlyphPen(None)
        rec.replay(TransformPen(pen, (scale, 0, 0, scale, 0, 0)))
        base['glyf'][name] = pen.glyph()
        adv, lsb = latin['hmtx'][src]
        g = base['glyf'][name]
        g.recalcBounds(base['glyf'])
        base['hmtx'][name] = (round(adv * scale), getattr(g, 'xMin', 0))
        added[cp] = name
    order = base['glyf'].glyphOrder  # glyf['x'] = ... ya agregó los nombres nuevos al final
    base.setGlyphOrder(order)
    for t in base['cmap'].tables:
        if t.isUnicode():
            t.cmap.update(added)
    base['maxp'].numGlyphs = len(order)
    if 'post' in base and base['post'].formatType == 2.0:
        base['post'].extraNames = []
        base['post'].mapping = {}
    base.save(out_path)
    check = TTFont(out_path).getBestCmap()
    missing = [hex(c) for c in map(ord, 'áéíóúñÑüÁÉÍÓÚ¿¡') if c not in check]
    assert not missing, missing
    print('agregados %d glifos latinos -> %s' % (len(added), out_path))


if __name__ == '__main__':
    main(*sys.argv[1:4])

#!/usr/bin/env python3
"""Prepara un ejecutable iOS 32 bits (Mach-O MH_EXECUTE, ARMv6/ARMv7) para el loader de la Vita.

Genera dos tablas que el loader lee desde ux0:data/<slug>/:

  rebase.bin  'RBS1' u32 count, count * u32 vmaddr   palabras con una dirección absoluta (+slide)
  binds.bin   'BND1' u32 count, count * {u32 vmaddr, u32 addend, u8 lazy, char name[] NUL}
              slots que apuntan a un símbolo externo (frameworks de Apple)

Dos casos:
  * PIE / LC_DYLD_INFO (iOS 4.3+, ej. Rayman Fiesta Run): rebase y binds exactos, de los opcodes.
  * Clásico sin relocs locales (iPhone OS 2.x/3.x, -mdynamic-no-pic, ej. Gameloft 2009-2010):
    el código tiene direcciones absolutas en los literal pools y NO hay relocs. Se reconstruyen:
      - exacto: __mod_init_func, __cfstring, __objc_* (recorrido estructural de clases,
        metaclases, ro, listas de métodos/ivars/protocolos/propiedades, categorías),
        __nl_symbol_ptr locales, __symbol_stub4, relocs locales si las hay.
      - código: descenso recursivo ARM/Thumb con capstone desde main/start, IMPs de ObjC,
        __mod_init_func y símbolos; cada `ldr rX, [pc, #imm]` alcanzado define un literal; si su
        valor cae en la imagen y el uso no es aritmético, es puntero. Tablas `ldr pc,[pc,rX,lsl#2]`.
      - datos genéricos: palabra que apunta a __DATA, a un inicio de cadena o a un inicio de
        función conocido (o una corrida >=2 de ellas: vtables).
    Lo dudoso va a report.txt para el triage en consola.

Uso: python3 -I ios_prep.py <ejecutable> <outdir> [--arch armv6|armv7]
"""
import bisect, struct, sys
from collections import defaultdict

LC_SEGMENT, LC_SYMTAB, LC_DYSYMTAB, LC_UNIXTHREAD = 0x1, 0x2, 0xB, 0x5
LC_DYLD_INFO, LC_DYLD_INFO_ONLY, LC_MAIN = 0x22, 0x80000022, 0x80000028
LC_FUNCTION_STARTS, LC_DATA_IN_CODE = 0x26, 0x29
S_ZEROFILL, S_NON_LAZY, S_LAZY, S_STUBS, S_GB_ZEROFILL = 0x1, 0x6, 0x7, 0x8, 0xC
INDIRECT_LOCAL, INDIRECT_ABS = 0x80000000, 0x40000000
N_STAB, N_TYPE, N_SECT = 0xE0, 0x0E, 0x0E
N_ARM_THUMB_DEF = 0x0008

# Nombres de imports que son datos (variables/clases), no funciones.
DATA_PREFIXES = ('_OBJC_CLASS_$_', '_OBJC_METACLASS_$_', '_OBJC_EHTYPE_', '_kCF', '_kEAGL', '_kAudio',
                 '_kCG', '_kSC', '_kUT', '_kSec', '_kAB', '_kCT', '_kCV', '_UIApplication', '_UIKeyboard',
                 '_UITextField', '_UIDevice', '_UIScreen', '_NSDefault', '_NSRunLoop', '_NSLocale',
                 '_NSError', '_NSURL', '_NSFile', '_NSUnderlying', '_NSInvalid', '_NSGeneric', '_NSFound',
                 '_MPMedia', '_MPMovie', '_MPMusic', '_GK', '_AVAudio', '_AVMedia', '_AVLayer',
                 '___CFConstantStringClassReference', '___stderrp', '___stdoutp', '___stdinp',
                 '__DefaultRuneLocale', '___stack_chk_guard', '__objc_empty', '___sF', '_NX',
                 '_environ', '_optarg', '_optind', '_mach_task_self_', '_vm_page_size', '_errno',
                 '_CGPointZero', '_CGRectZero', '_CGSizeZero', '_CGAffineTransformIdentity',
                 '_CGRectInfinite', '_CGRectNull', '__ZTV', '__ZTI', '__ZTS', '__ZNSs4_Rep20_S_empty',
                 '__ZSt4cout', '__ZSt4cerr', '__ZSt4endl', '_UIEdgeInsetsZero', '_NSZeroRect')


def is_data_name(n):
    return n.startswith(DATA_PREFIXES) or (n.endswith('Key') or n.endswith('Notification')) and not n.startswith('_obj')


class MachO:
    def __init__(self, path, want=None):
        raw = open(path, 'rb').read()
        off = 0
        if raw[:4] in (b'\xca\xfe\xba\xbe',):
            n = struct.unpack_from('>I', raw, 4)[0]
            arches = [struct.unpack_from('>5I', raw, 8 + i * 20) for i in range(n)]
            pick = None
            for cpu, sub, o, sz, al in arches:
                name = {6: 'armv6', 9: 'armv7', 11: 'armv7s'}.get(sub)
                if cpu == 12 and (want == name or (want is None and name == 'armv7')):
                    pick = (o, sz)
            if not pick:
                pick = (arches[0][2], arches[0][3])
            off = pick[0]
            raw = raw[pick[0]:pick[0] + pick[1]]
        self.d = raw
        self.slice_off = off
        magic, cpu, self.sub, ftype, ncmds, _sz, self.flags = struct.unpack_from('<7I', raw, 0)
        assert magic == 0xFEEDFACE and ftype == 2, 'se espera MH_EXECUTE de 32 bits'
        self.pie = bool(self.flags & 0x200000)
        self.segs, self.sects = [], []
        self.symtab = self.dysym = self.dyldinfo = None
        self.entry = None
        self.func_starts = None
        self.data_in_code = []
        o = 28
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<2I', raw, o)
            if cmd == LC_SEGMENT:
                name = raw[o + 8:o + 24].rstrip(b'\0').decode()
                vm, vs, fo, fs, mp, ip, ns, fl = struct.unpack_from('<8I', raw, o + 24)
                self.segs.append(dict(name=name, vm=vm, vs=vs, fo=fo, fs=fs, prot=ip))
                for i in range(ns):
                    so = o + 56 + i * 68
                    sn = raw[so:so + 16].rstrip(b'\0').decode()
                    sg = raw[so + 16:so + 32].rstrip(b'\0').decode()
                    a, s, of, al, ro, nr, f, r1, r2 = struct.unpack_from('<9I', raw, so + 32)
                    self.sects.append(dict(name=sn, seg=sg, addr=a, size=s, off=of, flags=f, r1=r1, r2=r2,
                                           reloff=ro, nreloc=nr))
            elif cmd == LC_SYMTAB:
                self.symtab = struct.unpack_from('<4I', raw, o + 8)
            elif cmd == LC_DYSYMTAB:
                self.dysym = struct.unpack_from('<18I', raw, o + 8)
            elif cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
                self.dyldinfo = struct.unpack_from('<10I', raw, o + 8)
            elif cmd == LC_UNIXTHREAD:
                regs = struct.unpack_from('<17I', raw, o + 16)
                self.entry = regs[15]
            elif cmd == LC_FUNCTION_STARTS:
                fo, fs = struct.unpack_from('<2I', raw, o + 8)
                self.func_starts = self._uleb_deltas(fo, fs)
            elif cmd == LC_DATA_IN_CODE:
                fo, fs = struct.unpack_from('<2I', raw, o + 8)
                for i in range(fs // 8):
                    so, ln, kind = struct.unpack_from('<IHH', raw, fo + i * 8)
                    self.data_in_code.append((so, ln, kind))
            o += size
        self.text = next(s for s in self.segs if s['name'] == '__TEXT')
        self.lo = min(s['vm'] for s in self.segs if s['name'] not in ('__PAGEZERO', '__LINKEDIT'))
        self.hi = max(s['vm'] + s['vs'] for s in self.segs if s['name'] not in ('__PAGEZERO', '__LINKEDIT'))
        self.syms = []
        if self.symtab:
            so, ns, sto, _ = self.symtab
            for i in range(ns):
                strx, typ, sect, desc, val = struct.unpack_from('<IBBHI', raw, so + i * 12)
                end = raw.index(b'\0', sto + strx)
                self.syms.append((raw[sto + strx:end].decode('latin1'), typ, sect, desc, val))

    def _uleb_deltas(self, fo, fs):
        out, addr, p, end = [], self.text_vm_guess(), fo, fo + fs
        while p < end:
            v, sh = 0, 0
            while True:
                b = self.d[p]; p += 1
                v |= (b & 0x7F) << sh; sh += 7
                if not b & 0x80:
                    break
            if v == 0:
                break
            addr += v
            out.append(addr)
        return out

    def text_vm_guess(self):
        for s in self.segs:
            if s['name'] == '__TEXT':
                return s['vm']
        return 0

    def sect(self, name, seg=None):
        for s in self.sects:
            if s['name'] == name and (seg is None or s['seg'] == seg):
                return s
        return None

    def file_off(self, vm):
        for s in self.segs:
            if s['vm'] <= vm < s['vm'] + s['fs']:
                return s['fo'] + vm - s['vm']
        return None

    def u32(self, vm):
        fo = self.file_off(vm)
        if fo is None or fo + 4 > len(self.d):
            return 0  # zerofill
        return struct.unpack_from('<I', self.d, fo)[0]

    def cstr(self, vm):
        fo = self.file_off(vm)
        if fo is None:
            return ''
        return self.d[fo:self.d.index(b'\0', fo)].decode('latin1')

    def section_of(self, vm):
        for s in self.sects:
            if s['addr'] <= vm < s['addr'] + s['size']:
                return s
        return None


def read_uleb(d, p):
    v, sh = 0, 0
    while True:
        b = d[p]; p += 1
        v |= (b & 0x7F) << sh; sh += 7
        if not b & 0x80:
            return v, p


def read_sleb(d, p):
    v, sh = 0, 0
    while True:
        b = d[p]; p += 1
        v |= (b & 0x7F) << sh; sh += 7
        if not b & 0x80:
            if b & 0x40:
                v -= 1 << sh
            return v, p


# ------------------------------------------------------------------------------------- PIE

def dyld_info_tables(m):
    rb_off, rb_sz, bd_off, bd_sz, wk_off, wk_sz, lz_off, lz_sz, _, _ = m.dyldinfo
    segs = [s for s in m.segs]
    d = m.d
    rebases = []
    p, end, seg, off, typ = rb_off, rb_off + rb_sz, 0, 0, 1
    while p < end:
        b = d[p]; p += 1
        op, imm = b & 0xF0, b & 0x0F
        if op == 0x00:
            break
        elif op == 0x10:
            typ = imm
        elif op == 0x20:
            seg = imm; off, p = read_uleb(d, p)
        elif op == 0x30:
            v, p = read_uleb(d, p); off += v
        elif op == 0x40:
            off += imm * 4
        elif op == 0x50:
            for _ in range(imm):
                rebases.append(segs[seg]['vm'] + off); off += 4
        elif op == 0x60:
            n, p = read_uleb(d, p)
            for _ in range(n):
                rebases.append(segs[seg]['vm'] + off); off += 4
        elif op == 0x70:
            rebases.append(segs[seg]['vm'] + off)
            v, p = read_uleb(d, p); off += 4 + v
        elif op == 0x80:
            n, p = read_uleb(d, p); skip, p = read_uleb(d, p)
            for _ in range(n):
                rebases.append(segs[seg]['vm'] + off); off += 4 + skip
        else:
            raise ValueError('rebase opcode %x' % op)

    binds = []

    def run_binds(start, size, lazy):
        p, end = start, start + size
        seg = off = addend = 0
        name = ''
        while p < end:
            b = d[p]; p += 1
            op, imm = b & 0xF0, b & 0x0F
            if op == 0x00:
                if lazy:
                    continue
                break
            elif op in (0x10, 0x30):
                pass
            elif op == 0x20:
                _, p = read_uleb(d, p)
            elif op == 0x40:
                e = d.index(b'\0', p)
                name = d[p:e].decode('latin1'); p = e + 1
            elif op == 0x50:
                pass
            elif op == 0x60:
                addend, p = read_sleb(d, p)
            elif op == 0x70:
                seg = imm; off, p = read_uleb(d, p)
            elif op == 0x80:
                v, p = read_uleb(d, p); off = (off + v) & 0xFFFFFFFF
            elif op == 0x90:
                binds.append((segs[seg]['vm'] + off, addend, lazy, name)); off += 4
            elif op == 0xA0:
                binds.append((segs[seg]['vm'] + off, addend, lazy, name))
                v, p = read_uleb(d, p); off = (off + 4 + v) & 0xFFFFFFFF
            elif op == 0xB0:
                binds.append((segs[seg]['vm'] + off, addend, lazy, name)); off += 4 + imm * 4
            elif op == 0xC0:
                n, p = read_uleb(d, p); skip, p = read_uleb(d, p)
                for _ in range(n):
                    binds.append((segs[seg]['vm'] + off, addend, lazy, name)); off += 4 + skip
            else:
                raise ValueError('bind opcode %x' % op)

    run_binds(bd_off, bd_sz, False)
    run_binds(wk_off, wk_sz, False)
    run_binds(lz_off, lz_sz, True)
    return rebases, binds


# ------------------------------------------------------------------------------- clásico

def local_or_ext(m, idx, addr, addend, lazy):
    # Símbolo definido en el propio binario (weak/coalesced de C++): no es import. Se emite como
    # bind con nombre '' y addend = dirección absoluta: el loader escribe addend + slide.
    name, typ, sect, desc, val = m.syms[idx]
    if not typ & N_STAB and (typ & N_TYPE) == N_SECT:
        return (addr, (val | (1 if desc & N_ARM_THUMB_DEF else 0)) + addend, lazy, '')
    return (addr, addend, lazy, name)


def classic_binds(m):
    """Imports por la tabla indirecta y relocs externas. Devuelve (binds, rebases_exactos)."""
    binds, rebases = [], set()
    d = m.d
    iso = m.dysym[12]  # indirectsymoff
    for s in m.sects:
        t = s['flags'] & 0xFF
        if t in (S_LAZY, S_NON_LAZY):
            for j in range(s['size'] // 4):
                idx = struct.unpack_from('<I', d, iso + (s['r1'] + j) * 4)[0]
                a = s['addr'] + j * 4
                if idx & INDIRECT_LOCAL:
                    if not idx & INDIRECT_ABS:
                        rebases.add(a)
                    continue
                if idx & INDIRECT_ABS:
                    continue
                binds.append(local_or_ext(m, idx, a, 0, t == S_LAZY))
        elif t == S_STUBS and s['r2'] in (8, 12) and s['seg'] == '__TEXT' and m.u32(s['addr']) in (0xE51FF004, 0xE59FC000):
            # __symbol_stub4 (no PIC): ldr pc, [pc, #-4]; .long lazy_ptr  |  ldr ip, [pc]; ldr pc, [ip]; .long lazy_ptr
            n = s['r2']
            for j in range(s['size'] // n):
                rebases.add(s['addr'] + j * n + n - 4)
    extreloff, nextrel, locreloff, nlocrel = m.dysym[14], m.dysym[15], m.dysym[16], m.dysym[17]
    for i in range(nextrel):
        addr, info = struct.unpack_from('<iI', d, extreloff + i * 8)
        symnum, pcrel, length, ext, typ = info & 0xFFFFFF, (info >> 24) & 1, (info >> 25) & 3, (info >> 27) & 1, info >> 28
        assert ext and length == 2 and not pcrel and typ == 0, 'reloc externa rara en 0x%x' % addr
        binds.append(local_or_ext(m, symnum, addr, m.u32(addr), False))
    for i in range(nlocrel):
        addr, info = struct.unpack_from('<II', d, locreloff + i * 8)
        if addr & 0x80000000:  # scattered: r_address = 24 bits bajos
            rebases.add(addr & 0xFFFFFF)
        else:
            rebases.add(addr)
    return binds, rebases


def objc_walk(m, bound):
    """Recorre la metadata ObjC 2. Devuelve (punteros exactos, IMPs)."""
    ptrs, imps, seen = set(), set(), set()

    def P(a):  # campo puntero en a
        if a in bound:
            return m.u32(a) if False else None
        v = m.u32(a)
        if v and m.lo <= v < m.hi:
            ptrs.add(a)
            return v
        return None

    def mlist(a):
        if not a or a in seen:
            return
        seen.add(a)
        ent = m.u32(a) & ~3
        n = m.u32(a + 4)
        for i in range(n):
            e = a + 8 + i * ent
            P(e); P(e + 4)
            imp = P(e + 8)
            if imp:
                imps.add(imp)

    def ivlist(a):
        if not a or a in seen:
            return
        seen.add(a)
        ent, n = m.u32(a), m.u32(a + 4)
        for i in range(n):
            e = a + 8 + i * ent
            P(e); P(e + 4); P(e + 8)

    def plist(a):
        if not a or a in seen:
            return
        seen.add(a)
        ent, n = m.u32(a), m.u32(a + 4)
        for i in range(n):
            P(a + 8 + i * ent); P(a + 12 + i * ent)

    def protos(a):
        if not a or a in seen:
            return
        seen.add(a)
        n = m.u32(a)
        for i in range(n):
            proto(P(a + 4 + i * 4))

    def proto(a):
        if not a or a in seen:
            return
        seen.add(a)
        P(a)  # isa
        P(a + 4)  # name
        protos(P(a + 8))
        for k in (12, 16, 20, 24):
            mlist(P(a + k))
        plist(P(a + 28))

    def ro(a):
        if not a or a in seen:
            return
        seen.add(a)
        P(a + 12)  # ivarLayout
        P(a + 16)  # name
        mlist(P(a + 20))
        protos(P(a + 24))
        ivlist(P(a + 28))
        P(a + 32)  # weakIvarLayout
        plist(P(a + 36))

    def cls(a):
        if not a or a in seen:
            return
        seen.add(a)
        isa = P(a)
        sup = P(a + 4)
        P(a + 8); P(a + 12)
        ro(P(a + 16) and (m.u32(a + 16) & ~3))
        if isa:
            cls(isa)
        if sup:
            cls(sup)

    for name in ('__objc_classlist', '__objc_nlclslist'):
        s = m.sect(name)
        if s:
            for i in range(s['size'] // 4):
                cls(P(s['addr'] + i * 4))
    for name in ('__objc_catlist', '__objc_nlcatlist'):
        s = m.sect(name)
        if s:
            for i in range(s['size'] // 4):
                c = P(s['addr'] + i * 4)
                if c and c not in seen:
                    seen.add(c)
                    P(c); P(c + 4)
                    mlist(P(c + 8)); mlist(P(c + 12)); protos(P(c + 16)); plist(P(c + 20))
    for name in ('__objc_protolist', '__objc_selrefs', '__objc_classrefs', '__objc_superrefs', '__objc_protorefs'):
        s = m.sect(name)
        if s:
            for i in range(s['size'] // 4):
                v = P(s['addr'] + i * 4)
                if name == '__objc_protolist':
                    proto(v)
    s = m.sect('__cfstring')
    if s:
        for i in range(s['size'] // 16):
            P(s['addr'] + i * 16 + 8)
    for name in ('__mod_init_func', '__mod_term_func'):
        s = m.sect(name)
        if s:
            for i in range(s['size'] // 4):
                v = P(s['addr'] + i * 4)
                if v:
                    imps.add(v)
    return ptrs, imps


class CodeScan:
    """Descenso recursivo sobre __text (ARM y Thumb) con capstone."""

    def __init__(self, m, roots, known_funcs):
        import capstone as cs
        self.m = m
        self.cs_arm = cs.Cs(cs.CS_ARCH_ARM, cs.CS_MODE_ARM)
        self.cs_thumb = cs.Cs(cs.CS_ARCH_ARM, cs.CS_MODE_THUMB)
        for c in (self.cs_arm, self.cs_thumb):
            c.detail = True
        self.cs = cs
        t = m.sect('__text', '__TEXT')
        self.t0, self.t1 = t['addr'], t['addr'] + t['size']
        self.tbytes = m.d[t['off']:t['off'] + t['size']]
        self.code = set()  # direcciones de instrucción decodificadas
        self.literals = {}  # addr literal -> (uso, dirección de la instrucción)
        self.funcs = set(known_funcs)
        self.jump_tables = set()
        self.ends = set()  # dirección siguiente a un salto incondicional (posible inicio de función)
        self.bl_targets = set()
        self.after_bl = set()
        # stubs de asignadores y de GL/AL: lo que se les pasa es un tamaño o un enum (GL_VERTEX_ARRAY =
        # 0x8074 cae dentro de __text), nunca un puntero a código
        self.alloc_stubs = set()
        iso = m.dysym[12]
        for s in m.sects:
            if s['flags'] & 0xFF == S_STUBS and s['r2']:
                for j in range(s['size'] // s['r2']):
                    idx = struct.unpack_from('<I', m.d, iso + (s['r1'] + j) * 4)[0]
                    if not idx & (INDIRECT_LOCAL | INDIRECT_ABS) and (m.syms[idx][0] in (
                            '__Znwm', '__Znam', '__Znwj', '__Znaj', '_malloc', '_calloc', '_realloc')
                            or m.syms[idx][0].startswith(('_gl', '_al'))):
                        self.alloc_stubs.add(s['addr'] + j * s['r2'])
        self.todo = list(roots)

    def run(self):
        cs = self.cs
        from capstone import arm as A
        while self.todo:
            a = self.todo.pop()
            thumb = a & 1
            a &= ~1
            if not (self.t0 <= a < self.t1) or a in self.code:
                continue
            md = self.cs_thumb if thumb else self.cs_arm
            pc = a
            last_cmp = {}
            while self.t0 <= pc < self.t1 and pc not in self.code and pc not in self.literals:
                off = pc - self.t0
                insn = next(md.disasm(self.tbytes[off:off + 4], pc, 1), None)
                if insn is None:
                    break
                self.code.add(pc)
                mn = insn.insn_name()
                ops = insn.operands
                nxt = pc + insn.size
                cond = insn.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID) if hasattr(A, 'ARM_CC_AL') else False
                # cmp rX, #N (para tablas de salto)
                if mn == 'cmp' and len(ops) == 2 and ops[1].type == A.ARM_OP_IMM:
                    last_cmp[ops[0].reg] = ops[1].imm
                # literal: ldr rd, [pc, #imm]
                if mn in ('ldr', 'ldrb', 'ldrh', 'ldrsh', 'ldrsb', 'ldrd') and len(ops) >= 2 and ops[-1].type == A.ARM_OP_MEM \
                        and ops[-1].mem.base == A.ARM_REG_PC:
                    mem = ops[-1].mem
                    if mem.index == 0:
                        base = (pc + (4 if thumb else 8)) & ~3
                        disp = mem.disp if not getattr(ops[-1], 'subtracted', False) else -mem.disp
                        lit = base + disp
                        if mn == 'ldr':
                            use = self._classify_use(md, nxt, ops[0].reg, insn.cc) if ops[0].reg != A.ARM_REG_PC else 'ptr'
                            self.literals.setdefault(lit, (use, pc))
                            if ops[0].reg == A.ARM_REG_PC:
                                v = self.m.u32(lit)
                                if self.t0 <= (v & ~1) < self.t1:
                                    self.todo.append(v)
                                if not cond:
                                    break
                        else:
                            self.literals.setdefault(lit, ('int', pc))
                            if mn == 'ldrd':
                                self.literals.setdefault(lit + 4, ('int', pc))
                    elif ops[0].reg == A.ARM_REG_PC and mem.index and ops[-1].shift.value == 2:
                        # ldrls pc, [pc, rX, lsl #2] + tabla de direcciones absolutas
                        n = last_cmp.get(mem.index)
                        if n is not None:
                            tbl = pc + 8
                            for k in range(n + 1):
                                ta = tbl + k * 4
                                self.jump_tables.add(ta)
                                self.literals[ta] = ('ptr', pc)
                                self.todo.append(self.m.u32(ta) | thumb)
                            nxt = tbl + (n + 1) * 4
                        if not cond:
                            break
                        pc = nxt
                        continue
                if mn in ('vldr',) and ops[-1].type == A.ARM_OP_MEM and ops[-1].mem.base == A.ARM_REG_PC:
                    base = (pc + (4 if thumb else 8)) & ~3
                    lit = base + ops[-1].mem.disp * (-1 if getattr(ops[-1], 'subtracted', False) else 1)
                    for k in range(0, 8 if ops[0].reg >= A.ARM_REG_D0 and ops[0].reg <= A.ARM_REG_D31 else 4, 4):
                        self.literals.setdefault(lit + k, ('int', pc))
                # ramas
                if mn in ('bl', 'blx') and ops and ops[0].type == A.ARM_OP_IMM:
                    tgt = ops[0].imm
                    mode = thumb ^ (1 if mn == 'blx' else 0)
                    self.funcs.add(tgt | mode)
                    self.bl_targets.add(tgt & ~1)
                    self.todo.append(tgt | mode)
                    self.after_bl.add(nxt)  # bl noreturn (__cxa_rethrow...): lo siguiente puede ser un landing pad SjLj
                elif mn in ('b', 'cbz', 'cbnz') and ops and ops[-1].type == A.ARM_OP_IMM:
                    self.todo.append(ops[-1].imm | thumb)
                    if mn == 'b' and not cond and not (thumb and insn.cc != A.ARM_CC_AL and insn.size == 2):
                        if insn.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID):
                            break
                elif mn == 'bx' or (mn in ('pop', 'ldm', 'ldmia', 'ldmfd') and any(
                        o.type == A.ARM_OP_REG and o.reg == A.ARM_REG_PC for o in ops)):
                    if insn.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID):
                        break
                elif mn == 'add' and len(ops) == 3 and ops[0].reg == A.ARM_REG_PC and ops[1].type == A.ARM_OP_REG \
                        and ops[1].reg == A.ARM_REG_PC and ops[2].type == A.ARM_OP_REG and ops[2].shift.value == 2:
                    # addls pc, pc, rX, lsl #2 + tabla de `b` (relativa: no se rebasea, pero es código)
                    n = last_cmp.get(ops[2].reg)
                    if n is not None:
                        for k in range(n + 1):
                            self.todo.append((pc + 8 + k * 4) | thumb)
                    if insn.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID):
                        break
                elif mn in ('mov', 'add', 'sub') and ops and ops[0].type == A.ARM_OP_REG and ops[0].reg == A.ARM_REG_PC:
                    if insn.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID):
                        break
                elif mn == 'blx' and ops and ops[0].type == A.ARM_OP_REG:
                    pass
                elif mn == 'tbb' or mn == 'tbh':
                    break  # Thumb-2: tablas relativas, no hacen falta rebases
                pc = nxt
            else:
                continue
            if insn is not None:
                self.ends.add(pc + insn.size)  # solo tras salto/retorno incondicional (break)

    def _classify_use(self, md, start, reg, cc=None):
        """Mira las instrucciones siguientes: ¿el registro se usa como dirección o como número?"""
        from capstone import arm as A
        pc = start
        for _ in range(12):
            off = pc - self.t0
            if not (0 <= off < len(self.tbytes)):
                break
            insn = next(md.disasm(self.tbytes[off:off + 4], pc, 1), None)
            if insn is None:
                break
            mn = insn.insn_name()
            ops = insn.operands
            if cc not in (None, A.ARM_CC_AL) and insn.cc not in (cc, A.ARM_CC_AL, A.ARM_CC_INVALID):
                pc += insn.size  # rama condicional opuesta: no usa este valor
                continue
            if mn in ('mul', 'mla', 'mls', 'smull', 'umull', 'smlal', 'umlal') and any(
                    o.type == A.ARM_OP_REG and o.reg == reg for o in ops[1:]):
                return 'mul'
            reads = [o for o in ops[1:] if o.type == A.ARM_OP_REG and o.reg == reg]
            # PIC: add rX, pc, reg / ldr rX, [pc, reg] -> el literal es un desplazamiento, no puntero
            if reads and any(o.type == A.ARM_OP_REG and o.reg == A.ARM_REG_PC for o in ops[1:]):
                return 'pcrel'
            if any(o.type == A.ARM_OP_MEM and o.mem.base == A.ARM_REG_PC and o.mem.index == reg for o in ops):
                return 'pcrel'
            memuse = [o for o in ops if o.type == A.ARM_OP_MEM and (o.mem.base == reg)]
            if memuse:
                return 'ptr'
            # índice de memoria / suma con otro registro: desplazamiento (campo de un objeto enorme), nunca
            # puntero a __text (Real Soccer: str r2, [r0, r3] con r3 = 0x39d04)
            if any(o.type == A.ARM_OP_MEM and o.mem.index == reg for o in ops):
                return 'regadd'
            if mn in ('add', 'sub') and reads and sum(o.type == A.ARM_OP_REG for o in ops[1:]) >= 2:
                return 'regadd'
            if mn in ('blx', 'bx') and ops and ops[0].type == A.ARM_OP_REG and ops[0].reg == reg:
                return 'ptr'
            if mn.startswith(('str', 'push', 'stm')) and any(o.type == A.ARM_OP_REG and o.reg == reg for o in ops):
                return 'store'
            if mn in ('cmp', 'cmn', 'tst', 'teq', 'mul', 'mla', 'mls', 'and', 'orr', 'eor', 'bic', 'lsl', 'lsr',
                      'asr', 'ror', 'rsb', 'sdiv', 'udiv', 'smull', 'umull', 'vmov', 'mvn', 'clz', 'uxth',
                      'uxtb', 'sxth', 'sxtb', 'ubfx', 'sbfx', 'smlal', 'umlal') and (
                    reads or (ops and ops[0].type == A.ARM_OP_REG and ops[0].reg == reg and mn in ('cmp', 'cmn', 'tst'))):
                return 'int'
            if mn in ('add', 'sub') and reads:
                return 'arith'
            if mn == 'mov' and reads:
                return 'move'
            if mn in ('bl', 'blx', 'b'):
                if ops and ops[0].type == A.ARM_OP_IMM and ops[0].imm in self.alloc_stubs:
                    return 'int'
                return 'arg' if reg <= A.ARM_REG_R3 or reg in (A.ARM_REG_R0, A.ARM_REG_R1, A.ARM_REG_R2, A.ARM_REG_R3) else 'unknown'
            if ops and ops[0].type == A.ARM_OP_REG and ops[0].reg == reg and not mn.startswith(('str', 'cmp', 'tst')):
                return 'dead'  # se sobrescribe sin leerse
            pc += insn.size
        return 'unknown'


def main():
    args = sys.argv[1:]
    arch = None
    if '--arch' in args:
        i = args.index('--arch'); arch = args[i + 1]; del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    path, outdir = args
    m = MachO(path, arch)
    rep = []
    if m.dyldinfo and m.pie:
        rebases, binds = dyld_info_tables(m)
        rebases = sorted(set(rebases))
        # binds (sobre todo weak) a símbolos que define el propio binario: van a esa definición
        defined = {}
        for name, typ, sect, desc, val in m.syms:
            if not typ & N_STAB and (typ & N_TYPE) == N_SECT and (typ & 1):
                defined.setdefault(name, val | (1 if desc & N_ARM_THUMB_DEF else 0))
        nlocal = 0
        for i, (a, add, lazy, name) in enumerate(binds):
            if name in defined:
                binds[i] = (a, defined[name] + add, lazy, '')
                nlocal += 1
        # varios binds al mismo slot (weak + regular): queda el último, como en dyld
        last = {}
        for b in binds:
            last[b[0]] = b
        binds = list(last.values())
        rep.append('modo: PIE/dyld_info (exacto); %d binds resueltos dentro de la imagen' % nlocal)
    else:
        binds, exact = classic_binds(m)
        bound = {b[0] for b in binds}
        optr, imps = objc_walk(m, bound)
        exact |= optr
        exact -= bound
        # cadenas (inicios)
        cstarts = set()
        for s in m.sects:
            if s['name'] in ('__cstring', '__objc_methname', '__objc_classname', '__objc_methtype', '__ustring'):
                a, end = s['addr'], s['addr'] + s['size']
                prev0 = True
                fo = s['off']
                for k in range(s['size']):
                    if prev0:
                        cstarts.add(a + k)
                    prev0 = m.d[fo + k] == 0
        datasecs = [s for s in m.sects if s['seg'] != '__TEXT']
        dlo = min(s['addr'] for s in datasecs)
        dhi = max(s['addr'] + s['size'] for s in datasecs)
        tconst = [s for s in m.sects if s['seg'] == '__TEXT' and s['name'] not in ('__text', '__cstring', '__symbol_stub4', '__ustring')]
        # raíces de código
        known = set()
        for n, typ, sect, desc, val in m.syms:
            if not typ & N_STAB and (typ & N_TYPE) == N_SECT and sect == 1:
                known.add(val | (1 if desc & N_ARM_THUMB_DEF else 0))
        roots = set(imps) | known
        if m.entry:
            roots.add(m.entry)
        # punteros a código en datos (iteración: vtables, callbacks)
        scan = CodeScan(m, roots, known | set(imps))
        scan.run()

        def in_text(v):
            return scan.t0 <= (v & ~1) < scan.t1

        def looks_func(v):
            if v in scan.funcs or (v & ~1) in scan.funcs or v in imps:
                return True
            return False

        def prologue(v):
            if v & 1 or v & 3 or not in_text(v):
                return False
            w = m.u32(v)
            return (w & 0xFFFF4000) == 0xE92D4000 or w == 0xE1A0C00D or (w & 0xFFFFF000) == 0xE52DE000

        data_words = []
        for s in m.sects:
            t = s['flags'] & 0xFF
            if t in (S_ZEROFILL, S_GB_ZEROFILL) or s['name'] in ('__text', '__cstring', '__ustring', '__symbol_stub4',
                                                                   '__objc_methname', '__objc_classname', '__objc_methtype'):
                continue
            if t in (S_LAZY, S_NON_LAZY):
                continue
            for k in range(0, s['size'] - 3, 4):
                a = s['addr'] + k
                if a in bound or a in exact:
                    continue
                data_words.append(a)

        # pasada 1: punteros a código con prólogo -> nuevas raíces
        for _ in range(3):
            new = []
            for a in data_words:
                v = m.u32(a)
                if in_text(v) and not looks_func(v) and prologue(v):
                    new.append(v)
            if not new:
                break
            scan.funcs.update(new)
            scan.todo.extend(new)
            scan.run()

        for _ in range(4):
            new, run = [], []
            for a in data_words + [None]:
                v = m.u32(a) if a is not None else 0
                ok = a is not None and in_text(v) and (v & 3) == 0
                if ok and (not run or run[-1][0] == a - 4):
                    run.append((a, v)); continue
                if len(run) >= 2 and any(looks_func(x) or prologue(x) for _, x in run):
                    new += [x for _, x in run if (x & ~1) not in scan.code]
                run = [(a, v)] if ok else []
            if not new:
                break
            scan.funcs.update(new)
            scan.todo.extend(new)
            scan.run()

        # barrido de huecos: prólogos ARM (stmfd sp!, {..., lr}) fuera del código ya visto
        for _ in range(6):
            new = []
            for a in range(scan.t0, scan.t1, 4):
                if a in scan.code or a in scan.literals:
                    continue
                if prologue(a) or (a in scan.ends and m.u32(a) >> 28 == 0xE and m.u32(a + 4) >> 28 == 0xE):
                    new.append(a)
            if not new:
                break
            scan.funcs.update(new)
            scan.todo.extend(new)
            scan.run()

        # Inicios de función "fuertes": símbolos, IMPs de ObjC, destinos de bl, prólogo ARM. Los
        # "débiles" (instrucción tras un salto incondicional) solo cuentan con más evidencia.
        strong = {v & ~1 for v in known} | {v & ~1 for v in imps} | scan.bl_targets

        def is_strong(v):
            return (v & ~1) in strong or prologue(v & ~1)

        def is_weak(v):
            return (v & ~1) in scan.ends and (v & ~1) in scan.code and (v & 0xFF) != 0 and (v & 0xFFF0) != 0

        def in_ptr_sect(v):
            return dlo <= v < dhi or v in cstarts or any(s['addr'] <= v < s['addr'] + s['size'] for s in tconst)

        heur = set()
        doubt = []
        # vtables: corrida >=2 de punteros a código (todos fuertes o débiles, al menos uno fuerte),
        # precedida por la cabecera de vtable (0 / puntero a typeinfo) o compuesta solo de fuertes.
        vt = set()
        run = []
        for a in data_words + [None]:
            v = m.u32(a) if a is not None else 0
            ok = a is not None and in_text(v) and (v & 3) == 0 and (is_strong(v) or is_weak(v))
            if ok and (not run or run[-1][0] == a - 4):
                run.append((a, v))
                continue
            if len(run) >= 2:
                head = m.u32(run[0][0] - 4)
                header = head == 0 or in_ptr_sect(head)
                n_strong = sum(1 for _, x in run if is_strong(x))
                if n_strong == len(run) or (header and n_strong):
                    vt.update(x for x, _ in run)
            run = [(a, v)] if ok else []
        def numeric_table(a):
            # Tabla de enteros empaquetados (p. ej. lenfix/distfix de zlib: {op, bits, val} en 32 bits):
            # la mayoría de los 8 vecinos son enteros de 24 bits fuera de la imagen. Un puntero suelto en
            # una tabla así es casualidad (val dentro del rango de direcciones).
            # También: vecinos fuera de la imagen con el mismo byte 1 (campo `bits` fijo, distfix).
            # Y arreglos de uint16 chicos (lbase/lext/dext de zlib): ambas mitades < 0x400 en el candidato y
            # en la mayoría de los vecinos (en una tabla de punteros la mitad baja < 0x400 es 1 de cada 64).
            c = same = pairs = 0
            v = m.u32(a)
            small = lambda w: w != 0 and (w >> 16) < 0x400 and (w & 0xFFFF) < 0x400
            for k in (-4, -3, -2, -1, 1, 2, 3, 4):
                try:
                    w = m.u32(a + 4 * k)
                except Exception:
                    continue
                c += m.hi <= w < 0x01000000
                same += (w & 0xFF00) == (v & 0xFF00) and (w & 0xFF) != 0 and not (m.lo <= w < m.hi)
                pairs += small(w)
            return c >= 5 or same >= 5 or (small(v) and pairs >= 5)

        # __TEXT,__const en binarios sin PIC: tablas numéricas de solo lectura (senos, raíces, animaciones);
        # las vtables y tablas de punteros a funciones van en __DATA. Un "puntero a código" ahí es un número.
        tc_rng = [(x['addr'], x['addr'] + x['size']) for x in m.sects if x['seg'] == '__TEXT' and x['name'] == '__const']
        in_tconst = lambda a: any(lo <= a < hi for lo, hi in tc_rng)
        for a in data_words:
            v = m.u32(a)
            if v == 0 or not (m.lo <= v < m.hi):
                continue
            # Ídem hacia __DATA: las tablas Huffman/cuantización de Musepack ({code,len} empaquetados, 0x1a0006)
            # caían en el rango de datos y se rebaseaban -> música destrozada (Shrek Kart, corrida 008).
            if in_tconst(a) and v not in cstarts:
                doubt.append(('data-textconst->text' if in_text(v) else 'data-textconst->data', a, v))
                continue
            if a not in vt and numeric_table(a):
                doubt.append(('data-numtable', a, v))
                continue
            if in_ptr_sect(v):
                heur.add(a)
            elif in_text(v):
                if is_strong(v) or a in vt:
                    heur.add(a)
                else:
                    doubt.append(('data->text-nofunc', a, v))
            else:
                doubt.append(('data->other', a, v))
        # literales del código
        lit_ptr = set()
        for lit, (use, ipc) in scan.literals.items():
            if lit in scan.jump_tables:
                lit_ptr.add(lit)
                continue
            v = m.u32(lit)
            if not (m.lo <= v < m.hi) or v < 0x1000:
                continue
            if use == 'pcrel':
                continue
            if use in ('int', 'mul', 'regadd') and in_text(v):
                doubt.append(('lit-int-use', lit, v))
                continue
            # GL por puntero (ldr rN,[nl_ptr]; blx rN): el stub no se ve y el enum parece puntero a código.
            # ponytail: lista fija de enums de argumento que caen en __text; ampliar si aparece otro en GLERR.
            if use in ('arg', 'unknown') and (0x8074 <= v <= 0x8078 or 0x84C0 <= v <= 0x84C1 or v in (0x8892, 0x8893)):
                doubt.append(('lit-gl-enum', lit, v))
                continue
            if in_ptr_sect(v):
                # fuera de __text un entero de >1 MB es raro: puntero salvo uso multiplicativo
                # o valor impar que no es inicio de cadena
                if use == 'mul' or (use == 'int' and v % 4 and v not in cstarts):
                    doubt.append(('lit-int-use', lit, v))
                else:
                    lit_ptr.add(lit)
            elif in_text(v):
                # función con evidencia fuerte, o débil usada como callback/puntero (no como número)
                # tras un bl solo si se guarda (landing pad SjLj en el contexto); como arg suele ser un tamaño
                if is_strong(v) or (is_weak(v) and use in ('arg', 'store', 'move', 'ptr')) or (
                        use == 'store' and v in scan.after_bl and v in scan.code and v & 0xFF):
                    lit_ptr.add(lit)
                else:
                    doubt.append(('lit->text-nofunc', lit, v))
            else:
                doubt.append(('lit->other', lit, v))
        rebases = sorted(exact | heur | lit_ptr)
        rep.append('modo: clásico sin relocs (heurístico + estructural)')
        rep.append('exactos (objc/cfstring/nl locales/stubs/relocs): %d' % len(exact))
        rep.append('datos heurísticos: %d' % len(heur))
        rep.append('literales de código: %d (de %d literales vistos, %d instrucciones)' % (
            len(lit_ptr), len(scan.literals), len(scan.code)))
        rep.append('tablas de salto: %d palabras' % len(scan.jump_tables))
        tsz = scan.t1 - scan.t0
        rep.append('cobertura de __text: %.1f%% (%d de %d palabras)' % (
            100.0 * len(scan.code) * 4 / tsz, len(scan.code), tsz // 4))
        rep.append('dudosos (NO rebaseados): %d' % len(doubt))
        rep.append('funciones conocidas: %d' % len(scan.funcs))
        rep.append('')
        for kind, a, v in sorted(doubt, key=lambda x: x[1]):
            rep.append('DUDA %-22s 0x%08X = 0x%08X' % (kind, a, v))

    with open(outdir + '/rebase.bin', 'wb') as f:
        f.write(struct.pack('<2I', 0x31534252, len(rebases)))
        f.write(struct.pack('<%dI' % len(rebases), *rebases))
    with open(outdir + '/binds.bin', 'wb') as f:
        f.write(struct.pack('<2I', 0x31444E42, len(binds)))
        for a, add, lazy, name in binds:
            f.write(struct.pack('<IIB', a, add & 0xFFFFFFFF, 1 if lazy else 0) + name.encode('latin1') + b'\0')
    names = defaultdict(int)
    for a, add, lazy, name in binds:
        if name:
            names[name] += 1
    hdr = ['ejecutable: %s  pie=%s  entry=0x%x  imagen=[0x%x,0x%x)' % (path, m.pie, m.entry or 0, m.lo, m.hi),
           'rebase: %d palabras   binds: %d slots / %d símbolos' % (len(rebases), len(binds), len(names))]
    with open(outdir + '/report.txt', 'w') as f:
        f.write('\n'.join(hdr + rep) + '\n\n# imports\n')
        for n in sorted(names):
            f.write('%s%s\n' % (n, '  [dato]' if is_data_name(n) else ''))
    print('\n'.join(hdr + rep[:12]))


if __name__ == '__main__':
    main()

"""Source-level optimisations of the lifted TUs (build_native.py --lift-opt).

transform(name, text) returns the C to compile in place of a generated file.
Every rewrite here must keep the guest's behaviour bit for bit: the
deterministic clock makes a benchmark's screenshots and tick counts the
same on every build, and that is how a rewrite is checked
(tools/compare_bench_shots.py).

Rewrites (all exact: a store is dropped only where dataflow proves the
memory already holds the value, a reload only where the callee provably
cannot have changed it, ...; a function with any form the parser does not
know is left alone):
  1. spills `s->F = F;` of locals that equal s->F on every path (975k lines)
  2. post-call reloads of fields the callee's call closure never writes
  3. `if (recomp_jmp_pending) recomp_run_pending(s);` after calls to functions
     that cannot park a jump (70k of 120k direct call sites)
  4. `T = R + const; MEMx(T)` -> MEMxB(R, const) (needs the recomp_rt.h
     patches 0013/0014; |const| <= 0xffff)
  5. the re-entry prologue `if (g_reentry_eip != 0u)` marked unlikely
  6. (off by default, ISAAC_LIFT_OPT_ICALL=1, needs patch 0015) indirect call
     sites probe the dispatch cache inline
  7. FS_OFFSET (never written by lifted code) is read where used instead of
     being kept in a local (ISAAC_LIFT_OPT_FS=0 disables)
  8. the re-entry prologue takes its target through a cold call
     (recomp_reentry_take(), patch 0016) and each TU gets
     `#pragma GCC optimize("reorder-blocks-and-partition")`: GCC moves the
     re-entry dispatch of every function into .text.unlikely (about 12% less
     code in the executed part of a function on aarch64)
     (ISAAC_LIFT_OPT_COLD=0 disables)
  9. ISAAC_LIFT_HOT=<json {sub_XXXXXXXX: console samples}> (made with
     `python3 lift_opt.py --hot-from build/bench/switch-horde@3b-* -o hot.json`)
     puts the functions the profile saw (>= ISAAC_LIFT_HOT_PLACE_MIN samples,
     default 2) into .text.hot.<name>, which the linker script gathers
 10. (off by default, ISAAC_LIFT_INLINE_MAX=<statements>) callees of at most
     that many statements, or hot ones up to ISAAC_LIFT_INLINE_HOT_MAX
     (default 200) with >= ISAAC_LIFT_INLINE_HOT_MIN samples (default 30),
     are pasted into their direct call sites (see "Inlining" below)
The interprocedural facts come from a pass over every gu/lifted_*.c (about 15 s
on 16 cores, ISAAC_LIFT_GU overrides the directory).

Settings come from the environment (ISAAC_LIFT_*) or from
build/agent-lift/lift_opt.json (a container build sees the repository, not the
environment): {"hot": "build/agent-lift/hot.json", "inline_max": 0, "icall": false,
"cold": true, "fs": true, "bo": true, "hot_place_min": 2, "inline_hot_max": 200,
"inline_hot_min": 30}.

Scope: the ordinary lifted functions
    void sub_XXXXXXXX(CpuState *restrict s) {
whose control flow is only labels, gotos, `if (c) { goto L; }` blocks and
`switch` blocks. A function with anything else in it (the LIFT-SPLIT
dispatch loops, the __lifted wrappers) is copied unchanged.
"""
import hashlib
import os
import re
import sys

VERSION = 1


def _load_conf():
    """Settings: environment variables first, then build/agent-lift/lift_opt.json
    (build_native.py --profile switch runs inside a container that does not pass
    the environment on, but does see the repository)."""
    import json
    path = os.environ.get('ISAAC_LIFT_CONF') or os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        'build', 'agent-lift', 'lift_opt.json')
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


_CONF = _load_conf()


def _opt(env, key, default):
    v = os.environ.get(env)
    if v is None:
        v = _CONF.get(key, default)
    return v


def _flag(env, key, default):
    return str(_opt(env, key, default)).lower() not in ('0', 'false', 'no', '')


ENABLE_ICALL = _flag('ISAAC_LIFT_OPT_ICALL', 'icall', False)
HOT_PLACE = _flag('ISAAC_LIFT_HOT_PLACE', 'hot_place', True)
HOT_PLACE_MIN = int(_opt('ISAAC_LIFT_HOT_PLACE_MIN', 'hot_place_min', 2))
ENABLE_COLD = _flag('ISAAC_LIFT_OPT_COLD', 'cold', True)
ENABLE_FS = _flag('ISAAC_LIFT_OPT_FS', 'fs', True)
ENABLE_BO = _flag('ISAAC_LIFT_OPT_BO', 'bo', True)

# ---------------------------------------------------------------------------
# Function-level model
#
# A lifted function keeps the guest registers (and the flags the lifter
# materialises) in C locals that the prologue loads from the CpuState, and it
# spills them back to `s` around every call and at every exit:
#
#     s->EBX = EBX; ... s->EIP = EIP;
#     sub_00401234(s); if (recomp_jmp_pending) recomp_run_pending(s);
#     EBX = s->EBX; ... EIP = s->EIP;
#
# For a tracked field F the state "clean" means: the local F equals s->F.
# A spill of a clean field writes back what is already there, and GCC cannot
# see that (`s` is passed to the callee, and the reload comes out of a call).
# The analysis below is a forward may-be-dirty dataflow over the function's
# gotos; a spill of a field that is clean on every path is deleted.
# ---------------------------------------------------------------------------

FUNC_RE = re.compile(r'^void (sub_[0-9a-f]{8})\(CpuState \*restrict s\) \{$')
PRO_RE = re.compile(r'^  u?int\d+_t (\w+) = s->(\w+);$')
STORE_RE = re.compile(r'^\s*s->(\w+) = (\w+);$')
LOAD_RE = re.compile(r'^\s*(\w+) = s->(\w+);$')
S_ASSIGN_RE = re.compile(r'^\s*s->(\w+)\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)')
CALL_DIRECT_RE = re.compile(r'^  (sub_[0-9a-f]{8})\(s\); if \(recomp_jmp_pending\) recomp_run_pending\(s\);$')
CALL_IND_RE = re.compile(r'^  recomp_call_indirect\(s, \w+\); if \(recomp_jmp_pending\) recomp_run_pending\(s\);$')
CALL_IMP_RE = re.compile(r'^  imp_\w+\(s\);$')
LABEL_RE = re.compile(r'^(L_[0-9a-f]{8}(?:_[ie]\d+)*): ;$')
GOTO_RE = re.compile(r'^goto (L_[0-9a-f]{8}(?:_[ie]\d+)*);(?:\s*/\*.*\*/)?$')
CASE_RE = re.compile(r'^(?:case 0x[0-9a-f]+u?: ?)+')
IF_RE = re.compile(r'^if \(.*\) \{$')
SWITCH_RE = re.compile(r'^switch \(\w+\) \{$')
S_ESC_RE = re.compile(r'(?:[(,] ?)s[,)]')
DECL_RE = re.compile(r'^u?int\d+_t \w+(?:\[\d+\])? = (?:0|\{0\});$')
ESC_SAFE_RE = re.compile(r'^recomp_other(?:w)?_\w+\(s[,)]')
ADDR_DEF_RE = re.compile(r'^  ([ubt][0-9a-f]+_[0-9]+) = \(uint32_t\)\((?:(\w+) \+ \(\(uint32_t\)(0x[0-9a-f]+)u\)|\(\(uint32_t\)(0x[0-9a-f]+)u\) \+ (\w+))\);$')
MEM_USE_RE = re.compile(r'\bMEM([RW])(8|16|32|64)\(([ubt][0-9a-f]+_[0-9]+)([,)])')
PTR_USE_RE = re.compile(r'\bRECOMP_PTR\(([ubt][0-9a-f]+_[0-9]+)\)')
BO_MAX = 0xffff
TEMP_ASG_RE = re.compile(r'^  ([ubt][0-9a-f]+_[0-9]+) = ')
PRO2_RE = re.compile(r'^  (u?int\d+_t) (\w+) = s->(\w+);$')
PEND_RE = re.compile(r'^  if \(recomp_jmp_pending\) recomp_run_pending\(s\);$')
UNREACH_GOTO_RE = re.compile(r'^recomp_unreachable\(s, 0x[0-9a-f]+u\); goto L_[0-9a-f]{8}(?:_[ie]\d+)*;$')
BLOCK_OPEN = '  { /*INL*/'
BLOCK_CLOSE = '  } /*INLEND*/'
TAIL_RE = re.compile(r'^recomp_jmp_target = (0x[0-9a-f]+)u; recomp_jmp_pending = 1u; return;$')

K_PLAIN, K_STORE, K_LOAD, K_ASSIGN, K_MWRITE, K_CALL, K_CALLI, K_ESC, K_LABEL, K_GOTO, \
    K_IF, K_SWITCH, K_CASE, K_DEFAULT, K_CLOSE, K_RET, K_BREAK, K_BLOCK, K_BLOCKEND = range(19)


def _same_local(local, field):
    """the local that mirrors CpuState field `field`: its own name, or the
    name with an inlining suffix"""
    return local == field or (local.startswith(field + '_i') and local[len(field) + 2:].isdigit())


class Bail(Exception):
    pass


def split_functions(text):
    """-> list of chunks (is_func, name, lines)."""
    lines = text.split('\n')
    out = []
    i = 0
    n = len(lines)
    start = 0
    while i < n:
        m = FUNC_RE.match(lines[i])
        if m:
            if start < i:
                out.append((False, None, lines[start:i]))
            j = i + 1
            while j < n and lines[j] != '}':
                j += 1
            if j >= n:
                raise Bail('unterminated function ' + m.group(1))
            out.append((True, m.group(1), lines[i:j + 1]))
            i = j + 1
            start = i
        else:
            i += 1
    if start < n:
        out.append((False, None, lines[start:n]))
    return out


class FuncInfo:
    __slots__ = ('name', 'lines', 'tracked', 'bit', 'ops', 'ok', 'why', 'lab')


def parse_function(name, lines):
    """Classify every line of a function. Returns a FuncInfo; ok=False means
    the function is left alone."""
    fi = FuncInfo()
    fi.name = name
    fi.lines = lines
    fi.ok = False
    fi.why = ''
    # prologue: the tracked locals
    tracked = {}
    k = 1
    while k < len(lines):
        m = PRO_RE.match(lines[k])
        if not m:
            break
        if m.group(1) != m.group(2):
            fi.why = 'prologue alias'
            return fi
        tracked[m.group(1)] = len(tracked)
        k += 1
    if not tracked:
        fi.why = 'no prologue'
        return fi
    # locals declared further down (the blocks of inlined callees) are tracked too
    for l in lines[k:]:
        if l.startswith('  ') and l[2:3] in 'ui':
            m = PRO2_RE.match(l)
            if m and m.group(2) not in tracked:
                if not _same_local(m.group(2), m.group(3)):
                    fi.why = 'inner alias'
                    return fi
                tracked[m.group(2)] = len(tracked)
    fi.tracked = tracked
    bit = {n: 1 << i for n, i in tracked.items()}
    fi.bit = bit
    names = '|'.join(sorted(tracked, key=len, reverse=True))
    asg_re = re.compile(r'(?<![\w.>&])(' + names + r')\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)')
    addr_re = re.compile(r'(?<![&\w])&(?:' + names + r')\b')
    ops = [None] * len(lines)
    for i in range(k, len(lines) - 1):
        l = lines[i]
        s = l.strip()
        if not s:
            continue
        c = s[0]
        if s == '{ /*INL*/':
            ops[i] = (K_BLOCK,)
            continue
        if s == '} /*INLEND*/':
            ops[i] = (K_BLOCKEND,)
            continue
        if c == 'u' or c == 'i':
            m = PRO2_RE.match(l)
            if m:
                if not _same_local(m.group(2), m.group(3)) or m.group(2) not in bit:
                    fi.why = 'inner prologue'
                    return fi
                ops[i] = (K_LOAD, bit[m.group(2)])
                continue
        # ---- structure
        if c == 'L' and s.startswith('L_'):
            m = LABEL_RE.match(s)
            if not m:
                fi.why = 'odd label: ' + s
                return fi
            ops[i] = (K_LABEL, m.group(1))
            continue
        if c == 'g' and s.startswith('goto '):
            m = GOTO_RE.match(s)
            if not m:
                fi.why = 'odd goto: ' + s
                return fi
            ops[i] = (K_GOTO, m.group(1))
            continue
        if c == 'i' and s.startswith('if (') and PEND_RE.match(l):
            ops[i] = (K_CALLI, 'pend')
            continue
        if c == 'i' and s.startswith('if ('):
            if not IF_RE.match(s):
                fi.why = 'odd if: ' + s
                return fi
            ops[i] = (K_IF,)
            continue
        if c == 's' and s.startswith('switch ('):
            if not SWITCH_RE.match(s):
                fi.why = 'odd switch: ' + s
                return fi
            ops[i] = (K_SWITCH,)
            continue
        if c == '}':
            if s != '}':
                fi.why = 'odd close: ' + s
                return fi
            ops[i] = (K_CLOSE,)
            continue
        if c == 'c' and s.startswith('case '):
            m = CASE_RE.match(s)
            if not m:
                fi.why = 'odd case: ' + s
                return fi
            rest = s[m.end():]
            if rest == '':
                ops[i] = (K_CASE, None)
            else:
                g = GOTO_RE.match(rest)
                if not g:
                    fi.why = 'odd case body: ' + s
                    return fi
                ops[i] = (K_CASE, g.group(1))
            continue
        if c == 'd' and s.startswith('default:'):
            if s != 'default: break;':
                fi.why = 'odd default: ' + s
                return fi
            ops[i] = (K_DEFAULT,)
            continue
        if c == 'b' and s == 'break;':
            ops[i] = (K_BREAK,)
            continue
        if c in 'fwd' and (s.startswith('for ') or s.startswith('while ') or s.startswith('do ')):
            fi.why = 'loop'
            return fi
        if c == 'u' and DECL_RE.match(s):
            continue
        if '{' in s or '__pend' in s or '__next' in s:
            fi.why = 'braces: ' + s[:60]
            return fi
        # ---- spills / reloads
        if c == 's' and s.startswith('s->'):
            m = STORE_RE.match(s)
            if m and _same_local(m.group(2), m.group(1)) and m.group(2) in bit:
                ops[i] = (K_STORE, bit[m.group(2)], m.group(1))
                continue
            m = S_ASSIGN_RE.match(s)
            if m:
                f = m.group(1)
                ops[i] = (K_MWRITE, bit.get(f, 0), f)
                continue
            fi.why = 'odd s-> line: ' + s[:60]
            return fi
        # ---- calls
        if c == 's' and s.startswith('sub_'):
            m = CALL_DIRECT_RE.match(l)
            if m:
                ops[i] = (K_CALL, m.group(1))
                continue
            fi.why = 'odd call: ' + s[:60]
            return fi
        if c == 'r' and s.startswith('recomp_unreachable(') and UNREACH_GOTO_RE.match(s):
            ops[i] = (K_RET, 'unreachable')
            continue
        if c == 'i' and PEND_RE.match(l):
            ops[i] = (K_CALLI, 'pend')
            continue
        if c == 'r' and s.startswith('recomp_call_indirect('):
            if CALL_IND_RE.match(l):
                ops[i] = (K_CALLI, None)
                continue
            fi.why = 'odd indirect call: ' + s[:60]
            return fi
        if c == 'i' and s.startswith('imp_'):
            if CALL_IMP_RE.match(l):
                ops[i] = (K_CALLI, 'imp')
                continue
            fi.why = 'odd imp call: ' + s[:60]
            return fi
        if s.endswith('return;'):
            if s == 'return;':
                ops[i] = (K_RET, 'ret')
            elif s.startswith('recomp_unreachable('):
                ops[i] = (K_RET, 'unreachable')
            elif s.startswith('recomp_jump_indirect(s, '):
                ops[i] = (K_RET, 'jump_indirect')
            else:
                m = TAIL_RE.match(s)
                if not m:
                    fi.why = 'odd return: ' + s[:60]
                    return fi
                ops[i] = (K_RET, 'tail', 'sub_%08x' % int(m.group(1), 16))
            continue
        # ---- tracked locals
        if c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ':
            m = LOAD_RE.match(s)
            if m and _same_local(m.group(1), m.group(2)) and m.group(1) in bit:
                ops[i] = (K_LOAD, bit[m.group(1)])
                continue
        m = asg_re.search(s)
        if m:
            mask = 0
            for mm in asg_re.finditer(s):
                mask |= bit[mm.group(1)]
            ops[i] = (K_ASSIGN, mask)
            if addr_re.search(s):
                fi.why = 'address of tracked local'
                return fi
            continue
        if addr_re.search(s):
            fi.why = 'address of tracked local'
            return fi
        # ---- opaque helpers that receive the CPU state
        if S_ESC_RE.search(s):
            ops[i] = (K_ESC, s)
            continue
    fi.ops = ops
    fi.ok = True
    return fi


def join(a, b):
    if a is None:
        return b
    if b is None:
        return a
    return a | b


def sweep(fi, final):
    """One forward pass. Returns (changed, removable store line indexes,
    summary) where summary has the facts the interprocedural step needs."""
    ops = fi.ops
    lab = fi.lab
    allm = (1 << len(fi.tracked)) - 1
    cur = 0
    stack = []
    changed = False
    removable = []
    stored = set()    # fields with a store that stays
    mwritten = set()
    calls = []        # (line index, callee, cur-before)
    opaque = False
    tails = set()
    unresolved = False
    has_imp = False
    for i, op in enumerate(ops):
        if op is None:
            continue
        k = op[0]
        if k == K_LABEL:
            prev = lab.get(op[1])
            nxt = join(cur, prev)
            if nxt != prev:
                lab[op[1]] = nxt
                changed = True
            cur = nxt
        elif k == K_GOTO:
            if cur is not None:
                prev = lab.get(op[1])
                nxt = join(prev, cur)
                if nxt != prev:
                    lab[op[1]] = nxt
                    changed = True
            cur = None
        elif k == K_BLOCK:
            stack.append([2, cur, None, False])
        elif k == K_BLOCKEND:
            t = stack.pop()
            if t[0] != 2:
                raise Bail('block mismatch')
        elif k == K_IF:
            stack.append([0, cur, None, False])
        elif k == K_SWITCH:
            stack.append([1, cur, None, False])
        elif k == K_CASE:
            sw = stack[-1]
            cur = join(cur, sw[1])
            if op[1] is not None:
                if cur is not None:
                    prev = lab.get(op[1])
                    nxt = join(prev, cur)
                    if nxt != prev:
                        lab[op[1]] = nxt
                        changed = True
                cur = None
        elif k == K_DEFAULT:
            sw = stack[-1]
            sw[3] = True
            cur = join(cur, sw[1])
            sw[2] = join(sw[2], cur)
            cur = None
        elif k == K_BREAK:
            sw = stack[-1]
            sw[2] = join(sw[2], cur)
            cur = None
        elif k == K_CLOSE:
            t = stack.pop()
            if t[0] == 2:
                raise Bail('block mismatch')
            if t[0] == 0:
                cur = join(t[1], cur)
            else:
                cur = join(t[2], cur)
                if not t[3]:
                    cur = join(cur, t[1])
        elif cur is None:
            if final and k == K_STORE:
                stored.add(op[2])
            continue
        elif k == K_STORE:
            if final:
                if not (cur & op[1]):
                    removable.append(i)
                else:
                    stored.add(op[2])
            cur &= ~op[1]
        elif k == K_LOAD:
            cur &= ~op[1]
        elif k == K_ASSIGN:
            cur |= op[1]
        elif k == K_MWRITE:
            cur |= op[1]
            mwritten.add(op[2])
        elif k == K_CALL:
            if final:
                calls.append((i, op[1], cur))
            cur = allm
        elif k == K_CALLI:
            opaque = True
            if op[1] == 'imp':
                has_imp = True
            cur = allm
        elif k == K_ESC:
            if not ESC_SAFE_RE.match(op[1]):
                opaque = True
                has_imp = True
            cur = allm
        elif k == K_RET:
            if op[1] == 'tail':
                tails.add(op[2])
            elif op[1] == 'jump_indirect':
                unresolved = True
            cur = None
    if stack:
        raise Bail('unbalanced')
    return changed, removable, stored, mwritten, calls, (opaque, tails, unresolved, has_imp)


def analyse(fi):
    fi.lab = {}
    for _ in range(64):
        changed, *_r = sweep(fi, False)
        if not changed:
            break
    else:
        raise Bail('no fixpoint')
    return sweep(fi, True)


def summarise_function(fi, removable, stored, mwritten, calls, flags):
    """What the interprocedural step needs to know about a function."""
    opaque, tails, unresolved, has_imp = flags
    return {
        'S': sorted(stored | mwritten),
        'callees': sorted({c for _, c, _ in calls}),
        'tails': sorted(tails),
        'bad': bool(opaque or unresolved),
        # may the function return with recomp_jmp_pending set?
        'pend': bool(tails or unresolved or has_imp),
    }


def summarise_text(text):
    """-> {function name: summary or None (unanalysable)} for a TU."""
    out = {}
    for is_func, fname, lines in split_functions(text):
        if not is_func:
            continue
        fi = parse_function(fname, lines)
        if not fi.ok:
            out[fname] = None
            continue
        try:
            _, removable, stored, mwritten, calls, flags = analyse(fi)
        except Bail:
            out[fname] = None
            continue
        out[fname] = summarise_function(fi, removable, stored, mwritten, calls, flags)
    return out


class Closure:
    """M[f]: the CpuState fields f (then the tail jumps it parks) may write.
    P: functions that may return with a parked jump."""

    def __init__(self, summaries):
        self.fields = {}
        self.summ = summaries
        allbits = 0
        names = set()
        for v in summaries.values():
            if v:
                names.update(v['S'])
        for n in sorted(names):
            self.fields[n] = 1 << len(self.fields)
        self.allbits = (1 << (len(self.fields) + 1)) - 1
        M = {}
        rev = {}
        for f, v in summaries.items():
            if v is None or v['bad']:
                M[f] = self.allbits
            else:
                m = 0
                for n in v['S']:
                    m |= self.fields[n]
                M[f] = m
        for f, v in summaries.items():
            if v is None:
                continue
            for g in v['callees'] + v['tails']:
                if g not in summaries or summaries[g] is None:
                    M[f] = self.allbits
                else:
                    rev.setdefault(g, []).append(f)
        queue = [f for f in summaries]
        while queue:
            f = queue.pop()
            v = summaries[f]
            if v is None:
                continue
            m = M[f]
            for g in v['callees'] + v['tails']:
                if g in M:
                    m |= M[g]
            if m != M[f]:
                M[f] = m
                queue.extend(rev.get(f, ()))
        self.M = M

    def writes(self, callee):
        """bitmask of fields the callee may write; None = unknown"""
        v = self.summ.get(callee)
        if v is None:
            return None
        return self.M[callee]

    def may_park(self, callee):
        v = self.summ.get(callee)
        return v is None or v['pend']


# ---------------------------------------------------------------------------
# Inlining of small callees
#
# `sub_G(s);` costs the caller the spills and reloads around the call, the
# return-address plumbing, the pending-jump check, and the callee its own
# prologue loads, frame, exit stores and re-entry test. For a small G the
# call is most of its cost. The body of G is pasted at the call site:
#   * G's register locals that the caller also keeps are the caller's own
#     (what G would load from s is what the caller has spilled, and what the
#     caller would reload is what G leaves in them), so the callee's loads,
#     its exit stores and the caller's reloads of them disappear;
#   * the other locals (flags, registers the caller does not keep) are
#     declared in the block with G's own prologue loads, so G sees exactly
#     the memory it would have seen, and keeps its exit stores;
#   * labels get a per-site suffix, `return;` becomes a goto to the end of
#     the block, the re-entry prologue is dropped (the standalone G, which the
#     dispatcher and the replay loops still use, keeps it).
# The standalone functions stay as they are.
# ---------------------------------------------------------------------------

INLINE_MAX = int(_opt('ISAAC_LIFT_INLINE_MAX', 'inline_max', 0))      # statements; 0 = off
INLINE_HOT_MAX = int(_opt('ISAAC_LIFT_INLINE_HOT_MAX', 'inline_hot_max', 200))   # statements, for callees in the hot list
INLINE_HOT_MIN = int(_opt('ISAAC_LIFT_INLINE_HOT_MIN', 'inline_hot_min', 30))    # samples
INLINE_CALLER_MAX = 20000                                           # lines
INLINE_DEPTH = 0
LABEL_SUB_RE = re.compile(r'\bL_[0-9a-f]{8}(?:_[ie]\d+)*\b')
_HOT = {}            # name -> console profile samples (ISAAC_LIFT_HOT=<json>)
_FUNCS = {}          # name -> original lines (set by _prepare)
_NOINLINE = set()    # functions whose compiled text differs from the one in _FUNCS
_CALLEE_CACHE = {}
_INLINE_STATS = {}


def _callee(name, depth):
    """-> prepared callee or None. The callee's own small callees are already
    inlined (depth-limited)."""
    key = (name, depth)
    if key in _CALLEE_CACHE:
        return _CALLEE_CACHE[key]
    _CALLEE_CACHE[key] = None            # in progress: recursion is not inlined
    res = None
    lines = _FUNCS.get(name)
    if lines is not None and name not in _NOINLINE:
        if depth > 0:
            lines = inline_calls(name, lines, depth - 1)
        fi = parse_function(name, lines)
        ok = fi.ok
        if ok:
            for op in fi.ops:
                if op is not None and op[0] == K_RET and op[1] not in ('ret', 'unreachable'):
                    ok = False
                    break
        if ok:
            try:
                analyse(fi)
            except Bail:
                ok = False
        if ok:
            pro = []
            k = 1
            while k < len(lines) and PRO2_RE.match(lines[k]):
                pro.append(lines[k])
                k += 1
            body = lines[k:-1]
            # drop the re-entry prologue
            for a, l in enumerate(body):
                if l == REENTRY_IF:
                    for b in range(a + 1, len(body)):
                        if body[b] == '  }':
                            body = body[:a] + body[b + 1:]
                            break
                    else:
                        ok = False
                    break
            nstmt = sum(1 for l in body if 'RECOMP_VA' not in l and not l.startswith('  u') and not l.startswith('  b')
                        and not DECL_RE.match(l.strip()) and l.strip())
            if ok:
                res = {'pro': pro, 'body': body, 'nstmt': nstmt, 'tracked': fi.tracked}
    _CALLEE_CACHE[key] = res
    return res


def inline_calls(name, lines, depth=INLINE_DEPTH):
    if not INLINE_MAX or lines[0] != 'void %s(CpuState *restrict s) {' % name:
        return lines
    ft = {}
    k = 1
    while k < len(lines):
        m = PRO2_RE.match(lines[k])
        if not m:
            break
        ft[m.group(2)] = m.group(1)
        k += 1
    if not ft:
        return lines
    out = []
    n = len(lines)
    i = 0
    site = 0
    grown = 0
    while i < n:
        l = lines[i]
        m = CALL_DIRECT_RE.match(l) if l.startswith('  sub_') else None
        if m:
            g = m.group(1)
            c = _callee(g, depth) if g != name else None
            if c is not None and (c['nstmt'] <= INLINE_MAX or (c['nstmt'] <= INLINE_HOT_MAX
                                                           and _HOT.get(g, 0) >= INLINE_HOT_MIN)) \
                    and n + grown < INLINE_CALLER_MAX:
                site += 1
                sfx = '_i%d' % site
                gt = c['tracked']
                # registers both keep: the callee's are the caller's
                alias = set()
                for pl in c['pro']:
                    mm = PRO2_RE.match(pl)
                    if mm.group(1) == 'uint32_t' and ft.get(mm.group(2)) == 'uint32_t':
                        alias.add(mm.group(2))
                j = i + 1
                reloads = []
                while j < n:
                    mm = LOAD_RE.match(lines[j])
                    if not mm or mm.group(1) != mm.group(2) or mm.group(1) not in ft:
                        break
                    reloads.append(lines[j])
                    j += 1
                end = 'L_%s_e%d' % (g[4:], site)
                blk = [BLOCK_OPEN]
                ren = []
                for pl in c['pro']:
                    mm = PRO2_RE.match(pl)
                    if mm.group(2) in alias:
                        continue
                    if mm.group(1) != 'uint32_t':
                        # flags and segment registers get a private name: the caller's own are not shadowed
                        ren.append((re.compile(r'(?<![\w.>])' + mm.group(2) + r'\b'), mm.group(2) + sfx))
                        blk.append('  %s %s%s = s->%s;' % (mm.group(1), mm.group(2), sfx, mm.group(3)))
                    else:
                        blk.append(pl)
                body = c['body']
                nb = len(body)
                for bi, bl in enumerate(body):
                    st = bl.strip()
                    if st == 'return;':
                        # the exit stores of the registers the caller keeps
                        t = len(blk)
                        while t > 1:
                            mm = STORE_RE.match(blk[t - 1])
                            if mm and mm.group(1) == mm.group(2) and mm.group(1) in alias:
                                t -= 1
                            else:
                                break
                        # only stores that belong to this exit: they were copied just before
                        del blk[t:]
                        blk.append('  goto %s;' % end)
                        continue
                    if st.startswith('recomp_unreachable(') and st.endswith('return;'):
                        blk.append('  ' + st[:-len('return;')] + 'goto %s;' % end)
                        continue
                    if 'L_' in bl:
                        bl = LABEL_SUB_RE.sub(lambda mo: mo.group(0) + sfx, bl)
                    for rx, new in ren:
                        bl = rx.sub(new, bl)
                    blk.append(bl)
                blk.append('%s: ;' % end)
                blk.append(BLOCK_CLOSE)
                out.extend(blk)
                grown += len(blk)
                if _MAY_PARK(g):
                    out.append('  if (recomp_jmp_pending) recomp_run_pending(s);')
                for rl in reloads:
                    if rl.strip().split(' = ')[0] not in alias:
                        out.append(rl)
                _INLINE_STATS[g] = _INLINE_STATS.get(g, 0) + 1
                i = j
                continue
        out.append(l)
        i += 1
    return out


def _MAY_PARK(g):
    clo = _STATE.get('clo')
    return True if clo is None else clo.may_park(g)


def base_offset_pass(fi, lines, repl, stats):
    """T = R + const; ... MEMx(T) -> MEMxB(R, const) while R and T keep their
    values. The guest address then reaches the compiler as base + R + const
    (one addressing mode, and one shared `base + R` for several fields of
    the same object) instead of a 32-bit add whose result must be wrapped
    before it can be added to the base. The two differ only when R + const
    leaves the 32-bit range, which a pointer register with a small offset
    does not do (|const| <= 0xffff; a global's address as the constant, with
    a possibly negative index in R, is left alone)."""
    ops = fi.ops
    bit = fi.bit
    pend = {}
    for i, op in enumerate(ops):
        l = repl.get(i, lines[i])
        if op is not None:
            k = op[0]
            if k in (K_LABEL, K_CALL, K_CALLI, K_ESC, K_CASE, K_DEFAULT, K_BLOCK, K_BLOCKEND):
                pend.clear()
            elif k in (K_LOAD, K_ASSIGN):
                m = op[1]
                if pend:
                    for t in [t for t, v in pend.items() if bit[v[0]] & m]:
                        del pend[t]
        if pend:
            def sub_mem(mm):
                v = pend.get(mm.group(3))
                if v is None:
                    return mm.group(0)
                stats['bo_mem'] = stats.get('bo_mem', 0) + 1
                return 'MEM%s%sB(%s, %d%s' % (mm.group(1), mm.group(2), v[0], v[1], mm.group(4))
            def sub_ptr(mm):
                v = pend.get(mm.group(1))
                if v is None:
                    return mm.group(0)
                stats['bo_ptr'] = stats.get('bo_ptr', 0) + 1
                return 'RECOMP_PTR_BO(%s, %d)' % (v[0], v[1])
            l2 = MEM_USE_RE.sub(sub_mem, l)
            if 'RECOMP_PTR(' in l2:
                l2 = PTR_USE_RE.sub(sub_ptr, l2)
            if l2 != l:
                repl[i] = l2
                l = l2
        m = ADDR_DEF_RE.match(lines[i]) if (lines[i][2:3] in 'ubt' and lines[i].startswith('  ')) else None
        # an assignment to a temp kills what is known about it
        if op is None or op[0] in (K_ASSIGN, K_STORE, K_LOAD, K_MWRITE, K_RET, K_IF, K_GOTO) or True:
            am = TEMP_ASG_RE.match(lines[i])
            if am and am.group(1) in pend:
                del pend[am.group(1)]
        if m:
            r = m.group(2) or m.group(5)
            if r in bit:
                off = int(m.group(3) or m.group(4), 16)
                if off >= 0x80000000:
                    off -= 1 << 32
                if abs(off) <= BO_MAX:
                    pend[m.group(1)] = (r, off)


FS_ASSIGN_RE = re.compile(r'(?<![\w.>])FS_OFFSET\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)')
FS_USE_RE = re.compile(r'(?<![\w.>])FS_OFFSET\b')


def drop_fs_local(lines, stats):
    """FS_OFFSET (the TEB base) is only ever read by the lifted code: its
    local mirrors s->FS_OFFSET at every point. Reading the field where it is
    used lets go of one live register and of its loads and stores around calls."""
    if not any(l == '  uint32_t FS_OFFSET = s->FS_OFFSET;' for l in lines[:40]):
        return lines
    out = []
    for l in lines:
        st = l.strip()
        if st in ('uint32_t FS_OFFSET = s->FS_OFFSET;', 'FS_OFFSET = s->FS_OFFSET;', 's->FS_OFFSET = FS_OFFSET;'):
            continue
        if 'FS_OFFSET' in l:
            if FS_ASSIGN_RE.search(l):
                return lines                       # written somewhere: keep the local
            l = FS_USE_RE.sub('s->FS_OFFSET', l)
        out.append(l)
    stats['fs_offset_dropped'] = stats.get('fs_offset_dropped', 0) + 1
    return out


def optimise_function(name, lines, stats, clo):
    if INLINE_MAX:
        before = len(lines)
        lines = inline_calls(name, lines)
        if len(lines) != before:
            stats['inlined_lines'] = stats.get('inlined_lines', 0) + len(lines) - before
    if ENABLE_FS and lines[0] == 'void %s(CpuState *restrict s) {' % name and parse_function(name, lines).ok:
        lines = drop_fs_local(lines, stats)
    fi = parse_function(name, lines)
    if not fi.ok:
        k = 'bail:' + fi.why.split(':')[0]
        stats[k] = stats.get(k, 0) + 1
        return lines
    try:
        _, removable, stored, mwritten, calls, flags = analyse(fi)
    except Bail as e:
        stats['bail:' + str(e)] = stats.get('bail:' + str(e), 0) + 1
        return lines
    stats['funcs'] = stats.get('funcs', 0) + 1
    stats['stores_removed'] = stats.get('stores_removed', 0) + len(removable)
    drop = set(removable)
    repl = {}
    ops = fi.ops
    if clo is not None:
        for i, callee, before in calls:
            stats['calls'] = stats.get('calls', 0) + 1
            if not clo.may_park(callee):
                # nothing under the callee can park a jump: the check after the call is dead
                repl[i] = '  %s(s);' % callee
                stats['pending_checks_removed'] = stats.get('pending_checks_removed', 0) + 1
            wr = clo.writes(callee)
            j = i + 1
            while j < len(ops) and ops[j] is not None and ops[j][0] == K_LOAD:
                if wr is not None:
                    f = lines[j].strip().split(' = ')[0]
                    if (before & ops[j][1]) == 0 and not (wr & clo.fields.get(f, clo.allbits)):
                        drop.add(j)
                        stats['reloads_removed'] = stats.get('reloads_removed', 0) + 1
                    else:
                        stats['reloads_kept'] = stats.get('reloads_kept', 0) + 1
                else:
                    stats['reloads_kept'] = stats.get('reloads_kept', 0) + 1
                j += 1
    if ENABLE_BO:
        base_offset_pass(fi, lines, repl, stats)
    out = []
    for i, l in enumerate(lines):
        if i in drop:
            continue
        out.append(repl.get(i, l))
    return out


def transform_stats(name, text, clo=None):
    stats = {}
    out = []
    for is_func, fname, lines in split_functions(text):
        if is_func:
            out.extend(optimise_function(fname, lines, stats, clo))
        else:
            out.extend(lines)
    return '\n'.join(out), stats


REENTRY_IF = '  if (g_reentry_eip != 0u) {'
REENTRY_IF_X = '  if (__builtin_expect(g_reentry_eip != 0u, 0)) {'


def rewrite_text(text, clo, stats=None):
    """The whole rewrite of one TU."""
    out = []
    stats = {} if stats is None else stats
    for is_func, fname, lines in split_functions(text):
        if is_func:
            lines = optimise_function(fname, lines, stats, clo)
        out.extend(lines)
    res = '\n'.join(out)
    if HOT_PLACE and _HOT:
        res = place_hot(res, stats)
    if ENABLE_ICALL:
        n = res.count('\n  recomp_call_indirect(s, ')
        if n:
            res = res.replace('\n  recomp_call_indirect(s, ', '\n  recomp_call_indirect_inl(s, ')
            stats['icall_inline'] = stats.get('icall_inline', 0) + n
    # the re-entry prologue is a cold path (only the longjmp replay and the
    # mid-function dispatcher take it): keep it out of the straight line
    if ENABLE_COLD:
        old = '    uint32_t _rva = g_reentry_eip;\n    g_reentry_eip = 0u;\n'
        nc = res.count(old)
        if nc:
            # a call to a cold function makes GCC treat the re-entry block as never
            # executed and, with block partitioning, move it out of the hot function
            res = '#pragma GCC optimize("reorder-blocks-and-partition")\n' + res.replace(
                old, '    uint32_t _rva = recomp_reentry_take();\n')
            stats['reentry_cold'] = stats.get('reentry_cold', 0) + nc
    n = res.count(REENTRY_IF)
    if n:
        res = res.replace(REENTRY_IF, REENTRY_IF_X)
        stats['reentry_unlikely'] = stats.get('reentry_unlikely', 0) + n
    return res, stats


HOT_DEF_RE = re.compile(r'^void (sub_[0-9a-f]{8}(?:__lifted)?)\(CpuState \*restrict s\) \{', re.M)


def place_hot(text, stats):
    """Functions that the console profile saw go to .text.hot.<name>: the
    linker script gathers them in one run instead of leaving them 20-50 KB
    apart in 55 MB of code (fewer pages, fewer TLB entries and cache lines
    shared with code that never runs)."""
    def sub(m):
        if _HOT.get(m.group(1), 0) >= HOT_PLACE_MIN:
            stats['hot_placed'] = stats.get('hot_placed', 0) + 1
            return '__attribute__((section(".text.hot.%s"))) %s' % (m.group(1), m.group(0))
        return m.group(0)
    return HOT_DEF_RE.sub(sub, text)


def _gu_dir():
    d = _opt('ISAAC_LIFT_GU', 'gu', None)
    if d:
        return d
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    return os.path.join(repo, 'build', 'browsing-port', 'repo', 'output', 'recomp', 'lift', 'gu')


def _summ_job(path):
    with open(path) as f:
        text = f.read()
    small = {}
    if INLINE_MAX:
        for is_func, fname, lines in split_functions(text):
            if is_func and len(lines) <= 12 * max(INLINE_MAX, INLINE_HOT_MAX) + 60:
                small[fname] = lines
    return summarise_text(text), small


def _xf_job(args):
    path, clo = args
    with open(path) as f:
        text = f.read()
    return os.path.basename(path), hashlib.sha1(text.encode()).hexdigest(), rewrite_text(text, clo)


_STATE = {}


def _hooked_functions():
    """Functions build_native.py patches after the fact (LIFT_HOOKS): their
    compiled text is not the one in gu/, so a pasted copy would lack the hook."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import build_native
    names = set()
    gu = _gu_dir()
    for fname, anchor, _call in build_native.LIFT_HOOKS:
        with open(os.path.join(gu, fname)) as f:
            text = f.read()
        pos = text.find(anchor)
        if pos < 0:
            raise SystemExit('lift_opt: hook anchor not found in %s' % fname)
        a = text.rfind('\nvoid sub_', 0, pos)
        m = re.match(r'\nvoid (sub_[0-9a-f]{8}[\w]*)\(', text[a:a + 80])
        if a < 0 or not m:
            raise SystemExit('lift_opt: no function around the hook in %s' % fname)
        names.add(m.group(1))
    return names


def _prepare():
    """Closure over every lifted function (the interprocedural facts), then
    the rewrite of every lifted TU in parallel; transform() picks the result
    that matches the text it is handed."""
    if 'clo' in _STATE:
        return
    import glob
    import multiprocessing as mp
    files = sorted(glob.glob(os.path.join(_gu_dir(), 'lifted_*.c')))
    if not files:
        raise SystemExit('lift_opt: no lifted_*.c under %s' % _gu_dir())
    ctx = mp.get_context('fork')
    nproc = min(len(files), os.cpu_count() or 1)
    with ctx.Pool(nproc) as pool:
        summaries = {}
        for part, small in pool.map(_summ_job, files):
            summaries.update(part)
            _FUNCS.update(small)
        clo = Closure(summaries)
    hot = _opt('ISAAC_LIFT_HOT', 'hot', None)
    if hot and not os.path.isabs(hot):
        hot = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), hot)
    if hot:
        import json
        with open(hot) as f:
            _HOT.update(json.load(f))
    if INLINE_MAX:
        _NOINLINE.update(_hooked_functions())
    _STATE['clo'] = clo
    _STATE['pre'] = {}
    global _CLO_FOR_JOBS
    _CLO_FOR_JOBS = clo
    with ctx.Pool(nproc) as pool:
        for nm, digest, res in pool.imap_unordered(_xf_job_g, files):
            _STATE['pre'][(nm, digest)] = res


_CLO_FOR_JOBS = None


def _xf_job_g(path):
    return _xf_job((path, _CLO_FOR_JOBS))


def transform(name: str, text: str) -> str:
    if not (name.startswith('lifted_') and name.endswith('.c')):
        return text
    _prepare()
    key = (name, hashlib.sha1(text.encode()).hexdigest())
    hit = _STATE['pre'].get(key)
    if hit is None:
        # a TU build_native.py edited after generation (hooks): the edited
        # functions must not be pasted anywhere
        if INLINE_MAX:
            with open(os.path.join(_gu_dir(), name)) as f:
                orig = {n: l for fn, n, l in split_functions(f.read()) if fn}
            for fn, n, l in split_functions(text):
                if fn and orig.get(n) != l and n not in _NOINLINE:
                    raise SystemExit('lift_opt: %s differs from gu/ and is not a known hook' % n)
        hit = rewrite_text(text, _STATE['clo'])
    return hit[0]


def hot_from_runs(run_dirs):
    """{sub_XXXXXXXX: samples} from console profile runs (build/bench/switch-*):
    each holds an nxlink.log with the PROF rows and the run's elf.syms."""
    import bisect
    import collections
    from pathlib import Path
    total = collections.Counter()
    for rd in map(Path, run_dirs):
        logs = sorted(rd.rglob('nxlink.log'))
        if not logs or not (rd / 'elf.syms').exists():
            continue
        log = logs[-1].read_bytes().decode('utf-8', 'replace')
        m = re.search(r'PROFBASE main=0x([0-9a-f]+)', log)
        rows = re.findall(r'^PROF ([0-9a-f]+) ([0-9a-f]+) (\d+)$', log, re.M)
        if not m or not rows:
            continue
        syms = []
        for line in (rd / 'elf.syms').read_text().splitlines():
            p = line.split()
            if len(p) == 4 and p[2] in 'tTwW':
                syms.append((int(p[0], 16), int(p[1], 16), p[3]))
        syms.sort()
        addrs = [a for a, _, _ in syms]
        main = next(a for a, _, n in syms if n == 'main')
        slide = int(m.group(1), 16) - main
        for pc, _lr, n in rows:
            i = bisect.bisect_right(addrs, int(pc, 16) - slide) - 1
            if i >= 0 and syms[i][2].startswith('sub_'):
                total[syms[i][2]] += int(n)
    return dict(total)


if __name__ == '__main__':
    import json
    import argparse
    ap = argparse.ArgumentParser(description='lift_opt helpers')
    ap.add_argument('--hot-from', nargs='+', metavar='RUN_DIR',
                    help='console profile runs (build/bench/switch-*) -> the hot list')
    ap.add_argument('-o', '--output', required=True)
    a = ap.parse_args()
    hot = hot_from_runs(a.hot_from)
    with open(a.output, 'w') as f:
        json.dump(hot, f)
    print('%d lifted functions, %d samples -> %s' % (len(hot), sum(hot.values()), a.output))

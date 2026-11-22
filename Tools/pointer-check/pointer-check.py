#!/usr/bin/env python3
"""Lists the raw pointers that Duet3Expansion declares as class members or outside functions and that can later point elsewhere
(reseatable), and fails for each one that allowlist.txt does not name. See README.md.

The declarations come from the DWARF debug information of firmware built with -g, read with the readelf of the Arm GNU
toolchain, so the check sees what the compiler built for each configuration and needs no other compiler.

    pointer-check.py [--allowlist FILE] [--update] [--keys] [--root DIR] ELF...

A declaration counts when its type, after typedefs and volatile, is a pointer or an array of pointers and the pointer itself is
not const: T *const is fixed, const T * is reseatable. Function pointers count. Declarations inside functions (locals, static
locals, local classes) and the members of lambda closures do not.
"""
import argparse
import os
import re
import subprocess
import sys

# Code under src/ that Duet3Expansion takes from elsewhere
VENDORED = ('libc/',)

CLASS_TAGS = {'DW_TAG_structure_type', 'DW_TAG_class_type', 'DW_TAG_union_type'}
SCOPE_TAGS = CLASS_TAGS | {'DW_TAG_namespace'}
FUNCTION_TAGS = {'DW_TAG_subprogram', 'DW_TAG_lexical_block', 'DW_TAG_inlined_subroutine', 'DW_TAG_subroutine_type'}
TYPE_TAGS = CLASS_TAGS | {'DW_TAG_base_type', 'DW_TAG_enumeration_type', 'DW_TAG_typedef', 'DW_TAG_pointer_type',
                          'DW_TAG_const_type', 'DW_TAG_volatile_type', 'DW_TAG_restrict_type', 'DW_TAG_atomic_type',
                          'DW_TAG_array_type', 'DW_TAG_subroutine_type', 'DW_TAG_reference_type',
                          'DW_TAG_rvalue_reference_type', 'DW_TAG_ptr_to_member_type', 'DW_TAG_unspecified_type'}

DIE_RE = re.compile(r'^\s*<(\d+)><([0-9a-f]+)>: Abbrev Number: \d+(?: \((\w+)\))?')
ATTR_RE = re.compile(r'^\s*<[0-9a-f]+>\s+(DW_AT_\w+)\s*:\s*(.*)$')


def readelf(tool, what, elf):
    """Yields the lines of readelf --debug-dump=WHAT without keeping the whole dump (hundreds of MB) in memory."""
    with subprocess.Popen([tool, '--debug-dump=' + what, elf], stdout=subprocess.PIPE, text=True, errors='replace') as p:
        yield from p.stdout
    if p.returncode:
        sys.exit(f'error: {tool} --debug-dump={what} {elf} failed with exit code {p.returncode}')


def strval(v):
    m = re.match(r'^\((?:indirect|inline)[^)]*\):\s*(.*)$', v.strip())
    return (m.group(1) if m else v).strip()


def ref(v):
    m = re.match(r'^<0x([0-9a-f]+)>', v)
    return int(m.group(1), 16) if m else None


def plain(name):
    """Strips template arguments, so that every instance of a class template has one key."""
    while True:
        stripped = re.sub(r'<[^<>]*>', '', name)
        if stripped == name:
            return name
        name = stripped


def line_tables(lines):
    """Maps the offset of each line table to its directories and files, as readelf --debug-dump=rawline prints them."""
    tables, cur, mode = {}, None, None
    for line in lines:
        m = re.match(r'^\s*Offset:\s+(0x[0-9a-f]+|\d+)\s*$', line)
        if m:
            cur = {'dirs': {}, 'files': {}}
            tables[int(m.group(1), 0)] = cur
            mode = None
        elif cur is None:
            continue
        elif line.startswith(' The Directory Table'):
            mode = 'dirs'
        elif line.startswith(' The File Name Table'):
            mode = 'files'
        elif not line.strip():
            mode = None
        elif mode:
            fields = line.rstrip('\n').split('\t')
            if not fields[0].strip().isdigit():
                continue                                    # column header
            if mode == 'dirs':
                cur['dirs'][int(fields[0])] = strval(fields[-1])
            elif len(fields) >= 3:
                cur['files'][int(fields[0])] = (int(fields[1]), strval(fields[-1]))
    return tables


def scan(elf, root, tool):
    """Returns {key: (path, line, type)} for the reseatable raw pointers declared in root/src, outside VENDORED."""
    tables = line_tables(readelf(tool, 'rawline', elf))
    types, decls, stack, closures = {}, {}, [], set()
    cu, die, units = None, None, []
    src = os.path.join(root, 'src') + os.sep

    def finish(d):
        if d is None:
            return
        if d['tag'] in TYPE_TAGS:
            types[d['off']] = (d['tag'], d.get('type'), d.get('name'))
        if d['tag'] in ('DW_TAG_member', 'DW_TAG_variable'):
            decls[d['off']] = d
        # GCC gives a lambda's closure type no name, only a compiler-made operator()
        if d['tag'] == 'DW_TAG_subprogram' and d.get('name') == 'operator()' and d.get('artificial') \
                and d['parent'][0] in CLASS_TAGS:
            closures.add(d['parent'][2])

    for line in readelf(tool, 'info', elf):
        m = DIE_RE.match(line)
        if m:
            finish(die)
            depth, off, tag = int(m.group(1)), int(m.group(2), 16), m.group(3)
            del stack[depth:]
            if tag is None:                                 # end of the children of the entry above
                die = None
                continue
            if tag == 'DW_TAG_compile_unit':
                cu = {'comp_dir': '', 'stmt': None, 'name': ''}
                units.append(cu)
            parents = stack[1:]
            die = {'off': off, 'tag': tag, 'cu': cu,
                   'in_function': any(t in FUNCTION_TAGS for t, _, _ in parents),
                   'parent': parents[-1] if parents else (None, None, None),
                   'scope': '::'.join(plain(n or ('(anonymous namespace)' if t == 'DW_TAG_namespace' else '(anonymous)'))
                                      for t, n, _ in parents if t in SCOPE_TAGS)}
            stack.append([tag, None, off])
            continue
        if die is None:
            continue
        m = ATTR_RE.match(line)
        if not m:
            continue
        attr, value = m.group(1), m.group(2).strip()
        if attr == 'DW_AT_name':
            die['name'] = strval(value)
            stack[-1][1] = die['name']
            if die['tag'] == 'DW_TAG_compile_unit':
                cu['name'] = die['name']
        elif attr == 'DW_AT_comp_dir':
            cu['comp_dir'] = strval(value)
        elif attr == 'DW_AT_stmt_list':
            cu['stmt'] = int(value, 0)
        elif attr == 'DW_AT_type':
            die['type'] = ref(value)
        elif attr == 'DW_AT_specification':
            die['spec'] = ref(value)
        elif attr == 'DW_AT_decl_file':
            die['file'] = int(value.split()[0])
        elif attr == 'DW_AT_decl_line':
            die['line'] = int(value.split()[0])
        elif attr == 'DW_AT_external':
            die['external'] = True
        elif attr == 'DW_AT_artificial':
            die['artificial'] = True
    finish(die)
    if not any(os.path.normpath(os.path.join(c['comp_dir'], c['name'])).startswith(src) for c in units):
        sys.exit(f'error: {elf} has no debug information for {src}, build it with DEBUG_FLAGS=-g')

    def reseatable(t):
        const = False
        for _ in range(64):
            tag, nxt, _name = types.get(t, (None, None, None))
            if tag in ('DW_TAG_typedef', 'DW_TAG_restrict_type', 'DW_TAG_atomic_type', 'DW_TAG_volatile_type',
                       'DW_TAG_array_type'):
                t = nxt
            elif tag == 'DW_TAG_const_type':
                const = True
                t = nxt
            else:
                return tag == 'DW_TAG_pointer_type' and not const
        return False

    def render(t, depth=0):
        tag, nxt, name = types.get(t, (None, None, None))
        if depth > 16 or (t is not None and tag is None):
            return '?'
        if t is None:
            return 'void'
        if tag == 'DW_TAG_pointer_type':
            return render(nxt, depth + 1) + ' *'
        if tag == 'DW_TAG_const_type':
            return render(nxt, depth + 1) + ' const'
        if tag == 'DW_TAG_volatile_type':
            return render(nxt, depth + 1) + ' volatile'
        if tag == 'DW_TAG_array_type':
            return render(nxt, depth + 1) + ' []'
        if tag == 'DW_TAG_subroutine_type':
            return 'function'
        return plain(name) if name else '(anonymous)'

    def path_of(d):
        c = d['cu']
        table = tables.get(c['stmt'])
        entry = table and table['files'].get(d.get('file'))
        if not entry:
            return None
        directory = table['dirs'].get(entry[0], c['comp_dir'] if entry[0] == 0 else '')
        return os.path.normpath(os.path.join(c['comp_dir'], directory, entry[1]))

    found = {}
    for d in decls.values():
        decl = d                                            # a definition outside its class names the declaration
        while 'spec' in decl and decl['spec'] in decls:
            decl = decls[decl['spec']]
        ptag, _, poff = decl['parent']
        if decl['in_function'] or poff in closures:
            continue
        member = ptag in CLASS_TAGS
        if decl['tag'] == 'DW_TAG_member' and not member:
            continue
        t = d.get('type', decl.get('type'))
        if not reseatable(t):
            continue
        path = path_of(d if 'file' in d else decl)        # None for compiler-made members such as the vtable pointer
        line = d.get('line', decl.get('line'))
        if path is None or line is None or not path.startswith(src) or path[len(src):].startswith(VENDORED) \
                or not decl.get('name'):
            continue
        rel = path[len(src):]
        name = decl['name'] if not decl['scope'] else decl['scope'] + '::' + decl['name']
        # Without a name that is unique in the firmware, the file name makes the key unique: a static variable, a variable in
        # an anonymous namespace, a member of an anonymous class or of a class in an anonymous namespace
        unique = (decl.get('external') or member) and '(anonymous' not in decl['scope']
        key = name if unique else f'{rel}: {name}'
        if key not in found or (rel, line) < found[key][:2]:
            found[key] = (rel, line, render(t))
    return found


def read_allowlist(path):
    """Returns the leading comment block and {key: comment}."""
    header, keys = [], {}
    if path and os.path.exists(path):
        with open(path) as f:
            for line in f:
                text, _, comment = line.rstrip('\n').partition('#')
                if text.strip():
                    keys[text.strip()] = comment.strip()
                elif not keys:
                    header.append(line.rstrip('\n'))
    return header, keys


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('elf', nargs='+', help='firmware ELF files built with DEBUG_FLAGS=-g')
    ap.add_argument('--allowlist', help='pointers that may stay raw, default none')
    ap.add_argument('--update', action='store_true', help='rewrite the allowlist with the pointers found, keeping comments')
    ap.add_argument('--keys', action='store_true', help='print the key of every pointer found and nothing else')
    ap.add_argument('--root', default=os.path.normpath(os.path.join(here, '..', '..')),
                    help='the Duet3Expansion directory the firmware was compiled in, default ../..')
    ap.add_argument('--readelf', default=os.environ.get('CROSS_COMPILE', 'arm-none-eabi-') + 'readelf')
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    found = {}
    for elf in args.elf:
        for key, value in scan(elf, root, args.readelf).items():
            if key not in found or value[:2] < found[key][:2]:
                found[key] = value

    if args.keys:
        print('\n'.join(sorted(found)))
        return 0

    header, allowed = read_allowlist(args.allowlist)
    if args.update:
        with open(args.allowlist, 'w') as f:
            for line in header:
                f.write(line + '\n')
            width = max((len(k) for k in found), default=0)
            for key in sorted(found):
                rel, line, type_name = found[key]
                comment = allowed.get(key) or type_name
                f.write(f'{key:{width}}  # {comment}\n')
        print(f'{args.allowlist}: {len(found)} pointers ({len(set(found) - set(allowed))} added, '
              f'{len(set(allowed) - set(found))} removed)')
        return 0

    new = sorted(set(found) - set(allowed))
    for key in new:
        rel, line, type_name = found[key]
        print(f'src/{rel}:{line}: error: new raw pointer that can be reseated: {key} ({type_name}). Use a pointer class '
              f'that states how it is shared, or add it to {args.allowlist or "the allowlist"} with the reason')
    for key in sorted(set(allowed) - set(found)):
        print(f'note: {key} is in the allowlist but not declared in the checked firmware')
    print(f'{len(found)} reseatable raw pointers in {len(args.elf)} ELF file(s), {len(found) - len(new)} allowlisted, '
          f'{len(new)} new')
    return 1 if new else 0


if __name__ == '__main__':
    sys.exit(main())

"""Guest address placement shared by the lifter, runtime build and PS5 smoke title.

Guest addresses equal host addresses, and the lifter writes absolute addresses into the generated C, so the
generator and the runtime must agree on every module base. This is the single place that decides them: the generator
calls layout() and writes the result into generated/<version>/layout.h and layout.json, which the runtime and the
build read.

Modules are placed in Horizon's load order (rtld is dropped; the host links the modules): main, subsdk0..9, sdk.
Each base is aligned to 2 MiB. The 39-bit IPC address limit is independent of the placement.
"""
import json
import os
from contextlib import contextmanager
from dataclasses import asdict, dataclass
from pathlib import Path

ALIGN = 0x200000
ADDRESS_LIMIT = 1 << 39
ORDER = ['main'] + [f'subsdk{i}' for i in range(10)] + ['sdk']
GIB = 1 << 30
KERNEL_SIZE = 256 << 20
HOSTFN_SIZE = 1 << 20
LOW_SEARCH_FROM = 4 * GIB
LOW_SIZE = 14 * GIB
ALIAS_SIZE = 64 * GIB
ALIAS_ALIGN = 4 * GIB
DEFAULT = 'desktop'
INCOMPLETE = 'generation.pending'


@dataclass(frozen=True)
class Placement:
    module_base: int
    kernel_base: int
    hostfn_base: int
    low_base: int = 0        # Zero means search the desktop range at startup.
    alias_base: int = 0


PLACEMENTS = {
    DEFAULT: Placement(0x7000000000, 0x7010000000, 0x7020000000),
    # Candidate from the smoke title. Console availability is still unverified.
    'ps5-smoke': Placement(160 * GIB, 160 * GIB + KERNEL_SIZE, 160 * GIB + 2 * KERNEL_SIZE, 16 * GIB, 32 * GIB),
    # The console smoke title held these ranges together in both reservation trials.
    'ps5-low36': Placement(160 * GIB, 160 * GIB + KERNEL_SIZE, 160 * GIB + 2 * KERNEL_SIZE, 36 * GIB, 64 * GIB),
    # The console held the full layout and backed all five sampled units.
    'ps5-high144': Placement(160 * GIB, 160 * GIB + KERNEL_SIZE, 160 * GIB + 2 * KERNEL_SIZE, 144 * GIB, 64 * GIB),
}


def align_up(v, a=ALIGN):
    return (v + a - 1) & ~(a - 1)


def first_lower_ranges(placement):
    """The fixed lower ranges or the desktop search's first candidate."""
    low = placement.low_base or LOW_SEARCH_FROM
    return low, placement.alias_base or align_up(low + LOW_SIZE, ALIAS_ALIGN)


def validate_placement(placement):
    p = placement
    if p.module_base <= 0 or p.module_base % ALIGN or p.kernel_base % ALIGN or p.hostfn_base % ALIGN:
        raise ValueError('module, kernel and host-function bases must be 2 MiB aligned')
    if not (p.module_base < p.kernel_base <= p.hostfn_base - KERNEL_SIZE and p.hostfn_base + HOSTFN_SIZE <= ADDRESS_LIMIT):
        raise ValueError('fixed guest windows overlap or exceed the 39-bit address limit')
    if bool(p.low_base) != bool(p.alias_base):
        raise ValueError('stack/heap and alias bases must both be fixed or both searched')
    low, alias = first_lower_ranges(p)
    low_end, alias_end = low + LOW_SIZE, alias + ALIAS_SIZE
    if (min(low, alias) < LOW_SEARCH_FROM or low % GIB or alias % ALIAS_ALIGN or
            max(low_end, alias_end) > p.module_base):
        raise ValueError('stack/heap and alias reservations must be aligned and fit below the module window')
    if low < alias_end and alias < low_end:
        raise ValueError('stack/heap and alias reservations overlap')


def validate_modules(mods, placement):
    validate_placement(placement)
    names = [n for n, _, _ in mods]
    if not names or names[0] != 'main' or names != [n for n in ORDER if n in names]:
        raise ValueError('modules must start with main and follow ExeFS load order')
    base = placement.module_base
    for name, at, size in mods:
        if at != base:
            raise ValueError(f'{name} base {at:#x} does not match selected layout ({base:#x})')
        if size <= 0 or size & 0xfff or at + size > placement.kernel_base:
            raise ValueError(f'{name} has an invalid size or reaches the kernel window')
        base = align_up(at + size)


def layout(exefs, profile=DEFAULT):
    """[(name, base, size)] for the modules present in the ExeFS directory, in load order."""
    from nso import Nso

    exefs = Path(exefs)
    placement = PLACEMENTS[profile]
    out, base = [], placement.module_base
    for name in ORDER:
        path = exefs / name
        if not path.exists():
            continue
        size = Nso(str(path)).size
        out.append((name, base, size))
        base = align_up(base + size)
    validate_modules(out, placement)
    return out


def module_header(mods):
    lines = ['// Module layout chosen by recomp/layout.py. Generated; do not edit.', '#pragma once', '']
    for n, b, s in mods:
        lines.append(f'#define MOD_{n.upper()}_BASE {b:#x}ull')
        lines.append(f'#define MOD_{n.upper()}_SIZE {s:#x}ull')
    lines += ['', f'#define MOD_COUNT {len(mods)}',
              '#define MOD_LIST(X) ' + ' '.join(f'X({n}, {n.upper()})' for n, _, _ in mods), '']
    return '\n'.join(lines)


def placement_info(profile):
    return {'profile': profile, **asdict(PLACEMENTS[profile])}


def write(mods, outdir, profile=DEFAULT):
    """Write the module files and the fixed-placement snapshot for the builder."""
    outdir = Path(outdir)
    validate_modules(mods, PLACEMENTS[profile])
    outdir.mkdir(parents=True, exist_ok=True)
    metadata = {
        'placement.json': json.dumps(placement_info(profile), indent=1) + '\n',
        'layout.json': json.dumps(
            [{'name': n, 'base': f'{b:#x}', 'size': f'{s:#x}'} for n, b, s in mods], indent=1) + '\n',
        'layout.h': module_header(mods),
    }
    for name, contents in metadata.items():
        path = outdir / name
        if not path.exists() or path.read_text() != contents:
            temporary = outdir / f'.{name}.tmp'
            temporary.write_text(contents)
            temporary.replace(path)


def read(outdir):
    """[(name, base, size)] from a layout.json written by write()."""
    data = json.loads((Path(outdir) / 'layout.json').read_text())
    return [(m['name'], int(m['base'], 16), int(m['size'], 16)) for m in data]


def check_profile(outdir, profile):
    """A missing snapshot is valid only for legacy desktop generation."""
    path = Path(outdir) / 'placement.json'
    if path.exists():
        if json.loads(path.read_text()) != placement_info(profile):
            raise ValueError(f'{path} does not match the selected {profile!r} placement')
    elif profile != DEFAULT:
        raise ValueError(f'{path} is required for the selected {profile!r} placement')


def check_output_profile(outdir, profile):
    """Keep code from different placements out of the same generation directory."""
    outdir = Path(outdir)
    if (outdir / 'placement.json').exists():
        check_profile(outdir, profile)
    if (outdir / 'layout.json').exists():
        mods = read(outdir)
        if mods and mods[0][1] != PLACEMENTS[profile].module_base:
            raise ValueError(f'{outdir} already contains a different placement')
    elif profile != DEFAULT and not (outdir / 'placement.json').exists() and any(outdir.glob('*/*.c')):
        raise ValueError(f'{outdir} contains generated code with no placement metadata')


@contextmanager
def output_lock(outdir):
    """Exclude generation and build together. The OS releases the lock after an interrupted process."""
    with (outdir / '.generation.lock').open('ab') as file:
        try:
            if os.name == 'nt':
                import msvcrt

                file.seek(0)
                msvcrt.locking(file.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(file, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise ValueError(f'{outdir} is already in use') from error
        yield


def read_checked(outdir, profile):
    outdir = Path(outdir)
    if (outdir / INCOMPLETE).exists():
        raise ValueError(f'{outdir} has incomplete generation; rerun gen.py before building')
    check_profile(outdir, profile)
    mods = read(outdir)
    validate_modules(mods, PLACEMENTS[profile])
    if (outdir / 'layout.h').read_text() != module_header(mods):
        raise ValueError(f'{outdir}/layout.h does not match layout.json')
    if profile != DEFAULT:
        for name, base, _ in mods:
            folder = outdir / name
            manifest = json.loads((folder / 'manifest.json').read_text())
            sources = {p.name for p in folder.glob('*.c')}
            if manifest['module'] != name or int(manifest['base'], 16) != base:
                raise ValueError(f'{folder} manifest does not match the selected layout')
            if sources != set(manifest['chunks']) | {manifest['table']}:
                raise ValueError(f'{folder} sources do not match the generation manifest')
    return mods


def placement_defines(profile, prefix='RT_'):
    """C constants from the same placement used to generate module addresses."""
    p = PLACEMENTS[profile]
    validate_placement(p)
    values = {
        'ADDRESS_LIMIT': ADDRESS_LIMIT, 'MODULE_ALIGN': ALIGN, 'MODULE_BASE': p.module_base, 'KERNEL_BASE': p.kernel_base,
        'KERNEL_SIZE': KERNEL_SIZE, 'HOSTFN_BASE': p.hostfn_base, 'HOSTFN_SIZE': HOSTFN_SIZE,
        'LOW_BASE': p.low_base, 'LOW_SEARCH_FROM': LOW_SEARCH_FROM, 'LOW_SIZE': LOW_SIZE,
        'ALIAS_BASE': p.alias_base, 'ALIAS_ALIGN': ALIAS_ALIGN, 'ALIAS_SIZE': ALIAS_SIZE,
    }
    return ''.join(f'#define {prefix}{name} {value:#x}ull\n' for name, value in values.items())

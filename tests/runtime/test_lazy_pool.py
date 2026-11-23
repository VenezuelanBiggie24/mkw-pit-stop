"""Run lazy Vulkan pool backing (runtime/gfx/vk/mem/vk_mem_back.c lazy_back) with synthetic allocation results: a pool
that gets no storage stays lazy and is tried again from the next submission; one that gets it is published and marked
used by the current submission. Usage: uv run pytest tests/runtime/test_lazy_pool.py."""

import shutil
import subprocess
from pathlib import Path

import pytest

SOURCE = Path(__file__).resolve().parents[2] / "runtime/gfx/vk/mem/vk_mem_back.c"

HARNESS = r"""
#include <assert.h>
#include <stdint.h>

typedef struct {
    uint64_t lo, size;
    unsigned flags;
    int lazy, buf;
    uint64_t touch_sub, lazy_retry;
} PoolRec;

static uint64_t vk_replay_submit = 41;
static int import_ok;
static int32_t plat_inc32(volatile int32_t *n) { return ++*n; }
static void mem_thaw(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
static void *vk_ctx(void) { return 0; }
static void rt_log(const char *format, ...) { (void)format; }

static int import_pool(void *ctx, PoolRec *r, uint64_t addr, uint64_t size) {
    (void)ctx;
    (void)addr;
    (void)size;
    if (import_ok) r->buf = 1;
    return import_ok;
}

#include "lazy.inc"

int main(void) {
    // out of memory: still lazy, tried again from the next submission on, no buffer
    PoolRec failed = {.lo = 0x1000, .size = 0x4000, .flags = 0x22, .lazy = 1};
    lazy_back(&failed);
    assert(failed.lazy && !failed.buf);
    assert(failed.lazy_retry == vk_replay_submit + 1);

    // backed: not lazy any more, its buffer published, marked used by this submission
    PoolRec backed = {.lo = 0x8000, .size = 0x4000, .flags = 0x22, .lazy = 1};
    import_ok = 1;
    lazy_back(&backed);
    assert(!backed.lazy && backed.buf);
    assert(backed.touch_sub == vk_replay_submit);
    return 0;
}
"""


def test_lazy_pool_backing_retries_and_marks_use(tmp_path: Path) -> None:
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index("static long n_lazy_backed;")
    end = source.index("\n}", start) + len("\n}")
    (tmp_path / "lazy.inc").write_text(source[start:end], encoding="utf-8")
    harness = tmp_path / "main.c"
    harness.write_text(HARNESS, encoding="utf-8")
    executable = tmp_path / "lazy_pool.exe"
    subprocess.run([cc, "-std=c11", str(harness), "-o", str(executable)], check=True, timeout=30)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr

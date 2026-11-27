// C heap profile (heap_prof.c): MKW_HEAP_PROFILE=1 on the PS5 tracks every block of the title heap by call site and
// logs "heap: ..." every 60 s. A diagnostic: it slows every malloc and free.
#pragma once

void heap_prof_init(void);   // main.c, right after mem_init (mkw.ini is read): starts it when the switch is on

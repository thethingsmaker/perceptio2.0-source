/*
 * Diagnostics (added 0.3.23 for the heap-corruption hunt of 2026-10-29: the partition list in RAM was overwritten,
 * suspected task stack overrun). Needs CONFIG_FREERTOS_USE_TRACE_FACILITY.
 */
#pragma once

/* One log line with the tasks that have the least stack left (bytes never used since boot), lowest first;
 * a W line when any task is under 256 B. Called once a minute by main. */
void diag_stack_report(void);

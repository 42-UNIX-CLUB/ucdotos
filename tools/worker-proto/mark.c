// SPDX-License-Identifier: GPL-2.0-or-later
/* Worker-process prototype, approach B: linking this object into a program marks it to run
 * in its own Worker (boot/worker-proto/wasm.mjs looks for the export). One possible way of
 * marking a program; see docs/WORKER-PROCESSES.md for the others. */
__attribute__((export_name("ucos_worker"))) void ucos_worker(void) {}

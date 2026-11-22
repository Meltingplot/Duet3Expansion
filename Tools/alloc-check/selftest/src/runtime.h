// The names that alloc-check.py knows, for the selftest: the region primitives of FreeRTOS, the heap functions of
// Duet3Expansion's src/libc/nano-mallocr.c, and the instructions that CoreN2G's Core.h and CoreIO.h use to mask interrupts.
// HAS_BASEPRI is 1 for the cores that have BASEPRI (Cortex-M4 of the SAME51), 0 for the Cortex-M0+ of the SAMC21.
// runtime.cpp defines the functions; they do nothing useful, the selftest only reads their machine code.

#ifndef ALLOC_CHECK_SELFTEST_RUNTIME_H
#define ALLOC_CHECK_SELFTEST_RUNTIME_H

#include <cstddef>
#include <cstdint>

extern "C"
{
	void vTaskSuspendAll() noexcept;
	long xTaskResumeAll() noexcept;
	void vPortEnterCritical() noexcept;
	void vPortExitCritical() noexcept;
	void *malloc(size_t n) noexcept;
	void free(void *p) noexcept;
}

static inline __attribute__((always_inline)) uint32_t GetPrimask() noexcept
{
	uint32_t r;
	__asm volatile ("mrs %0, primask" : "=r" (r) :: "memory");
	return r;
}

static inline __attribute__((always_inline)) void DisableIrq() noexcept { __asm volatile ("cpsid i" ::: "memory"); }
static inline __attribute__((always_inline)) void SetPrimask(uint32_t v) noexcept { __asm volatile ("msr primask, %0" :: "r" (v) : "memory"); }

#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
# define HAS_BASEPRI	1

static inline __attribute__((always_inline)) uint32_t GetBasepri() noexcept
{
	uint32_t r;
	__asm volatile ("mrs %0, basepri" : "=r" (r) :: "memory");
	return r;
}

static inline __attribute__((always_inline)) void SetBasepriMax(uint32_t v) noexcept { __asm volatile ("msr basepri_max, %0" :: "r" (v) : "memory"); }
static inline __attribute__((always_inline)) void SetBasepri(uint32_t v) noexcept { __asm volatile ("msr basepri, %0" :: "r" (v) : "memory"); }
#else
# define HAS_BASEPRI	0
#endif

#endif

/*
 * Copyright (c) 2012-2015 Wind River Systems, Inc.
 * Copyright (c) 2021 Synopsys, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

void __do_global_ctors_aux(void);
void __do_init_array_aux(void);

void z_init_static(void)
{
#if defined(CONFIG_STATIC_INIT_GNU)
	__do_global_ctors_aux();
	__do_init_array_aux();
#elif defined(__CCAC__) /* ARC MWDT */
	__do_global_ctors_aux();
#endif
}

/**
 * @section - Constructor module
 * @brief
 * The ctors section contains a list of function pointers that execute both the C++ constructors of
 * static global objects, as well as either C or C++ initializer functions (declared with the
 * attribute constructor). These must be executed before the application's main() routine.
 *
 * NOTE: Not all compilers put those function pointers into the ctors section;
 * some put them into the init_array section instead.
 */

#ifdef CONFIG_STATIC_INIT_GNU

/* What a constructor function pointer looks like */

typedef void (*CtorFuncPtr)(void);

/* Constructor function pointer list is generated by the linker script. */

extern CtorFuncPtr __ZEPHYR_CTOR_LIST__[];
extern CtorFuncPtr __ZEPHYR_CTOR_END__[];

/**
 *
 * @brief Invoke all C++ style global object constructors
 *
 * This routine is invoked by the kernel prior to the execution of the
 * application's main().
 */
void __do_global_ctors_aux(void)
{
	unsigned int nCtors;

	nCtors = (unsigned long)__ZEPHYR_CTOR_LIST__[0];

	while (nCtors >= 1U) {
		__ZEPHYR_CTOR_LIST__[nCtors--]();
	}
}

#endif

/*
 * @section
 * @brief Execute initialization routines referenced in .init_array section
 */

#ifdef CONFIG_STATIC_INIT_GNU

typedef void (*func_ptr)(void);

extern func_ptr __zephyr_init_array_start[];
extern func_ptr __zephyr_init_array_end[];

/**
 * @brief Execute initialization routines referenced in .init_array section
 */
void __do_init_array_aux(void)
{
	for (func_ptr *func = __zephyr_init_array_start;
		func < __zephyr_init_array_end;
		func++) {
		(*func)();
	}
}

#endif
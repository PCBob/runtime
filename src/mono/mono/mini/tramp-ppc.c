/*
 * tramp-ppc.c: JIT trampoline code for PowerPC
 *
 * Authors:
 *   Dietmar Maurer (dietmar@ximian.com)
 *   Paolo Molaro (lupus@ximian.com)
 *   Carlos Valiente <yo@virutass.net>
 *   Andreas Faerber <andreas.faerber@web.de>
 *
 * (C) 2001 Ximian, Inc.
 * (C) 2007-2008 Andreas Faerber
 */

#include <config.h>
#include <glib.h>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/tabledefs.h>
#include <mono/arch/ppc/ppc-codegen.h>

#include "mini.h"
#include "mini-ppc.h"

static guint8* nullified_class_init_trampoline;

/* Same as mono_create_ftnptr, but doesn't require a domain */
static gpointer
mono_ppc_create_ftnptr (guint8 *code)
{
#ifdef PPC_USES_FUNCTION_DESCRIPTOR
	MonoPPCFunctionDescriptor *ftnptr = mono_global_codeman_reserve (sizeof (MonoPPCFunctionDescriptor));

	ftnptr->code = code;
	ftnptr->toc = NULL;
	ftnptr->env = NULL;

	return ftnptr;
#else
	return code;
#endif
}

/*
 * Return the instruction to jump from code to target, 0 if not
 * reachable with a single instruction
 */
static guint32
branch_for_target_reachable (guint8 *branch, guint8 *target)
{
	gint diff = target - branch;
	g_assert ((diff & 3) == 0);
	if (diff >= 0) {
		if (diff <= 33554431)
			return (18 << 26) | (diff);
	} else {
		/* diff between 0 and -33554432 */
		if (diff >= -33554432)
			return (18 << 26) | (diff & ~0xfc000000);
	}
	return 0;
}

/*
 * get_unbox_trampoline:
 * @gsctx: the generic sharing context
 * @m: method pointer
 * @addr: pointer to native code for @m
 *
 * when value type methods are called through the vtable we need to unbox the
 * this argument. This method returns a pointer to a trampoline which does
 * unboxing before calling the method
 */
gpointer
mono_arch_get_unbox_trampoline (MonoGenericSharingContext *gsctx, MonoMethod *m, gpointer addr)
{
	guint8 *code, *start;
	int this_pos = 3;
	guint32 short_branch;
	MonoDomain *domain = mono_domain_get ();
	int size = MONO_PPC_32_64_CASE (20, 32) + PPC_FTNPTR_SIZE;

	addr = mono_get_addr_from_ftnptr (addr);

	if (MONO_TYPE_ISSTRUCT (mono_method_signature (m)->ret))
		this_pos = 4;
	    
	mono_domain_lock (domain);
	start = code = mono_domain_code_reserve (domain, size);
	code = mono_ppc_create_pre_code_ftnptr (code);
	short_branch = branch_for_target_reachable (code + 4, addr);
	if (short_branch)
		mono_domain_code_commit (domain, code, size, 8);
	mono_domain_unlock (domain);

	if (short_branch) {
		ppc_addi (code, this_pos, this_pos, sizeof (MonoObject));
		ppc_emit32 (code, short_branch);
	} else {
		ppc_load (code, ppc_r0, addr);
		ppc_mtctr (code, ppc_r0);
		ppc_addi (code, this_pos, this_pos, sizeof (MonoObject));
		ppc_bcctr (code, 20, 0);
	}
	mono_arch_flush_icache (start, code - start);
	g_assert ((code - start) <= size);
	/*g_print ("unbox trampoline at %d for %s:%s\n", this_pos, m->klass->name, m->name);
	g_print ("unbox code is at %p for method at %p\n", start, addr);*/

	return start;
}

/*
 * mono_arch_get_static_rgctx_trampoline:
 *
 *   Create a trampoline which sets RGCTX_REG to MRGCTX, then jumps to ADDR.
 */
gpointer
mono_arch_get_static_rgctx_trampoline (MonoMethod *m, MonoMethodRuntimeGenericContext *mrgctx, gpointer addr)
{
	guint8 *code, *start, *p;
	guint8 imm_buf [128];
	guint32 short_branch;
	MonoDomain *domain = mono_domain_get ();
	int imm_size;
	int size = MONO_PPC_32_64_CASE (24, (PPC_LOAD_SEQUENCE_LENGTH * 2) + 8) + PPC_FTNPTR_SIZE;

	addr = mono_get_addr_from_ftnptr (addr);

	/* Compute size of code needed to emit mrgctx */
	p = imm_buf;
	ppc_load (p, MONO_ARCH_RGCTX_REG, mrgctx);
	imm_size = p - imm_buf;

	mono_domain_lock (domain);
	start = code = mono_domain_code_reserve (domain, size);
	code = mono_ppc_create_pre_code_ftnptr (code);
	short_branch = branch_for_target_reachable (code + imm_size, addr);
	if (short_branch)
		mono_domain_code_commit (domain, code, size, imm_size + 4);
	mono_domain_unlock (domain);

	if (short_branch) {
		ppc_load (code, MONO_ARCH_RGCTX_REG, mrgctx);
		ppc_emit32 (code, short_branch);
	} else {
		ppc_load (code, ppc_r0, addr);
		ppc_mtctr (code, ppc_r0);
		ppc_load (code, MONO_ARCH_RGCTX_REG, mrgctx);
		ppc_bcctr (code, 20, 0);
	}
	mono_arch_flush_icache (start, code - start);
	g_assert ((code - start) <= size);

	return start;
}

void
mono_arch_patch_callsite (guint8 *method_start, guint8 *code_ptr, guint8 *addr)
{
	guint32 *code = (guint32*)code_ptr;

	addr = mono_get_addr_from_ftnptr (addr);

	/* This is the 'blrl' instruction */
	--code;
	
	/*
	 * Note that methods are called also with the bl opcode.
	 */
	if (((*code) >> 26) == 18) {
		/*g_print ("direct patching\n");*/
		ppc_patch ((guint8*)code, addr);
		mono_arch_flush_icache ((guint8*)code, 4);
		return;
	}
	
	/* Sanity check */
	g_assert (mono_ppc_is_direct_call_sequence (code));

	ppc_patch ((guint8*)code, addr);
}

void
mono_arch_patch_plt_entry (guint8 *code, gpointer *got, mgreg_t *regs, guint8 *addr)
{
	guint32 ins1, ins2, offset;
	mgreg_t *r = (mgreg_t*)regs;

	/* Patch the jump table entry used by the plt entry */

	/* Should be a lis+ori */
	ins1 = ((guint32*)code)[0];
	g_assert (ins1 >> 26 == 15);
	ins2 = ((guint32*)code)[1];
	g_assert (ins2 >> 26 == 24);
	offset = ((ins1 & 0xffff) << 16) | (ins2 & 0xffff);

	/* Either got or regs is set */
	if (!got)
		got = (gpointer*)r [30];
	*(guint8**)((guint8*)got + offset) = addr;
}

void
mono_arch_nullify_class_init_trampoline (guint8 *code, mgreg_t *regs)
{
	mono_arch_patch_callsite (NULL, code, nullified_class_init_trampoline);
}

void
mono_arch_nullify_plt_entry (guint8 *code, mgreg_t *regs)
{
	if (mono_aot_only && !nullified_class_init_trampoline)
		nullified_class_init_trampoline = mono_aot_get_named_code ("nullified_class_init_trampoline");

	mono_arch_patch_plt_entry (code, NULL, regs, nullified_class_init_trampoline);
}

/* Stack size for trampoline function 
 * PPC_MINIMAL_STACK_SIZE + 16 (args + alignment to ppc_magic_trampoline)
 * + MonoLMF + 14 fp regs + 31 gregs + alignment
 */
#define STACK (PPC_MINIMAL_STACK_SIZE + 4 * sizeof (gulong) + sizeof (MonoLMF) + 14 * sizeof (double) + 31 * sizeof (gulong))

/* Method-specific trampoline code fragment size */
#define METHOD_TRAMPOLINE_SIZE 64

/* Jump-specific trampoline code fragment size */
#define JUMP_TRAMPOLINE_SIZE   64

#ifdef PPC_USES_FUNCTION_DESCRIPTOR
#define PPC_TOC_REG ppc_r2
#else
#define PPC_TOC_REG -1
#endif

guchar*
mono_arch_create_trampoline_code (MonoTrampolineType tramp_type)
{
	MonoJumpInfo *ji;
	guint32 code_size;
	guchar *code;
	GSList *unwind_ops, *l;

	code = mono_arch_create_trampoline_code_full (tramp_type, &code_size, &ji, &unwind_ops, FALSE);

	//mono_save_trampoline_xdebug_info ("<generic_trampoline>", code, code_size, unwind_ops);

	for (l = unwind_ops; l; l = l->next)
		g_free (l->data);
	g_slist_free (unwind_ops);

	return code;
}

/*
 * Stack frame description when the generic trampoline is called.
 * caller frame
 * --------------------
 *  MonoLMF
 *  -------------------
 *  Saved FP registers 0-13
 *  -------------------
 *  Saved general registers 0-30
 *  -------------------
 *  param area for 3 args to ppc_magic_trampoline
 *  -------------------
 *  linkage area
 *  -------------------
 */
guchar*
mono_arch_create_trampoline_code_full (MonoTrampolineType tramp_type, guint32 *code_size, MonoJumpInfo **ji, GSList **out_unwind_ops, gboolean aot)
{
	guint8 *buf, *code = NULL;
	int i, offset;
	gconstpointer tramp_handler;
	int size = MONO_PPC_32_64_CASE (600, 800);

	/* Now we'll create in 'buf' the PowerPC trampoline code. This
	   is the trampoline code common to all methods  */

	code = buf = mono_global_codeman_reserve (size);

	*ji = NULL;
	*out_unwind_ops = NULL;

	ppc_stptr_update (buf, ppc_r1, -STACK, ppc_r1);

	/* start building the MonoLMF on the stack */
	offset = STACK - sizeof (double) * MONO_SAVED_FREGS;
	for (i = 14; i < 32; i++) {
		ppc_stfd (buf, i, offset, ppc_r1);
		offset += sizeof (double);
	}
	/* 
	 * now the integer registers.
	 */
	offset = STACK - sizeof (MonoLMF) + G_STRUCT_OFFSET (MonoLMF, iregs);
	ppc_store_multiple_regs (buf, ppc_r13, offset, ppc_r1);

	/* Now save the rest of the registers below the MonoLMF struct, first 14
	 * fp regs and then the 31 gregs.
	 */
	offset = STACK - sizeof (MonoLMF) - (14 * sizeof (double));
	for (i = 0; i < 14; i++) {
		ppc_stfd (buf, i, offset, ppc_r1);
		offset += sizeof (double);
	}
#define GREGS_OFFSET (STACK - sizeof (MonoLMF) - (14 * sizeof (double)) - (31 * sizeof (gulong)))
	offset = GREGS_OFFSET;
	for (i = 0; i < 31; i++) {
		ppc_stptr (buf, i, offset, ppc_r1);
		offset += sizeof (gulong);
	}

	/* we got here through a jump to the ctr reg, we must save the lr
	 * in the parent frame (we do it here to reduce the size of the
	 * method-specific trampoline)
	 */
	ppc_mflr (buf, ppc_r0);
	ppc_stptr (buf, ppc_r0, STACK + PPC_RET_ADDR_OFFSET, ppc_r1);

	/* ok, now we can continue with the MonoLMF setup, mostly untouched 
	 * from emit_prolog in mini-ppc.c
	 */
	if (aot) {
		buf = mono_arch_emit_load_aotconst (code, buf, ji, MONO_PATCH_INFO_JIT_ICALL_ADDR, "mono_get_lmf_addr");
#ifdef PPC_USES_FUNCTION_DESCRIPTOR
		ppc_ldptr (buf, ppc_r2, sizeof (gpointer), ppc_r11);
		ppc_ldptr (buf, ppc_r11, 0, ppc_r11);
#endif
		ppc_mtlr (buf, ppc_r11);
		ppc_blrl (buf);
	}  else {
		ppc_load_func (buf, ppc_r0, mono_get_lmf_addr);
		ppc_mtlr (buf, ppc_r0);
		ppc_blrl (buf);
	}
	/* we build the MonoLMF structure on the stack - see mini-ppc.h
	 * The pointer to the struct is put in ppc_r11.
	 */
	ppc_addi (buf, ppc_r11, ppc_sp, STACK - sizeof (MonoLMF));
	ppc_stptr (buf, ppc_r3, G_STRUCT_OFFSET(MonoLMF, lmf_addr), ppc_r11);
	/* new_lmf->previous_lmf = *lmf_addr */
	ppc_ldptr (buf, ppc_r0, G_STRUCT_OFFSET(MonoLMF, previous_lmf), ppc_r3);
	ppc_stptr (buf, ppc_r0, G_STRUCT_OFFSET(MonoLMF, previous_lmf), ppc_r11);
	/* *(lmf_addr) = r11 */
	ppc_stptr (buf, ppc_r11, G_STRUCT_OFFSET(MonoLMF, previous_lmf), ppc_r3);
	/* save method info (it's stored on the stack, so get it first and put it
	 * in r5 as it's the third argument to the function)
	 */
	if (tramp_type == MONO_TRAMPOLINE_GENERIC_CLASS_INIT)
		ppc_ldptr (buf, ppc_r5, GREGS_OFFSET + PPC_FIRST_ARG_REG * sizeof (gpointer), ppc_r1);
	else
		ppc_ldptr (buf, ppc_r5, GREGS_OFFSET, ppc_r1);
	if ((tramp_type == MONO_TRAMPOLINE_JIT) || (tramp_type == MONO_TRAMPOLINE_JUMP))
		ppc_stptr (buf, ppc_r5, G_STRUCT_OFFSET(MonoLMF, method), ppc_r11);
	/* store the frame pointer of the calling method */
	ppc_addi (buf, ppc_r0, ppc_sp, STACK);
	ppc_stptr (buf, ppc_r0, G_STRUCT_OFFSET(MonoLMF, ebp), ppc_r11);
	/* save the IP (caller ip) */
	if (tramp_type == MONO_TRAMPOLINE_JUMP) {
		ppc_li (buf, ppc_r0, 0);
	} else {
		ppc_ldptr (buf, ppc_r0, STACK + PPC_RET_ADDR_OFFSET, ppc_r1);
	}
	ppc_stptr (buf, ppc_r0, G_STRUCT_OFFSET(MonoLMF, eip), ppc_r11);

	/*
	 * Now we're ready to call trampoline (mgreg_t *regs, guint8 *code, gpointer value, guint8 *tramp)
	 * Note that the last argument is unused.
	 */
	/* Arg 1: a pointer to the registers */
	ppc_addi (buf, ppc_r3, ppc_r1, GREGS_OFFSET);
		
	/* Arg 2: code (next address to the instruction that called us) */
	if (tramp_type == MONO_TRAMPOLINE_JUMP)
		ppc_li (buf, ppc_r4, 0);
	else
		ppc_ldptr  (buf, ppc_r4, STACK + PPC_RET_ADDR_OFFSET, ppc_r1);

	/* Arg 3: MonoMethod *method. It was put in r5 already above */
	/*ppc_mr  (buf, ppc_r5, ppc_r5);*/

	if (aot) {
		buf = mono_arch_emit_load_aotconst (code, buf, ji, MONO_PATCH_INFO_JIT_ICALL_ADDR, g_strdup_printf ("trampoline_func_%d", tramp_type));
#ifdef PPC_USES_FUNCTION_DESCRIPTOR
		ppc_ldptr (buf, ppc_r2, sizeof (gpointer), ppc_r11);
		ppc_ldptr (buf, ppc_r11, 0, ppc_r11);
#endif
		ppc_mtlr (buf, ppc_r11);
		ppc_blrl (buf);
	} else {
		tramp_handler = mono_get_trampoline_func (tramp_type);
		ppc_load_func (buf, ppc_r0, tramp_handler);
		ppc_mtlr (buf, ppc_r0);
		ppc_blrl (buf);
	}
		
	/* OK, code address is now on r3. Move it to the counter reg
	 * so it will be ready for the final jump: this is safe since we
	 * won't do any more calls.
	 */
	if (!MONO_TRAMPOLINE_TYPE_MUST_RETURN (tramp_type)) {
#ifdef PPC_USES_FUNCTION_DESCRIPTOR
		ppc_ldptr (buf, ppc_r2, sizeof (gpointer), ppc_r3);
		ppc_ldptr (buf, ppc_r3, 0, ppc_r3);
#endif
		ppc_mtctr (buf, ppc_r3);
	}

	/*
	 * Now we restore the MonoLMF (see emit_epilogue in mini-ppc.c)
	 * and the rest of the registers, so the method called will see
	 * the same state as before we executed.
	 * The pointer to MonoLMF is in ppc_r11.
	 */
	ppc_addi (buf, ppc_r11, ppc_r1, STACK - sizeof (MonoLMF));
	/* r5 = previous_lmf */
	ppc_ldptr (buf, ppc_r5, G_STRUCT_OFFSET(MonoLMF, previous_lmf), ppc_r11);
	/* r6 = lmf_addr */
	ppc_ldptr (buf, ppc_r6, G_STRUCT_OFFSET(MonoLMF, lmf_addr), ppc_r11);
	/* *(lmf_addr) = previous_lmf */
	ppc_stptr (buf, ppc_r5, G_STRUCT_OFFSET(MonoLMF, previous_lmf), ppc_r6);
	/* restore iregs */
	ppc_load_multiple_regs (buf, ppc_r13, G_STRUCT_OFFSET(MonoLMF, iregs), ppc_r11);
	/* restore fregs */
	for (i = 14; i < 32; i++)
		ppc_lfd (buf, i, G_STRUCT_OFFSET(MonoLMF, fregs) + ((i-14) * sizeof (gdouble)), ppc_r11);

	/* restore the volatile registers, we skip r1, of course */
	offset = STACK - sizeof (MonoLMF) - (14 * sizeof (double));
	for (i = 0; i < 14; i++) {
		ppc_lfd (buf, i, offset, ppc_r1);
		offset += sizeof (double);
	}
	offset = STACK - sizeof (MonoLMF) - (14 * sizeof (double)) - (31 * sizeof (gulong));
	ppc_ldptr (buf, ppc_r0, offset, ppc_r1);
	offset += 2 * sizeof (gulong);
	for (i = 2; i < 13; i++) {
		if (i != PPC_TOC_REG && (i != 3 || tramp_type != MONO_TRAMPOLINE_RGCTX_LAZY_FETCH))
			ppc_ldptr (buf, i, offset, ppc_r1);
		offset += sizeof (gulong);
	}

	/* Non-standard function epilogue. Instead of doing a proper
	 * return, we just jump to the compiled code.
	 */
	/* Restore stack pointer and LR and jump to the code */
	ppc_ldptr  (buf, ppc_r1,  0, ppc_r1);
	ppc_ldptr  (buf, ppc_r11, PPC_RET_ADDR_OFFSET, ppc_r1);
	ppc_mtlr (buf, ppc_r11);
	if (MONO_TRAMPOLINE_TYPE_MUST_RETURN (tramp_type))
		ppc_blr (buf);
	else
		ppc_bcctr (buf, 20, 0);

	/* Flush instruction cache, since we've generated code */
	mono_arch_flush_icache (code, buf - code);
	
	*code_size = buf - code;

	/* Sanity check */
	g_assert ((buf - code) <= size);

	if (tramp_type == MONO_TRAMPOLINE_CLASS_INIT) {
		guint32 code_len;

		/* Initialize the nullified class init trampoline */
		nullified_class_init_trampoline = mono_ppc_create_ftnptr (mono_arch_get_nullified_class_init_trampoline (&code_len));
	}

	return code;
}

#define TRAMPOLINE_SIZE (MONO_PPC_32_64_CASE (24, (5+5+1+1)*4))
gpointer
mono_arch_create_specific_trampoline (gpointer arg1, MonoTrampolineType tramp_type, MonoDomain *domain, guint32 *code_len)
{
	guint8 *code, *buf, *tramp;
	guint32 short_branch;

	tramp = mono_get_trampoline_code (tramp_type);

	mono_domain_lock (domain);
	code = buf = mono_domain_code_reserve_align (domain, TRAMPOLINE_SIZE, 4);
	short_branch = branch_for_target_reachable (code + MONO_PPC_32_64_CASE (8, 5*4), tramp);
#ifdef __mono_ppc64__
	/* FIXME: make shorter if possible */
#else
	if (short_branch)
		mono_domain_code_commit (domain, code, TRAMPOLINE_SIZE, 12);
#endif
	mono_domain_unlock (domain);

	if (short_branch) {
		ppc_load_sequence (buf, ppc_r0, (gulong) arg1);
		ppc_emit32 (buf, short_branch);
	} else {
		/* Prepare the jump to the generic trampoline code.*/
		ppc_load (buf, ppc_r0, (gulong) tramp);
		ppc_mtctr (buf, ppc_r0);
	
		/* And finally put 'arg1' in r0 and fly! */
		ppc_load (buf, ppc_r0, (gulong) arg1);
		ppc_bcctr (buf, 20, 0);
	}
	
	/* Flush instruction cache, since we've generated code */
	mono_arch_flush_icache (code, buf - code);

	g_assert ((buf - code) <= TRAMPOLINE_SIZE);
	if (code_len)
		*code_len = buf - code;

	return code;
}

static guint8*
emit_trampoline_jump (guint8 *code, guint8 *tramp)
{
	guint32 short_branch = branch_for_target_reachable (code, tramp);

	/* FIXME: we can save a few bytes here by committing if the
	   short branch is possible */
	if (short_branch) {
		ppc_emit32 (code, short_branch);
	} else {
		ppc_load (code, ppc_r0, tramp);
		ppc_mtctr (code, ppc_r0);
		ppc_bcctr (code, 20, 0);
	}

	return code;
}

gpointer
mono_arch_create_rgctx_lazy_fetch_trampoline (guint32 slot)
{
	guint32 code_size;
	MonoJumpInfo *ji;

	return mono_arch_create_rgctx_lazy_fetch_trampoline_full (slot, &code_size, &ji, FALSE);
}

gpointer
mono_arch_create_rgctx_lazy_fetch_trampoline_full (guint32 slot, guint32 *code_size, MonoJumpInfo **ji, gboolean aot)
{
#ifdef MONO_ARCH_VTABLE_REG
	guint8 *tramp;
	guint8 *code, *buf;
	guint8 **rgctx_null_jumps;
	int tramp_size;
	int depth, index;
	int i;
	gboolean mrgctx;

	*ji = NULL;

	mrgctx = MONO_RGCTX_SLOT_IS_MRGCTX (slot);
	index = MONO_RGCTX_SLOT_INDEX (slot);
	if (mrgctx)
		index += sizeof (MonoMethodRuntimeGenericContext) / sizeof (gpointer);
	for (depth = 0; ; ++depth) {
		int size = mono_class_rgctx_get_array_size (depth, mrgctx);

		if (index < size - 1)
			break;
		index -= size - 1;
	}

	tramp_size = MONO_PPC_32_64_CASE (40, 52) + 12 * depth;
	if (mrgctx)
		tramp_size += 4;
	else
		tramp_size += 12;
	if (aot)
		tramp_size += 32;

	code = buf = mono_global_codeman_reserve (tramp_size);

	rgctx_null_jumps = g_malloc (sizeof (guint8*) * (depth + 2));

	if (mrgctx) {
		/* get mrgctx ptr */
		ppc_mr (code, ppc_r4, PPC_FIRST_ARG_REG);
	} else {
		/* load rgctx ptr from vtable */
		ppc_ldptr (code, ppc_r4, G_STRUCT_OFFSET (MonoVTable, runtime_generic_context), PPC_FIRST_ARG_REG);
		/* is the rgctx ptr null? */
		ppc_compare_reg_imm (code, 0, ppc_r4, 0);
		/* if yes, jump to actual trampoline */
		rgctx_null_jumps [0] = code;
		ppc_bc (code, PPC_BR_TRUE, PPC_BR_EQ, 0);
	}

	for (i = 0; i < depth; ++i) {
		/* load ptr to next array */
		if (mrgctx && i == 0)
			ppc_ldptr (code, ppc_r4, sizeof (MonoMethodRuntimeGenericContext), ppc_r4);
		else
			ppc_ldptr (code, ppc_r4, 0, ppc_r4);
		/* is the ptr null? */
		ppc_compare_reg_imm (code, 0, ppc_r4, 0);
		/* if yes, jump to actual trampoline */
		rgctx_null_jumps [i + 1] = code;
		ppc_bc (code, PPC_BR_TRUE, PPC_BR_EQ, 0);
	}

	/* fetch slot */
	ppc_ldptr (code, ppc_r4, sizeof (gpointer) * (index  + 1), ppc_r4);
	/* is the slot null? */
	ppc_compare_reg_imm (code, 0, ppc_r4, 0);
	/* if yes, jump to actual trampoline */
	rgctx_null_jumps [depth + 1] = code;
	ppc_bc (code, PPC_BR_TRUE, PPC_BR_EQ, 0);
	/* otherwise return r4 */
	/* FIXME: if we use r3 as the work register we can avoid this copy */
	ppc_mr (code, ppc_r3, ppc_r4);
	ppc_blr (code);

	for (i = mrgctx ? 1 : 0; i <= depth + 1; ++i)
		ppc_patch (rgctx_null_jumps [i], code);

	g_free (rgctx_null_jumps);

	/* move the rgctx pointer to the VTABLE register */
	ppc_mr (code, MONO_ARCH_VTABLE_REG, ppc_r3);

	if (aot) {
		code = mono_arch_emit_load_aotconst (buf, code, ji, MONO_PATCH_INFO_JIT_ICALL_ADDR, g_strdup_printf ("specific_trampoline_lazy_fetch_%u", slot));
		/* Branch to the trampoline */
#ifdef PPC_USES_FUNCTION_DESCRIPTOR
		ppc_ldptr (code, ppc_r11, 0, ppc_r11);
#endif
		ppc_mtctr (code, ppc_r11);
		ppc_bcctr (code, PPC_BR_ALWAYS, 0);
	} else {
		tramp = mono_arch_create_specific_trampoline (GUINT_TO_POINTER (slot),
			MONO_TRAMPOLINE_RGCTX_LAZY_FETCH, mono_get_root_domain (), NULL);

		/* jump to the actual trampoline */
		code = emit_trampoline_jump (code, tramp);
	}

	mono_arch_flush_icache (buf, code - buf);

	g_assert (code - buf <= tramp_size);

	*code_size = code - buf;

	return buf;
#else
	g_assert_not_reached ();
#endif
}

gpointer
mono_arch_create_generic_class_init_trampoline (void)
{
	guint32 code_size;
	MonoJumpInfo *ji;

	return mono_arch_create_generic_class_init_trampoline_full (&code_size, &ji, FALSE);
}

gpointer
mono_arch_create_generic_class_init_trampoline_full (guint32 *code_size, MonoJumpInfo **ji, gboolean aot)
{
	guint8 *tramp;
	guint8 *code, *buf;
	static int byte_offset = -1;
	static guint8 bitmask;
	guint8 *jump;
	int tramp_size;

	tramp_size = MONO_PPC_32_64_CASE (32, 44);
	if (aot)
		tramp_size += 32;

	code = buf = mono_global_codeman_reserve (tramp_size);

	*ji = NULL;

	if (byte_offset < 0)
		mono_marshal_find_bitfield_offset (MonoVTable, initialized, &byte_offset, &bitmask);

	ppc_lbz (code, ppc_r4, byte_offset, PPC_FIRST_ARG_REG);
	ppc_andid (code, ppc_r4, ppc_r4, bitmask);
	jump = code;
	ppc_bc (code, PPC_BR_TRUE, PPC_BR_EQ, 0);

	ppc_blr (code);

	ppc_patch (jump, code);

	if (aot) {
		code = mono_arch_emit_load_aotconst (buf, code, ji, MONO_PATCH_INFO_JIT_ICALL_ADDR, "specific_trampoline_generic_class_init");
		/* Branch to the trampoline */
#ifdef PPC_USES_FUNCTION_DESCRIPTOR
		ppc_ldptr (code, ppc_r11, 0, ppc_r11);
#endif
		ppc_mtctr (code, ppc_r11);
		ppc_bcctr (code, PPC_BR_ALWAYS, 0);
	} else {
		tramp = mono_arch_create_specific_trampoline (NULL, MONO_TRAMPOLINE_GENERIC_CLASS_INIT,
			mono_get_root_domain (), NULL);

		/* jump to the actual trampoline */
		code = emit_trampoline_jump (code, tramp);
	}

	mono_arch_flush_icache (buf, code - buf);

	*code_size = code - buf;

	g_assert (code - buf <= tramp_size);

	return buf;
}

gpointer
mono_arch_get_nullified_class_init_trampoline (guint32 *code_len)
{
	guint8 *code, *buf;
	guint32 tramp_size = 64;

	code = buf = mono_global_codeman_reserve (tramp_size);
	ppc_blr (code);

	mono_arch_flush_icache (buf, code - buf);

	*code_len = code - buf;

	g_assert (code - buf <= tramp_size);

	return buf;
}

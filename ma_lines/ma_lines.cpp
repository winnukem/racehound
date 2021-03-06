/* "ma_lines" - GCC plugin that outputs the list of the source locations
 * where memory accesses may happen.
 *
 * See Readme.txt for the usage details.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * Copyright (C) 2014, Eugene Shatokhin <eugene.shatokhin@rosalab.ru>
 */

/* Some of the functions here are based on the implementation of 
 * ThreadSanitizer from GCC 4.9 (gcc/tsan.c). */

#include <assert.h>
#include <string.h>
#include <gcc-plugin.h>
#include <plugin-version.h>

#include "common_includes.h"

#include <stdio.h>
#include <sys/file.h>

#include <string>
#include <set>

#include <common/util.h>
/* ====================================================================== */

/* Use this to mark the functions to be exported from this plugin. The 
 * remaining functions will not be visible from outside of this plugin even
 * if they are not static (-fvisibility=hidden GCC option is used to achieve
 * this). */
#define PLUGIN_EXPORT __attribute__ ((visibility("default")))
/* ====================================================================== */

#if BUILDING_GCC_VERSION >= 6000
typedef gimple * rh_stmt;
#else
typedef gimple rh_stmt;
#endif
/* ====================================================================== */

#ifdef __cplusplus
extern "C" {
#endif

/* This symbol is needed for GCC to know the plugin is GPL-compatible. */
PLUGIN_EXPORT int plugin_is_GPL_compatible;

/* Plugin initialization function, the only function to be exported */
PLUGIN_EXPORT int
plugin_init(struct plugin_name_args *plugin_info,
	    struct plugin_gcc_version *version);

#ifdef __cplusplus
}
#endif
/* ====================================================================== */

/* The file to dump the info about interesting source locations to. Can be 
 * specified as an argument to the plugin:
 * -fplugin-arg-ma_lines-file=<path_to_output_file> */
static std::string out_file = "./dump_memory_accesses.list";
/* ====================================================================== */

/* These memory/string operations may be replaced by their implementation
 * in assembly in the code. The reads/writes they perform may be needed to 
 * track. */
static std::string _funcs[] = {
	"strstr",
	"strspn",
	"strsep",
	"strrchr",
	"strpbrk",
	"strnstr",
	"strnlen",
	"strnicmp",
	"strncpy",
	"strncmp",
	"strnchr",
	"strncat",
	"strncasecmp",
	"strlen",
	"strlcpy",
	"strlcat",
	"strcspn",
	"strcpy",
	"strcmp",
	"strchr",
	"strcat",
	"strcasecmp",
	"memset",
	"memcpy",
	"memcmp",
	"memscan",
	"memmove",
	"memchr",
	"__memcpy",
};
static std::set<std::string> special_functions(
	&_funcs[0], &_funcs[0] + sizeof(_funcs) / sizeof(_funcs[0]));
/* ====================================================================== */

static void
output_ma_location(rh_stmt stmt, EAccessType ma_type)
{
	int ret;
	
	location_t loc = gimple_location(stmt);
	const char *src = LOCATION_FILE(loc);
	unsigned int line = LOCATION_LINE(loc);
	/* No need to output the column, it seems to be ignored in debug
	 * info. */
	
	if (src == NULL) {
		fprintf(stderr, 
"[ma_lines] Path to the source file is missing, skipping the statement.\n");
		return;
	}
	
	FILE *out = fopen(out_file.c_str(), "a");
	if (out == NULL) {
		fprintf(stderr, 
	"[ma_lines] Failed to open file \"%s\".\n", out_file.c_str());
		return;
	}
	
	int fd = fileno(out);
	if (fd == -1) {
		fprintf(stderr, "[ma_lines] Internal error.\n");
		goto end;
	}
	
	/* Lock the output file in case GCC processes 2 or more files at the
	 * same time. */
	ret = flock(fd, LOCK_EX);
	if (ret != 0) {
		fprintf(stderr, 
			"[ma_lines] Failed to lock the output file.\n");
		goto end;
	}
	
	/* Another instance of this plugin might have written to the file 
	 * after we have opened it but before we have got the lock.
	 * We need to set the current position in the file to its end again.
	 */
	ret = fseek(out, 0, SEEK_END);
	if (ret != 0) {
		fprintf(stderr, 
			"[ma_lines] Failed to seek to the end of file.\n");
		goto unlock;
	}
	
	fprintf(out, "%s:%u", src, line);
	if (ma_type == AT_READ) {
		fprintf(out, ":read\n");
	}
	else if (ma_type == AT_WRITE) {
		fprintf(out, ":write\n");
	}
	else /* AT_BOTH and all other values */ {
		fprintf(out, "\n");
	}

unlock:
	ret = flock(fd, LOCK_UN);
	if (ret != 0) {
		fprintf(stderr, 
			"[ma_lines] Failed to unlock the output file.\n");
	}
end:
	fclose(out);
}

/* Report source locations for the calls to the functions of interest (those
 * that might be replaced with their implementation in assembly, like 
 * memcpy(), etc. */
static void
process_function_call(gimple_stmt_iterator *gsi)
{
	rh_stmt stmt = gsi_stmt(*gsi);
	tree fndecl = gimple_call_fndecl(stmt);
	
	if (!fndecl) { 
		/* Indirect call, nothing to do. */
		return;
	}

	const char *name = IDENTIFIER_POINTER(DECL_NAME(fndecl));
	if (special_functions.find(name) != special_functions.end()) {
		output_ma_location(stmt, AT_BOTH);
	}
}

static void
process_expr(gimple_stmt_iterator gsi, tree expr, bool is_write)
{
#if BUILDING_GCC_VERSION >= 6000
	int reversep = 0;
#endif
	rh_stmt stmt = gsi_stmt(gsi);
	HOST_WIDE_INT size = int_size_in_bytes(TREE_TYPE (expr));
	if (size < 1)
		return;

	/* TODO: Check how this works when bit fields are accessed, update 
	 * if needed (~ report touching the corresponding bytes as a 
	 * whole?) */
	HOST_WIDE_INT bitsize;
	HOST_WIDE_INT bitpos;
	tree offset;
	enum machine_mode mode;
	int volatilep = 0;
	int unsignedp = 0;
#if BUILDING_GCC_VERSION >= 6000
	tree base = get_inner_reference(
		expr, &bitsize, &bitpos, &offset, &mode, &unsignedp,
		&reversep, &volatilep, false);
#else
	tree base = get_inner_reference(
		expr, &bitsize, &bitpos, &offset, &mode, &unsignedp, 
		&volatilep, false);
#endif
	if (DECL_P(base)) {
		struct pt_solution pt;
		memset(&pt, 0, sizeof(pt));
		pt.escaped = 1;
		pt.ipa_escaped = flag_ipa_pta != 0;
		pt.nonlocal = 1;
		if (!pt_solution_includes(&pt, base)) {
			//<>
			//fprintf(stderr, "[DBG] The decl does not escape.\n");
			//<>
			return;
		}
		if (!is_global_var(base) && !may_be_aliased(base)) {
			//<>
			//fprintf(stderr, "[DBG] Neither global nor may be aliased.\n");
			//<>
			return;
		}
	}

	if (TREE_READONLY (base) || 
	   (TREE_CODE (base) == VAR_DECL && DECL_HARD_REGISTER (base))) {
		//<>
		//fprintf(stderr, "[DBG] Read-only or register variable.\n");
		//<>
		return;
	}

	// TODO: bit field access. How to handle it properly?
	if (bitpos % (size * BITS_PER_UNIT) ||
	    bitsize != size * BITS_PER_UNIT) {
		return;
	}

	output_ma_location(stmt, (is_write ? AT_WRITE : AT_READ));
}

static void
process_gimple(gimple_stmt_iterator *gsi)
{
	rh_stmt stmt;
	stmt = gsi_stmt(*gsi);
	
	if (is_gimple_call(stmt)) {
		process_function_call(gsi);
	}
	else if (is_gimple_assign(stmt) && !gimple_clobber_p(stmt)) {
		if (gimple_store_p(stmt)) {
			tree lhs = gimple_assign_lhs(stmt);
			process_expr(*gsi, lhs, true);
		}
		if (gimple_assign_load_p(stmt)) {
			tree rhs = gimple_assign_rhs1(stmt);
			process_expr(*gsi, rhs, false);
		}
	}
}

/* The main function of the pass. Called for each function to be processed.
 */
static unsigned int
do_execute(function *func)
{
	//<>
	/*fprintf(stderr, "[DBG] Processing function \"%s\".\n",
		current_function_name());*/
	//<>
	
	basic_block bb;
	gimple_stmt_iterator gsi;
	
	FOR_EACH_BB_FN (bb, func) {
		for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); 
		     gsi_next(&gsi)) {
			process_gimple(&gsi);
		}
	}
	return 0;
}
/* ====================================================================== */

static const struct pass_data my_pass_data = {
		.type = 	GIMPLE_PASS,
		.name = 	"racehound_ma_lines",
		.optinfo_flags = OPTGROUP_NONE,

#if BUILDING_GCC_VERSION < 5000
		/* GCC 4.9 */
		.has_gate	= false,
		.has_execute	= true,
#endif
		.tv_id = 	TV_NONE,
		.properties_required = PROP_ssa | PROP_cfg,
		.properties_provided = 0,
		.properties_destroyed = 0,
		.todo_flags_start = 0,
		.todo_flags_finish = TODO_verify_all | TODO_update_ssa
};

namespace {
class my_pass : public gimple_opt_pass {
public:
	my_pass() 
	  : gimple_opt_pass(my_pass_data, g) 
	{}
#  if BUILDING_GCC_VERSION >= 5000
	bool gate (function *) { return true; }
	virtual unsigned int execute(function *func)
	{
		return do_execute(func);
	}
#  else
	unsigned int execute() { return do_execute(cfun); }
#  endif
}; /* class my_pass */
}  /* anon namespace */

static struct opt_pass *make_my_pass(void)
{
	return new my_pass();
}
/* ====================================================================== */

int
plugin_init(struct plugin_name_args *plugin_info,
	    struct plugin_gcc_version *version)
{
	struct register_pass_info pass_info;
	
	if (!plugin_default_version_check(version, &gcc_version))
		return 1;
	
	pass_info.pass = make_my_pass();
	
	/* For some reason, GCC does not allow to place this pass after 
	 * "inline" IPA pass, complaining that the latter is not found.
	 * So I place the new pass before pass_lower_eh_dispatch ("ehdisp") 
	 * which is guaranteed to execute after all IPA passes. */
	pass_info.reference_pass_name = "ehdisp";
	/* consider only the 1st occasion of the reference pass */
	pass_info.ref_pass_instance_number = 1;
	pass_info.pos_op = PASS_POS_INSERT_BEFORE;
	
	if (plugin_info->argc > 0) {
		struct plugin_argument *arg = &plugin_info->argv[0];
		if (strcmp(arg->key, "file") == 0)
			out_file = arg->value;
	}
	
	/* Register the pass */
	register_callback(plugin_info->base_name, PLUGIN_PASS_MANAGER_SETUP,
			  NULL, &pass_info);
	return 0;
}
/* ====================================================================== */

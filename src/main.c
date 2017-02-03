/*
 * main.c
 *
 * Entrypoint of PG-Strom extension, and misc uncategolized functions.
 * ----
 * Copyright 2011-2017 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2017 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "postgres.h"
#include "access/hash.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
#include <float.h>
#include <limits.h>
#include "pg_strom.h"

PG_MODULE_MAGIC;

/*
 * miscellaneous GUC parameters
 */
bool		pgstrom_enabled;
bool		pgstrom_perfmon_enabled;
bool		pgstrom_debug_kernel_source;
bool		pgstrom_bulkexec_enabled;
bool		pgstrom_cpu_fallback_enabled;
int			pgstrom_max_async_tasks;
int			pgstrom_min_async_tasks;
double		pgstrom_num_threads_margin;
double		pgstrom_chunk_size_margin;
static int	pgstrom_chunk_size_kb;
static int	pgstrom_chunk_limit_kb;

/* cost factors */
double		pgstrom_gpu_setup_cost;
double		pgstrom_gpu_dma_cost;
double		pgstrom_gpu_operator_cost;

/* misc static variables */
static planner_hook_type	planner_hook_next;
static CustomPathMethods	pgstrom_dummy_path_methods;
static CustomScanMethods	pgstrom_dummy_plan_methods;

/* pg_strom.chunk_size */
Size
pgstrom_chunk_size(void)
{
	return ((Size)pgstrom_chunk_size_kb) << 10;
}

/* pg_strom.chunk_size_limit */
Size
pgstrom_chunk_size_limit(void)
{
	return ((Size)pgstrom_chunk_limit_kb) << 10;
}

static void
pgstrom_init_misc_guc(void)
{
	/* turn on/off PG-Strom feature */
	DefineCustomBoolVariable("pg_strom.enabled",
							 "Enables the planner's use of PG-Strom",
							 NULL,
							 &pgstrom_enabled,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* turn on/off performance monitor on EXPLAIN ANALYZE */
	DefineCustomBoolVariable("pg_strom.perfmon",
							 "Enables the performance monitor of PG-Strom",
							 NULL,
							 &pgstrom_perfmon_enabled,
							 false,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* turn on/off bulkload feature to exchange PG-Strom nodes */
	DefineCustomBoolVariable("pg_strom.bulkexec",
							 "Enables the bulk-execution mode of PG-Strom",
							 NULL,
							 &pgstrom_bulkexec_enabled,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* turn on/off CPU fallback if GPU could not execute the query */
	DefineCustomBoolVariable("pg_strom.cpu_fallback",
							 "Enables CPU fallback if GPU is ",
							 NULL,
							 &pgstrom_cpu_fallback_enabled,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* turn on/off cuda kernel source saving */
	DefineCustomBoolVariable("pg_strom.debug_kernel_source",
							 "Turn on/off to display the kernel source path",
							 NULL,
							 &pgstrom_debug_kernel_source,
							 false,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* soft limit for number of concurrent GpuTask per GPU device */
	DefineCustomIntVariable("pg_strom.max_async_tasks",
				"Soft limit for number of concurrent tasks per GPU server",
							NULL,
							&pgstrom_max_async_tasks,
							32,
							4,
							INT_MAX,
							PGC_USERSET,
							GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	/* maximum number of GpuTask can concurrently executed */
	DefineCustomIntVariable("pg_strom.min_async_tasks",
				"Minimum guarantee for number of concurrent tasks per process",
							NULL,
							&pgstrom_min_async_tasks,
							4,
							1,
							Max(pgstrom_max_async_tasks/4, 4),
							PGC_USERSET,
							GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	/* default length of pgstrom_data_store */
	DefineCustomIntVariable("pg_strom.chunk_size",
							"default size of pgstrom_data_store",
							NULL,
							&pgstrom_chunk_size_kb,
							32768 - (2 * BLCKSZ / 1024),	/* almost 32MB */
							4096,
							MAX_KILOBYTES,
							PGC_INTERNAL,
							GUC_NOT_IN_SAMPLE | GUC_UNIT_KB,
							NULL, NULL, NULL);
	/* maximum length of pgstrom_data_store */
	DefineCustomIntVariable("pg_strom.chunk_limit",
							"limit size of pgstrom_data_store",
							NULL,
							&pgstrom_chunk_limit_kb,
							5 * pgstrom_chunk_size_kb,
							4096,
							MAX_KILOBYTES,
							PGC_INTERNAL,
							GUC_NOT_IN_SAMPLE | GUC_UNIT_KB,
							NULL, NULL, NULL);
	/* factor for margin of buffer size */
	DefineCustomRealVariable("pg_strom.chunk_size_margin",
							 "margin of chunk size if not predictable exactly",
							 NULL,
							 &pgstrom_chunk_size_margin,
							 1.25,
							 1.00,
							 DBL_MAX,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* cost factor for Gpu setup */
	DefineCustomRealVariable("pg_strom.gpu_setup_cost",
							 "Cost to setup GPU device to run",
							 NULL,
							 &pgstrom_gpu_setup_cost,
							 4000 * DEFAULT_SEQ_PAGE_COST,
							 0,
							 DBL_MAX,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* cost factor for each Gpu task */
	DefineCustomRealVariable("pg_strom.gpu_dma_cost",
							 "Cost to send/recv data via DMA",
							 NULL,
							 &pgstrom_gpu_dma_cost,
							 10 * DEFAULT_SEQ_PAGE_COST,
							 0,
							 DBL_MAX,
                             PGC_USERSET,
                             GUC_NOT_IN_SAMPLE,
                             NULL, NULL, NULL);
	/* cost factor for Gpu operator */
	DefineCustomRealVariable("pg_strom.gpu_operator_cost",
							 "Cost of processing each operators by GPU",
							 NULL,
							 &pgstrom_gpu_operator_cost,
							 DEFAULT_CPU_OPERATOR_COST / 16.0,
							 0,
							 DBL_MAX,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
}

/*
 * pgstrom_create_dummy_path
 */
Path *
pgstrom_create_dummy_path(PlannerInfo *root,
						  Path *subpath,
						  PathTarget *target)
{
	CustomPath *cpath = makeNode(CustomPath);

	cpath->path.pathtype		= T_CustomScan;
	cpath->path.parent			= subpath->parent;
	cpath->path.pathtarget		= target;
	cpath->path.param_info		= NULL;
	cpath->path.parallel_aware	= subpath->parallel_aware;
	cpath->path.parallel_safe	= subpath->parallel_safe;
	cpath->path.parallel_workers = subpath->parallel_workers;
	cpath->path.pathkeys		= subpath->pathkeys;
	cpath->path.rows			= subpath->rows;
	cpath->path.startup_cost	= subpath->startup_cost;
	cpath->path.total_cost		= subpath->total_cost;

	cpath->custom_paths			= list_make1(subpath);
	cpath->methods      		= &pgstrom_dummy_path_methods;

	return &cpath->path;
}

/*
 * pgstrom_dummy_create_plan - PlanCustomPath callback
 */
static Plan *
pgstrom_dummy_create_plan(PlannerInfo *root,
						  RelOptInfo *rel,
						  CustomPath *best_path,
						  List *tlist,
						  List *clauses,
						  List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);

	Assert(list_length(custom_plans) == 1);
	cscan->scan.plan.parallel_aware = best_path->path.parallel_aware;
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.plan.lefttree = linitial(custom_plans);
	cscan->scan.scanrelid = 0;
	cscan->custom_scan_tlist = tlist;
	cscan->methods = &pgstrom_dummy_plan_methods;

	return &cscan->scan.plan;
}

/*
 * pgstrom_dummy_remove_plan
 */
static Plan *
pgstrom_dummy_remove_plan(PlannedStmt *pstmt, CustomScan *cscan)
{
	Plan	   *subplan = outerPlan(cscan);
	ListCell   *lc1;
	ListCell   *lc2;

	Assert(innerPlan(cscan) == NULL &&
		   cscan->custom_plans == NIL);
	Assert(list_length(cscan->scan.plan.targetlist) ==
		   list_length(subplan->targetlist));
	/*
	 * Push down the resource name to subplan
	 */
	forboth (lc1, cscan->scan.plan.targetlist,
			 lc2, subplan->targetlist)
	{
		TargetEntry	   *tle_1 = lfirst(lc1);
		TargetEntry	   *tle_2 = lfirst(lc2);

		if (exprType((Node *)tle_1->expr) != exprType((Node *)tle_2->expr))
			elog(ERROR, "Bug? dummy custom scan node has incompatible tlist");

		if (tle_2->resname != NULL &&
			(tle_1->resname == NULL ||
			 strcmp(tle_1->resname, tle_2->resname) != 0))
		{
			elog(DEBUG2,
				 "attribute %d of subplan: [%s] is over-written by [%s]",
				 tle_2->resno,
				 tle_2->resname,
				 tle_1->resname);
		}
		if (tle_1->resjunk != tle_2->resjunk)
			elog(DEBUG2,
				 "attribute %d of subplan: [%s] is marked as %s attribute",
				 tle_2->resno,
                 tle_2->resname,
				 tle_1->resjunk ? "junk" : "non-junk");

		tle_2->resname = tle_1->resname;
		tle_2->resjunk = tle_1->resjunk;
	}
	return outerPlan(cscan);
}

/*
 * pgstrom_dummy_create_scan_state - CreateCustomScanState callback
 */
static Node *
pgstrom_dummy_create_scan_state(CustomScan *cscan)
{
	elog(ERROR, "Bug? dummy custom scan node still remain on executor stage");
}

/*
 * pgstrom_post_planner
 *
 * remove 'dummy' custom scan node.
 *
 *
 *
 */
static void
pgstrom_post_planner_recurse(PlannedStmt *pstmt, Plan **p_plan)
{
	Plan	   *plan = *p_plan;
	ListCell   *lc;

	Assert(plan != NULL);

	switch (nodeTag(plan))
	{
		case T_ModifyTable:
			{
				ModifyTable *splan = (ModifyTable *) plan;

				foreach (lc, splan->plans)
					pgstrom_post_planner_recurse(pstmt, (Plan **)&lfirst(lc));
			}
			break;
			
		case T_Append:
			{
				Append	   *splan = (Append *) plan;

				foreach (lc, splan->appendplans)
					pgstrom_post_planner_recurse(pstmt, (Plan **)&lfirst(lc));
			}
			break;

		case T_MergeAppend:
			{
				MergeAppend *splan = (MergeAppend *) plan;

				foreach (lc, splan->mergeplans)
					pgstrom_post_planner_recurse(pstmt, (Plan **)&lfirst(lc));
			}
			break;

		case T_BitmapAnd:
			{
				BitmapAnd  *splan = (BitmapAnd *) plan;

				foreach (lc, splan->bitmapplans)
					pgstrom_post_planner_recurse(pstmt, (Plan **)&lfirst(lc));
			}
			break;

		case T_BitmapOr:
			{
				BitmapOr   *splan = (BitmapOr *) plan;

				foreach (lc, splan->bitmapplans)
					pgstrom_post_planner_recurse(pstmt, (Plan **)&lfirst(lc));
			}
			break;

		case T_CustomScan:
			{
				CustomScan *cscan = (CustomScan *) plan;

				if (cscan->methods == &pgstrom_dummy_plan_methods)
				{
					*p_plan = pgstrom_dummy_remove_plan(pstmt, cscan);
					pgstrom_post_planner_recurse(pstmt, p_plan);
					return;
				}
				else if (pgstrom_plan_is_gpupreagg(&cscan->scan.plan))
					gpupreagg_post_planner(pstmt, cscan);

				foreach (lc, cscan->custom_plans)
					pgstrom_post_planner_recurse(pstmt, (Plan **)&lfirst(lc));
			}
			break;

		default:
			break;
	}

	if (plan->lefttree)
		pgstrom_post_planner_recurse(pstmt, &plan->lefttree);
	if (plan->righttree)
		pgstrom_post_planner_recurse(pstmt, &plan->righttree);
}

static PlannedStmt *
pgstrom_post_planner(Query *parse,
					 int cursorOptions,
					 ParamListInfo boundParams)
{
	PlannedStmt	   *pstmt;
	ListCell	   *lc;

	if (planner_hook_next)
		pstmt = planner_hook_next(parse, cursorOptions, boundParams);
	else
		pstmt = standard_planner(parse, cursorOptions, boundParams);

	pgstrom_post_planner_recurse(pstmt, &pstmt->planTree);
	foreach (lc, pstmt->subplans)
		pgstrom_post_planner_recurse(pstmt, (Plan **)&lfirst(lc));

	return pstmt;
}


/*
 * _PG_init
 *
 * Main entrypoint of PG-Strom. It shall be invoked only once when postmaster
 * process is starting up, then it calls other sub-systems to initialize for
 * each ones.
 */
void
_PG_init(void)
{
	/*
	 * PG-Strom has to be loaded using shared_preload_libraries option
	 */
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		errmsg("PG-Strom must be loaded via shared_preload_libraries")));

	/* dump version number */
	elog(LOG, "PG-Strom version %s built for PostgreSQL %s",
		 PGSTROM_VERSION, PG_MAJORVERSION);

	/* init GPU/CUDA infrastracture */
	pgstrom_init_misc_guc();
	pgstrom_init_gpu_device();
	pgstrom_init_dma_buffer();
	pgstrom_init_gpu_context();
	pgstrom_init_gpu_server();
	pgstrom_init_nvme_strom();

	/* init NVRTC (run-time compiler) stuff */
	pgstrom_init_cuda_program();

	/* registration of custom-scan providers */
	pgstrom_init_gputasks();
	pgstrom_init_gpuscan();
	pgstrom_init_gpujoin();
	pgstrom_init_gpupreagg();

	/* miscellaneous initializations */
	pgstrom_init_codegen();
//	pgstrom_init_plcuda();

	/* dummy custom-scan node */
	memset(&pgstrom_dummy_path_methods, 0, sizeof(CustomPathMethods));
	pgstrom_dummy_path_methods.CustomName	= "Dummy";
	pgstrom_dummy_path_methods.PlanCustomPath
		= pgstrom_dummy_create_plan;

	memset(&pgstrom_dummy_plan_methods, 0, sizeof(CustomScanMethods));
	pgstrom_dummy_plan_methods.CustomName	= "Dummy";
	pgstrom_dummy_plan_methods.CreateCustomScanState
		= pgstrom_dummy_create_scan_state;

	/* planner hook registration */
	planner_hook_next = planner_hook;
	planner_hook = pgstrom_post_planner;
}

#if 1
// legacy interface
/* ------------------------------------------------------------
 *
 * Misc routines to support EXPLAIN command
 *
 * ------------------------------------------------------------
 */
void
pgstrom_explain_expression(List *expr_list, const char *qlabel,
						   PlanState *planstate, List *deparse_context,
						   List *ancestors, ExplainState *es,
						   bool force_prefix, bool convert_to_and)
{
	bool        useprefix = (force_prefix | es->verbose);
	char       *exprstr;

	/* No work if empty expression list */
	if (expr_list == NIL)
		return;

	/* Deparse the expression */
	/* List shall be replaced by explicit AND, if needed */
	exprstr = deparse_expression(convert_to_and
								 ? (Node *) make_ands_explicit(expr_list)
								 : (Node *) expr_list,
								 deparse_context,
								 useprefix,
								 false);
	/* And add to es->str */
	ExplainPropertyText(qlabel, exprstr, es);
}

#if 0
#endif

void
show_scan_qual(List *qual, const char *qlabel,
               PlanState *planstate, List *ancestors,
               ExplainState *es)
{
	bool        useprefix;
	Node	   *node;
	List       *context;
	char       *exprstr;

	useprefix = (IsA(planstate->plan, SubqueryScan) || es->verbose);

	/* No work if empty qual */
	if (qual == NIL)
		return;

	/* Convert AND list to explicit AND */
	node = (Node *) make_ands_explicit(qual);

	/* Set up deparsing context */
	context = set_deparse_context_planstate(es->deparse_cxt,
											(Node *) planstate,
											ancestors);
	/* Deparse the expression */
	exprstr = deparse_expression(node, context, useprefix, false);

	/* And add to es->str */
	ExplainPropertyText(qlabel, exprstr, es);
}

/*
 * If it's EXPLAIN ANALYZE, show instrumentation information for a plan node
 *
 * "which" identifies which instrumentation counter to print
 */
void
show_instrumentation_count(const char *qlabel, int which,
						   PlanState *planstate, ExplainState *es)
{
	double		nfiltered;
	double		nloops;

	if (!es->analyze || !planstate->instrument)
		return;

	if (which == 2)
		nfiltered = planstate->instrument->nfiltered2;
	else
		nfiltered = planstate->instrument->nfiltered1;
	nloops = planstate->instrument->nloops;

	/* In text mode, suppress zero counts; they're not interesting enough */
	if (nfiltered > 0 || es->format != EXPLAIN_FORMAT_TEXT)
	{
		if (nloops > 0)
			ExplainPropertyFloat(qlabel, nfiltered / nloops, 0, es);
		else
			ExplainPropertyFloat(qlabel, 0.0, 0, es);
	}
}

#if 0
void
pgstrom_init_perfmon(GpuTaskState *gts)
{
	GpuContext	   *gcontext = gts->gcontext;

	memset(&gts->pfm, 0, sizeof(pgstrom_perfmon));
	gts->pfm.enabled = pgstrom_perfmon_enabled;
	gts->pfm.prime_in_gpucontext = (gcontext && gcontext->refcnt == 1);
	gts->pfm.extra_flags = gts->extra_flags;
}

static void
pgstrom_explain_perfmon(GpuTaskState *gts, ExplainState *es)
{
	pgstrom_perfmon	   *pfm = &gts->pfm;
	char				buf[1024];

	if (!pfm->enabled)
		return;

	/* common performance statistics */
	ExplainPropertyInteger("Number of tasks", pfm->num_tasks, es);

#define EXPLAIN_KERNEL_PERFMON(label,num_field,tv_field)		\
	do {														\
		if (pfm->num_field > 0)									\
		{														\
			snprintf(buf, sizeof(buf),							\
					 "total: %s, avg: %s, count: %u",			\
					 format_millisec(pfm->tv_field),			\
					 format_millisec(pfm->tv_field /			\
									 (double)pfm->num_field),	\
					 pfm->num_field);							\
			ExplainPropertyText(label, buf, es);				\
		}														\
	} while(0)

	/* GpuScan: kernel execution */
	if ((pfm->extra_flags & DEVKERNEL_NEEDS_GPUSCAN) != 0)
	{
		EXPLAIN_KERNEL_PERFMON("gpuscan_exec_quals",
							   gscan.num_kern_exec_quals,
							   gscan.tv_kern_exec_quals);
		EXPLAIN_KERNEL_PERFMON("gpuscan_projection",
							   gscan.num_kern_projection,
							   gscan.tv_kern_projection);
	}

	/* GpuJoin: kernel execution */
	if ((pfm->extra_flags & DEVKERNEL_NEEDS_GPUJOIN) != 0)
	{
		EXPLAIN_KERNEL_PERFMON("gpujoin_main()",
							   gjoin.num_kern_main,
							   gjoin.tv_kern_main);
		EXPLAIN_KERNEL_PERFMON(" - gpujoin_exec_outerscan",
							   gjoin.num_kern_outer_scan,
							   gjoin.tv_kern_outer_scan);
		EXPLAIN_KERNEL_PERFMON(" - gpujoin_exec_nestloop",
							   gjoin.num_kern_exec_nestloop,
							   gjoin.tv_kern_exec_nestloop);
		EXPLAIN_KERNEL_PERFMON(" - gpujoin_exec_hashjoin",
							   gjoin.num_kern_exec_hashjoin,
							   gjoin.tv_kern_exec_hashjoin);
		EXPLAIN_KERNEL_PERFMON(" - gpujoin_outer_nestloop",
							   gjoin.num_kern_outer_nestloop,
							   gjoin.tv_kern_outer_nestloop);
		EXPLAIN_KERNEL_PERFMON(" - gpujoin_outer_hashjoin",
							   gjoin.num_kern_outer_hashjoin,
							   gjoin.tv_kern_outer_hashjoin);
		EXPLAIN_KERNEL_PERFMON(" - gpujoin_projection",
							   gjoin.num_kern_projection,
							   gjoin.tv_kern_projection);
		EXPLAIN_KERNEL_PERFMON(" - gpujoin_count_rows_dist",
							   gjoin.num_kern_rows_dist,
							   gjoin.tv_kern_rows_dist);
		if (pfm->gjoin.num_global_retry > 0 ||
			pfm->gjoin.num_major_retry > 0 ||
			pfm->gjoin.num_minor_retry > 0)
		{
			snprintf(buf, sizeof(buf), "global: %u, major: %u, minor: %u",
					 pfm->gjoin.num_global_retry,
					 pfm->gjoin.num_major_retry,
					 pfm->gjoin.num_minor_retry);
			ExplainPropertyText("Retry Loops", buf, es);
		}
	}

	/* GpuPreAgg: kernel execution */
	if ((pfm->extra_flags & DEVKERNEL_NEEDS_GPUPREAGG) != 0)
	{
		EXPLAIN_KERNEL_PERFMON("gpupreagg_main()",
							   gpreagg.num_kern_main,
							   gpreagg.tv_kern_main);
		EXPLAIN_KERNEL_PERFMON(" - gpupreagg_preparation()",
							   gpreagg.num_kern_prep,
							   gpreagg.tv_kern_prep);
		EXPLAIN_KERNEL_PERFMON(" - gpupreagg_nogroup_reduction()",
							   gpreagg.num_kern_nogrp,
							   gpreagg.tv_kern_nogrp);
		EXPLAIN_KERNEL_PERFMON(" - gpupreagg_local_reduction()",
							   gpreagg.num_kern_lagg,
							   gpreagg.tv_kern_lagg);
		EXPLAIN_KERNEL_PERFMON(" - gpupreagg_global_reduction()",
							   gpreagg.num_kern_gagg,
							   gpreagg.tv_kern_gagg);
		EXPLAIN_KERNEL_PERFMON(" - gpupreagg_final_reduction()",
							   gpreagg.num_kern_fagg,
							   gpreagg.tv_kern_fagg);
		EXPLAIN_KERNEL_PERFMON(" - gpupreagg_fixup_varlena()",
							   gpreagg.num_kern_fixvar,
							   gpreagg.tv_kern_fixvar);
	}
#ifdef NOT_USED
	/* GpuSort: kernel execution */
	if ((pfm->extra_flags & DEVKERNEL_NEEDS_GPUSORT) != 0)
	{
		EXPLAIN_KERNEL_PERFMON("gpusort_projection()",
							   gsort.num_kern_proj,
							   gsort.tv_kern_proj);
		EXPLAIN_KERNEL_PERFMON("gpusort_main()",
							   gsort.num_kern_main,
							   gsort.tv_kern_main);
		EXPLAIN_KERNEL_PERFMON(" - gpusort_bitonic_local()",
							   gsort.num_kern_lsort,
							   gsort.tv_kern_lsort);
		EXPLAIN_KERNEL_PERFMON(" - gpusort_bitonic_step()",
							   gsort.num_kern_ssort,
							   gsort.tv_kern_ssort);
		EXPLAIN_KERNEL_PERFMON(" - gpusort_bitonic_merge()",
							   gsort.num_kern_msort,
							   gsort.tv_kern_msort);
		EXPLAIN_KERNEL_PERFMON(" - gpusort_fixup_pointers()",
							   gsort.num_kern_fixvar,
							   gsort.tv_kern_fixvar);
		snprintf(buf, sizeof(buf), "total: %s",
				 format_millisec(pfm->gsort.tv_cpu_sort));
		ExplainPropertyText("CPU merge sort", buf, es);
	}
#endif	/* NOT_USED */
#undef EXPLAIN_KERNEL_PERFMON
	/* Time of I/O stuff */
	if ((pfm->extra_flags & DEVKERNEL_NEEDS_GPUJOIN) != 0)
	{
		snprintf(buf, sizeof(buf), "%s",
				 format_millisec(pfm->time_inner_load));
		ExplainPropertyText("Time of inner load", buf, es);
		snprintf(buf, sizeof(buf), "%s",
				 format_millisec(pfm->time_outer_load));
		ExplainPropertyText("Time of outer load", buf, es);
	}
	else
	{
		snprintf(buf, sizeof(buf), "%s",
				 format_millisec(pfm->time_outer_load));
		ExplainPropertyText("Time of load", buf, es);
	}

	snprintf(buf, sizeof(buf), "%s",
			 format_millisec(pfm->time_materialize));
	ExplainPropertyText("Time of materialize", buf, es);

	/* DMA Send/Recv performance */
	if (pfm->num_dma_send > 0)
	{
		Size	band = (Size)((double)pfm->bytes_dma_send *
							  1000.0 / pfm->time_dma_send);
		snprintf(buf, sizeof(buf),
				 "%s/sec, len: %s, time: %s, count: %u",
				 format_bytesz(band),
				 format_bytesz((double)pfm->bytes_dma_send),
				 format_millisec(pfm->time_dma_send),
				 pfm->num_dma_send);
		ExplainPropertyText("DMA send", buf, es);
	}

	if (pfm->num_dma_recv > 0)
	{
		Size	band = (Size)((double)pfm->bytes_dma_recv *
							  1000.0 / pfm->time_dma_recv);
		snprintf(buf, sizeof(buf),
				 "%s/sec, len: %s, time: %s, count: %u",
				 format_bytesz(band),
				 format_bytesz((double)pfm->bytes_dma_recv),
				 format_millisec(pfm->time_dma_recv),
				 pfm->num_dma_recv);
		ExplainPropertyText("DMA recv", buf, es);
	}

	/* Time to build CUDA code */
	if (pfm->tv_build_start.tv_sec > 0 &&
		pfm->tv_build_end.tv_sec > 0 &&
		(pfm->tv_build_start.tv_sec < pfm->tv_build_end.tv_sec ||
		 (pfm->tv_build_start.tv_sec == pfm->tv_build_end.tv_sec &&
		  pfm->tv_build_start.tv_usec < pfm->tv_build_end.tv_usec)))
	{
		cl_double	tv_cuda_build = PERFMON_TIMEVAL_DIFF(pfm->tv_build_start,
														 pfm->tv_build_end);
		snprintf(buf, sizeof(buf), "%s", format_millisec(tv_cuda_build));
		ExplainPropertyText("Build CUDA Program", buf, es);
	}

	/* Host/Device Memory Allocation (only prime node) */
	if (pfm->prime_in_gpucontext)
	{
		GpuContext *gcontext = gts->gcontext;
		cl_int		num_host_malloc = *gcontext->p_num_host_malloc;
		cl_int		num_host_mfree = *gcontext->p_num_host_mfree;
		cl_int		num_dev_malloc = gcontext->num_dev_malloc;
		cl_int		num_dev_mfree = gcontext->num_dev_mfree;
		cl_double	tv_host_malloc =
			PFMON_TIMEVAL_AS_FLOAT(gcontext->p_tv_host_malloc);
		cl_double	tv_host_mfree =
			PFMON_TIMEVAL_AS_FLOAT(gcontext->p_tv_host_mfree);
		cl_double	tv_dev_malloc =
			PFMON_TIMEVAL_AS_FLOAT(&gcontext->tv_dev_malloc);
		cl_double	tv_dev_mfree =
			PFMON_TIMEVAL_AS_FLOAT(&gcontext->tv_dev_mfree);

		snprintf(buf, sizeof(buf),
				 "alloc (count: %u, time: %s), free (count: %u, time: %s)",
				 num_host_malloc, format_millisec(tv_host_malloc),
				 num_host_mfree, format_millisec(tv_host_mfree));
		ExplainPropertyText("CUDA host memory", buf, es);

		snprintf(buf, sizeof(buf),
				 "alloc (count: %u, time: %s), free (count: %u, time: %s)",
				 num_dev_malloc, format_millisec(tv_dev_malloc),
				 num_dev_mfree, format_millisec(tv_dev_mfree));
		ExplainPropertyText("CUDA device memory", buf, es);
	}
}

/*
 * pgstrom_explain_gputaskstate
 *
 * common additional explain output for all the GpuTaskState nodes
 */
void
pgstrom_explain_gputaskstate(GpuTaskState *gts, ExplainState *es)
{
	/*
	 * Extra features if any
	 */
	if (es->verbose)
	{
		char	temp[256];
		int		ofs = 0;

		/* run per-chunk-execution? */
		if (gts->outer_bulk_exec)
			ofs += snprintf(temp+ofs, sizeof(temp) - ofs,
							"%souter-bulk-exec",
							ofs > 0 ? ", " : "");
		/* per-chunk-execution support? */
		if (gts->cb_bulk_exec != NULL)
			ofs += snprintf(temp+ofs, sizeof(temp) - ofs,
							"%sbulk-exec-support",
							ofs > 0 ? ", " : "");
		/* preferable result format */
		if (gts->be_row_format)
			ofs += snprintf(temp+ofs, sizeof(temp) - ofs, "%srow-format",
							ofs > 0 ? ", " : "");
		if (ofs > 0)
			ExplainPropertyText("Extra", temp, es);
	}

#if 0
	/*
	 * Show source path of the GPU kernel
	 */
	if (es->verbose &&
		gts->kern_source != NULL &&
		pgstrom_debug_kernel_source)
	{
		const char *cuda_source = pgstrom_cuda_source_file(gts);

		ExplainPropertyText("Kernel Source", cuda_source, es);
	}
#endif

	/*
	 * Show performance information
	 */
	if (es->analyze && gts->pfm.enabled)
		pgstrom_explain_perfmon(gts, es);
}
#endif
#endif

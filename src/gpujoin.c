/*
 * gpujoin.c
 *
 * GPU accelerated relations join, based on nested-loop or hash-join
 * algorithm.
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
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/heap.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_crc.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include <math.h>
#include "pg_strom.h"
#include "cuda_numeric.h"
#include "cuda_gpujoin.h"

/*
 * GpuJoinPath
 */
typedef struct
{
	CustomPath		cpath;
	int				num_rels;
	Index			outer_relid;	/* valid, if outer scan pull-up */
	List		   *outer_quals;	/* qualifier of outer scan */
	cl_uint			outer_nrows_per_block;
	struct {
		JoinType	join_type;		/* one of JOIN_* */
		double		join_nrows;		/* intermediate nrows in this depth */
		Path	   *scan_path;		/* outer scan path */
		List	   *hash_quals;		/* valid quals, if hash-join */
		List	   *join_quals;		/* all the device quals, incl hash_quals */
		Size		ichunk_size;	/* expected inner chunk size */
	} inners[FLEXIBLE_ARRAY_MEMBER];
} GpuJoinPath;

/*
 * GpuJoinInfo - private state object of CustomScan(GpuJoin)
 */
typedef struct
{
	int			num_rels;
	char	   *kern_source;
	int			extra_flags;
	List	   *used_params;
	List	   *outer_quals;
	double		outer_ratio;
	double		outer_nrows;		/* number of estimated outer nrows*/
	int			outer_width;		/* copy of @plan_width in outer path */
	Cost		outer_startup_cost;	/* copy of @startup_cost in outer path */
	Cost		outer_total_cost;	/* copy of @total_cost in outer path */
	cl_uint		outer_nrows_per_block;
	/* for each depth */
	List	   *plan_nrows_in;	/* list of floatVal for planned nrows_in */
	List	   *plan_nrows_out;	/* list of floatVal for planned nrows_out */
	List	   *ichunk_size;
	List	   *join_types;
	List	   *join_quals;
	List	   *other_quals;
	List	   *hash_inner_keys;	/* if hash-join */
	List	   *hash_outer_keys;	/* if hash-join */
	/* supplemental information of ps_tlist */
	List	   *ps_src_depth;	/* source depth of the ps_tlist entry */
	List	   *ps_src_resno;	/* source resno of the ps_tlist entry */
	cl_uint		extra_maxlen;	/* max length of extra area per rows */
} GpuJoinInfo;

static inline void
form_gpujoin_info(CustomScan *cscan, GpuJoinInfo *gj_info)
{
	List	   *privs = NIL;
	List	   *exprs = NIL;

	privs = lappend(privs, makeInteger(gj_info->num_rels));
	privs = lappend(privs, makeString(pstrdup(gj_info->kern_source)));
	privs = lappend(privs, makeInteger(gj_info->extra_flags));
	exprs = lappend(exprs, gj_info->used_params);
	exprs = lappend(exprs, gj_info->outer_quals);
	privs = lappend(privs, pmakeFloat(gj_info->outer_ratio));
	privs = lappend(privs, pmakeFloat(gj_info->outer_nrows));
	privs = lappend(privs, makeInteger(gj_info->outer_width));
	privs = lappend(privs, pmakeFloat(gj_info->outer_startup_cost));
	privs = lappend(privs, pmakeFloat(gj_info->outer_total_cost));
	privs = lappend(privs, makeInteger(gj_info->outer_nrows_per_block));
	/* for each depth */
	privs = lappend(privs, gj_info->plan_nrows_in);
	privs = lappend(privs, gj_info->plan_nrows_out);
	privs = lappend(privs, gj_info->ichunk_size);
	privs = lappend(privs, gj_info->join_types);
	exprs = lappend(exprs, gj_info->join_quals);
	exprs = lappend(exprs, gj_info->other_quals);
	exprs = lappend(exprs, gj_info->hash_inner_keys);
	exprs = lappend(exprs, gj_info->hash_outer_keys);

	privs = lappend(privs, gj_info->ps_src_depth);
	privs = lappend(privs, gj_info->ps_src_resno);
	privs = lappend(privs, makeInteger(gj_info->extra_maxlen));

	cscan->custom_private = privs;
	cscan->custom_exprs = exprs;
}

static inline GpuJoinInfo *
deform_gpujoin_info(CustomScan *cscan)
{
	GpuJoinInfo *gj_info = palloc0(sizeof(GpuJoinInfo));
	List	   *privs = cscan->custom_private;
	List	   *exprs = cscan->custom_exprs;
	int			pindex = 0;
	int			eindex = 0;

	gj_info->num_rels = intVal(list_nth(privs, pindex++));
	gj_info->kern_source = strVal(list_nth(privs, pindex++));
	gj_info->extra_flags = intVal(list_nth(privs, pindex++));
	gj_info->used_params = list_nth(exprs, eindex++);
	gj_info->outer_quals = list_nth(exprs, eindex++);
	gj_info->outer_ratio = floatVal(list_nth(privs, pindex++));
	gj_info->outer_nrows = floatVal(list_nth(privs, pindex++));
	gj_info->outer_width = intVal(list_nth(privs, pindex++));
	gj_info->outer_startup_cost = floatVal(list_nth(privs, pindex++));
	gj_info->outer_total_cost = floatVal(list_nth(privs, pindex++));
	gj_info->outer_nrows_per_block = intVal(list_nth(privs, pindex++));
	/* for each depth */
	gj_info->plan_nrows_in = list_nth(privs, pindex++);
	gj_info->plan_nrows_out = list_nth(privs, pindex++);
	gj_info->ichunk_size = list_nth(privs, pindex++);
	gj_info->join_types = list_nth(privs, pindex++);
    gj_info->join_quals = list_nth(exprs, eindex++);
	gj_info->other_quals = list_nth(exprs, eindex++);
	gj_info->hash_inner_keys = list_nth(exprs, eindex++);
    gj_info->hash_outer_keys = list_nth(exprs, eindex++);

	gj_info->ps_src_depth = list_nth(privs, pindex++);
	gj_info->ps_src_resno = list_nth(privs, pindex++);
	gj_info->extra_maxlen = intVal(list_nth(privs, pindex++));
	Assert(pindex == list_length(privs));
	Assert(eindex == list_length(exprs));

	return gj_info;
}

/*
 * GpuJoinState - execution state object of GpuJoin
 */
typedef struct
{
	/*
	 * Execution status
	 */
	PlanState		   *state;
	ExprContext		   *econtext;
	pgstrom_data_store *pds_in;

	/*
	 * Join properties; both nest-loop and hash-join
	 */
	int					depth;
	JoinType			join_type;
	double				nrows_ratio;
	cl_uint				ichunk_size;
	List			   *join_quals;		/* single element list of ExprState */
	List			   *other_quals;	/* single element list of ExprState */

	/*
	 * Join properties; only hash-join
	 */
	List			   *hash_outer_keys;
	List			   *hash_inner_keys;
	List			   *hash_keylen;
	List			   *hash_keybyval;
	List			   *hash_keytype;

	/* CPU Fallback related */
	AttrNumber		   *inner_dst_resno;
	AttrNumber			inner_src_anum_min;
	AttrNumber			inner_src_anum_max;
	cl_long				fallback_inner_index;
	pg_crc32			fallback_inner_hash;
	cl_bool				fallback_inner_matched;
	cl_bool				fallback_right_outer;
} innerState;

typedef struct
{
	GpuTaskState	gts;
	/* shared state (inner hash/heap buffer) */
	GpuJoinSharedState *gj_sstate;
	/* expressions to be used in fallback path */
	List		   *join_types;
	List		   *outer_quals;	/* list of ExprState */
	double			outer_ratio;
	double			outer_nrows;
	List		   *hash_outer_keys;
	List		   *join_quals;
	/* result width per tuple for buffer length calculation */
	int				result_width;
	/* expected extra length per result tuple  */
	cl_uint			extra_maxlen;

	/* buffer for row materialization  */
	HeapTupleData	curr_tuple;

	/*
	 * The first RIGHT OUTER JOIN depth, if any. It is a hint for optimization
	 * because it is obvious the shallower depth will produce no tuples when
	 * no input tuples are supplied.
	 */
	cl_int			first_right_outer_depth;

	/*
	 * CPU Fallback
	 */
	TupleTableSlot *slot_fallback;
	ProjectionInfo *proj_fallback;		/* slot_fallback -> scan_slot */
	AttrNumber	   *outer_dst_resno;	/* destination attribute number to */
	AttrNumber		outer_src_anum_min;	/* be mapped on the slot_fallback */
	AttrNumber		outer_src_anum_max;
	cl_long			fallback_outer_index;

	/*
	 * Template for pds_dst
	 */
	kern_data_store	*kds_dst_head;

	/*
	 * Properties of underlying inner relations
	 */
	int				num_rels;
	innerState		inners[FLEXIBLE_ARRAY_MEMBER];
} GpuJoinState;

/* DSM object of GpuJoin for CPU parallel */
typedef struct
{
	GpuJoinSharedState *gj_sstate;
	char		data[FLEXIBLE_ARRAY_MEMBER];	/* GpuScanParallelDSM if any */
} GpuJoinParallelDSM;

/*
 * GpuJoinSharedState - shared inner hash/heap buffer
 */
struct GpuJoinSharedState
{
	pg_atomic_uint32 preload_done;	/* non-zero, if preloaded */
	Size			head_length;	/* length of the header portion */
	Size			total_length;	/* total length of the inner buffer */
	pgstrom_data_store **inner_chunks;	/* array of inner PDS */

	/*
	 * Run-time statistics
	 */
	slock_t			lock;			/* protection for statistics */
	size_t			source_ntasks;	/* number of sampled tasks */
	size_t			source_nitems;	/* number of sampled source items */
	size_t			results_nitems;	/* number of joined result items */
	size_t			results_usage;	/* sum of kds_dst->usage */
	size_t		   *inner_nitems;	/* number of inner join results items */
	size_t		   *right_nitems;	/* number of right join results items */
	cl_double	   *row_dist_score;	/* degree of result row distribution */
	bool			row_dist_score_valid; /* true, if RDS is valid */

	/*
	 * MEMO: The coordinator process is responsible to allocate / free
	 * the inner hash/heap buffer on DMA buffer. Even if CPU parallel
	 * execution, PostgreSQL ensures the background workers are terminated
	 * prior to ExecEnd or ExecReScan handler (or callback of resource owner
	 * in case of ereport), it can be released safely.
	 * @nr_tasks means per-device (including CPU) reference counter.
	 * Once per-device counter gets zero when @outer_scan_done is set,
	 * it means outer-join map will not be updated any more, thus, good
	 * chance to write back to the @h_ojmaps.
	 * Once total sum of @nr_tasks gets zero, it means all the INNER or
	 * LEFT OUTER JOIN stuff gets completed and ready for RIGHT/FULL OUTER
	 * JOIN based on the latest outer join map.
	 */
	pthread_mutex_t	mutex;
	cl_bool			outer_scan_done; /* True, if outer scan reached end of
									  * the relation, thus, no more tasks
									  * will be produced. */
	cl_bool			outer_join_kicked; /* true, if outer-join was kicked */
	cl_bool			had_cpu_fallback;/* true, if CPU fallback happen */
	cl_int		   *nr_tasks;	/* # of concurrent tasks (per device) */
	cl_bool		   *is_loaded;	/* True, if inner hash/heap buffer is already
								 * loaded to device memory (per PGworker) */
	CUdeviceptr	   *m_kmrels;	/* GPU buffer for kmrels (per device) */
	CUdeviceptr	   *m_ojmaps;	/* GPU buffer for outer join (per device) */
	cl_bool		   *h_ojmaps;	/* Host memory for outer join map
								 * valid only if RIGHT/FULL OUTER JOIN */
	kern_multirels	kern;
};

/*
 * GpuJoinTask - task object of GpuJoin
 */
typedef struct
{
	GpuTask				task;
	cl_bool				with_nvme_strom;/* true, if NVMe-Strom */
	cl_bool				is_terminator;	/* true, if dummay terminator */
	kern_data_store	   *kds_dst_head;	/* template for pds_dst */
	/* DMA buffers */
	GpuJoinSharedState *gj_sstate;	/* inner heap/hash buffer */
	pgstrom_data_store *pds_src;	/* data store of outer relation */
	pgstrom_data_store *pds_dst;	/* data store of result buffer */
	kern_gpujoin		kern;		/* kern_gpujoin of this request */
} GpuJoinTask;

/* static variables */
static set_join_pathlist_hook_type set_join_pathlist_next;
static CustomPathMethods	gpujoin_path_methods;
static CustomScanMethods	gpujoin_plan_methods;
static CustomExecMethods	gpujoin_exec_methods;
static bool					enable_gpunestloop;
static bool					enable_gpuhashjoin;

/* static functions */
static GpuTask *gpujoin_next_task(GpuTaskState *gts);
static void gpujoin_ready_task(GpuTaskState *gts, GpuTask *gtask);
static void gpujoin_switch_task(GpuTaskState *gts, GpuTask *gtask);
static TupleTableSlot *gpujoin_next_tuple(GpuTaskState *gts);
static TupleTableSlot *gpujoin_next_tuple_fallback(GpuJoinState *gjs,
												   GpuJoinTask *pgjoin);
static pg_crc32 get_tuple_hashvalue(innerState *istate,
									bool is_inner_hashkeys,
									TupleTableSlot *slot,
									bool *p_is_null_keys);

static char *gpujoin_codegen(PlannerInfo *root,
							 CustomScan *cscan,
							 GpuJoinInfo *gj_info,
							 List *tlist,
							 codegen_context *context);
static bool gpujoin_inner_preload(GpuJoinState *gjs);

static GpuJoinSharedState *createGpuJoinSharedState(GpuJoinState *gjs);
static void releaseGpuJoinSharedState(GpuJoinState *gjs, bool is_rescan);

static GpuJoinSharedState *
gpujoinGetInnerBuffer(GpuContext *gcontext, GpuJoinSharedState *gj_sstate);
static bool gpujoinLoadInnerBuffer(GpuContext *gcontext,
								   GpuJoinSharedState *gj_sstate);
static void gpujoinPutInnerBuffer(
	GpuJoinSharedState *gj_sstate,
	void (*outerjoin_kicker)(GpuJoinSharedState *gj_sstate, void *private),
	void *private);

/*
 * misc declarations
 */

/* copied from joinpath.c */
#define PATH_PARAM_BY_REL(path, rel)  \
	((path)->param_info && bms_overlap(PATH_REQ_OUTER(path), (rel)->relids))

/*
 * returns true, if pathnode is GpuJoin
 */
bool
pgstrom_path_is_gpujoin(Path *pathnode)
{
	CustomPath *cpath = (CustomPath *) pathnode;

	if (IsA(cpath, CustomPath) &&
		cpath->methods == &gpujoin_path_methods)
		return true;
	return false;
}

/*
 * returns true, if plannode is GpuJoin
 */
bool
pgstrom_plan_is_gpujoin(const Plan *plannode)
{
	if (IsA(plannode, CustomScan) &&
		((CustomScan *) plannode)->methods == &gpujoin_plan_methods)
		return true;
	return false;
}

/*
 * returns true, if planstate node is GpuJoin
 */
bool
pgstrom_planstate_is_gpujoin(const PlanState *ps)
{
	if (IsA(ps, CustomScanState) &&
		((CustomScanState *) ps)->methods == &gpujoin_exec_methods)
		return true;
	return false;
}

/*
 * dump_gpujoin_path
 *
 * Dumps candidate GpuJoinPath for debugging
 */
static void
__dump_gpujoin_path(StringInfo buf, PlannerInfo *root, Path *pathnode)
{
	RelOptInfo *rel = pathnode->parent;
	Relids		relids = rel->relids;
	List	   *range_tables = root->parse->rtable;
	int			rtindex = -1;
	bool		is_first = true;


	if (rel->reloptkind != RELOPT_BASEREL)
		appendStringInfo(buf, "(");

	while ((rtindex = bms_next_member(relids, rtindex)) >= 0)
	{
		RangeTblEntry  *rte = rt_fetch(rtindex, range_tables);
		Alias		   *eref = rte->eref;

		appendStringInfo(buf, "%s%s",
						 is_first ? "" : ", ",
						 eref->aliasname);
		is_first = false;
	}

	if (rel->reloptkind != RELOPT_BASEREL)
		appendStringInfo(buf, ")");
}

/*
 * estimate_inner_buffersize
 */
static Size
estimate_inner_buffersize(PlannerInfo *root,
						  RelOptInfo *joinrel,
						  Path *outer_path,
						  GpuJoinPath *gpath,
						  double num_chunks,
						  double *p_kern_nloops)
{
	Size		inner_total_sz;
	Size		buffer_size;
	double		kern_nloops = 1.0;
	double		join_ntuples;
	cl_int		ncols;
	cl_int		i, num_rels = gpath->num_rels;

	/*
	 * Estimation: size of inner hash/heap buffer
	 */
	inner_total_sz = STROMALIGN(offsetof(kern_multirels,
										 chunks[num_rels]));
	for (i=0; i < num_rels; i++)
	{
		Path	   *inner_path = gpath->inners[i].scan_path;
		RelOptInfo *inner_rel = inner_path->parent;
		PathTarget *inner_reltarget = inner_rel->reltarget;
		Size		inner_ntuples = (Size)inner_path->rows;
		Size		chunk_size;
		Size		entry_size;
		Size		num_items;

		/*
		 * NOTE: PathTarget->width is not reliable for base relations 
		 * because this fields shows the length of attributes which
		 * are actually referenced, however, we usually load physical
		 * tuples on the KDS/KHash buffer if base relation.
		 */
		ncols = list_length(inner_reltarget->exprs);

		if (gpath->inners[i].hash_quals != NIL)
			entry_size = offsetof(kern_hashitem, t.htup);
		else
			entry_size = offsetof(kern_tupitem, htup);

		entry_size += MAXALIGN(offsetof(HeapTupleHeaderData,
										t_bits[BITMAPLEN(ncols)]));
		if (inner_rel->reloptkind != RELOPT_BASEREL)
			entry_size += MAXALIGN(inner_reltarget->width);
		else
		{
			entry_size += MAXALIGN(((double)(BLCKSZ -
											 SizeOfPageHeaderData)
									* inner_rel->pages
									/ Max(inner_rel->tuples, 1.0))
								   - sizeof(ItemIdData)
								   - SizeofHeapTupleHeader);
		}

		/*
		 * estimation of the inner chunk in this depth
		 */
		if (gpath->inners[i].hash_quals != NIL)
			chunk_size = KDS_CALCULATE_HASH_LENGTH(ncols,
												   inner_ntuples,
												   inner_ntuples * entry_size);
		else
			chunk_size = KDS_CALCULATE_ROW_LENGTH(ncols,
												  inner_ntuples,
												  inner_ntuples * entry_size);
		gpath->inners[i].ichunk_size = chunk_size;
		inner_total_sz += chunk_size;

		/*
		 * NOTE: amount of the intermediation result buffer is preferable
		 * to fit pgstrom_chunk_size(). If too large number of rows are
		 * expected, in-kernel GpuJoin logic internally repeat a series of
		 * JOIN steps.
		 */
		join_ntuples = gpath->inners[i].join_nrows / (double)num_chunks;
		num_items = (Size)((double)(i+2) * join_ntuples);
		buffer_size = offsetof(kern_gpujoin, jscale[num_rels + 1])
			+ BLCKSZ	/* alternative of kern_parambuf */
			+ STROMALIGN(offsetof(kern_resultbuf, results[num_items]))
			+ STROMALIGN(offsetof(kern_resultbuf, results[num_items]));
		if (buffer_size > pgstrom_chunk_size())
		{
			Size	nsplits = (buffer_size / pgstrom_chunk_size()) + 1;

			kern_nloops *= nsplits;
		}
	}
	*p_kern_nloops = kern_nloops;

	return inner_total_sz;
}

/*
 * cost_gpujoin
 *
 * estimation of GpuJoin cost
 */
static bool
cost_gpujoin(PlannerInfo *root,
			 GpuJoinPath *gpath,
			 RelOptInfo *joinrel,
			 List *final_tlist,
			 Path *outer_path,
			 Relids required_outer,
			 int parallel_nworkers)
{
	PathTarget *join_reltarget = joinrel->reltarget;
	Cost		startup_cost = 0.0;
	Cost		run_cost = 0.0;
	Cost		run_cost_per_chunk = 0.0;
	Cost		startup_delay;
	Size		inner_buffer_sz = 0;
	double		gpu_ratio = pgstrom_gpu_operator_cost / cpu_operator_cost;
	double		parallel_divisor = 1.0;
	double		num_chunks;
	double		chunk_ntuples;
	double		page_fault_factor = 1.0;
	double		kern_nloops = 1.0;
	int			i, num_rels = gpath->num_rels;

	/*
	 * Cost comes from the outer-path
	 */
	if (gpath->outer_relid > 0)
	{
		double		dummy;

		cost_gpuscan_common(root,
							outer_path->parent,
							gpath->outer_quals,
							parallel_nworkers,
							&parallel_divisor,
							&dummy,		/* equivalent to outer_path->rows */
							&num_chunks,
							&gpath->outer_nrows_per_block,
							&startup_cost,
							&run_cost);
	}
	else
	{
		startup_cost = pgstrom_gpu_setup_cost + outer_path->startup_cost;
		run_cost = outer_path->total_cost - outer_path->startup_cost;
		num_chunks = estimate_num_chunks(outer_path);
	}

	/*
	 * Estimation of inner hash/heap buffer, and number of internal loop
	 * to process in-kernel Join logic
	 */
	inner_buffer_sz = estimate_inner_buffersize(root,
												joinrel,
												outer_path,
												gpath,
												num_chunks,
												&kern_nloops);
	if (inner_buffer_sz > gpuMemMaxAllocSize())
	{
		double	ratio = ((double)(gpuMemMaxAllocSize() - inner_buffer_sz) /
						 (double)(gpuMemMaxAllocSize()));
		page_fault_factor += ratio * ratio;
		if (inner_buffer_sz > 5 * gpuMemMaxAllocSize())
			startup_cost += disable_cost;
	}

	/*
	 * Cost for each depth
	 */
	chunk_ntuples = outer_path->rows / num_chunks;
	for (i=0; i < num_rels; i++)
	{
		Path	   *scan_path = gpath->inners[i].scan_path;
		List	   *hash_quals = gpath->inners[i].hash_quals;
		List	   *join_quals = gpath->inners[i].join_quals;
		double		join_nrows = gpath->inners[i].join_nrows;
		QualCost	join_quals_cost;

		/* cost to load all the tuples from inner-path */
		startup_cost += scan_path->total_cost;

		/* cost for join_qual startup */
		cost_qual_eval(&join_quals_cost, join_quals, root);
		join_quals_cost.per_tuple *= gpu_ratio;
		startup_cost += join_quals_cost.startup;

		/*
		 * cost to evaluate join qualifiers according to
		 * the GpuJoin logic
		 */
		if (hash_quals != NIL)
		{
			/*
			 * GpuHashJoin - It computes hash-value of inner tuples by CPU,
			 * but outer tuples by GPU, then it evaluates join-qualifiers
			 * for each items on inner hash table by GPU.
			 */
			cl_uint		num_hashkeys = list_length(hash_quals);
			double		hash_nsteps = scan_path->rows /
				(double)__KDS_NSLOTS((Size)scan_path->rows);

			/* cost to compute inner hash value by CPU */
			startup_cost += (cpu_operator_cost * num_hashkeys *
							 scan_path->rows);
			/* cost to compute hash value by GPU */
			run_cost_per_chunk += (pgstrom_gpu_operator_cost *
								   num_hashkeys *
								   chunk_ntuples);
			/* cost to evaluate join qualifiers */
			run_cost_per_chunk += (join_quals_cost.per_tuple *
								   Max(hash_nsteps, 1.0));
		}
		else
		{
			/*
			 * GpuNestLoop - It evaluates join-qual for each pair of outer
			 * and inner tuples. So, its run_cost is usually higher than
			 * GpuHashJoin.
			 */
			double		inner_ntuples = scan_path->rows;

			/* cost to preload inner heap tuples by CPU */
			startup_cost += cpu_tuple_cost * inner_ntuples;

			/* cost to evaluate join qualifiers */
			run_cost_per_chunk += (join_quals_cost.per_tuple *
								   chunk_ntuples *
								   inner_ntuples);
		}
		/* number of outer items on the next depth */
		chunk_ntuples = join_nrows / num_chunks;
	}
	/* total GPU execution cost */
	run_cost += (run_cost_per_chunk *
				 num_chunks *
				 kern_nloops *
				 page_fault_factor);
	/* outer DMA send cost */
	run_cost += (double)num_chunks * pgstrom_gpu_dma_cost;
	/* inner DMA send cost */
	run_cost += ((double)inner_buffer_sz /
				 (double)pgstrom_chunk_size()) * pgstrom_gpu_dma_cost;

	/*
	 * cost discount by GPU projection, if this join is the last level
	 */
	if (final_tlist != NIL)
	{
		Cost		discount_per_tuple = 0.0;
		Cost		discount_total;
		QualCost	qcost;
		cl_uint		num_vars = 0;
		ListCell   *lc;

		foreach (lc, final_tlist)
		{
			TargetEntry	   *tle = lfirst(lc);

			if (IsA(tle->expr, Var) ||
				IsA(tle->expr, Const) ||
				IsA(tle->expr, Param))
				num_vars++;
			else if (pgstrom_device_expression(tle->expr))
            {
                cost_qual_eval_node(&qcost, (Node *)tle->expr, root);
                discount_per_tuple += (qcost.per_tuple *
                                       Max(1.0 - gpu_ratio, 0.0) / 10.0);
                num_vars++;
            }
            else
            {
				List	   *vars_list
					= pull_vars_of_level((Node *)tle->expr, 0);
				num_vars += list_length(vars_list);
				list_free(vars_list);
			}
		}

		if (num_vars > list_length(join_reltarget->exprs))
			discount_per_tuple -= cpu_tuple_cost *
				(double)(num_vars - list_length(join_reltarget->exprs));
		discount_total = Max(discount_per_tuple, 0.0) * joinrel->rows;

		run_cost = Max(run_cost - discount_total, 0.0);
	}

	/*
	 * delay to fetch the first tuple
	 */
	startup_delay = run_cost * (1.0 / num_chunks);

	/*
	 * cost of final materialization, but GPU does projection
	 */
//	run_cost += cpu_tuple_cost * gpath->cpath.path.rows;

	/*
	 * Put cost value on the gpath.
	 */
	gpath->cpath.path.startup_cost = startup_cost + startup_delay;
	gpath->cpath.path.total_cost = startup_cost + run_cost;

	/*
	 * NOTE: If very large number of rows are estimated, it may cause
	 * overflow of variables, then makes nearly negative infinite cost
	 * even though the plan is very bad.
	 * At this moment, we put assertion to detect it.
	 */
	Assert(gpath->cpath.path.startup_cost >= 0.0 &&
		   gpath->cpath.path.total_cost >= 0.0);

	if (add_path_precheck(gpath->cpath.path.parent,
						  gpath->cpath.path.startup_cost,
						  gpath->cpath.path.total_cost,
						  NULL, required_outer))
	{
		/* Dumps candidate GpuJoinPath for debugging */
		if (client_min_messages <= DEBUG1)
		{
			StringInfoData buf;

			initStringInfo(&buf);
			__dump_gpujoin_path(&buf, root, outer_path);
			for (i=0; i < gpath->num_rels; i++)
			{
				JoinType	join_type = gpath->inners[i].join_type;
				Path	   *inner_path = gpath->inners[i].scan_path;
				bool		is_nestloop = (gpath->inners[i].hash_quals == NIL);

				appendStringInfo(&buf, " %s%s ",
								 join_type == JOIN_FULL ? "F" :
								 join_type == JOIN_LEFT ? "L" :
								 join_type == JOIN_RIGHT ? "R" : "I",
								 is_nestloop ? "NL" : "HJ");

				__dump_gpujoin_path(&buf, root, inner_path);
			}
			elog(DEBUG1, "GpuJoin: %s Cost=%.2f..%.2f",
				 buf.data,
				 gpath->cpath.path.startup_cost,
				 gpath->cpath.path.total_cost);
			pfree(buf.data);
		}
		return true;
	}
	return false;
}

typedef struct
{
	JoinType	join_type;
	Path	   *inner_path;
	List	   *join_quals;
	List	   *hash_quals;
	double		join_nrows;
} inner_path_item;

static GpuJoinPath *
create_gpujoin_path(PlannerInfo *root,
					RelOptInfo *joinrel,
					Path *outer_path,
					List *inner_path_items_list,
					List *final_tlist,
					ParamPathInfo *param_info,
					Relids required_outer,
					bool try_parallel_path)
{
	GpuJoinPath *gjpath;
	cl_int		num_rels = list_length(inner_path_items_list);
	ListCell   *lc;
	int			parallel_nworkers = 0;
	bool		inner_parallel_safe = true;
	int			i;

	/* parallel path must have parallel_safe sub-paths */
	if (try_parallel_path)
	{
		if (!outer_path->parallel_safe)
			return NULL;
		foreach (lc, inner_path_items_list)
		{
			inner_path_item *ip_item = lfirst(lc);

			if (!ip_item->inner_path->parallel_safe)
				return NULL;
		}
		parallel_nworkers = outer_path->parallel_workers;
	}

	gjpath = palloc0(offsetof(GpuJoinPath, inners[num_rels + 1]));
	NodeSetTag(gjpath, T_CustomPath);
	gjpath->cpath.path.pathtype = T_CustomScan;
	gjpath->cpath.path.parent = joinrel;
	gjpath->cpath.path.pathtarget = joinrel->reltarget;
	gjpath->cpath.path.param_info = param_info;	// XXXXXX
	gjpath->cpath.path.pathkeys = NIL;
	gjpath->cpath.path.rows = joinrel->rows;
	gjpath->cpath.flags = 0;
	gjpath->cpath.methods = &gpujoin_path_methods;
	gjpath->outer_relid = 0;
	gjpath->outer_quals = NIL;
	gjpath->num_rels = num_rels;

	i = 0;
	foreach (lc, inner_path_items_list)
	{
		inner_path_item *ip_item = lfirst(lc);
		List	   *hash_quals;

		if (enable_gpuhashjoin && ip_item->hash_quals != NIL)
			hash_quals = ip_item->hash_quals;
		else if (enable_gpunestloop &&
				 (ip_item->join_type == JOIN_INNER ||
				  ip_item->join_type == JOIN_LEFT))
			hash_quals = NIL;
		else
		{
			pfree(gjpath);
			return NULL;
		}
		if (!ip_item->inner_path->parallel_safe)
			inner_parallel_safe = false;
		gjpath->inners[i].join_type = ip_item->join_type;
		gjpath->inners[i].join_nrows = ip_item->join_nrows;
		gjpath->inners[i].scan_path = ip_item->inner_path;
		gjpath->inners[i].hash_quals = hash_quals;
		gjpath->inners[i].join_quals = ip_item->join_quals;
		gjpath->inners[i].ichunk_size = 0;		/* to be set later */
		i++;
	}
	Assert(i == num_rels);

	/* Try to pull up outer scan if enough simple */
	pgstrom_pullup_outer_scan(outer_path,
							  &gjpath->outer_relid,
							  &gjpath->outer_quals);

	/*
	 * cost calculation of GpuJoin, then, add this path to the joinrel,
	 * unless its cost is not obviously huge.
	 */
	if (cost_gpujoin(root,
					 gjpath,
					 joinrel,
					 final_tlist,
					 outer_path,
					 required_outer,
					 parallel_nworkers))
	{
		List   *custom_paths = list_make1(outer_path);

		/* informs planner a list of child pathnodes */
		for (i=0; i < num_rels; i++)
			custom_paths = lappend(custom_paths, gjpath->inners[i].scan_path);
		gjpath->cpath.custom_paths = custom_paths;
		gjpath->cpath.path.parallel_safe = (joinrel->consider_parallel &&
											outer_path->parallel_safe &&
											inner_parallel_safe);
		if (!gjpath->cpath.path.parallel_safe)
			gjpath->cpath.path.parallel_workers = 0;
		else
			gjpath->cpath.path.parallel_workers = parallel_nworkers;
		return gjpath;
	}
	pfree(gjpath);
	return NULL;
}

/*
 * gpujoin_find_cheapest_path
 *
 * finds the cheapest path-node but not parameralized by other relations
 * involved in this GpuJoin.
 */
static Path *
gpujoin_find_cheapest_path(PlannerInfo *root,
						   RelOptInfo *joinrel,
						   RelOptInfo *inputrel,
						   bool only_parallel_safe)
{
	Path	   *input_path = inputrel->cheapest_total_path;
	Relids		other_relids;
	ListCell   *lc;

	other_relids = bms_difference(joinrel->relids, inputrel->relids);
	if ((only_parallel_safe && !input_path->parallel_safe) ||
		bms_overlap(PATH_REQ_OUTER(input_path), other_relids))
	{
		/*
		 * We try to find out the second best path if cheapest path is not
		 * sufficient for the requiement of GpuJoin
		 */
		foreach (lc, inputrel->pathlist)
		{
			Path   *curr_path = lfirst(lc);

			if (only_parallel_safe && !curr_path->parallel_safe)
				continue;
			if (bms_overlap(PATH_REQ_OUTER(curr_path), other_relids))
				continue;
			if (input_path == NULL ||
				input_path->total_cost > curr_path->total_cost)
				input_path = curr_path;
		}
	}
	bms_free(other_relids);

	return input_path;
}

/*
 * extract_gpuhashjoin_quals - pick up qualifiers usable for GpuHashJoin
 */
static List *
extract_gpuhashjoin_quals(PlannerInfo *root,
						  RelOptInfo *outer_rel,
						  RelOptInfo *inner_rel,
						  JoinType jointype,
						  List *restrict_clauses)
{
	List	   *hash_quals = NIL;
	ListCell   *lc;

	foreach (lc, restrict_clauses)
	{
		RestrictInfo   *rinfo = (RestrictInfo *) lfirst(lc);

		/*
		 * If processing an outer join, only use its own join clauses
		 * for hashing.  For inner joins we need not be so picky.
		 */
		if (IS_OUTER_JOIN(jointype) && rinfo->is_pushed_down)
			continue;

		/* Is it hash-joinable clause? */
		if (!rinfo->can_join || !OidIsValid(rinfo->hashjoinoperator))
			continue;

		/*
		 * Check if clause has the form "outer op inner" or
		 * "inner op outer". If suitable, we may be able to choose
		 * GpuHashJoin logic. See clause_sides_match_join also.
		 */
		if ((bms_is_subset(rinfo->left_relids,  outer_rel->relids) &&
			 bms_is_subset(rinfo->right_relids, inner_rel->relids)) ||
			(bms_is_subset(rinfo->left_relids,  inner_rel->relids) &&
			 bms_is_subset(rinfo->right_relids, outer_rel->relids)))
		{
			/* OK, it is hash-joinable qualifier */
			hash_quals = lappend(hash_quals, rinfo);
		}
	}
	return hash_quals;
}

/*
 * try_add_gpujoin_paths
 */
static void
try_add_gpujoin_paths(PlannerInfo *root,
					  RelOptInfo *joinrel,
					  List *final_tlist,
					  Path *outer_path,
					  Path *inner_path,
					  JoinType join_type,
					  JoinPathExtraData *extra,
					  bool try_parallel_path)
{
	Relids			required_outer;
	ParamPathInfo  *param_info;
	inner_path_item *ip_item;
	List		   *ip_items_list;
	List		   *restrict_clauses = extra->restrictlist;
	ListCell	   *lc;

	/* Quick exit if unsupported join type */
	if (join_type != JOIN_INNER &&
		join_type != JOIN_FULL &&
		join_type != JOIN_RIGHT &&
		join_type != JOIN_LEFT)
		return;

	/*
	 * Check to see if proposed path is still parameterized, and reject
	 * if the parameterization wouldn't be sensible.
	 * Note that GpuNestLoop does not support parameterized nest-loop,
	 * only cross-join or non-symmetric join are supported, therefore,
	 * calc_non_nestloop_required_outer() is sufficient.
	 */
	required_outer = calc_non_nestloop_required_outer(outer_path,
													  inner_path);
	if (required_outer &&
		!bms_overlap(required_outer, extra->param_source_rels))
	{
		bms_free(required_outer);
		return;
	}

	/*
	 * Get param info
	 */
	param_info = get_joinrel_parampathinfo(root,
										   joinrel,
										   outer_path,
										   inner_path,
										   extra->sjinfo,
										   required_outer,
										   &restrict_clauses);
	/*
	 * It makes no sense to run cross join on GPU devices without
	 * GPU projection opportunity.
	 */
	if (!final_tlist && !restrict_clauses)
		return;

	/*
	 * All the join-clauses must be executable on GPU device.
	 * Even though older version supports HostQuals to be
	 * applied post device join, it leads undesirable (often
	 * unacceptable) growth of the result rows in device join.
	 * So, we simply reject any join that contains host-only
	 * qualifiers.
	 */
	foreach (lc, restrict_clauses)
	{
		RestrictInfo   *rinfo = lfirst(lc);

		if (!pgstrom_device_expression(rinfo->clause))
			return;
	}

	/*
	 * setup inner_path_item
	 */
	ip_item = palloc0(sizeof(inner_path_item));
	ip_item->join_type = join_type;
	ip_item->inner_path = inner_path;
	ip_item->join_quals = restrict_clauses;
	ip_item->hash_quals = extract_gpuhashjoin_quals(root,
													outer_path->parent,
													inner_path->parent,
													join_type,
													restrict_clauses);
	ip_item->join_nrows = joinrel->rows;
	ip_items_list = list_make1(ip_item);

	for (;;)
	{
		GpuJoinPath	   *gjpath = create_gpujoin_path(root,
													 joinrel,
													 outer_path,
													 ip_items_list,
													 final_tlist,
													 param_info,
													 required_outer,
													 try_parallel_path);
		if (!gjpath)
			break;

		gjpath->cpath.path.parallel_aware = try_parallel_path;
		if (try_parallel_path)
			add_partial_path(joinrel, (Path *)gjpath);
		else
			add_path(joinrel, (Path *)gjpath);

		/*
		 * pull up outer and 
		 */
		if (pgstrom_path_is_gpujoin(outer_path))
		{
			GpuJoinPath	   *gjpath = (GpuJoinPath *) outer_path;
			int				i;

			for (i=gjpath->num_rels-1; i>=0; i--)
			{
				inner_path_item *ip_temp = palloc0(sizeof(inner_path_item));

				ip_temp->join_type  = gjpath->inners[i].join_type;
				ip_temp->inner_path = gjpath->inners[i].scan_path;
				ip_temp->join_quals = gjpath->inners[i].join_quals;
				ip_temp->hash_quals = gjpath->inners[i].hash_quals;
				ip_temp->join_nrows = gjpath->inners[i].join_nrows;

				ip_items_list = lcons(ip_temp, ip_items_list);
			}
			outer_path = linitial(gjpath->cpath.custom_paths);
		}
		else if (outer_path->pathtype == T_NestLoop ||
				 outer_path->pathtype == T_HashJoin ||
				 outer_path->pathtype == T_MergeJoin)
		{
			JoinPath   *join_path = (JoinPath *) outer_path;

			/*
			 * We cannot pull-up outer join path if its inner/outer paths
			 * are mutually parameterized.
			 */
			if (bms_overlap(PATH_REQ_OUTER(join_path->innerjoinpath),
							join_path->outerjoinpath->parent->relids) ||
				bms_overlap(PATH_REQ_OUTER(join_path->outerjoinpath),
							join_path->innerjoinpath->parent->relids))
				return;

			ip_item = palloc0(sizeof(inner_path_item));
			ip_item->join_type = join_path->jointype;
			ip_item->inner_path = join_path->innerjoinpath;
			ip_item->join_quals = join_path->joinrestrictinfo;
			ip_item->hash_quals = extract_gpuhashjoin_quals(
										root,
										join_path->outerjoinpath->parent,
										join_path->innerjoinpath->parent,
										join_path->jointype,
										join_path->joinrestrictinfo);
			ip_item->join_nrows = join_path->path.parent->rows;
			ip_items_list = lcons(ip_item, ip_items_list);
			outer_path = join_path->outerjoinpath;
		}
		else
			break;
	}
}

/*
 * gpujoin_add_join_path
 *
 * entrypoint of the GpuJoin logic
 */
static void
gpujoin_add_join_path(PlannerInfo *root,
					  RelOptInfo *joinrel,
					  RelOptInfo *outerrel,
					  RelOptInfo *innerrel,
					  JoinType jointype,
					  JoinPathExtraData *extra)
{
	Path	   *outer_path;
	Path	   *inner_path;
	List	   *final_tlist = NIL;
	ListCell   *lc1, *lc2;

	/* calls secondary module if exists */
	if (set_join_pathlist_next)
		set_join_pathlist_next(root,
							   joinrel,
							   outerrel,
							   innerrel,
							   jointype,
							   extra);

	/* nothing to do, if PG-Strom is not enabled */
	if (!pgstrom_enabled)
		return;

	/*
	 * Pay attention for the device projection cost if this joinrel may become
	 * the root of plan tree, thus generates the final results.
	 * The cost for projection shall be added at apply_projection_to_path()
	 * later, so we decrement the estimated benefit by GpuProjection.
	 */
	if (bms_equal(root->all_baserels, joinrel->relids))
	{
		foreach (lc1, root->processed_tlist)
		{
			TargetEntry	   *tle = lfirst(lc1);

			if (!IsA(tle->expr, Var) &&
				!IsA(tle->expr, Const) &&
				!IsA(tle->expr, Param) &&
				pgstrom_device_expression(tle->expr))
				break;
		}
		if (!lc1)
			final_tlist = root->processed_tlist;
	}

	/*
	 * make a traditional sequential path
	 */
	inner_path = gpujoin_find_cheapest_path(root, joinrel, innerrel, false);
	if (!inner_path)
		return;
	outer_path = gpujoin_find_cheapest_path(root, joinrel, outerrel, false);
	if (!outer_path)
		return;
	try_add_gpujoin_paths(root, joinrel, final_tlist,
						  outer_path, inner_path,
						  jointype, extra, false);

	/*
	 * consider partial paths if any partial outers
	 */
	if (joinrel->consider_parallel)
	{
		Relids	other_relids = bms_difference(joinrel->relids,
											  outerrel->relids);
		foreach (lc1, innerrel->pathlist)
		{
			inner_path = lfirst(lc1);

			if (!inner_path->parallel_safe ||
				bms_overlap(PATH_REQ_OUTER(inner_path), other_relids))
				continue;

			foreach (lc2, outerrel->partial_pathlist)
			{
				outer_path = lfirst(lc2);

				if (!outer_path->parallel_safe ||
					outer_path->parallel_workers == 0 ||
					bms_overlap(PATH_REQ_OUTER(outer_path), other_relids))
					continue;
				try_add_gpujoin_paths(root, joinrel, final_tlist,
									  outer_path, inner_path,
									  jointype, extra, true);
			}
		}
	}
}

/*
 * build_flatten_qualifier
 *
 * It makes a flat AND expression that is equivalent to the given list.
 */
static Expr *
build_flatten_qualifier(List *clauses)
{
	List	   *args = NIL;
	ListCell   *lc;

	foreach (lc, clauses)
	{
		Node   *expr = lfirst(lc);

		if (!expr)
			continue;
		Assert(exprType(expr) == BOOLOID);
		if (IsA(expr, BoolExpr) &&
			((BoolExpr *) expr)->boolop == AND_EXPR)
			args = list_concat(args, ((BoolExpr *) expr)->args);
		else
			args = lappend(args, expr);
	}
	if (list_length(args) == 0)
		return NULL;
	if (list_length(args) == 1)
		return linitial(args);
	return make_andclause(args);
}

/*
 * build_device_targetlist
 *
 * It constructs a tentative custom_scan_tlist, according to
 * the expression to be evaluated, returned or shown in EXPLAIN.
 * Usually, all we need to pay attention is columns referenced by host-
 * qualifiers and target-list. However, we may need to execute entire
 * JOIN operations on CPU if GPU raised CpuReCheck error. So, we also
 * adds columns which are also referenced by device qualifiers.
 * (EXPLAIN command has to solve the name, so we have to have these
 * Var nodes in the custom_scan_tlist.)
 *
 * pgstrom_post_planner_gpujoin() may update the custom_scan_tlist
 * to push-down CPU projection. In this case, custom_scan_tlist will
 * have complicated expression not only simple Var-nodes, to simplify
 * targetlist of the CustomScan to reduce cost for CPU projection as
 * small as possible we can.
 */
typedef struct
{
	List		   *ps_tlist;
	List		   *ps_depth;
	List		   *ps_resno;
	GpuJoinPath	   *gpath;
	List		   *custom_plans;
	Index			outer_scanrelid;
	bool			resjunk;
} build_device_tlist_context;

static bool
build_device_tlist_walker(Node *node, build_device_tlist_context *context)
{
	GpuJoinPath	   *gpath = context->gpath;
	RelOptInfo	   *rel;
	ListCell	   *cell;
	int				i;

	if (!node)
		return false;
	if (IsA(node, Var))
	{
		Var	   *varnode = (Var *) node;
		Var	   *ps_node;

		foreach (cell, context->ps_tlist)
		{
			TargetEntry	   *tle = lfirst(cell);

			if (!IsA(tle->expr, Var))
				continue;

			ps_node = (Var *) tle->expr;
			if (ps_node->varno == varnode->varno &&
				ps_node->varattno == varnode->varattno &&
				ps_node->varlevelsup == varnode->varlevelsup)
			{
				/* sanity checks */
				Assert(ps_node->vartype == varnode->vartype &&
					   ps_node->vartypmod == varnode->vartypmod &&
					   ps_node->varcollid == varnode->varcollid);
				return false;
			}
		}

		/*
		 * Not in the pseudo-scan targetlist, so append this one
		 */
		for (i=0; i <= gpath->num_rels; i++)
		{
			if (i == 0)
			{
				Path   *outer_path = linitial(gpath->cpath.custom_paths);

				rel = outer_path->parent;
				/* special case if outer scan was pulled up */
				if (varnode->varno == context->outer_scanrelid)
				{
					TargetEntry	   *ps_tle =
						makeTargetEntry((Expr *) copyObject(varnode),
										list_length(context->ps_tlist) + 1,
										NULL,
										context->resjunk);
					context->ps_tlist = lappend(context->ps_tlist, ps_tle);
					context->ps_depth = lappend_int(context->ps_depth, i);
					context->ps_resno = lappend_int(context->ps_resno,
													varnode->varattno);
					Assert(bms_is_member(varnode->varno, rel->relids));
					Assert(varnode->varno == rel->relid);
					return false;
				}
			}
			else
				rel = gpath->inners[i-1].scan_path->parent;

			if (bms_is_member(varnode->varno, rel->relids))
			{
				Plan   *plan = list_nth(context->custom_plans, i);

				foreach (cell, plan->targetlist)
				{
					TargetEntry *tle = lfirst(cell);

					if (equal(varnode, tle->expr))
					{
						TargetEntry	   *ps_tle =
							makeTargetEntry((Expr *) copyObject(varnode),
											list_length(context->ps_tlist) + 1,
											NULL,
											context->resjunk);
						context->ps_tlist = lappend(context->ps_tlist, ps_tle);
						context->ps_depth = lappend_int(context->ps_depth, i);
						context->ps_resno = lappend_int(context->ps_resno,
														tle->resno);
						return false;
					}
				}
				break;
			}
		}
		elog(ERROR, "Bug? uncertain origin of Var-node: %s",
			 nodeToString(varnode));
	}
	else if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phvnode = (PlaceHolderVar *) node;

		foreach (cell, context->ps_tlist)
		{
			TargetEntry	   *tle = lfirst(cell);

			if (equal(phvnode, tle->expr))
				return false;
		}

		/* Not in the pseudo-scan target-list, so append a new one */
		for (i=0; i <= gpath->num_rels; i++)
		{
			if (i == 0)
			{
				/*
				 * NOTE: We don't assume PlaceHolderVar that references the
				 * outer-path which was pulled-up, because only simple scan
				 * paths (SeqScan or GpuScan with no host-only qualifiers)
				 * can be pulled-up, thus, no chance for SubQuery paths.
				 */
				Index	outer_scanrelid = context->outer_scanrelid;
				Path   *outer_path = linitial(gpath->cpath.custom_paths);

				if (outer_scanrelid != 0 &&
					bms_is_member(outer_scanrelid, phvnode->phrels))
					elog(ERROR, "Bug? PlaceHolderVar referenced simple scan outer-path, not expected: %s", nodeToString(phvnode));

				rel = outer_path->parent;
			}
			else
				rel = gpath->inners[i-1].scan_path->parent;

			if (bms_is_subset(phvnode->phrels, rel->relids))
			{
				Plan   *plan = list_nth(context->custom_plans, i);

				foreach (cell, plan->targetlist)
				{
					TargetEntry	   *tle = lfirst(cell);
					TargetEntry	   *ps_tle;
					AttrNumber		ps_resno;

					if (!equal(phvnode, tle->expr))
						continue;

					ps_resno = list_length(context->ps_tlist) + 1;
					ps_tle = makeTargetEntry((Expr *) copyObject(phvnode),
											 ps_resno,
											 NULL,
											 context->resjunk);
					context->ps_tlist = lappend(context->ps_tlist, ps_tle);
					context->ps_depth = lappend_int(context->ps_depth, i);
					context->ps_resno = lappend_int(context->ps_resno,
													tle->resno);
					return false;
				}
			}
		}
		elog(ERROR, "Bug? uncertain origin of PlaceHolderVar-node: %s",
			 nodeToString(phvnode));
	}
	else if (!context->resjunk &&
			 pgstrom_device_expression((Expr *)node))
	{
		TargetEntry	   *ps_tle;

		foreach (cell, context->ps_tlist)
		{
			TargetEntry	   *tle = lfirst(cell);

			if (equal(node, tle->expr))
				return false;
		}

		ps_tle = makeTargetEntry((Expr *) copyObject(node),
								 list_length(context->ps_tlist) + 1,
								 NULL,
								 context->resjunk);
		context->ps_tlist = lappend(context->ps_tlist, ps_tle);
		context->ps_depth = lappend_int(context->ps_depth, -1);	/* dummy */
		context->ps_resno = lappend_int(context->ps_resno, -1);	/* dummy */

		return false;
	}
	return expression_tree_walker(node, build_device_tlist_walker,
								  (void *) context);
}

static void
build_device_targetlist(GpuJoinPath *gpath,
						CustomScan *cscan,
						GpuJoinInfo *gj_info,
						List *targetlist,
						List *custom_plans)
{
	build_device_tlist_context	context;

	Assert(outerPlan(cscan)
		   ? cscan->scan.scanrelid == 0
		   : cscan->scan.scanrelid != 0);

	memset(&context, 0, sizeof(build_device_tlist_context));
	context.gpath = gpath;
	context.custom_plans = custom_plans;
	context.outer_scanrelid = cscan->scan.scanrelid;
	context.resjunk = false;

	build_device_tlist_walker((Node *)targetlist, &context);

	/*
	 * Above are host referenced columns. On the other hands, the columns
	 * newly added below are device-only columns, so it will never
	 * referenced by the host-side. We mark it resjunk=true.
	 *
	 * Also note that any Var nodes in the device executable expression
	 * must be added with resjunk=true to solve the variable name.
	 */
	context.resjunk = true;
	build_device_tlist_walker((Node *)gj_info->outer_quals, &context);
	build_device_tlist_walker((Node *)gj_info->join_quals, &context);
	build_device_tlist_walker((Node *)gj_info->other_quals, &context);
	build_device_tlist_walker((Node *)gj_info->hash_inner_keys, &context);
	build_device_tlist_walker((Node *)gj_info->hash_outer_keys, &context);
	build_device_tlist_walker((Node *)targetlist, &context);

	Assert(list_length(context.ps_tlist) == list_length(context.ps_depth) &&
		   list_length(context.ps_tlist) == list_length(context.ps_resno));

	gj_info->ps_src_depth = context.ps_depth;
	gj_info->ps_src_resno = context.ps_resno;
	cscan->custom_scan_tlist = context.ps_tlist;
}

/*
 * PlanGpuJoinPath
 *
 * Entrypoint to create CustomScan(GpuJoin) node
 */
static Plan *
PlanGpuJoinPath(PlannerInfo *root,
				RelOptInfo *rel,
				CustomPath *best_path,
				List *tlist,
				List *clauses,
				List *custom_plans)
{
	GpuJoinPath	   *gjpath = (GpuJoinPath *) best_path;
	GpuJoinInfo		gj_info;
	CustomScan	   *cscan;
	codegen_context	context;
	char		   *kern_source;
	Plan		   *outer_plan;
	ListCell	   *lc;
	double			outer_nrows;
	int				i;

	Assert(gjpath->num_rels + 1 == list_length(custom_plans));
	outer_plan = linitial(custom_plans);

	cscan = makeNode(CustomScan);
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->flags = best_path->flags;
	cscan->methods = &gpujoin_plan_methods;
	cscan->custom_plans = list_copy_tail(custom_plans, 1);

	memset(&gj_info, 0, sizeof(GpuJoinInfo));
	gj_info.outer_ratio = 1.0;
	gj_info.outer_nrows = outer_plan->plan_rows;
	gj_info.outer_width = outer_plan->plan_width;
	gj_info.outer_startup_cost = outer_plan->startup_cost;
	gj_info.outer_total_cost = outer_plan->total_cost;
	gj_info.num_rels = gjpath->num_rels;

	outer_nrows = outer_plan->plan_rows;
	for (i=0; i < gjpath->num_rels; i++)
	{
		List	   *hash_inner_keys = NIL;
		List	   *hash_outer_keys = NIL;
		List	   *join_quals = NIL;
		List	   *other_quals = NIL;

		foreach (lc, gjpath->inners[i].hash_quals)
		{
			Path		   *scan_path = gjpath->inners[i].scan_path;
			RelOptInfo	   *scan_rel = scan_path->parent;
			RestrictInfo   *rinfo = lfirst(lc);
			OpExpr		   *op_clause = (OpExpr *) rinfo->clause;
			Relids			relids1;
			Relids			relids2;
			Node		   *arg1;
			Node		   *arg2;

			Assert(is_opclause(op_clause));
			arg1 = (Node *) linitial(op_clause->args);
			arg2 = (Node *) lsecond(op_clause->args);
			relids1 = pull_varnos(arg1);
			relids2 = pull_varnos(arg2);
			if (bms_is_subset(relids1, scan_rel->relids) &&
				!bms_is_subset(relids2, scan_rel->relids))
			{
				hash_inner_keys = lappend(hash_inner_keys, arg1);
				hash_outer_keys = lappend(hash_outer_keys, arg2);
			}
			else if (bms_is_subset(relids2, scan_rel->relids) &&
					 !bms_is_subset(relids1, scan_rel->relids))
			{
				hash_inner_keys = lappend(hash_inner_keys, arg2);
				hash_outer_keys = lappend(hash_outer_keys, arg1);
			}
			else
				elog(ERROR, "Bug? hash-clause reference bogus varnos");
		}

		/*
		 * Add properties of GpuJoinInfo
		 */
		gj_info.plan_nrows_in = lappend(gj_info.plan_nrows_in,
										pmakeFloat(outer_nrows));
		gj_info.plan_nrows_out = lappend(gj_info.plan_nrows_out,
									pmakeFloat(gjpath->inners[i].join_nrows));
		gj_info.ichunk_size = lappend_int(gj_info.ichunk_size,
										  gjpath->inners[i].ichunk_size);
		gj_info.join_types = lappend_int(gj_info.join_types,
										 gjpath->inners[i].join_type);

		if (IS_OUTER_JOIN(gjpath->inners[i].join_type))
		{
			extract_actual_join_clauses(gjpath->inners[i].join_quals,
										&join_quals, &other_quals);
		}
		else
		{
			join_quals = extract_actual_clauses(gjpath->inners[i].join_quals,
												false);
			other_quals = NIL;
		}
		gj_info.join_quals = lappend(gj_info.join_quals,
									 build_flatten_qualifier(join_quals));
		gj_info.other_quals = lappend(gj_info.other_quals,
									  build_flatten_qualifier(other_quals));
		gj_info.hash_inner_keys = lappend(gj_info.hash_inner_keys,
										  hash_inner_keys);
		gj_info.hash_outer_keys = lappend(gj_info.hash_outer_keys,
										  hash_outer_keys);
		outer_nrows = gjpath->inners[i].join_nrows;
	}

	/*
	 * If outer-plan node is simple relation scan; SeqScan or GpuScan with
	 * device executable qualifiers, GpuJoin can handle the relation scan
	 * for better i/o performance. Elsewhere, call the child outer node.
	 */
	if (gjpath->outer_relid)
	{
		cscan->scan.scanrelid = gjpath->outer_relid;
		gj_info.outer_quals = gjpath->outer_quals;
	}
	else
	{
		outerPlan(cscan) = outer_plan;
	}
	gj_info.outer_nrows_per_block = gjpath->outer_nrows_per_block;

	/*
	 * Build a tentative pseudo-scan targetlist. At this point, we cannot
	 * know which expression shall be applied on the final results, thus,
	 * all we can construct is a pseudo-scan targetlist that is consists
	 * of Var-nodes only.
	 */
	build_device_targetlist(gjpath, cscan, &gj_info, tlist, custom_plans);

	/*
	 * construct kernel code
	 */
	pgstrom_init_codegen_context(&context);
	kern_source = gpujoin_codegen(root, cscan, &gj_info, tlist, &context);
	if (context.func_defs || context.expr_defs)
	{
		StringInfoData	buf;

		initStringInfo(&buf);
		pgstrom_codegen_func_declarations(&buf, &context);
		pgstrom_codegen_expr_declarations(&buf, &context);
		appendStringInfo(&buf, "%s", kern_source);

		kern_source = buf.data;
	}
	gj_info.kern_source = kern_source;
	gj_info.extra_flags = (DEVKERNEL_NEEDS_GPUSCAN |
						   DEVKERNEL_NEEDS_GPUJOIN |
						   DEVKERNEL_NEEDS_DYNPARA |
						   context.extra_flags);
	gj_info.used_params = context.used_params;

	form_gpujoin_info(cscan, &gj_info);

	return &cscan->scan.plan;
}

typedef struct
{
	int		depth;
	List   *ps_src_depth;
	List   *ps_src_resno;
} fixup_varnode_to_origin_context;

static Node *
fixup_varnode_to_origin_mutator(Node *node,
								fixup_varnode_to_origin_context *context)
{
	if (!node)
		return NULL;
	if (IsA(node, Var))
	{
		Var	   *varnode = (Var *) node;
		int		varattno = varnode->varattno;
		int		src_depth;

		Assert(varnode->varno == INDEX_VAR);
		src_depth = list_nth_int(context->ps_src_depth,
								 varnode->varattno - 1);
		if (src_depth == context->depth)
		{
			Var	   *newnode = copyObject(varnode);

			newnode->varno = INNER_VAR;
			newnode->varattno = list_nth_int(context->ps_src_resno,
											 varattno - 1);
			return (Node *) newnode;
		}
		else if (src_depth > context->depth)
			elog(ERROR, "Expression reference deeper than current depth");
	}
	return expression_tree_mutator(node, fixup_varnode_to_origin_mutator,
								   (void *) context);
}

static List *
fixup_varnode_to_origin(int depth, List *ps_src_depth, List *ps_src_resno,
						List *expr_list)
{
	fixup_varnode_to_origin_context	context;

	Assert(IsA(expr_list, List));
	context.depth = depth;
	context.ps_src_depth = ps_src_depth;
	context.ps_src_resno = ps_src_resno;

	return (List *) fixup_varnode_to_origin_mutator((Node *) expr_list,
													&context);
}

/*
 * assign_gpujoin_session_info
 *
 * Gives some definitions to the static portion of GpuJoin implementation
 */
void
assign_gpujoin_session_info(StringInfo buf, GpuTaskState *gts)
{
	TupleTableSlot *slot = gts->css.ss.ss_ScanTupleSlot;
	TupleDesc		tupdesc = slot->tts_tupleDescriptor;

	Assert(gts->css.methods == &gpujoin_exec_methods);
	appendStringInfo(
		buf,
		"#define GPUJOIN_DEVICE_PROJECTION_NFIELDS %u\n"
		"#define GPUJOIN_DEVICE_PROJECTION_EXTRA_SIZE %u\n",
		tupdesc->natts,
		((GpuJoinState *) gts)->extra_maxlen);
}

static Node *
gpujoin_create_scan_state(CustomScan *node)
{
	GpuJoinState   *gjs;
	GpuJoinInfo	   *gj_info = deform_gpujoin_info(node);
	cl_int			num_rels = gj_info->num_rels;

	Assert(num_rels == list_length(node->custom_plans));
	gjs = palloc0(offsetof(GpuJoinState, inners[num_rels]));

	NodeSetTag(gjs, T_CustomScanState);
	gjs->gts.css.flags = node->flags;
	gjs->gts.css.methods = &gpujoin_exec_methods;

	return (Node *) gjs;
}

static void
ExecInitGpuJoin(CustomScanState *node, EState *estate, int eflags)
{
	GpuContext	   *gcontext = NULL;
	GpuJoinState   *gjs = (GpuJoinState *) node;
	ScanState	   *ss = &gjs->gts.css.ss;
	CustomScan	   *cscan = (CustomScan *) node->ss.ps.plan;
	GpuJoinInfo	   *gj_info = deform_gpujoin_info(cscan);
	TupleDesc		result_tupdesc = GTS_GET_RESULT_TUPDESC(gjs);
	TupleDesc		scan_tupdesc;
	TupleDesc		junk_tupdesc;
	List		   *tlist_fallback = NIL;
	bool			fallback_needs_projection = false;
	bool			fallback_meets_resjunk = false;
	ListCell	   *lc1;
	ListCell	   *lc2;
	cl_int			i, j, nattrs;
	cl_int			first_right_outer_depth = -1;
	StringInfoData	kern_define;
	ProgramId		program_id;
	bool			with_connection = ((eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0);

	/* activate a GpuContext for CUDA kernel execution */
	gcontext = AllocGpuContext(with_connection);

	/*
	 * Re-initialization of scan tuple-descriptor and projection-info,
	 * because commit 1a8a4e5cde2b7755e11bde2ea7897bd650622d3e of
	 * PostgreSQL makes to assign result of ExecTypeFromTL() instead
	 * of ExecCleanTypeFromTL; that leads unnecessary projection.
	 * So, we try to remove junk attributes from the scan-descriptor.
	 *
	 * Also note that the supplied TupleDesc that contains junk attributes
	 * are still useful to run CPU fallback code. So, we keep this tuple-
	 * descriptor to initialize the related stuff.
	 */
	junk_tupdesc = gjs->gts.css.ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	scan_tupdesc = ExecCleanTypeFromTL(cscan->custom_scan_tlist, false);
	ExecAssignScanType(&gjs->gts.css.ss, scan_tupdesc);
	ExecAssignScanProjectionInfoWithVarno(&gjs->gts.css.ss, INDEX_VAR);

	/* Setup common GpuTaskState fields */
	pgstromInitGpuTaskState(&gjs->gts,
							gcontext,
							GpuTaskKind_GpuJoin,
							gj_info->used_params,
							estate);
	gjs->gts.cb_next_task	= gpujoin_next_task;
	gjs->gts.cb_next_tuple	= gpujoin_next_tuple;
	gjs->gts.cb_ready_task	= gpujoin_ready_task;
	gjs->gts.cb_switch_task	= gpujoin_switch_task;
	if (pgstrom_bulkexec_enabled &&
		gjs->gts.css.ss.ps.qual == NIL &&
		gjs->gts.css.ss.ps.ps_ProjInfo == NULL)
		gjs->gts.cb_bulk_exec = pgstromBulkExecGpuTaskState;
	gjs->gts.outer_nrows_per_block = gj_info->outer_nrows_per_block;

	/*
	 * NOTE: outer_quals, hash_outer_keys and join_quals are intended
	 * to use fallback routine if GPU kernel required host-side to
	 * retry a series of hash-join/nest-loop operation. So, we need to
	 * pay attention which slot is actually referenced.
	 * Right now, ExecEvalScalarVar can reference only three slots
	 * simultaneously (scan, inner and outer). So, varno of varnodes
	 * has to be initialized according to depth of the expression.
	 *
	 * TODO: we have to initialize above expressions carefully for
	 * CPU fallback implementation.
	 */
	gjs->num_rels = gj_info->num_rels;
	gjs->join_types = gj_info->join_types;
	gjs->outer_quals = NIL;
	foreach (lc1, gj_info->outer_quals)
	{
		ExprState  *expr_state = ExecInitExpr(lfirst(lc1), &ss->ps);
		gjs->outer_quals = lappend(gjs->outer_quals, expr_state);
	}
	gjs->outer_ratio = gj_info->outer_ratio;
	gjs->outer_nrows = gj_info->outer_nrows;
	gjs->gts.css.ss.ps.qual = (List *)
		ExecInitExpr((Expr *)cscan->scan.plan.qual, &ss->ps);

	/*
	 * Init OUTER child node
	 */
	if (gjs->gts.css.ss.ss_currentRelation)
	{
		nattrs = RelationGetDescr(gjs->gts.css.ss.ss_currentRelation)->natts;
	}
	else
	{
		TupleTableSlot *outer_slot;

		outerPlanState(gjs) = ExecInitNode(outerPlan(cscan), estate, eflags);
		outer_slot = outerPlanState(gjs)->ps_ResultTupleSlot;
		nattrs = outer_slot->tts_tupleDescriptor->natts;
	}

	/*
	 * Init CPU fallback stuff
	 */
	foreach (lc1, cscan->custom_scan_tlist)
	{
		TargetEntry	   *tle = lfirst(lc1);
		Var			   *var;

		/*
		 * NOTE: Var node inside of general expression shall reference
		 * the custom_scan_tlist recursively. Thus, we don't need to
		 * care about varno/varattno fixup here.
		 */
		Assert(IsA(tle, TargetEntry));

		/*
		 * Because ss_ScanTupleSlot does not contain junk attribute,
		 * we have to remove junk attribute by projection, if any of
		 * target-entry in custom_scan_tlist (that is tuple format to
		 * be constructed by CPU fallback) are junk.
		 */
		if (tle->resjunk)
		{
			fallback_needs_projection = true;
			fallback_meets_resjunk = true;
		}
		else
		{
			/* no valid attribute after junk attribute */
			if (fallback_meets_resjunk)
				elog(ERROR, "Bug? a valid attribute appear after junk ones");

			Assert(!fallback_meets_resjunk);

			if (IsA(tle->expr, Var))
			{
				tle = copyObject(tle);
				var = (Var *) tle->expr;
				var->varnoold	= var->varno;
				var->varoattno	= var->varattno;
				var->varno		= INDEX_VAR;
				var->varattno	= tle->resno;
			}
			else
			{
				/* also, non-simple Var node needs projection */
				fallback_needs_projection = true;
			}
			tlist_fallback = lappend(tlist_fallback,
									 ExecInitExpr((Expr *) tle, &ss->ps));
		}
	}

	if (fallback_needs_projection)
	{
		gjs->slot_fallback = MakeSingleTupleTableSlot(junk_tupdesc);
		gjs->proj_fallback = ExecBuildProjectionInfo(tlist_fallback,
													 ss->ps.ps_ExprContext,
													 ss->ss_ScanTupleSlot,
													 junk_tupdesc);
	}
	else
	{
		gjs->slot_fallback = ss->ss_ScanTupleSlot;
		gjs->proj_fallback = NULL;
	}

	gjs->outer_src_anum_min = nattrs;
	gjs->outer_src_anum_max = FirstLowInvalidHeapAttributeNumber;
	nattrs -= FirstLowInvalidHeapAttributeNumber;
	gjs->outer_dst_resno = palloc0(sizeof(AttrNumber) * nattrs);
	j = 1;
	forboth (lc1, gj_info->ps_src_depth,
			 lc2, gj_info->ps_src_resno)
	{
		int		depth = lfirst_int(lc1);
		int		resno = lfirst_int(lc2);

		if (depth == 0)
		{
			if (gjs->outer_src_anum_min > resno)
				gjs->outer_src_anum_min = resno;
			if (gjs->outer_src_anum_max < resno)
				gjs->outer_src_anum_max = resno;
			resno -= FirstLowInvalidHeapAttributeNumber;
			Assert(resno > 0 && resno <= nattrs);
			gjs->outer_dst_resno[resno - 1] = j;
		}
		j++;
	}

	/*
	 * Init INNER child nodes for each depth
	 */
	for (i=0; i < gj_info->num_rels; i++)
	{
		Plan	   *inner_plan = list_nth(cscan->custom_plans, i);
		innerState *istate = &gjs->inners[i];
		Expr	   *join_quals;
		Expr	   *other_quals;
		List	   *hash_inner_keys;
		List	   *hash_outer_keys;
		TupleTableSlot *inner_slot;
		double		plan_nrows_in;
		double		plan_nrows_out;
		bool		be_row_format = false;

		/* row-format is preferable if plan is self-managed one */
		if (pgstrom_plan_is_gpuscan(inner_plan) ||
			pgstrom_plan_is_gpujoin(inner_plan))
			be_row_format = true;
		istate->state = ExecInitNode(inner_plan, estate, eflags);
		if (be_row_format)
			((GpuTaskState *)istate->state)->row_format = true;
		istate->econtext = CreateExprContext(estate);
		istate->depth = i + 1;
		plan_nrows_in = floatVal(list_nth(gj_info->plan_nrows_in, i));
		plan_nrows_out = floatVal(list_nth(gj_info->plan_nrows_out, i));
		istate->nrows_ratio = plan_nrows_out / Max(plan_nrows_in, 1.0);
		istate->ichunk_size = list_nth_int(gj_info->ichunk_size, i);
		istate->join_type = (JoinType)list_nth_int(gj_info->join_types, i);

		if (first_right_outer_depth < 0 &&
			(istate->join_type == JOIN_RIGHT ||
			 istate->join_type == JOIN_FULL))
			first_right_outer_depth = istate->depth;

		/*
		 * NOTE: We need to deal with Var-node references carefully,
		 * because varno/varattno pair depends on the context when
		 * ExecQual() is called.
		 * - join_quals and hash_outer_keys are only called for
		 * fallback process when CpuReCheck error was returned.
		 * So, we can expect values are stored in ecxt_scantuple
		 * according to the pseudo-scan-tlist.
		 *- hash_inner_keys are only called to construct hash-table
		 * prior to GPU execution, so, we can expect input values
		 * are deployed according to the result of child plans.
		 */
		join_quals = list_nth(gj_info->join_quals, i);
		if (!join_quals)
			istate->join_quals = NIL;
		else
		{
			ExprState  *expr_state = ExecInitExpr(join_quals, &ss->ps);
			istate->join_quals = list_make1(expr_state);
		}

		other_quals = list_nth(gj_info->other_quals, i);
		if (!other_quals)
			istate->other_quals = NIL;
		else
		{
			ExprState  *expr_state = ExecInitExpr(other_quals, &ss->ps);
			istate->other_quals = list_make1(expr_state);
		}

		hash_inner_keys = list_nth(gj_info->hash_inner_keys, i);
		if (hash_inner_keys != NIL)
		{
			hash_inner_keys = fixup_varnode_to_origin(i+1,
													  gj_info->ps_src_depth,
													  gj_info->ps_src_resno,
													  hash_inner_keys);
			foreach (lc1, hash_inner_keys)
			{
				Expr	   *expr = lfirst(lc1);
				ExprState  *expr_state = ExecInitExpr(expr, &ss->ps);
				Oid			type_oid = exprType((Node *)expr);
				int16		typlen;
				bool		typbyval;

				istate->hash_inner_keys =
					lappend(istate->hash_inner_keys, expr_state);

				get_typlenbyval(type_oid, &typlen, &typbyval);
				istate->hash_keytype =
					lappend_oid(istate->hash_keytype, type_oid);
				istate->hash_keylen =
					lappend_int(istate->hash_keylen, typlen);
				istate->hash_keybyval =
					lappend_int(istate->hash_keybyval, typbyval);
			}
			/* outer keys also */
			hash_outer_keys = list_nth(gj_info->hash_outer_keys, i);
			Assert(hash_outer_keys != NIL);
			istate->hash_outer_keys = (List *)
				ExecInitExpr((Expr *)hash_outer_keys, &ss->ps);

			Assert(IsA(istate->hash_outer_keys, List) &&
				   list_length(istate->hash_inner_keys) ==
				   list_length(istate->hash_outer_keys));
		}

		/*
		 * CPU fallback setup for INNER reference
		 */
		inner_slot = istate->state->ps_ResultTupleSlot;
		nattrs = inner_slot->tts_tupleDescriptor->natts;
		istate->inner_src_anum_min = nattrs;
		istate->inner_src_anum_max = FirstLowInvalidHeapAttributeNumber;
		nattrs -= FirstLowInvalidHeapAttributeNumber;
		istate->inner_dst_resno = palloc0(sizeof(AttrNumber) * nattrs);

		j = 1;
		forboth (lc1, gj_info->ps_src_depth,
				 lc2, gj_info->ps_src_resno)
		{
			int		depth = lfirst_int(lc1);
			int		resno = lfirst_int(lc2);

			if (depth == istate->depth)
			{
				if (istate->inner_src_anum_min > resno)
					istate->inner_src_anum_min = resno;
				if (istate->inner_src_anum_max < resno)
					istate->inner_src_anum_max = resno;
				resno -= FirstLowInvalidHeapAttributeNumber;
				Assert(resno > 0 && resno <= nattrs);
				istate->inner_dst_resno[resno - 1] = j;
			}
			j++;
		}
		/* add inner state as children of this custom-scan */
		gjs->gts.css.custom_ps = lappend(gjs->gts.css.custom_ps,
										 istate->state);
	}

	/*
	 * Track the first RIGHT/FULL OUTER JOIN depth, if any
	 */
	gjs->first_right_outer_depth = Min(first_right_outer_depth,
									   gjs->num_rels + 1);

	/*
	 * Construct CUDA program, and kick asynchronous compile process.
	 * Note that assign_gpujoin_session_info() is called back from
	 * the pgstrom_assign_cuda_program(), thus, gjs->extra_maxlen has
	 * to be set prior to the program assignment.
	 */
	gjs->extra_maxlen = gj_info->extra_maxlen;

	initStringInfo(&kern_define);
	pgstrom_build_session_info(&kern_define,
							   &gjs->gts,
							   gj_info->extra_flags);
	program_id = pgstrom_create_cuda_program(gcontext,
											 gj_info->extra_flags,
											 gj_info->kern_source,
											 kern_define.data,
											 false);
	gjs->gts.program_id = program_id;
	pfree(kern_define.data);

	/* expected kresults buffer expand rate */
	gjs->result_width =
		MAXALIGN(offsetof(HeapTupleHeaderData,
						  t_bits[BITMAPLEN(result_tupdesc->natts)]) +
				 (result_tupdesc->tdhasoid ? sizeof(Oid) : 0)) +
		MAXALIGN(cscan->scan.plan.plan_width);	/* average width */

	/*
	 * Template of kds_dst_head to be copied to GpuJoin tasks
	 */
	gjs->kds_dst_head = palloc(offsetof(kern_data_store,
										colmeta[scan_tupdesc->natts]));
	init_kernel_data_store(gjs->kds_dst_head,
						   scan_tupdesc,
						   pgstrom_chunk_size(),
						   KDS_FORMAT_ROW,
						   INT_MAX);
}

/*
 * ExecReCheckGpuJoin
 *
 * Routine of EPQ recheck on GpuJoin. Join condition shall be checked on
 * the EPQ tuples.
 */
static bool
ExecReCheckGpuJoin(CustomScanState *node, TupleTableSlot *slot)
{
	/*
	 * TODO: Extract EPQ tuples on CPU fallback slot, then check
	 * join condition by CPU
	 */
	return true;
}

/*
 * ExecGpuJoin
 */
static TupleTableSlot *
ExecGpuJoin(CustomScanState *node)
{
	GpuJoinState   *gjs = (GpuJoinState *) node;

	if (!gjs->gj_sstate)
		gjs->gj_sstate = createGpuJoinSharedState(gjs);
	return ExecScan(&node->ss,
					(ExecScanAccessMtd) pgstromExecGpuTaskState,
					(ExecScanRecheckMtd) ExecReCheckGpuJoin);
}

static void
ExecEndGpuJoin(CustomScanState *node)
{
	GpuJoinState   *gjs = (GpuJoinState *) node;
	int				i;

	/* wait for completion of any asynchronous GpuTask */
	SynchronizeGpuContext(gjs->gts.gcontext);

	/* clean up inner hash/heap buffer */
	if (gjs->gj_sstate)
		releaseGpuJoinSharedState(gjs, false);

	/* shutdown inner/outer subtree */
	ExecEndNode(outerPlanState(node));
	for (i=0; i < gjs->num_rels; i++)
	{
		ExecEndNode(gjs->inners[i].state);
		if (gjs->inners[i].pds_in)
			PDS_release(gjs->inners[i].pds_in);
	}
	/* then other generic resources */
	pgstromReleaseGpuTaskState(&gjs->gts);
}

static void
ExecReScanGpuJoin(CustomScanState *node)
{
	GpuJoinState   *gjs = (GpuJoinState *) node;
	cl_int			i;

	/* wait for completion of any asynchronous GpuTask */
	SynchronizeGpuContext(gjs->gts.gcontext);
	/* rewind the outer relation */
	if (gjs->gts.css.ss.ss_currentRelation)
		gpuscanRewindScanChunk(&gjs->gts);
	else
		ExecReScan(outerPlanState(gjs));
	gjs->gts.scan_overflow = NULL;

	/*
	 * NOTE: ExecReScan() does not pay attention on the PlanState within
	 * custom_ps, so we need to assign its chgParam by ourself.
	 */
	if (gjs->gts.css.ss.ps.chgParam != NULL)
	{
		for (i=0; i < gjs->num_rels; i++)
		{
			innerState *istate = &gjs->inners[i];

			UpdateChangedParamSet(gjs->inners[i].state,
								  gjs->gts.css.ss.ps.chgParam);
			if (istate->state->chgParam != NULL)
			{
				if (istate->pds_in)
					PDS_release(istate->pds_in);
				istate->pds_in = NULL;
				ExecReScan(istate->state);
			}
		}
	}
	/* common rescan handling */
	pgstromRescanGpuTaskState(&gjs->gts);
	/* rewind the inner hash/heap buffer */
	if (gjs->gj_sstate)
		releaseGpuJoinSharedState(gjs, true);
}

static void
ExplainGpuJoin(CustomScanState *node, List *ancestors, ExplainState *es)
{
	GpuJoinState   *gjs = (GpuJoinState *) node;
	CustomScan	   *cscan = (CustomScan *) node->ss.ps.plan;
	GpuJoinInfo	   *gj_info = deform_gpujoin_info(cscan);
    GpuJoinSharedState *gj_sstate = gjs->gj_sstate;
	List		   *dcontext;
	ListCell	   *lc1;
	ListCell	   *lc2;
	ListCell	   *lc3;
	ListCell	   *lc4;
	char		   *temp;
	char			qlabel[128];
	int				depth;
	StringInfoData	str;

	initStringInfo(&str);
	/* deparse context */
	dcontext =  set_deparse_context_planstate(es->deparse_cxt,
											  (Node *) node,
											  ancestors);
	/* Device projection */
	resetStringInfo(&str);
	foreach (lc1, cscan->custom_scan_tlist)
	{
		TargetEntry	   *tle = lfirst(lc1);

#if 1
		/* disable this code block, if junk TLE is noisy */
		if (tle->resjunk)
			continue;
#endif
		if (lc1 != list_head(cscan->custom_scan_tlist))
			appendStringInfo(&str, ", ");
		if (tle->resjunk)
			appendStringInfoChar(&str, '[');
		temp = deparse_expression((Node *)tle->expr, dcontext, true, false);
		appendStringInfo(&str, "%s", temp);
		if (es->verbose)
		{
			temp = format_type_with_typemod(exprType((Node *)tle->expr),
											exprTypmod((Node *)tle->expr));
			appendStringInfo(&str, "::%s", temp);
		}
		if (tle->resjunk)
			appendStringInfoChar(&str, ']');
	}
	ExplainPropertyText("GPU Projection", str.data, es);

	/* statistics for outer scan, if it was pulled-up */
	if (es->analyze)
	{
		gjs->gts.outer_instrument.tuplecount = (gj_sstate->inner_nitems[0] +
												gj_sstate->right_nitems[0]);
		gjs->gts.outer_instrument.nfiltered1 = (gj_sstate->source_nitems -
												gj_sstate->inner_nitems[0] -
												gj_sstate->right_nitems[0]);
	}
	pgstromExplainOuterScan(&gjs->gts, dcontext, ancestors, es,
							gj_info->outer_quals,
                            gj_info->outer_startup_cost,
                            gj_info->outer_total_cost,
                            gj_info->outer_nrows,
                            gj_info->outer_width);
	/* join-qualifiers */
	depth = 1;
	forfour (lc1, gj_info->join_types,
			 lc2, gj_info->join_quals,
			 lc3, gj_info->other_quals,
			 lc4, gj_info->hash_outer_keys)
	{
		JoinType	join_type = (JoinType) lfirst_int(lc1);
		Expr	   *join_quals = lfirst(lc2);
		Expr	   *other_quals = lfirst(lc3);
		Expr	   *hash_outer_key = lfirst(lc4);
		innerState *istate = &gjs->inners[depth-1];
		int			indent_width;
		double		plan_nrows_in;
		double		plan_nrows_out;
		double		exec_nrows_in = 0.0;
		double		exec_nrows_out1 = 0.0;	/* by INNER JOIN */
		double		exec_nrows_out2 = 0.0;	/* by OUTER JOIN */

		/* fetch number of rows */
		plan_nrows_in = floatVal(list_nth(gj_info->plan_nrows_in, depth-1));
		plan_nrows_out = floatVal(list_nth(gj_info->plan_nrows_out, depth-1));
		if (es->analyze)
		{
			exec_nrows_in = (gj_sstate->inner_nitems[depth - 1] +
							 gj_sstate->right_nitems[depth - 1]);
			exec_nrows_out1 = gj_sstate->inner_nitems[depth];
			exec_nrows_out2 = gj_sstate->right_nitems[depth];
		}

		resetStringInfo(&str);
		if (hash_outer_key != NULL)
		{
			appendStringInfo(&str, "GpuHash%sJoin",
							 join_type == JOIN_FULL ? "Full" :
							 join_type == JOIN_LEFT ? "Left" :
							 join_type == JOIN_RIGHT ? "Right" : "");
		}
		else
		{
			appendStringInfo(&str, "GpuNestLoop%s",
							 join_type == JOIN_FULL ? "Full" :
							 join_type == JOIN_LEFT ? "Left" :
							 join_type == JOIN_RIGHT ? "Right" : "");
		}
		snprintf(qlabel, sizeof(qlabel), "Depth% 2d", depth);
		indent_width = es->indent * 2 + strlen(qlabel) + 2;

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			if (!es->analyze)
				appendStringInfo(&str, "  (nrows %.0f...%.0f)",
								 plan_nrows_in,
								 plan_nrows_out);
			else if (exec_nrows_out2 > 0.0)
				appendStringInfo(&str, "  (plan nrows: %.0f...%.0f,"
								 " actual nrows: %.0f...%.0f+%.0f)",
								 plan_nrows_in,
								 plan_nrows_out,
								 exec_nrows_in,
								 exec_nrows_out1,
								 exec_nrows_out2);
			else
				appendStringInfo(&str, "  (plan nrows: %.0f...%.0f,"
								 " actual nrows: %.0f...%.0f)",
								 plan_nrows_in,
								 plan_nrows_out,
								 exec_nrows_in,
								 exec_nrows_out1);
			ExplainPropertyText(qlabel, str.data, es);
		}
		else
		{
			ExplainPropertyText(qlabel, str.data, es);

			snprintf(qlabel, sizeof(qlabel),
					 "Depth% 2d Plan Rows-in", depth);
			ExplainPropertyFloat(qlabel, plan_nrows_in, 0, es);

			snprintf(qlabel, sizeof(qlabel),
					 "Depth% 2d Plan Rows-out", depth);
			ExplainPropertyFloat(qlabel, plan_nrows_out, 0, es);

			if (es->analyze)
			{
				snprintf(qlabel, sizeof(qlabel),
						 "Depth% 2d Actual Rows-in", depth);
				ExplainPropertyFloat(qlabel, exec_nrows_in, 0, es);

				snprintf(qlabel, sizeof(qlabel),
                         "Depth% 2d Actual Rows-out by inner join", depth);
				ExplainPropertyFloat(qlabel, exec_nrows_out1, 0, es);

				snprintf(qlabel, sizeof(qlabel),
						 "Depth% 2d Actual Rows-out by outer join", depth);
				ExplainPropertyFloat(qlabel, exec_nrows_out2, 0, es);
			}
		}

		/*
		 * HashJoinKeys, if any
		 */
		if (hash_outer_key)
		{
			temp = deparse_expression((Node *)hash_outer_key,
                                      dcontext, true, false);
			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				appendStringInfoSpaces(es->str, indent_width);
				appendStringInfo(es->str, "HashKeys: %s\n", temp);
			}
			else
			{
				snprintf(qlabel, sizeof(qlabel),
						 "Depth% 2d HashKeys", depth);
				ExplainPropertyText(qlabel, temp, es);
			}
		}

		/*
		 * JoinQuals, if any
		 */
		if (join_quals)
		{
			temp = deparse_expression((Node *)join_quals, dcontext,
									  true, false);
			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				appendStringInfoSpaces(es->str, indent_width);
				appendStringInfo(es->str, "JoinQuals: %s\n", temp);
			}
			else
			{
				snprintf(qlabel, sizeof(qlabel),
						 "Depth% 2d JoinQuals", depth);
				ExplainPropertyText(qlabel, temp, es);
			}
		}

		/*
		 * OtherQuals, if any
		 */
		if (other_quals)
		{
			temp = deparse_expression((Node *)other_quals, dcontext,
									  es->verbose, false);
			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				appendStringInfoSpaces(es->str, indent_width);
				appendStringInfo(es->str, "JoinFilter: %s\n", temp);
			}
			else
			{
				snprintf(qlabel, sizeof(qlabel), "Depth %02d-Filter", depth);
				ExplainPropertyText(qlabel, str.data, es);
			}
		}

		/*
		 * Inner KDS statistics
		 */
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			appendStringInfoSpaces(es->str, indent_width);
			if (!es->analyze)
			{
				appendStringInfo(es->str, "KDS-%s (size: %s)",
								 hash_outer_key ? "Hash" : "Heap",
								 format_bytesz(istate->ichunk_size));
			}
			else
			{
				appendStringInfo(es->str, "KDS-%s (size plan: %s, exec: %s)",
								 hash_outer_key ? "Hash" : "Heap",
								 format_bytesz(istate->ichunk_size),
								 format_bytesz(istate->pds_in->kds.length));
			}
			appendStringInfoChar(es->str, '\n');
		}
		else
		{
			Size		len;

			snprintf(qlabel, sizeof(qlabel), "Depth %02d KDS Type", depth);
			ExplainPropertyText(qlabel, hash_outer_key ? "Hash" : "Heap", es);

			snprintf(qlabel, sizeof(qlabel),
					 "Depth % 2d KDS Plan Size", depth);
			len = istate->ichunk_size;
			ExplainPropertyText(qlabel, format_bytesz(len), es);
			if (es->analyze)
			{
				snprintf(qlabel, sizeof(qlabel),
						 "Depth % 2d KDS Exec Size", depth);
				len = istate->pds_in->kds.length;
				ExplainPropertyText(qlabel, format_bytesz(len), es);
			}
		}
		depth++;
	}
	/* other common field */
	pgstromExplainGpuTaskState(&gjs->gts, es);
}

/*
 * ExecGpuJoinEstimateDSM
 */
static Size
ExecGpuJoinEstimateDSM(CustomScanState *node,
					   ParallelContext *pcxt)
{
	return offsetof(GpuJoinParallelDSM, data[0])
		+ ExecGpuScanEstimateDSM(node, pcxt);
}

/*
 * ExecGpuJoinInitDSM
 */
static void
ExecGpuJoinInitDSM(CustomScanState *node,
				   ParallelContext *pcxt,
				   void *coordinate)
{
	GpuJoinState   *gjs = (GpuJoinState *) node;
	GpuJoinParallelDSM *gjpdsm = (GpuJoinParallelDSM *) coordinate;

	/* save ParallelContext */
	gjs->gts.pcxt = pcxt;

	/* allocation of an empty multirel buffer */
	gjs->gj_sstate = createGpuJoinSharedState(gjs);
	gjpdsm->gj_sstate = gjs->gj_sstate;

	ExecGpuScanInitDSM(node, pcxt, gjpdsm->data);
}

/*
 * ExecGpuJoinInitWorker
 */
static void
ExecGpuJoinInitWorker(CustomScanState *node,
					  shm_toc *toc,
					  void *coordinate)
{
	GpuJoinState	   *gjs = (GpuJoinState *) node;
	GpuJoinParallelDSM *gjpdsm = (GpuJoinParallelDSM *) coordinate;

	gjs->gj_sstate = gjpdsm->gj_sstate;
	ExecGpuScanInitWorker(node, toc, gjpdsm->data);
}

/*
 * gpujoin_codegen_var_decl
 *
 * declaration of the variables in 'used_var' list
 */
static void
gpujoin_codegen_var_param_decl(StringInfo source,
							   GpuJoinInfo *gj_info,
							   int cur_depth,
							   codegen_context *context)
{
	List	   *kern_vars = NIL;
	ListCell   *cell;
	int			depth;

	Assert(cur_depth > 0 && cur_depth <= gj_info->num_rels);

	/*
	 * Pick up variables in-use and append its properties in the order
	 * corresponding to depth/resno.
	 */
	foreach (cell, context->used_vars)
	{
		Var		   *varnode = lfirst(cell);
		Var		   *kernode = NULL;
		ListCell   *lc1;
		ListCell   *lc2;
		ListCell   *lc3;

		Assert(IsA(varnode, Var));
		forthree (lc1, context->pseudo_tlist,
				  lc2, gj_info->ps_src_depth,
				  lc3, gj_info->ps_src_resno)
		{
			TargetEntry	*tle = lfirst(lc1);
			int		src_depth = lfirst_int(lc2);
			int		src_resno = lfirst_int(lc3);

			if (equal(tle->expr, varnode))
			{
				kernode = copyObject(varnode);
				kernode->varno = src_depth;			/* save the source depth */
				kernode->varattno = src_resno;		/* save the source resno */
				kernode->varoattno = tle->resno;	/* resno on the ps_tlist */
				if (src_depth < 0 || src_depth > cur_depth)
					elog(ERROR, "Bug? device varnode out of range");
				break;
			}
		}
		if (!kernode)
			elog(ERROR, "Bug? device varnode was not is ps_tlist: %s",
				 nodeToString(varnode));

		/*
		 * attach 'kernode' in the order corresponding to depth/resno.
		 */
		if (kern_vars == NIL)
			kern_vars = list_make1(kernode);
		else
		{
			lc2 = NULL;
			foreach (lc1, kern_vars)
			{
				Var	   *varnode = lfirst(lc1);

				if (varnode->varno > kernode->varno ||
					(varnode->varno == kernode->varno &&
					 varnode->varattno > kernode->varattno))
				{
					if (lc2 != NULL)
						lappend_cell(kern_vars, lc2, kernode);
					else
						kern_vars = lcons(kernode, kern_vars);
					break;
				}
				lc2 = lc1;
			}
			if (lc1 == NULL)
				kern_vars = lappend(kern_vars, kernode);
		}
	}

	/*
	 * parameter declaration
	 */
	pgstrom_codegen_param_declarations(source, context);

	/*
	 * variable declarations
	 */
	appendStringInfoString(
		source,
		"  HeapTupleHeaderData *htup  __attribute__((unused));\n"
		"  kern_data_store *kds_in    __attribute__((unused));\n"
		"  kern_colmeta *colmeta      __attribute__((unused));\n"
		"  void *datum                __attribute__((unused));\n");

	foreach (cell, kern_vars)
	{
		Var			   *kernode = lfirst(cell);
		devtype_info   *dtype;

		dtype = pgstrom_devtype_lookup(kernode->vartype);
		if (!dtype)
			elog(ERROR, "device type \"%s\" not found",
				 format_type_be(kernode->vartype));

		appendStringInfo(
			source,
			"  pg_%s_t KVAR_%u;\n",
			dtype->type_name,
			kernode->varoattno);
	}

	/*
	 * variable initialization
	 */
	depth = -1;
	foreach (cell, kern_vars)
	{
		Var			   *keynode = lfirst(cell);
		devtype_info   *dtype;

		dtype = pgstrom_devtype_lookup(keynode->vartype);
		if (!dtype)
			elog(ERROR, "device type \"%s\" not found",
				 format_type_be(keynode->vartype));

		if (depth != keynode->varno)
		{
			if (keynode->varno == 0)
			{
				/* htup from KDS */
				appendStringInfoString(
					source,
					"  /* variable load in depth-0 (outer KDS) */\n"
					"  colmeta = kds->colmeta;\n"
					"  if (!o_buffer)\n"
					"    htup = NULL;\n"
					"  else if (kds->format != KDS_FORMAT_BLOCK)\n"
					"    htup = KDS_ROW_REF_HTUP(kds,o_buffer[0],\n"
					"                            NULL,NULL);\n"
					"  else\n"
					"    htup = KDS_BLOCK_REF_HTUP(kds,o_buffer[0],\n"
					"                              NULL,NULL);\n");
			}
			else
			{
				/* in case of inner data store */
				appendStringInfo(
					source,
					"  /* variable load in depth-%u (data store) */\n"
					"  kds_in = KERN_MULTIRELS_INNER_KDS(kmrels, %u);\n"
					"  assert(kds_in->format == %s);\n"
					"  colmeta = kds_in->colmeta;\n",
					keynode->varno,
					keynode->varno,
					list_nth(gj_info->hash_outer_keys,
							 keynode->varno - 1) == NIL
					? "KDS_FORMAT_ROW"
					: "KDS_FORMAT_HASH");

				if (keynode->varno < cur_depth)
					appendStringInfo(
						source,
						"  if (!o_buffer)\n"
						"    htup = NULL;\n"
						"  else\n"
						"    htup = KDS_ROW_REF_HTUP(kds_in,o_buffer[%d],\n"
						"                            NULL, NULL);\n",
						keynode->varno);
				else if (keynode->varno == cur_depth)
					appendStringInfo(
						source,
						"  htup = i_htup;\n"
						);
				else
					elog(ERROR, "Bug? too deeper varnode reference");
			}
			depth = keynode->varno;
		}
		appendStringInfo(
			source,
			"  datum = GPUJOIN_REF_DATUM(colmeta,htup,%u);\n"
			"  KVAR_%u = pg_%s_datum_ref(kcxt,datum);\n",
			keynode->varattno - 1,
			keynode->varoattno,
			dtype->type_name);
	}
	appendStringInfo(source, "\n");
}

/*
 * codegen for:
 * STATIC_FUNCTION(cl_bool)
 * gpujoin_join_quals_depth%u(kern_context *kcxt,
 *                            kern_data_store *kds,
 *                            kern_multirels *kmrels,
 *                            cl_int *o_buffer,
 *                            HeapTupleHeaderData *i_htup,
 *                            cl_bool *joinquals_matched)
 */
static void
gpujoin_codegen_join_quals(StringInfo source,
						   GpuJoinInfo *gj_info,
						   int cur_depth,
						   codegen_context *context)
{
	List	   *join_quals;
	List	   *other_quals;
	char	   *join_quals_code = NULL;
	char	   *other_quals_code = NULL;

	Assert(cur_depth > 0 && cur_depth <= gj_info->num_rels);
	join_quals = list_nth(gj_info->join_quals, cur_depth - 1);
	other_quals = list_nth(gj_info->other_quals, cur_depth - 1);

	/*
	 * make a text representation of join_qual
	 */
	context->used_vars = NIL;
	context->param_refs = NULL;
	if (join_quals != NIL)
		join_quals_code = pgstrom_codegen_expression((Node *)join_quals,
													 context);
	if (other_quals != NIL)
		other_quals_code = pgstrom_codegen_expression((Node *)other_quals,
													  context);
	/*
	 * function declaration
	 */
	appendStringInfo(
		source,
		"STATIC_FUNCTION(cl_bool)\n"
		"gpujoin_join_quals_depth%d(kern_context *kcxt,\n"
		"                           kern_data_store *kds,\n"
        "                           kern_multirels *kmrels,\n"
		"                           cl_uint *o_buffer,\n"
		"                           HeapTupleHeaderData *i_htup,\n"
		"                           cl_bool *joinquals_matched)\n"
		"{\n",
		cur_depth);

	/*
	 * variable/params declaration & initialization
	 */
	gpujoin_codegen_var_param_decl(source, gj_info, cur_depth, context);

	/*
	 * evaluation of other-quals and join-quals
	 */
	if (join_quals_code != NULL)
	{
		appendStringInfo(
			source,
			"  if (i_htup && o_buffer && !EVAL(%s))\n"
			"  {\n"
			"    if (joinquals_matched)\n"
			"      *joinquals_matched = false;\n"
			"    return false;\n"
			"  }\n",
			join_quals_code);
	}
	appendStringInfo(
		source,
		"  if (joinquals_matched)\n"
		"    *joinquals_matched = true;\n");
	if (other_quals_code != NULL)
	{
		appendStringInfo(
			source,
			"  if (!EVAL(%s))\n"
			"    return false;\n",
			other_quals_code);
	}
	appendStringInfo(
		source,
		"  return true;\n"
		"}\n");
}

/*
 * codegen for:
 * STATIC_FUNCTION(cl_uint)
 * gpujoin_hash_value_depth%u(kern_context *kcxt,
 *                            cl_uint *pg_crc32_table,
 *                            kern_data_store *kds,
 *                            kern_multirels *kmrels,
 *                            cl_int *outer_index,
 *                            cl_bool *is_null_keys)
 */
static void
gpujoin_codegen_hash_value(StringInfo source,
						   GpuJoinInfo *gj_info,
						   int cur_depth,
						   codegen_context *context)
{
	StringInfoData	body;
	List		   *hash_outer_keys;
	ListCell	   *lc;

	Assert(cur_depth > 0 && cur_depth <= gj_info->num_rels);
	hash_outer_keys = list_nth(gj_info->hash_outer_keys, cur_depth - 1);
	Assert(hash_outer_keys != NIL);

	appendStringInfo(
		source,
		"STATIC_FUNCTION(cl_uint)\n"
		"gpujoin_hash_value_depth%u(kern_context *kcxt,\n"
		"                           cl_uint *pg_crc32_table,\n"
		"                           kern_data_store *kds,\n"
		"                           kern_multirels *kmrels,\n"
		"                           cl_uint *o_buffer,\n"
		"                           cl_bool *p_is_null_keys)\n"
		"{\n"
		"  pg_anytype_t temp    __attribute__((unused));\n"
		"  cl_uint hash;\n"
		"  cl_bool is_null_keys = true;\n"
		"\n",
		cur_depth);

	context->used_vars = NIL;
	context->param_refs = NULL;

	initStringInfo(&body);
	appendStringInfo(
		&body,
		"  /* Hash-value calculation */\n"
		"  INIT_LEGACY_CRC32(hash);\n");
	foreach (lc, hash_outer_keys)
	{
		Node	   *key_expr = lfirst(lc);
		Oid			key_type = exprType(key_expr);
		devtype_info *dtype;

		dtype = pgstrom_devtype_lookup(key_type);
		if (!dtype)
			elog(ERROR, "Bug? device type \"%s\" not found",
                 format_type_be(key_type));
		appendStringInfo(
			&body,
			"  temp.%s_v = %s;\n"
			"  if (!temp.%s_v.isnull)\n"
			"    is_null_keys = false;\n"
			"  hash = pg_%s_comp_crc32(pg_crc32_table, hash, temp.%s_v);\n",
			dtype->type_name,
			pgstrom_codegen_expression(key_expr, context),
			dtype->type_name,
			dtype->type_name,
			dtype->type_name);
	}
	appendStringInfo(&body, "  FIN_LEGACY_CRC32(hash);\n");

	/*
	 * variable/params declaration & initialization
	 */
	gpujoin_codegen_var_param_decl(source, gj_info, cur_depth, context);

	appendStringInfo(
		source,
		"%s"
		"\n"
		"  *p_is_null_keys = is_null_keys;\n"
		"  return hash;\n"
		"}\n"
		"\n",
		body.data);
	pfree(body.data);
}

/*
 * gpujoin_codegen_projection
 *
 * It makes a device function for device projection.
 */
static void
gpujoin_codegen_projection(StringInfo source,
						   CustomScan *cscan,
						   GpuJoinInfo *gj_info,
						   codegen_context *context,
						   cl_uint *p_extra_maxlen)
{
	List		   *tlist_dev = cscan->custom_scan_tlist;
	List		   *ps_src_depth = gj_info->ps_src_depth;
	List		   *ps_src_resno = gj_info->ps_src_resno;
	ListCell	   *lc1;
	ListCell	   *lc2;
	ListCell	   *lc3;
	AttrNumber	   *varattmaps;
	Bitmapset	   *refs_by_vars = NULL;
	Bitmapset	   *refs_by_expr = NULL;
	StringInfoData	body;
	StringInfoData	temp;
	cl_int			depth;
	cl_uint			extra_maxlen;
	cl_bool			is_first;

	varattmaps = palloc(sizeof(AttrNumber) * list_length(tlist_dev));
	initStringInfo(&body);
	initStringInfo(&temp);

	/*
	 * Pick up all the var-node referenced directly or indirectly by
	 * device expressions; which are resjunk==false.
	 */
	forthree (lc1, tlist_dev,
			  lc2, ps_src_depth,
			  lc3, ps_src_resno)
	{
		TargetEntry	*tle = lfirst(lc1);
		cl_int		src_depth = lfirst_int(lc2);

		if (tle->resjunk)
			continue;
		if (src_depth >= 0)
		{
			refs_by_vars = bms_add_member(refs_by_vars, tle->resno -
										  FirstLowInvalidHeapAttributeNumber);
		}
		else
		{
			List	   *expr_vars = pull_vars_of_level((Node *)tle->expr, 0);
			ListCell   *cell;

			foreach (cell, expr_vars)
			{
				TargetEntry	   *__tle = tlist_member(lfirst(cell), tlist_dev);

				if (!__tle)
					elog(ERROR, "Bug? no indirectly referenced Var-node exists in custom_scan_tlist");
				refs_by_expr = bms_add_member(refs_by_expr, __tle->resno -
										FirstLowInvalidHeapAttributeNumber);
			}
			list_free(expr_vars);
		}
	}

	appendStringInfoString(
		source,
		"STATIC_FUNCTION(void)\n"
		"gpujoin_projection(kern_context *kcxt,\n"
		"                   kern_data_store *kds_src,\n"
		"                   kern_multirels *kmrels,\n"
		"                   cl_uint *r_buffer,\n"
		"                   kern_data_store *kds_dst,\n"
		"                   Datum *tup_values,\n"
		"                   cl_bool *tup_isnull,\n"
		"                   cl_short *tup_depth,\n"
		"                   cl_char *extra_buf,\n"
		"                   cl_uint *extra_len)\n"
		"{\n"
		"  HeapTupleHeaderData *htup    __attribute__((unused));\n"
		"  kern_data_store *kds_in      __attribute__((unused));\n"
		"  ItemPointerData  t_self      __attribute__((unused));\n"
		"  char *addr                   __attribute__((unused));\n"
		"  char *extra_pos = extra_buf;\n"
		"  pg_anytype_t temp            __attribute__((unused));\n");

	for (depth=0; depth <= gj_info->num_rels; depth++)
	{
		List	   *kvars_srcnum = NIL;
		List	   *kvars_dstnum = NIL;
		const char *kds_label;
		cl_int		i, nattrs = -1;

		/* collect information in this depth */
		memset(varattmaps, 0, sizeof(AttrNumber) * list_length(tlist_dev));

		forthree (lc1, tlist_dev,
				  lc2, ps_src_depth,
				  lc3, ps_src_resno)
		{
			TargetEntry *tle = lfirst(lc1);
			cl_int		src_depth = lfirst_int(lc2);
			cl_int		src_resno = lfirst_int(lc3);
			cl_int		k = tle->resno - FirstLowInvalidHeapAttributeNumber;

			if (depth != src_depth)
				continue;
			if (bms_is_member(k, refs_by_vars))
				varattmaps[tle->resno - 1] = src_resno;
			if (bms_is_member(k, refs_by_expr))
			{
				kvars_srcnum = lappend_int(kvars_srcnum, src_resno);
				kvars_dstnum = lappend_int(kvars_dstnum, tle->resno);
			}
			if (bms_is_member(k, refs_by_vars) ||
				bms_is_member(k, refs_by_expr))
				nattrs = Max(nattrs, src_resno);
		}

		/* no need to extract inner/outer tuple in this depth */
		if (nattrs < 1)
			continue;

		appendStringInfo(
			&body,
			"  /* ---- extract %s relation (depth=%d) */\n",
			depth > 0 ? "inner" : "outer", depth);

		if (depth == 0)
			kds_label = "kds_src";
		else
		{
			appendStringInfo(
				&body,
				"  kds_in = KERN_MULTIRELS_INNER_KDS(kmrels, %d);\n",
				depth);
			kds_label = "kds_in";
		}
		appendStringInfo(
			&body,
			"  if (r_buffer[%d] == 0)\n"
			"    htup = NULL;\n",
			depth);
		if (depth == 0)
			appendStringInfo(
				&body,
				"  else if (%s->format == KDS_FORMAT_BLOCK)\n"
				"    htup = KDS_BLOCK_REF_HTUP(%s,r_buffer[%d],&t_self,NULL);\n",
				kds_label,
				kds_label, depth);
		appendStringInfo(
			&body,
			"  else\n"
			"    htup = KDS_ROW_REF_HTUP(%s,r_buffer[%d],&t_self,NULL);\n",
			kds_label, depth);

		/* System column reference if any */
		foreach (lc1, tlist_dev)
		{
			TargetEntry		   *tle = lfirst(lc1);
			Form_pg_attribute	attr;

			if (varattmaps[tle->resno-1] >= 0)
				continue;
			attr = SystemAttributeDefinition(varattmaps[tle->resno-1], true);
			appendStringInfo(
				&body,
				"  /* %s system column */\n"
				"  if (!htup)\n"
				"    tup_isnull[%d] = true;\n"
				"  else {\n"
				"    tup_isnull[%d] = false;\n"
				"    tup_values[%d] = kern_getsysatt_%s(%s, htup, &t_self);\n"
				"  }\n",
				NameStr(attr->attname),
				tle->resno-1,
				tle->resno-1,
				tle->resno-1,
				NameStr(attr->attname),
				kds_label);
		}

		/* begin to walk on the tuple */
		appendStringInfo(
			&body,
			"  EXTRACT_HEAP_TUPLE_BEGIN(addr, %s, htup);\n", kds_label);

		resetStringInfo(&temp);
		for (i=1; i <= nattrs; i++)
		{
			TargetEntry	   *tle;
			int16			typelen;
			bool			typebyval;
			cl_bool			referenced = false;

			foreach (lc1, tlist_dev)
			{
				tle = lfirst(lc1);

				if (varattmaps[tle->resno - 1] != i)
					continue;
				/* attribute shall be directly copied */
				get_typlenbyval(exprType((Node *)tle->expr),
								&typelen, &typebyval);
				if (!typebyval)
				{
					appendStringInfo(
						&temp,
						"  tup_isnull[%d] = (addr != NULL ? false : true);\n"
						"  tup_values[%d] = PointerGetDatum(addr);\n"
						"  tup_depth[%d] = %d;\n",
						tle->resno - 1,
						tle->resno - 1,
						tle->resno - 1, depth);
				}
				else
				{
					appendStringInfo(
						&temp,
						"  tup_isnull[%d] = (addr != NULL ? false : true);\n"
						"  if (addr)\n"
						"    tup_values[%d] = *((%s *) addr);\n"
						"  tup_depth[%d] = %d;\n",
						tle->resno - 1,
                        tle->resno - 1,
						(typelen == sizeof(cl_long)  ? "cl_long" :
						 typelen == sizeof(cl_int)   ? "cl_int" :
						 typelen == sizeof(cl_short) ? "cl_short"
													 : "cl_char"),
						tle->resno - 1, depth);
				}
				referenced = true;
			}

			forboth (lc1, kvars_srcnum,
					 lc2, kvars_dstnum)
			{
				devtype_info   *dtype;
				cl_int			src_num = lfirst_int(lc1);
				cl_int			dst_num = lfirst_int(lc2);
				Oid				type_oid;

				if (src_num != i)
					continue;
				/* add KVAR_%u declarations */
				tle = list_nth(tlist_dev, dst_num - 1);
				type_oid = exprType((Node *)tle->expr);
				dtype = pgstrom_devtype_lookup(type_oid);
				if (!dtype)
					elog(ERROR, "cache lookup failed for device type: %s",
						 format_type_be(type_oid));

				appendStringInfo(
					source,
					"  pg_%s_t KVAR_%u;\n",
					dtype->type_name,
					dst_num);
				appendStringInfo(
					&temp,
					"  KVAR_%u = pg_%s_datum_ref(kcxt,addr);\n",
					dst_num,
					dtype->type_name);

				referenced = true;
			}

			/* flush to the main buffer */
			if (referenced)
			{
				appendStringInfoString(&body, temp.data);
				resetStringInfo(&temp);
			}
			appendStringInfoString(
				&temp,
				"  EXTRACT_HEAP_TUPLE_NEXT(addr);\n");
		}
		appendStringInfoString(
			&body,
			"  EXTRACT_HEAP_TUPLE_END();\n");
	}

	/*
	 * Execution of the expression
	 */
	is_first = true;
	extra_maxlen = 0;
	forboth (lc1, tlist_dev,
			 lc2, ps_src_depth)
	{
		TargetEntry	   *tle = lfirst(lc1);
		cl_int			src_depth = lfirst_int(lc2);
		devtype_info   *dtype;

		if (tle->resjunk || src_depth >= 0)
			continue;

		if (is_first)
		{
			appendStringInfoString(
				&body,
				"\n"
				"  /* calculation of expressions */\n");
			is_first = false;
		}

		dtype = pgstrom_devtype_lookup(exprType((Node *) tle->expr));
		if (!dtype)
			elog(ERROR, "cache lookup failed for device type: %s",
				 format_type_be(exprType((Node *) tle->expr)));

		if (dtype->type_oid == NUMERICOID)
		{
			extra_maxlen += 32;
			appendStringInfo(
				&body,
				"  temp.%s_v = %s;\n"
				"  tup_isnull[%d] = temp.%s_v.isnull;\n"
				"  if (!temp.%s_v.isnull)\n"
				"  {\n"
				"    cl_uint numeric_len =\n"
				"        pg_numeric_to_varlena(kcxt, extra_pos,\n"
				"                              temp.%s_v.value,\n"
				"                              temp.%s_v.isnull);\n"
				"    tup_values[%d] = PointerGetDatum(extra_pos);\n"
				"    extra_pos += MAXALIGN(numeric_len);\n"
				"  }\n"
				"  tup_depth[%d] = -1;\n",	/* use of local extra_buf */
				dtype->type_name,
				pgstrom_codegen_expression((Node *)tle->expr, context),
				tle->resno - 1,
				dtype->type_name,
				dtype->type_name,
				dtype->type_name,
				dtype->type_name,
				tle->resno - 1,
				tle->resno - 1);
		}
		else if (dtype->type_byval)
		{
			/* fixed length built-in data type */
			appendStringInfo(
				&body,
				"  temp.%s_v = %s;\n"
				"  tup_isnull[%d] = temp.%s_v.isnull;\n"
				"  if (!temp.%s_v.isnull)\n"
				"    tup_values[%d] = pg_%s_to_datum(temp.%s_v.value);\n"
				"  tup_depth[%d] = -255;\n",	/* just a poison */
				dtype->type_name,
				pgstrom_codegen_expression((Node *)tle->expr, context),
				tle->resno - 1,
				dtype->type_name,
				dtype->type_name,
				tle->resno - 1,
				dtype->type_name,
				dtype->type_name,
				tle->resno - 1);
		}
		else if (dtype->type_length > 0)
		{
			/* fixed length pointer data type */
			extra_maxlen += MAXALIGN(dtype->type_length);
			appendStringInfo(
				&body,
				"  temp.%s_v = %s;\n"
				"  tup_isnull[%d] = temp.%s_v.isnull;\n"
				"  if (!temp.%s_v.isnull)\n"
				"  {\n"
				"    memcpy(extra_pos, &temp.%s_v.value,\n"
				"           sizeof(temp.%s_v.value));\n"
				"    tup_values[%d] = PointerGetDatum(extra_pos);\n"
				"    extra_pos += MAXALIGN(sizeof(temp.%s_v.value));\n"
				"  }\n"
				"  tup_depth[%d] = -1;\n",	/* use of local extra_buf */
				dtype->type_name,
				pgstrom_codegen_expression((Node *)tle->expr, context),
				tle->resno - 1,
				dtype->type_name,
				dtype->type_name,
				dtype->type_name,
				dtype->type_name,
				tle->resno - 1,
				dtype->type_name,
				tle->resno - 1);
		}
		else
		{
			/*
			 * variable length pointer data type
			 *
			 * Pay attention for the case when expression may return varlena
			 * data type, even though we have no device function that can
			 * return a varlena function. Like:
			 *   CASE WHEN x IS NOT NULL THEN x ELSE 'no value' END
			 * In this case, a varlena data returned by the expression is
			 * located on either any of KDS buffer or KPARAMS buffer.
			 *
			 * Unless it is not obvious by the node type, we have to walk on
			 * the possible buffer range to find out right one. :-(
			 */
			appendStringInfo(
				&body,
				"  temp.varlena_v = %s;\n"
				"  tup_isnull[%d] = temp.varlena_v.isnull;\n"
				"  tup_values[%d] = PointerGetDatum(temp.varlena_v.value);\n",
				pgstrom_codegen_expression((Node *)tle->expr, context),
				tle->resno - 1,
				tle->resno - 1);

			if (IsA(tle->expr, Const) || IsA(tle->expr, Param))
			{
				/* always references to the kparams buffer */
				appendStringInfo(
					&body,
					"  tup_depth[%d] = -2;\n",
					tle->resno - 1);
			}
			else
			{
				cl_int		i;

				appendStringInfo(
					&body,
					"  if (temp.varlena_v.isnull)\n"
					"    tup_depth[%d] = -9999; /* never referenced */\n"
					"  else if (pointer_on_kparams(temp.varlena_v.value,\n"
					"                              kcxt->kparams))\n"
					"    tup_depth[%d] = -2;\n"
					"  else if (pointer_on_kds(temp.varlena_v.value,\n"
					"                          kds_dst))\n"
					"    tup_depth[%d] = -1;\n"
					"  else if (pointer_on_kds(temp.varlena_v.value,\n"
					"                          kds_src))\n"
					"    tup_depth[%d] = 0;\n",
					tle->resno - 1,
					tle->resno - 1,
					tle->resno - 1,
					tle->resno - 1);
				for (i=1; i <= gj_info->num_rels; i++)
				{
					appendStringInfo(
						&body,
						"  else if (pointer_on_kds(temp.varlena_v.value,\n"
						"           KERN_MULTIRELS_INNER_KDS(kmrels,%d)))\n"
						"    tup_depth[%d] = %d;\n",
						i, tle->resno - 1, i);
				}
				appendStringInfo(
					&body,
					"  else\n"
					"    tup_depth[%d] = -9999; /* should never happen */\n",
					tle->resno - 1);
			}
		}
	}
	/* how much extra field required? */
	appendStringInfoString(
		&body,
		"\n"
		"  *extra_len = (cl_uint)(extra_pos - extra_buf);\n");
	/* add parameter declarations */
	pgstrom_codegen_param_declarations(source, context);
	/* merge with declaration part */
	appendStringInfo(source, "\n%s}\n", body.data);

	*p_extra_maxlen = extra_maxlen;

	pfree(body.data);
	pfree(temp.data);
}

static char *
gpujoin_codegen(PlannerInfo *root,
				CustomScan *cscan,
				GpuJoinInfo *gj_info,
				List *tlist,
				codegen_context *context)
{
	StringInfoData source;
	int			depth;
	ListCell   *cell;

	initStringInfo(&source);

	/*
	 * gpuscan_quals_eval
	 */
	codegen_gpuscan_quals(&source,
						  context,
						  cscan->scan.scanrelid,
						  gj_info->outer_quals);
	/*
	 * gpujoin_join_quals
	 */
	context->pseudo_tlist = cscan->custom_scan_tlist;
	for (depth=1; depth <= gj_info->num_rels; depth++)
		gpujoin_codegen_join_quals(&source, gj_info, depth, context);
	appendStringInfo(
		&source,
		"STATIC_FUNCTION(cl_bool)\n"
		"gpujoin_join_quals(kern_context *kcxt,\n"
		"                   kern_data_store *kds,\n"
		"                   kern_multirels *kmrels,\n"
		"                   int depth,\n"
		"                   cl_uint *o_buffer,\n"
		"                   HeapTupleHeaderData *i_htup,\n"
		"                   cl_bool *needs_outer_row)\n"
		"{\n"
		"  switch (depth)\n"
		"  {\n");

	for (depth=1; depth <= gj_info->num_rels; depth++)
	{
		appendStringInfo(
			&source,
			"  case %d:\n"
			"    return gpujoin_join_quals_depth%d(kcxt, kds, kmrels, o_buffer, i_htup, needs_outer_row);\n",
			depth, depth);
	}
	appendStringInfo(
		&source,
		"  default:\n"
		"    STROM_SET_ERROR(&kcxt->e, StromError_SanityCheckViolation);\n"
		"    break;\n"
		"  }\n"
		"  return false;\n"
		"}\n\n");


	depth = 1;
	foreach (cell, gj_info->hash_outer_keys)
	{
		if (lfirst(cell) != NULL)
			gpujoin_codegen_hash_value(&source, gj_info, depth, context);
		depth++;
	}

	/*
	 * gpujoin_hash_value
	 */
	appendStringInfo(
		&source,
		"STATIC_FUNCTION(cl_uint)\n"
		"gpujoin_hash_value(kern_context *kcxt,\n"
		"                   cl_uint *pg_crc32_table,\n"
		"                   kern_data_store *kds,\n"
		"                   kern_multirels *kmrels,\n"
		"                   cl_int depth,\n"
		"                   cl_uint *o_buffer,\n"
		"                   cl_bool *p_is_null_keys)\n"
		"{\n"
		"  switch (depth)\n"
		"  {\n");
	depth = 1;
	foreach (cell, gj_info->hash_outer_keys)
	{
		if (lfirst(cell) != NULL)
		{
			appendStringInfo(
				&source,
				"  case %u:\n"
				"    return gpujoin_hash_value_depth%u(kcxt,pg_crc32_table,\n"
				"                                      kds,kmrels,o_buffer,\n"
				"                                      p_is_null_keys);\n",
				depth, depth);
		}
		depth++;
	}
	appendStringInfo(
		&source,
		"  default:\n"
		"    STROM_SET_ERROR(&kcxt->e, StromError_SanityCheckViolation);\n"
		"    break;\n"
		"  }\n"
		"  return (cl_uint)(-1);\n"
		"}\n"
		"\n");

	/*
	 * gpujoin_projection
	 */
	gpujoin_codegen_projection(&source, cscan, gj_info, context,
							   &gj_info->extra_maxlen);

	return source.data;
}

/*
 * gpujoin_exec_estimate_nitems
 *
 * 
 *
 */
static double
gpujoin_exec_estimate_nitems(GpuJoinState *gjs,
							 GpuJoinTask *pgjoin,
							 kern_join_scale *jscale_old,
							 double ntuples_in,
							 int depth)
{
	GpuJoinSharedState *gj_sstate = pgjoin->gj_sstate;
	innerState		   *istate = (depth > 0 ? gjs->inners + depth - 1 : NULL);
	kern_join_scale	   *jscale = pgjoin->kern.jscale;
	size_t				source_ntasks;
	size_t				source_nitems;
	size_t				inner_nitems;
	size_t				right_nitems;
	double				ntuples_next;
	double				merge_ratio;
	double				plan_ratio;
	double				exec_ratio;

	/*
	 * Nrows estimation based on plan estimation and exec statistics.
	 * It shall be merged according to the task progress.
	 */
	SpinLockAcquire(&gj_sstate->lock);
	source_ntasks = gj_sstate->source_ntasks;
	source_nitems = gj_sstate->source_nitems;
	SpinLockRelease(&gj_sstate->lock);
	merge_ratio = Max((double) source_ntasks / 20.0,
					  gjs->outer_nrows > 0.0
					  ? ((double)(source_nitems) /
						 (double)(0.30 * gjs->outer_nrows))
					  : 0.0);
	merge_ratio = Min(1.0, merge_ratio);	/* up to 100% */

	/* special case handling for outer_quals evaluation */
	if (depth == 0)
	{
		pgstrom_data_store *pds_src = pgjoin->pds_src;

		/* RIGHT OUTER JOIN has no input rows to be processed */
		if (!pds_src)
			return 0.0;

		/*
		 * In case of the GpuJoin task re-enqueue with partition window,
		 * last execution result is the most reliable hint, because next
		 * task will have same evaluation to the same data, so we can
		 * expect same results.
		 */
		if (jscale_old != NULL)
		{
			ntuples_next = ((double)jscale[0].window_size *
							(double)(jscale_old[1].inner_nitems) /
							(double)(jscale_old[0].window_base +
									 jscale_old[0].window_size -
									 jscale_old[0].window_orig));
			if (pds_src->kds.format == KDS_FORMAT_BLOCK)
				ntuples_next *= 1.1 * (double)pds_src->kds.nrows_per_block;
			return ntuples_next;
		}

		if (!gjs->outer_quals)
		{
			/* nobody will filter out input rows if no outer quals */
			ntuples_next = (double) jscale[0].window_size;
		}
		else
		{
			/*
			 * We try to estimate amount of outer rows which are not elimiated
			 * by the qualifier, based on plan/exec time statistics
			 */
			SpinLockAcquire(&gj_sstate->lock);
			inner_nitems = gj_sstate->inner_nitems[0];
			right_nitems = gj_sstate->right_nitems[0];
			source_nitems = gj_sstate->source_nitems;
			SpinLockRelease(&gj_sstate->lock);

			/*
			 * If there are no run-time statistics, we have no options except
			 * for relying on the plan estimation
			 */
			if (source_nitems == 0)
				ntuples_next = jscale[0].window_size * gjs->outer_ratio;
			else
			{
				/*
				 * Elsewhere, we mix the plan estimation and run-time
				 * statistics according to the outer scan progress.
				 * Once merge_ratio gets 100%, plan estimation shall be
				 * entirely ignored.
				 */
				plan_ratio = gjs->outer_ratio;
				exec_ratio = ((double)(inner_nitems + right_nitems) /
							  (double)(source_nitems));
				ntuples_next =  ((exec_ratio * merge_ratio +
								  plan_ratio * (1.0 - merge_ratio)) *
								 (double) jscale[0].window_size);
			}
		}

		/*
		 * In case of KDS_FORMAT_BLOCK, kds->nitems means number of blocks,
		 * not tuples. So, we need to adjust ntuples_next for size estimation
		 * purpose
		 */
		if (pds_src->kds.format == KDS_FORMAT_BLOCK)
			ntuples_next *= 1.1 * (double)gjs->gts.outer_nrows_per_block;

		return ntuples_next;
	}

	/*
	 * Obviously, no input rows will produce an empty results without
	 * RIGHT OUTER JOIN.
	 */
	if (ntuples_in <= 0.0)
		ntuples_next = 0.0;
	else
	{
		/*
		 * In case of task re-enqueue with virtual partition window
		 * shift, last execution result is the most reliable hint.
		 */
		if (jscale_old &&
			(jscale_old[depth - 1].inner_nitems +
			 jscale_old[depth - 1].right_nitems) > 0)
		{
			ntuples_next = ntuples_in *
				((double)(jscale_old[depth].inner_nitems) /
				 (double)(jscale_old[depth - 1].inner_nitems +
						  jscale_old[depth - 1].right_nitems)) *
				((double)(jscale[depth].window_size) /
				 (double)(jscale_old[depth].window_base +
						  jscale_old[depth].window_size -
						  jscale_old[depth].window_orig));
		}
		else
		{
			pgstrom_data_store *pds_in = gj_sstate->inner_chunks[depth - 1];
			cl_uint			nitems_in = pds_in->kds.nitems;
			size_t			next_nitems;

			SpinLockAcquire(&gj_sstate->lock);
			inner_nitems = gj_sstate->inner_nitems[depth - 1];
			right_nitems = gj_sstate->right_nitems[depth - 1];
			next_nitems = gj_sstate->inner_nitems[depth];
			SpinLockRelease(&gj_sstate->lock);

			plan_ratio = istate->nrows_ratio;
			if (inner_nitems + right_nitems > 0)
				exec_ratio = ((double)(next_nitems) /
							  (double)(inner_nitems + right_nitems));
			else
				exec_ratio = 0.0;

			if (nitems_in == 0)
				ntuples_next = 0.0;
			else
				ntuples_next = ntuples_in *
					(exec_ratio * merge_ratio +
					 plan_ratio * (1.0 - merge_ratio)) *
					((double)jscale[depth].window_size / (double)nitems_in);
		}
	}

	/*
	 * RIGHT/FULL OUTER JOIN will suddenly produce rows in this depth
	 */
	if (!pgjoin->pds_src && (istate->join_type == JOIN_RIGHT ||
							 istate->join_type == JOIN_FULL))
	{
		pgstrom_data_store *pds_in = gj_sstate->inner_chunks[depth - 1];

		if (jscale[depth].window_size > 0)
		{
			/*
			 * In case of task re-enqueue with inner window shift,
			 * last execution result is the most reliable hint.
			 */
			if (jscale_old)
			{
				ntuples_next += (double) jscale_old[depth].right_nitems *
					((double)(jscale[depth].window_size) /
					 (double)(jscale_old[depth].window_base +
							  jscale_old[depth].window_size -
							  jscale_old[depth].window_orig));
			}
			else
			{
				/*
				 * Right now, we assume unmatched row ratio using
				 *  1.0 - SQRT(# of result rows) / (# of inner rows)
				 *
				 * XXX - We may need more exact statistics on outer_join_map
				 */
				cl_uint	nitems_in = pds_in->kds.nitems;
				double	match_ratio;

				if (nitems_in == 0)
					match_ratio = 1.0;	/* an obvious case */
				else
				{
					SpinLockAcquire(&gj_sstate->lock);
					inner_nitems = gj_sstate->inner_nitems[depth];
					right_nitems = gj_sstate->right_nitems[depth];
					SpinLockRelease(&gj_sstate->lock);
					match_ratio = sqrt((double)(inner_nitems + right_nitems) /
									   (double)(nitems_in));
					match_ratio = 1.0 - Min(1.0, match_ratio);
					match_ratio = Max(0.05, match_ratio);	/* at least 5% */
				}
				ntuples_next += match_ratio * jscale[depth].window_size;
			}
		}
	}
	return ntuples_next;
}


/*
 * gpujoin_attach_result_buffer
 */
static pgstrom_data_store *
gpujoin_attach_result_buffer(GpuJoinState *gjs,
							 GpuJoinTask *pgjoin,
							 double ntuples, cl_int target_depth)
{
	GpuContext	   *gcontext = gjs->gts.gcontext;
	TupleTableSlot *tupslot = gjs->gts.css.ss.ss_ScanTupleSlot;
	TupleDesc		tupdesc = tupslot->tts_tupleDescriptor;
	cl_int			ncols = tupdesc->natts;
	Size			nrooms = (Size)(ntuples * pgstrom_chunk_size_margin);
	GpuJoinSharedState *gj_sstate = pgjoin->gj_sstate;
	pgstrom_data_store *pds_dst;

	/*
	 * Calculation of the pds_dst length - If we have no run-time information,
	 * all we can do is statistic based estimation. Elsewhere, kds->nitems
	 * will tell us maximum number of row-slot consumption last time.
	 * If StromError_DataStoreNoSpace happen due to lack of kern_resultbuf,
	 * previous kds->nitems may shorter than estimation. So, for safety,
	 * we adopts the larger one.
	 */

	/*
	 * XXX - No longer we use KDS_FORMAT_SLOT format as result of GpuJoin.
	 * It always needs pds_src to reference variable length fields, thus,
	 * eventually consumes larger PCIe band than row-format.
	 */
	if (false /* !gjs->gts.row_format */)
	{
		/* KDS_FORMAT_SLOT */
		Size	length = (STROMALIGN(offsetof(kern_data_store,
											  colmeta[ncols])) +
						  LONGALIGN((sizeof(Datum) +
									 sizeof(char)) * ncols +
									gjs->extra_maxlen) * nrooms);

		/* Adjustment if too short or too large */
		if (ncols == 0)
		{
			/* MEMO: Typical usage of ncols == 0 is GpuJoin underlying
			 * COUNT(*) because it does not need to put any contents in
			 * the slot. So, we can allow to increment nitems as long as
			 * 32bit width. :-)
			 */
			Assert(gjs->extra_maxlen == 0);
			nrooms = INT_MAX;
		}
		else if (length < pgstrom_chunk_size() / 2)
		{
			/*
			 * MEMO: If destination buffer size is too small, we doubt
			 * incorrect estimation by planner, so we try to prepare at
			 * least half of the pgstrom_chunk_size().
			 */
			nrooms = (pgstrom_chunk_size() / 2 -
					  STROMALIGN(offsetof(kern_data_store,
										  colmeta[ncols])))
				/ (LONGALIGN((sizeof(Datum) +
							  sizeof(char)) * ncols) + gjs->extra_maxlen);
		}
		else if (length > pgstrom_chunk_size_limit())
		{
			/*
			 * MEMO: If expected result buffer length was too much,
			 * we retry size estimation with smaller inner window.
			 */
			cl_int	nsplit = length / pgstrom_chunk_size_limit() + 1;

			Assert(target_depth > 0 && target_depth <= gjs->num_rels);
			pgjoin->kern.jscale[target_depth].window_size
				= (pgjoin->kern.jscale[target_depth].window_size / nsplit) + 1;
			if (pgjoin->kern.jscale[target_depth].window_size <= 1)
				elog(ERROR, "Too much growth of result rows");
			return NULL;
		}
		pds_dst = PDS_create_slot(gjs->gts.gcontext,
								  tupdesc,
								  nrooms,
								  gjs->extra_maxlen * nrooms);
	}
	else
	{
		/* KDS_FORMAT_ROW */
		double		merge_ratio;
		double		tup_width;
		size_t		source_ntasks;
		size_t		source_nitems;
		size_t		results_nitems;
		size_t		results_usage;
		Size		length;

		/*
		 * Tuple width estimation also follow the logic when we estimate
		 * number of rows.
		 */
		SpinLockAcquire(&gj_sstate->lock);
		source_ntasks = gj_sstate->source_ntasks;
		source_nitems = gj_sstate->source_nitems;
		results_nitems = gj_sstate->results_nitems;
		results_usage = gj_sstate->results_usage;
		SpinLockRelease(&gj_sstate->lock);

		merge_ratio = Max((double) source_ntasks / 20.0,
						  (double) source_nitems /
						  (double)(0.30 * gjs->outer_nrows));
		if (results_nitems == 0)
		{
			tup_width = gjs->result_width;
		}
		else if (merge_ratio < 1.0)
		{
			double	plan_width = gjs->result_width;
			double	exec_width = ((double) results_usage /
								  (double) results_nitems);
			tup_width = (plan_width * (1.0 - merge_ratio) +
						 exec_width * merge_ratio);
		}
		else
		{
			tup_width = ((double) results_usage /
						 (double) results_nitems);
		}

		/* Expected buffer length */
		length = (STROMALIGN(offsetof(kern_data_store,
									  colmeta[ncols])) +
				  STROMALIGN(sizeof(cl_uint) * nrooms) +
				  MAXALIGN(offsetof(kern_tupitem, htup) +
						   ceill(tup_width)) * nrooms);
		if (length < pgstrom_chunk_size() / 2)
			length = pgstrom_chunk_size() / 2;
		else if (length > pgstrom_chunk_size_limit())
		{
			Size		small_nrooms;
			cl_int		nsplit;

			/* maximum number of tuples we can store */
			small_nrooms = (pgstrom_chunk_size_limit() -
							STROMALIGN(offsetof(kern_data_store,
												colmeta[ncols])))
				/ (sizeof(cl_uint) +
				   MAXALIGN(offsetof(kern_tupitem, htup) +
							ceill(tup_width)));
			nsplit = nrooms / small_nrooms + 1;
			pgjoin->kern.jscale[target_depth].window_size
				= pgjoin->kern.jscale[target_depth].window_size / nsplit + 1;
			if (pgjoin->kern.jscale[target_depth].window_size <= 1)
				elog(ERROR, "Too much growth of result rows");
			return NULL;
		}
		pds_dst = PDS_create_row(gcontext, tupdesc, length);
	}
	return pds_dst;
}



/*
 * gpujoin_create_task
 */
static GpuTask *
gpujoin_create_task(GpuJoinState *gjs,
					GpuJoinSharedState *gj_sstate,
					pgstrom_data_store *pds_src,
					int file_desc)
{
	GpuContext		   *gcontext = gjs->gts.gcontext;
	kern_data_store	   *kds_dst_head = gjs->kds_dst_head;
	GpuJoinTask		   *pgjoin;
	double				ntuples;
	double				ntuples_next;
	double				ntuples_delta;
	Size				length;
	Size				required;
	Size				max_items;
	cl_int				i, depth;
	cl_int				target_depth;
	cl_double			target_row_dist_score;
	kern_parambuf	   *kparams;

	/* allocation of GpuJoinTask */
	required = offsetof(GpuJoinTask, kern)
		+ STROMALIGN(offsetof(kern_gpujoin,
							  jscale[gjs->num_rels + 1]))	/* kgjoin */
		+ STROMALIGN(gjs->gts.kern_params->length)			/* kparams */
		+ STROMALIGN(offsetof(kern_data_store,
							  colmeta[kds_dst_head->ncols]));/* kds_dst_head */
	pgjoin = dmaBufferAlloc(gcontext, required);
	if (!pgjoin)
		werror("out of DMA buffer");
	memset(pgjoin, 0, required);

	pgstromInitGpuTask(&gjs->gts, &pgjoin->task);
	pgjoin->task.file_desc = file_desc;
	pgjoin->gj_sstate = gpujoinGetInnerBuffer(gcontext, gj_sstate);
	pgjoin->pds_src = pds_src;
	pgjoin->pds_dst = NULL;		/* to be set later */
	pgjoin->is_terminator = (pds_src == NULL);

	/* Is NVMe-Strom available to run this GpuJoin? */
	if (pds_src && pds_src->kds.format == KDS_FORMAT_BLOCK)
	{
		Assert(gjs->gts.nvme_sstate != NULL);
		pgjoin->with_nvme_strom = (pds_src->nblocks_uncached > 0);
	}

	pgjoin->kern.kresults_1_offset = 0xe7e7e7e7;	/* to be set later */
	pgjoin->kern.kresults_2_offset = 0x7e7e7e7e;	/* to be set later */
	pgjoin->kern.num_rels = gjs->num_rels;
	pgjoin->kern.nitems_filtered = 0;

	/* setup of kern_parambuf */
	/* NOTE: KERN_GPUJOIN_PARAMBUF() depends on pgjoin->kern.num_rels */
	pgjoin->kern.kparams_offset
		= STROMALIGN(offsetof(kern_gpujoin, jscale[gjs->num_rels + 1]));
	kparams = KERN_GPUJOIN_PARAMBUF(&pgjoin->kern);
	memcpy(kparams,
		   gjs->gts.kern_params,
		   gjs->gts.kern_params->length);

	/* setup of kds_dst_head */
	pgjoin->kds_dst_head = (kern_data_store *)
		((char *)kparams + kparams->length);
	memcpy(pgjoin->kds_dst_head, kds_dst_head,
		   offsetof(kern_data_store, colmeta[kds_dst_head->ncols]));

	/* setup initial jscale */
	for (i = gjs->num_rels; i >= 0; i--)
	{
		kern_join_scale	   *jscale = pgjoin->kern.jscale;
		cl_uint				nitems;

		if (i == 0)
			nitems = (!pgjoin->pds_src ? 0 : pgjoin->pds_src->kds.nitems);
		else
			nitems = gj_sstate->inner_chunks[i-1]->kds.nitems;

		jscale[i].window_base = 0;
		jscale[i].window_size = nitems;
		jscale[i].window_orig = jscale[i].window_base;
	}

	/*
	 * Estimation of the number of join result items for each depth
	 *
	 * XXX - Is it really needed? GpuJoin's in-kernel logic prevent overflow
	 */
major_retry:
	target_depth = 0;
	length = 0;
	ntuples = 0.0;
	ntuples_delta = 0.0;
	max_items = 0;

	/*
	 * Find out the largest distributed depth (if run-time statistics
	 * exists), or depth with largest delta elsewhere, for window-size
	 * reduction in the later stage.
	 * It might be a bit paranoia, however, all the score needs to be
	 * compared atomically.
	 */
	SpinLockAcquire(&gj_sstate->lock);
	if (gj_sstate->row_dist_score_valid)
	{
		target_row_dist_score = gj_sstate->row_dist_score[0];
		for (depth=1; depth < gjs->num_rels; depth++)
		{
			if (target_row_dist_score < gj_sstate->row_dist_score[depth])
			{
				target_row_dist_score = gj_sstate->row_dist_score[depth];
				target_depth = depth;
			}
		}
	}
	else
	{
		/* cannot determine by the runtime-stat, so use delta of ntuples */
		target_row_dist_score = -1.0;
	}
	SpinLockRelease(&gj_sstate->lock);

	for (depth = 0;
		 depth <= gjs->num_rels;
		 depth++, ntuples = ntuples_next)
	{
		Size		max_items_temp;

	minor_retry:
		ntuples_next = gpujoin_exec_estimate_nitems(gjs,
													pgjoin,
													NULL, //jscale_old,
													ntuples,
													depth);

		/* check expected length of the kern_gpujoin head */
		max_items_temp = (Size)((double)(depth+1) *
								ntuples_next *
								pgstrom_chunk_size_margin);
		length = KERN_GPUJOIN_HEAD_LENGTH(&pgjoin->kern) +
			STROMALIGN(offsetof(kern_resultbuf, results[max_items_temp])) +
			STROMALIGN(offsetof(kern_resultbuf, results[max_items_temp]));

		/* split inner window if too large */
		if (length > 2 * pgstrom_chunk_size())
		{
			static int __count = 0;

			pgjoin->kern.jscale[target_depth].window_size
				/= (length / (2 * pgstrom_chunk_size())) + 1;
			if (pgjoin->kern.jscale[depth].window_size < 1)
				elog(ERROR, "Too much growth of result rows");
			if (depth == target_depth)
				goto minor_retry;
			if (__count++ > 10000)
				((char *)NULL)[3] = 'a';	// SEGV
			goto major_retry;
		}
		max_items = Max(max_items, max_items_temp);

		/*
		 * Determine the target depth by delta of ntuples if run-time
		 * statistics are not available.
		 */
		if (target_row_dist_score < 0 &&
			depth > 0 &&
			(depth == 1 || ntuples_next - ntuples > ntuples_delta))
		{
			ntuples_delta = Max(ntuples_next - ntuples, 0.0);
			target_depth = depth;
		}
		ntuples = ntuples_next;
	}

	/*
	 * Minimum guarantee of the kern_gpujoin buffer.
	 *
	 * NOTE: we usually have large volatility when GpuJoin tries to filter
	 * many rows, especially row selectivity is less than 1-5%, then it leads
	 * unpreferable retry of GpuJoin tasks,
	 * Unless it does not exceeds several megabytes, larger kern_resultbuf
	 * buffer is usually harmless.
	 */
	if (length < pgstrom_chunk_size() / 4)
	{
		Size	max_items_temp
			= (pgstrom_chunk_size() / 4
			   - KERN_GPUJOIN_HEAD_LENGTH(&pgjoin->kern)
			   - STROMALIGN(offsetof(kern_resultbuf, results[0]))
			   - STROMALIGN(offsetof(kern_resultbuf, results[0])));
		Assert(max_items_temp >= max_items);
		length = pgstrom_chunk_size() / 4;
		max_items = max_items_temp;
	}

	/*
	 * Calculation of the destination buffer length.
	 * If expected ntuples was larger than limitation of chunk size, we
	 * have to reduce inner window size and estimate the join results.
	 * At that time, gpujoin_attach_result_buffer reduce inner_size based
	 * on the espected buffer length.
	 */
	pgjoin->pds_dst = gpujoin_attach_result_buffer(gjs, pgjoin, ntuples,
												   target_depth);
	if (!pgjoin->pds_dst)
		goto major_retry;

	/* offset of kern_resultbuf */
	pgjoin->kern.kresults_1_offset = KERN_GPUJOIN_HEAD_LENGTH(&pgjoin->kern);
	pgjoin->kern.kresults_2_offset = pgjoin->kern.kresults_1_offset +
		STROMALIGN(offsetof(kern_resultbuf, results[max_items]));
	pgjoin->kern.kresults_max_items = max_items;
	pgjoin->kern.num_rels = gjs->num_rels;

	return &pgjoin->task;
}

/*
 * gpujoin_clone_task
 */
static GpuJoinTask *
gpujoin_clone_task(GpuJoinTask *pgjoin_old)
{
	GpuJoinTask	   *pgjoin_new;
	GpuContext	   *gcontext;
	Size			required;

	gcontext = (IsGpuServerProcess()
				? pgjoin_old->task.gcontext
				: pgjoin_old->task.gts->gcontext);

	required = offsetof(GpuJoinTask, kern)
		+ STROMALIGN(offsetof(kern_gpujoin,
							  jscale[pgjoin_old->kern.num_rels + 1]))
		+ KERN_GPUJOIN_PARAMBUF_LENGTH(&pgjoin_old->kern);
	pgjoin_new = dmaBufferAlloc(gcontext, required);
	if (!pgjoin_new)
		werror("out of DMA buffer");
	memcpy(pgjoin_new, pgjoin_old, required);

	memset(&pgjoin_new->task.kerror, 0, sizeof(kern_errorbuf));
	memset(&pgjoin_new->task.chain, 0, sizeof(dlist_node));
	pgjoin_new->task.cpu_fallback = false;
	memset(&pgjoin_new->task.tv_wakeup, 0, sizeof(struct timeval));
	pgjoin_new->task.peer_fdesc = -1;
	pgjoin_new->is_terminator = false;
	pgjoin_new->gj_sstate = NULL;	/* caller should set */
	pgjoin_new->pds_src = NULL;		/* caller should set */
	pgjoin_new->pds_dst = NULL;		/* caller should set */

	return pgjoin_new;
}

/*
 * gpujoin_outerjoin_kicker
 */
static void
gpujoin_outerjoin_kicker(GpuJoinSharedState *gj_sstate, void *private)
{
	GpuJoinTask		*pgjoin_old = (GpuJoinTask *) private;
	GpuJoinTask		*pgjoin_new = gpujoin_clone_task(pgjoin_old);
	GpuContext		*gcontext = (IsGpuServerProcess()
								 ? pgjoin_old->task.gcontext
								 : pgjoin_old->task.gts->gcontext);
	pgjoin_new->gj_sstate = gpujoinGetInnerBuffer(gcontext, gj_sstate);
	if (IsGpuServerProcess())
		gpuservPushGpuTask(gcontext, &pgjoin_new->task);
	else
		gpuservSendGpuTask(gcontext, &pgjoin_new->task);
}

/*
 * gpujoin_next_task
 */
static GpuTask *
gpujoin_next_task(GpuTaskState *gts)
{
	GpuJoinState   *gjs = (GpuJoinState *) gts;
	GpuTask		   *gtask;
	pgstrom_data_store *pds = NULL;
	int				filedesc = -1;

	/*
	 * Preload inner buffer if not yet. Unlike older version, we never split
	 * inner hash/heap buffer no longer, because GPU's unified memory allows
	 * over commit of device memory and demand paging even if required size
	 * is larger than physical memory.
	 */
	if (!gpujoin_inner_preload(gjs))
		return NULL;	/* GpuJoin obviously produces an empty result */

	if (gjs->gts.css.ss.ss_currentRelation)
		pds = gpuscanExecScanChunk(gts, &filedesc);
	else
	{
		PlanState	   *outer_node = outerPlanState(gjs);
		TupleTableSlot *slot;

		for (;;)
		{
			if (gjs->gts.scan_overflow)
			{
				slot = gjs->gts.scan_overflow;
				gjs->gts.scan_overflow = NULL;
			}
			else
			{
				slot = ExecProcNode(outer_node);
				if (TupIsNull(slot))
					break;
			}

			/* creation of a new data-store on demand */
			if (!pds)
			{
				pds = PDS_create_row(gjs->gts.gcontext,
									 ExecGetResultType(outer_node),
									 pgstrom_chunk_size());
			}
			/* insert the tuple on the data-store */
			if (!PDS_insert_tuple(pds, slot))
			{
				gjs->gts.scan_overflow = slot;
				break;
			}
		}
	}
	gtask = gpujoin_create_task(gjs, gjs->gj_sstate, pds, filedesc);
	if (!pds)
	{
		GpuJoinSharedState *gj_sstate = gjs->gj_sstate;

		/*
		 * Set @outer_scan_done to mark no more GpuTask will not be produced
		 * on the inner hash/heap buffer. Once reference counter gets reached
		 * to zero next, it means the last task on the GpuJoin, thus we can
		 * kick RIGHT/FULL OUTER JOIN on this timing.
		 */
		pthreadMutexLock(&gj_sstate->mutex);
		gj_sstate->outer_scan_done = true;
		pthreadMutexUnlock(&gj_sstate->mutex);

		/* and, gpu_tasks.c will produce no tasks any more */
		gjs->gts.scan_done = true;
	}
	return gtask;
}

/*
 * gpujoin_ready_task - callback when a GpuJoinTask task gets processed
 * on the GPU server process then returned to the backend process again.
 */
static void
gpujoin_ready_task(GpuTaskState *gts, GpuTask *gtask)
{
	GpuJoinTask *pgjoin = (GpuJoinTask *) gtask;

	if (gtask->kerror.errcode != StromError_Success)
		elog(ERROR, "GpuJoin kernel internal error: %s",
			 errorTextKernel(&gtask->kerror));
	if (pgjoin->task.cpu_fallback
		? pgjoin->gj_sstate == NULL
		: pgjoin->gj_sstate != NULL)
		elog(FATAL, "Bug? incorrect status of inner hash/heap buffer");
}

/*
 * gpujoin_switch_task - callback when a GpuJoinTask task gets completed
 * and assigned on the gts->curr_task.
 */
static void
gpujoin_switch_task(GpuTaskState *gts, GpuTask *gtask)
{
	GpuJoinState   *gjs = (GpuJoinState *) gts;
	GpuJoinTask	   *pgjoin = (GpuJoinTask *) gtask;
	int				i;

	/* rewind the CPU fallback position */
	if (pgjoin->task.cpu_fallback)
	{
		Assert(pgjoin->gj_sstate != NULL);
		gjs->fallback_outer_index = -1;
		for (i=0; i < gjs->num_rels; i++)
		{
			gjs->inners[i].fallback_inner_index = -1;
			gjs->inners[i].fallback_right_outer = false;
		}
		ExecStoreAllNullTuple(gjs->slot_fallback);
	}
}

static TupleTableSlot *
gpujoin_next_tuple(GpuTaskState *gts)
{
	GpuJoinState   *gjs = (GpuJoinState *) gts;
	TupleTableSlot *slot = gjs->gts.css.ss.ss_ScanTupleSlot;
	GpuJoinTask	   *pgjoin = (GpuJoinTask *)gjs->gts.curr_task;
	pgstrom_data_store *pds_dst = pgjoin->pds_dst;

	if (pgjoin->task.cpu_fallback)
	{
		/*
		 * MEMO: We may reuse tts_values[]/tts_isnull[] of the previous
		 * tuple, to avoid same part of tuple extraction. For example,
		 * portion by depth < 2 will not be changed during iteration in
		 * depth == 3. You may need to pay attention on the core changes
		 * in the future version.
		 */
		slot = gpujoin_next_tuple_fallback(gjs, pgjoin);
	}
	else
	{
		/* fetch a result tuple */
		ExecClearTuple(slot);
		if (!PDS_fetch_tuple(slot, pds_dst, &gjs->gts))
			slot = NULL;
	}
#if 0
	/*
	 * MEMO: If GpuJoin generates a corrupted tuple, it may lead crash on
	 * the upper level of plan node. Even if we got a crash dump, it is not
	 * easy to analyze corrupted tuple later. ExecMaterializeSlot() can
	 * cause crash in proper level, and it will assist bug fixes.
	 */
	if (slot != NULL)
		(void) ExecMaterializeSlot(slot);
#endif
	return slot;
}

/* ----------------------------------------------------------------
 *
 * Routines for CPU fallback, if kernel code returned CpuReCheck
 * error code.
 *
 * ----------------------------------------------------------------
 */
static void
gpujoin_fallback_tuple_extract(TupleTableSlot *slot_fallback,
							   TupleDesc tupdesc, Oid table_oid,
							   kern_tupitem *tupitem,
							   AttrNumber *tuple_dst_resno,
							   AttrNumber src_anum_min,
							   AttrNumber src_anum_max)
{
	HeapTupleHeader	htup;
	bool		hasnulls;
	AttrNumber	fallback_nattrs __attribute__ ((unused));
	Datum	   *tts_values = slot_fallback->tts_values;
	bool	   *tts_isnull = slot_fallback->tts_isnull;
	char	   *tp;
	long		off;
	int			i, nattrs;
	AttrNumber	resnum;

	Assert(src_anum_min > FirstLowInvalidHeapAttributeNumber);
	Assert(src_anum_max <= tupdesc->natts);
	fallback_nattrs = slot_fallback->tts_tupleDescriptor->natts;

	/*
	 * Fill up the destination by NULL, if no tuple was supplied.
	 */
	if (!tupitem)
	{
		for (i = src_anum_min; i <= src_anum_max; i++)
		{
			resnum = tuple_dst_resno[i-FirstLowInvalidHeapAttributeNumber-1];
			if (resnum)
			{
				Assert(resnum > 0 && resnum <= fallback_nattrs);
				tts_values[resnum - 1] = (Datum) 0;
				tts_isnull[resnum - 1] = true;
			}
		}
		return;
	}

	htup = &tupitem->htup;
	hasnulls = ((htup->t_infomask & HEAP_HASNULL) != 0);

	/*
	 * Extract system columns if any
	 */
	if (src_anum_min < 0)
	{
		/* ctid */
		resnum = tuple_dst_resno[SelfItemPointerAttributeNumber -
								 FirstLowInvalidHeapAttributeNumber - 1];
		if (resnum)
		{
			Assert(resnum > 0 && resnum <= fallback_nattrs);
			tts_values[resnum - 1] = PointerGetDatum(&tupitem->t_self);
			tts_isnull[resnum - 1] = false;
		}

		/* cmax */
		resnum = tuple_dst_resno[MaxCommandIdAttributeNumber -
								 FirstLowInvalidHeapAttributeNumber - 1];
		if (resnum)
		{
			Assert(resnum > 0 && resnum <= fallback_nattrs);
			tts_values[resnum - 1]
				= CommandIdGetDatum(HeapTupleHeaderGetRawCommandId(htup));
			tts_isnull[resnum - 1] = false;
		}

		/* xmax */
		resnum = tuple_dst_resno[MaxTransactionIdAttributeNumber -
								 FirstLowInvalidHeapAttributeNumber - 1];
		if (resnum)
		{
			Assert(resnum > 0 && resnum <= fallback_nattrs);
			tts_values[resnum - 1]
				= TransactionIdGetDatum(HeapTupleHeaderGetRawXmax(htup));
			tts_isnull[resnum - 1] = false;
		}

		/* cmin */
		resnum = tuple_dst_resno[MinCommandIdAttributeNumber -
								 FirstLowInvalidHeapAttributeNumber - 1];
		if (resnum)
		{
			Assert(resnum > 0 && resnum <= fallback_nattrs);
			tts_values[resnum - 1]
				= CommandIdGetDatum(HeapTupleHeaderGetRawCommandId(htup));
			tts_isnull[resnum - 1] = false;
		}

		/* xmin */
		resnum = tuple_dst_resno[MinTransactionIdAttributeNumber -
								 FirstLowInvalidHeapAttributeNumber - 1];
		if (resnum)
		{
			Assert(resnum > 0 && resnum <= fallback_nattrs);
			tts_values[resnum - 1]
				= TransactionIdGetDatum(HeapTupleHeaderGetRawXmin(htup));
			tts_isnull[resnum - 1] = false;
		}

		/* oid */
		resnum = tuple_dst_resno[ObjectIdAttributeNumber -
								 FirstLowInvalidHeapAttributeNumber - 1];
		if (resnum)
		{
			Assert(resnum > 0 && resnum <= fallback_nattrs);
			tts_values[resnum - 1]
				= ObjectIdGetDatum(HeapTupleHeaderGetOid(htup));
			tts_isnull[resnum - 1] = false;
		}

		/* tableoid */
		resnum = tuple_dst_resno[TableOidAttributeNumber -
								 FirstLowInvalidHeapAttributeNumber - 1];
		if (resnum)
		{
			Assert(resnum > 0 && resnum <= fallback_nattrs);
			tts_values[resnum - 1] = ObjectIdGetDatum(table_oid);
			tts_isnull[resnum - 1] = false;
		}
	}

	/*
	 * Extract user defined columns, according to the logic in
	 * heap_deform_tuple(), but implemented by ourselves for performance.
	 */
	nattrs = HeapTupleHeaderGetNatts(htup);
	nattrs = Min3(nattrs, tupdesc->natts, src_anum_max);

	tp = (char *) htup + htup->t_hoff;
	off = 0;
	for (i=0; i < nattrs; i++)
	{
		Form_pg_attribute	attr = tupdesc->attrs[i];

		resnum = tuple_dst_resno[i - FirstLowInvalidHeapAttributeNumber];
		if (hasnulls && att_isnull(i, htup->t_bits))
		{
			if (resnum > 0)
			{
				Assert(resnum <= fallback_nattrs);
				tts_values[resnum - 1] = (Datum) 0;
				tts_isnull[resnum - 1] = true;
			}
			continue;
		}

		/* elsewhere field is not null */
		if (resnum > 0)
		{
			Assert(resnum <= fallback_nattrs);
			tts_isnull[resnum - 1] = false;
		}

		if (attr->attlen == -1)
			off = att_align_pointer(off, attr->attalign, -1, tp + off);
		else
			off = att_align_nominal(off, attr->attalign);

		if (resnum > 0)
		{
			Assert(resnum <= fallback_nattrs);
			tts_values[resnum - 1] = fetchatt(attr, tp + off);
		}
		off = att_addlength_pointer(off, attr->attlen, tp + off);
	}

	/*
     * If tuple doesn't have all the atts indicated by src_anum_max,
	 * read the rest as null
	 */
	for (; i < src_anum_max; i++)
	{
		resnum = tuple_dst_resno[i - FirstLowInvalidHeapAttributeNumber];
		if (resnum > 0)
		{
			Assert(resnum <= fallback_nattrs);
			tts_values[resnum - 1] = (Datum) 0;
			tts_isnull[resnum - 1] = true;
		}
	}
}

static bool
gpujoin_fallback_inner_recurse(GpuJoinState *gjs,
							   TupleTableSlot *slot_fallback,
							   GpuJoinTask *pgjoin,
							   int depth,
							   cl_bool do_right_outer_join)
{
	ExprContext		   *econtext = gjs->gts.css.ss.ps.ps_ExprContext;
	GpuJoinSharedState *gj_sstate = pgjoin->gj_sstate;
	innerState		   *istate = &gjs->inners[depth-1];
	TupleTableSlot	   *slot_in = istate->state->ps_ResultTupleSlot;
	TupleDesc			tupdesc = slot_in->tts_tupleDescriptor;
	kern_data_store	   *kds_in;
	kern_join_scale	   *jscale;
	bool				reload_inner_next;

	Assert(depth > 0 && depth <= gjs->num_rels);
	kds_in = &gj_sstate->inner_chunks[depth-1]->kds;
	jscale = &pgjoin->kern.jscale[depth];

	reload_inner_next = (istate->fallback_inner_index < 0 ||
						 depth == gjs->num_rels);
	for (;;)
	{
		cl_uint			i, kds_index;
		cl_uint			nvalids;

		if (reload_inner_next)
		{
			kern_tupitem   *tupitem = NULL;
			kern_hashitem  *khitem;

			ResetExprContext(econtext);

			if (do_right_outer_join)
			{
				/* already reached end of the inner relation */
				if (istate->fallback_inner_index == UINT_MAX)
					return false;

				kds_index = Max(jscale->window_orig,
								istate->fallback_inner_index + 1);
				if (istate->join_type == JOIN_RIGHT ||
					istate->join_type == JOIN_FULL)
				{
					cl_bool	   *host_ojmap = gj_sstate->h_ojmaps;

					Assert(host_ojmap != NULL);
					host_ojmap += gj_sstate->kern.chunks[depth-1].ojmap_offset;
					nvalids = Min(kds_in->nitems,
								  jscale->window_base + jscale->window_size);
					/*
					 * Make half-null tuples according to the outer join map,
					 * then kick inner join on the later depth.
					 * Once we reached end of the OJMap, walk down into the
					 * deeper depth.
					 */
					while (kds_index < nvalids)
					{
						if (!host_ojmap[kds_index])
						{
							ExecStoreAllNullTuple(slot_fallback);

							tupitem = KERN_DATA_STORE_TUPITEM(kds_in,
															  kds_index);
							istate->fallback_inner_index = kds_index;
							goto inner_fillup;
						}
						kds_index++;
					}
				}
				/* no need to walk down into deeper depth */
				if (depth == gjs->num_rels)
					return false;

				tupitem = NULL;
				istate->fallback_inner_index = UINT_MAX;
				istate->fallback_right_outer = true;
			}
			else if (!istate->hash_outer_keys)
			{
				/*
				 * Case of GpuNestLoop
				 */
				kds_index = Max(jscale->window_orig,
								istate->fallback_inner_index + 1);
				nvalids = Min(kds_in->nitems,
							  jscale->window_base + jscale->window_size);
				if (kds_index >= nvalids)
					return false;	/* end of inner/left join */
				tupitem = KERN_DATA_STORE_TUPITEM(kds_in, kds_index);
				istate->fallback_inner_index = kds_index;
				istate->fallback_inner_matched = false;
			}
			else if (istate->fallback_inner_index < 0)
			{
				/*
				 * Case of GpuHashJoin (first item)
				 */
				cl_uint		hash;
				bool		is_null_keys;

				hash = get_tuple_hashvalue(istate,
										   false,
										   slot_fallback,
										   &is_null_keys);
				/* all-NULL keys will never match to inner rows */
				if (is_null_keys)
				{
					if (istate->join_type == JOIN_LEFT ||
						istate->join_type == JOIN_FULL)
					{
						istate->fallback_inner_index = UINT_MAX;
						tupitem = NULL;
						goto inner_fillup;
					}
					return false;
				}

				/* Is the hash-value in range of the kds_in? */
				if (hash < kds_in->hash_min || hash > kds_in->hash_max)
					return false;

				khitem = KERN_HASH_FIRST_ITEM(kds_in, hash);
				if (!khitem)
				{
					if (istate->join_type == JOIN_LEFT ||
						istate->join_type == JOIN_FULL)
					{
						istate->fallback_inner_index = UINT_MAX;
						tupitem = NULL;
						goto inner_fillup;
					}
					return false;
				}
				kds_index = khitem->rowid;
				istate->fallback_inner_hash = hash;
				istate->fallback_inner_index = kds_index;
				istate->fallback_inner_matched = false;

				/* khitem is not visible if rowid is out of window range */
				if (khitem->rowid < jscale->window_base ||
					khitem->rowid >= jscale->window_base + jscale->window_size)
					continue;

				/* quick check whether khitem shall match */
				if (khitem->hash != istate->fallback_inner_hash)
					continue;

				tupitem = &khitem->t;
			}
			else if (istate->fallback_inner_index < UINT_MAX)
			{
				/*
				 * Case of GpuHashJoin (second or later item)
				 */
				kds_index = istate->fallback_inner_index;
				khitem = KERN_DATA_STORE_HASHITEM(kds_in, kds_index);
				Assert(khitem != NULL);
				khitem = KERN_HASH_NEXT_ITEM(kds_in, khitem);
				if (!khitem)
				{
					if (!istate->fallback_inner_matched &&
						(istate->join_type == JOIN_LEFT ||
						 istate->join_type == JOIN_FULL))
					{
						istate->fallback_inner_index = UINT_MAX;
						tupitem = NULL;
						goto inner_fillup;
					}
					return false;
				}
				kds_index = khitem->rowid;
				istate->fallback_inner_index = kds_index;

				/* khitem is not visible if rowid is out of window range */
				if (khitem->rowid < jscale->window_orig ||
					khitem->rowid >= jscale->window_base + jscale->window_size)
					continue;

				/* quick check whether khitem shall match */
				if (khitem->hash != istate->fallback_inner_hash)
					continue;

				tupitem = &khitem->t;
			}
			else
			{
				/*
				 * A dummy fallback_inner_index shall be set when a half-NULLs
				 * tuple is constructed on LEFT/FULL OUTER JOIN. It means this
				 * depth has no more capable to fetch next joined rows.
				 */
				Assert(istate->join_type == JOIN_LEFT ||
					   istate->join_type == JOIN_FULL);
				return false;
			}

			/*
			 * Extract inner columns to the slot_fallback
			 */
		inner_fillup:
			gpujoin_fallback_tuple_extract(slot_fallback,
										   tupdesc,
										   kds_in->table_oid,
										   tupitem,
										   istate->inner_dst_resno,
										   istate->inner_src_anum_min,
										   istate->inner_src_anum_max);
			/*
			 * Evaluation of the join_quals, if inner matched
			 */
			if (tupitem && !do_right_outer_join)
			{
				if (!ExecQual(istate->join_quals, econtext, false))
					continue;

				/* No RJ/FJ tuple is needed for this inner item */
				if (istate->join_type == JOIN_RIGHT ||
					istate->join_type == JOIN_FULL)
				{
					cl_bool	   *host_ojmaps = gj_sstate->h_ojmaps;

					Assert(host_ojmaps != NULL);
					host_ojmaps += gj_sstate->kern.chunks[depth-1].ojmap_offset;

					Assert(kds_index >= 0 && kds_index < kds_in->nitems);
					host_ojmaps[kds_index] = true;
				}
				/* No LJ/FJ tuple is needed for this outer item */
				istate->fallback_inner_matched = true;
			}

			/*
			 * Evaluation of the other_quals, if any
			 */
			if (!ExecQual(istate->other_quals, econtext, false))
				continue;

			/* Rewind the position of deeper levels */
			for (i = depth; i < gjs->num_rels; i++)
			{
				gjs->inners[i].fallback_inner_index = -1;
				gjs->inners[i].fallback_right_outer = false;
			}
		}

		/*
		 * Walk down into the next depth, if we have deeper level any more.
		 * If no more rows in deeper level, rewind them and try to pick up
		 * next tuple in this level.
		 */
		if (depth < gjs->num_rels &&
			!gpujoin_fallback_inner_recurse(gjs, slot_fallback,
											pgjoin, depth + 1,
											istate->fallback_right_outer))
		{
			reload_inner_next = true;
			continue;
		}
		break;
	}
	return true;
}

static TupleTableSlot *
gpujoin_next_tuple_fallback(GpuJoinState *gjs, GpuJoinTask *pgjoin)
{
	ExprContext		   *econtext = gjs->gts.css.ss.ps.ps_ExprContext;
	TupleDesc			tupdesc;
	ExprDoneCond		is_done;

	/* tuple descriptor of the outer relation */
	if (gjs->gts.css.ss.ss_currentRelation)
		tupdesc = RelationGetDescr(gjs->gts.css.ss.ss_currentRelation);
	else
		tupdesc = outerPlanState(gjs)->ps_ResultTupleSlot->tts_tupleDescriptor;

	/*
	 * tuple-table-slot to be constructed by CPU fallback.
	 *
	 * MEMO: For performance benefit, we reuse the contents of tts_values
	 * and tts_isnull unless its source tuple is not reloaded. The prior
	 * execution may create slot_fallback->tts_tuple based on the old values,
	 * so we have to clear it for each iteration. ExecClearTuple() also set
	 * zero on tts_nvalid, not only release of tts_tuple, so we enlarge
	 * 'tts_nvalid' by ExecStoreVirtualTuple(); which does not touch values
	 * of tts_values/tts_isnull.
	 */
	Assert(gjs->slot_fallback != NULL);
	ExecClearTuple(gjs->slot_fallback);
	ExecStoreVirtualTuple(gjs->slot_fallback);

	if (pgjoin->pds_src)
	{
		kern_data_store	   *kds_src = &pgjoin->pds_src->kds;
		kern_join_scale	   *jscale = pgjoin->kern.jscale;
		kern_tupitem	   *tupitem;
		bool				reload_outer_next;

		reload_outer_next = (gjs->fallback_outer_index < 0);
		for (;;)
		{
			econtext->ecxt_scantuple = gjs->slot_fallback;
			ResetExprContext(econtext);

			if (reload_outer_next)
			{
				cl_uint		i, kds_index;
				cl_uint		nvalids;

				kds_index = Max(jscale->window_orig,
								gjs->fallback_outer_index + 1);
				/* Do we still have any other rows more? */
				nvalids = Min(kds_src->nitems,
							  jscale->window_base + jscale->window_size);
				if (kds_index >= nvalids)
				{
					gpujoinPutInnerBuffer(pgjoin->gj_sstate,
										  gpujoin_outerjoin_kicker, pgjoin);
					pgjoin->gj_sstate = NULL;
					return NULL;
				}
				gjs->fallback_outer_index = kds_index;

				/* Fills up fields of the fallback_slot with outer columns */
				tupitem = KERN_DATA_STORE_TUPITEM(kds_src, kds_index);
				gpujoin_fallback_tuple_extract(gjs->slot_fallback,
											   tupdesc,
											   kds_src->table_oid,
											   tupitem,
											   gjs->outer_dst_resno,
											   gjs->outer_src_anum_min,
											   gjs->outer_src_anum_max);
				/* evaluation of the outer qual if any */
				if (!ExecQual(gjs->outer_quals, econtext, false))
					continue;
				/* ok, rewind the deeper levels prior to walk down */
				for (i=0; i < gjs->num_rels; i++)
				{
					gjs->inners[i].fallback_inner_index = -1;
					gjs->inners[i].fallback_right_outer = false;
				}
			}

			/* walk down to the deeper depth */
			if (!gpujoin_fallback_inner_recurse(gjs, gjs->slot_fallback,
												pgjoin, 1, false))
			{
				reload_outer_next = true;
				continue;
			}
			break;
		}
	}
	else
	{
		/*
		 * pds_src == NULL means the final chunk of RIGHT/FULL OUTER JOIN.
		 * We have to fill up outer columns with NULLs, then walk down into
		 * the inner depths.
		 */
		econtext->ecxt_scantuple = gjs->slot_fallback;
		ResetExprContext(econtext);

		if (gjs->fallback_outer_index < 0)
		{
			gpujoin_fallback_tuple_extract(gjs->slot_fallback,
										   tupdesc,
										   InvalidOid,
										   NULL,
										   gjs->outer_dst_resno,
										   gjs->outer_src_anum_min,
										   gjs->outer_src_anum_max);
			gjs->fallback_outer_index = 0;
			/* XXX - Do we need to rewind inners? Likely, No */
			/* gpujoin_switch_task() should rewind them already */
		}
		/* walk down into the deeper depth */
		if (!gpujoin_fallback_inner_recurse(gjs, gjs->slot_fallback,
											pgjoin, 1, true))
		{
			gpujoinPutInnerBuffer(pgjoin->gj_sstate,
								  gpujoin_outerjoin_kicker, pgjoin);
			pgjoin->gj_sstate = NULL;
			return NULL;
		}
	}

	Assert(!TupIsNull(gjs->slot_fallback));
	if (gjs->proj_fallback)
		return ExecProject(gjs->proj_fallback, &is_done);

	return gjs->slot_fallback;	/* no projection is needed? */
}

/* ----------------------------------------------------------------
 *
 * Routines to support unified GpuPreAgg + GpuJoin
 *
 * ----------------------------------------------------------------
 */
ProgramId
GpuJoinCreateUnifiedProgram(PlanState *node,
							GpuTaskState *gpa_gts,
							cl_uint gpa_extra_flags,
							const char *gpa_kern_source)
{
	GpuJoinState   *gjs = (GpuJoinState *) node;
	GpuJoinInfo	   *gj_info;
	StringInfoData	kern_define;
	StringInfoData	kern_source;
	cl_uint			extra_flags;
	ProgramId		program_id;

	initStringInfo(&kern_define);
	initStringInfo(&kern_source);

	gj_info = deform_gpujoin_info((CustomScan *) gjs->gts.css.ss.ps.plan);
	extra_flags = (gpa_extra_flags | gj_info->extra_flags);
	pgstrom_build_session_info(&kern_define,
							   gpa_gts,
							   extra_flags & ~DEVKERNEL_NEEDS_GPUJOIN);
	assign_gpujoin_session_info(&kern_define, &gjs->gts);

	appendStringInfoString(&kern_source,
						   "/* ======== BEGIN GpuJoin Portion ======== */");
	appendStringInfoString(&kern_source, gj_info->kern_source);
	appendStringInfoString(&kern_source,
						   "/* ======== BEGIN GpuPreAgg Portion ======== */");
	appendStringInfoString(&kern_source, gpa_kern_source);

	program_id = pgstrom_create_cuda_program(gpa_gts->gcontext,
											 extra_flags,
											 kern_source.data,
											 kern_define.data,
											 false);
	pfree(kern_source.data);
	pfree(kern_define.data);

	return program_id;
}

GpuJoinSharedState *
GpuJoinInnerPreload(PlanState *node)
{
	GpuJoinState   *gjs = (GpuJoinState *) node;

	Assert(pgstrom_planstate_is_gpujoin(node));
	if (!gjs->gj_sstate)
		gjs->gj_sstate = createGpuJoinSharedState(gjs);
	if (!gpujoin_inner_preload(gjs))
		return NULL;	/* obviously GpuJoin produces an empty result */
	return gjs->gj_sstate;
}

/* ----------------------------------------------------------------
 *
 * GpuTask handlers of GpuJoin
 *
 * ----------------------------------------------------------------
 */
void
gpujoin_release_task(GpuTask *gtask)
{
	GpuJoinTask	   *pgjoin = (GpuJoinTask *) gtask;

	/* detach multi-relations buffer, if any */
	if (pgjoin->gj_sstate)
		gpujoinPutInnerBuffer(pgjoin->gj_sstate,
							  gpujoin_outerjoin_kicker, pgjoin);
	/* unlink source data store */
	if (pgjoin->pds_src)
		PDS_release(pgjoin->pds_src);
	/* unlink destination data store */
	if (pgjoin->pds_dst)
		PDS_release(pgjoin->pds_dst);
	/* release this gpu-task itself */
	dmaBufferFree(pgjoin);
}

static void
update_runtime_statistics(GpuJoinTask *pgjoin)
{
	pgstrom_data_store *pds_dst = pgjoin->pds_dst;
	GpuJoinSharedState *gj_sstate = pgjoin->gj_sstate;
	kern_join_scale	   *jscale = pgjoin->kern.jscale;
	cl_int				i, num_rels = gj_sstate->kern.nrels;

	if (pgjoin->task.kerror.errcode != StromError_Success)
		return;

	SpinLockAcquire(&gj_sstate->lock);
	gj_sstate->source_ntasks++;
	gj_sstate->source_nitems += (jscale[0].window_base +
								 jscale[0].window_size -
								 jscale[0].window_orig);
	for (i=0; i <= num_rels; i++)
	{
		gj_sstate->inner_nitems[i] += jscale[i].inner_nitems;
		gj_sstate->right_nitems[i] += jscale[i].right_nitems;
		if (jscale[i].row_dist_score > 0.0)
		{
			gj_sstate->row_dist_score_valid = true;
			gj_sstate->row_dist_score[i] += jscale[i].row_dist_score;
		}
	}
	gj_sstate->results_nitems += pds_dst->kds.nitems;
	gj_sstate->results_usage += pds_dst->kds.usage;
	SpinLockRelease(&gj_sstate->lock);
}

static bool
gpujoin_process_kernel(GpuJoinTask *pgjoin, CUmodule cuda_module)
{
	GpuContext		   *gcontext = pgjoin->task.gcontext;
	GpuJoinSharedState *gj_sstate = pgjoin->gj_sstate;
	pgstrom_data_store *pds_src = pgjoin->pds_src;
	pgstrom_data_store *pds_dst = pgjoin->pds_dst;
	CUfunction		kern_main;
	CUdeviceptr		m_kgjoin = 0UL;
	CUdeviceptr		m_kmrels = 0UL;
	CUdeviceptr		m_ojmaps = 0UL;
	CUdeviceptr		m_kds_src = 0UL;
	CUdeviceptr		m_kds_dst = 0UL;
	Size			offset;
	Size			length;
	Size			pos;
	CUresult		rc;
	void		   *kern_args[10];

	/* sanity checks */
	Assert(!pds_src || (pds_src->kds.format == KDS_FORMAT_ROW ||
						pds_src->kds.format == KDS_FORMAT_BLOCK));
	Assert(pds_dst->kds.format == KDS_FORMAT_ROW ||
		   pds_dst->kds.format == KDS_FORMAT_SLOT);

	/* Lookup GPU kernel function */
	rc = cuModuleGetFunction(&kern_main, cuda_module, "gpujoin_main");
	if (rc != CUDA_SUCCESS)
		werror("failed on cuModuleGetFunction: %s", errorText(rc));

	/* 
	 * Device memory allocation
	 */
	length = pos = GPUMEMALIGN(pgjoin->kern.kresults_2_offset +
							   pgjoin->kern.kresults_2_offset -
							   pgjoin->kern.kresults_1_offset);
	if (pgjoin->with_nvme_strom)
	{
		Assert(pds_src->kds.format == KDS_FORMAT_BLOCK);
		rc = gpuMemAllocIOMap(pgjoin->task.gcontext,
							  &m_kds_src,
							  GPUMEMALIGN(pds_src->kds.length));
		if (rc == CUDA_ERROR_OUT_OF_MEMORY)
		{
			PDS_fillup_blocks(pds_src, pgjoin->task.peer_fdesc);
			pgjoin->with_nvme_strom = false;
			length += GPUMEMALIGN(pds_src->kds.length);
		}
		else if (rc != CUDA_SUCCESS)
			werror("failed on gpuMemAllocIOMap: %s", errorText(rc));
	}
	else if (pds_src)
		length += GPUMEMALIGN(pds_src->kds.length);

	length += GPUMEMALIGN(pds_dst->kds.length);

	rc = gpuMemAlloc(gcontext, &m_kgjoin, length);
	if (rc == CUDA_ERROR_OUT_OF_MEMORY)
		goto out_of_resource;
	else if (rc != CUDA_SUCCESS)
		werror("failed on gpuMemAlloc: %s", errorText(rc));

	if (pds_src && m_kds_src == 0UL)
	{
		m_kds_src = m_kgjoin + pos;
		pos += GPUMEMALIGN(pds_src->kds.length);
	}
	m_kds_dst = m_kgjoin + pos;

	/* inner hash/heap buffer should exist */
	m_kmrels = gj_sstate->m_kmrels[gcontext->gpuserv_id];
	m_ojmaps = gj_sstate->m_ojmaps[gcontext->gpuserv_id];

	/*
	 * OK, all the device memory and kernel objects are successfully
	 * constructed. Let's enqueue DMA send/recv and kernel invocations.
	 */

	/* kern_gpujoin + static portion of kern_resultbuf */
	length = KERN_GPUJOIN_HEAD_LENGTH(&pgjoin->kern);
	rc = cuMemcpyHtoD(m_kgjoin,
					  &pgjoin->kern,
					  length);
	if (rc != CUDA_SUCCESS)
		werror("failed on cuMemcpyHtoD: %s", errorText(rc));

	if (pds_src)
	{
		/* source outer relation */
		if (!pgjoin->with_nvme_strom)
		{
			rc = cuMemcpyHtoD(m_kds_src,
							  &pds_src->kds,
							  pds_src->kds.length);
			if (rc != CUDA_SUCCESS)
				werror("failed on cuMemcpyHtoD: %s", errorText(rc));
		}
		else
		{
			Assert(pds_src->kds.format == KDS_FORMAT_BLOCK);
			gpuMemCopyFromSSD(&pgjoin->task,
							  m_kds_src,
							  pds_src);
		}
	}

	/* kern_data_store (dst of head) */
	rc = cuMemcpyHtoD(m_kds_dst,
					  &pds_dst->kds,
					  pds_dst->kds.length);
	if (rc != CUDA_SUCCESS)
		werror("failed on cuMemcpyHtoD: %s", errorText(rc));

	/* Lunch:
	 * KERNEL_FUNCTION(void)
	 * gpujoin_main(kern_gpujoin *kgjoin,
	 *              kern_multirels *kmrels,
	 *              cl_bool *outer_join_map,
	 *              kern_data_store *kds_src,
	 *              kern_data_store *kds_dst,
	 *              cl_int cuda_index)
	 */
	kern_args[0] = &m_kgjoin;
	kern_args[1] = &m_kmrels;
	kern_args[2] = &m_ojmaps;
	kern_args[3] = &m_kds_src;
	kern_args[4] = &m_kds_dst;
	kern_args[5] = &gpuserv_cuda_dindex;

	rc = cuLaunchKernel(kern_main,
						1, 1, 1,
						1, 1, 1,
						sizeof(kern_errorbuf),
						NULL,
						kern_args,
						NULL);
	if (rc != CUDA_SUCCESS)
		werror("failed on cuLaunchKernel: %s", errorText(rc));

	/* DMA Recv: kern_gpujoin *kgjoin */
	length = offsetof(kern_gpujoin, jscale[pgjoin->kern.num_rels + 1]);
	rc = cuMemcpyDtoH(&pgjoin->kern,
					  m_kgjoin,
					  length);
	if (rc != CUDA_SUCCESS)
		werror("cuMemcpyDtoH: %s", errorText(rc));

	/* DMA Recv: kern_data_store *kds_dst */
	rc = cuMemcpyDtoH(&pds_dst->kds,
					  m_kds_dst,
					  pds_dst->kds.length);
	if (rc != CUDA_SUCCESS)
		werror("cuMemcpyDtoH: %s", errorText(rc));

#if 1
	/*
	 * DMA Recv: kern_data_store *kds_src, if NVMe-Strom is used and join
	 * results contains varlena/indirect datum
	 * XXX - only if KDS_FORMAT_SLOT?
	 */
	if (pds_src &&
		pds_src->kds.format == KDS_FORMAT_BLOCK &&
		pds_src->nblocks_uncached > 0 &&
		pds_dst->kds.has_notbyval)
	{
		cl_uint	nr_loaded = pds_src->kds.nitems - pds_src->nblocks_uncached;

		offset = ((char *)KERN_DATA_STORE_BLOCK_PGPAGE(&pds_src->kds,
                                                       nr_loaded) -
				  (char *)&pds_src->kds);
		length = pds_src->nblocks_uncached * BLCKSZ;
		rc = cuMemcpyDtoH((char *)&pds_src->kds + offset,
						  m_kds_src + offset,
						  length);
		if (rc != CUDA_SUCCESS)
			werror("failed on cuMemcpyHtoD: %s", errorText(rc));

		/*
		 * NOTE: Once GPU-to-RAM DMA gets completed, "uncached" blocks are
		 * filled up with valid blocks, so we can clear @nblocks_uncached
		 * not to write back GPU RAM twice even if CPU fallback.
		 */
		pds_src->nblocks_uncached = 0;
	}
#endif
	/* release CUDA resources */
	if (pgjoin->with_nvme_strom && m_kds_src)
		gpuMemFreeIOMap(gcontext, m_kds_src);
	if (m_kgjoin)
		gpuMemFree(gcontext, m_kgjoin);

	/*
	 * Clear the error code if CPU fallback case.
	 * Elsewhere, update run-time statistics.
	 */
	pgjoin->task.kerror = pgjoin->kern.kerror;
	if (pgstrom_cpu_fallback_enabled &&
		pgjoin->task.kerror.errcode == StromError_CpuReCheck)
	{
		pgjoin->task.kerror.errcode = StromError_Success;
		pgjoin->task.cpu_fallback = true;
		gj_sstate->had_cpu_fallback = true;
	}
	else
	{
		update_runtime_statistics(pgjoin);
	}
	return true;

out_of_resource:
	if (pgjoin->with_nvme_strom && m_kds_src)
		gpuMemFreeIOMap(gcontext, m_kds_src);
	if (m_kgjoin)
		gpuMemFree(gcontext, m_kgjoin);
	return false;
}

/*
 * gpujoin_try_rerun_kernel
 *
 * It checks window frame of the last GpuJoin kernel. If entire window was
 * not processed, we need to kick next kernel.
 */
static bool
gpujoin_try_rerun_kernel(GpuJoinTask *pgjoin, CUmodule cuda_module)
{
	GpuContext		   *gcontext = pgjoin->task.gcontext;
	SharedGpuContext   *shgcon = gcontext->shgcon;
	GpuJoinSharedState *gj_sstate = pgjoin->gj_sstate;
	kern_join_scale	   *jscale = pgjoin->kern.jscale;
	pgstrom_data_store *pds;
	cl_uint				nitems;
	int					i, j, num_rels = pgjoin->kern.num_rels;

	for (i=num_rels; i >=0; i--)
	{
		if (i == 0)
			pds = pgjoin->pds_src;
		else
			pds = gj_sstate->inner_chunks[i-1];
		nitems = (!pds ? 0 : pds->kds.nitems);

	   	if (jscale[i].window_base + jscale[i].window_size < nitems)
		{
			cl_uint		window_base = jscale[i].window_base;
			cl_uint		window_size = jscale[i].window_size;
			cl_uint		window_orig = jscale[i].window_orig;
		  	GpuJoinTask *resp;

			/*
			 * NOTE: Pay attention on a corner case - if CpuReCheck happen
			 * on RIGHT/FULL OUTER JOIN, we cannot continue asynchronous
			 * task execution no longer.
			 * Because outer-join-map can be updated during the execution
			 * of GpuJoin kernel with no valid PDS/KDS.
			 * For example, when depth=2 and depth=4 are RIGHT JOIN, depth=2
			 * will produce half-NULL tuples according to the outer-join-map.
			 * Then, these tuples shall be processed in the depth=3 and can
			 * also update the outer-join-map in the depth=4.
			 * Once a part of GpuJoin is processed on CPU or other GPU device,
			 * we have to synchronize its completion and co-location of the
			 * outer-join-map again. It usually makes less sense, so we move
			 * entire process to CPU fallback.
			 */
			if (!pgjoin->pds_src && pgjoin->task.cpu_fallback)
			{
				for (j=0; j < num_rels; j++)
				{
					pds = gj_sstate->inner_chunks[j];
					jscale[i+1].window_size = (pds->kds.nitems -
											   jscale[i+1].window_base);
				}
				break;
			}

			/*
			 * setup a responder to deliver the partial result of GpuJoin
			 */
			resp = gpujoin_clone_task(pgjoin);
			resp->pds_dst = pgjoin->pds_dst;
			pgjoin->pds_dst = NULL;

			/*
			 * Send back the responder
			 */
			SpinLockAcquire(&shgcon->lock);
			shgcon->num_async_tasks++;
			SpinLockRelease(&shgcon->lock);
			gpuservSendGpuTask(gcontext, &resp->task);

			/*
			 * Move to the next window frame
			 */
			jscale[i].window_base = (window_base + window_size);
			jscale[i].window_size = (window_base + window_size - window_orig);
			jscale[i].window_orig = jscale[i].window_base;
			if (jscale[i].window_base + jscale[i].window_size > nitems)
				jscale[i].window_size = nitems - jscale[i].window_base;
			for (j=i+1; j <= num_rels; j++)
			{
				pds = gj_sstate->inner_chunks[j-1];
				jscale[j].window_base = 0;
				jscale[j].window_size = pds->kds.nitems;
				jscale[j].window_orig = 0;
			}
			return true;	/* run next */
		}
		Assert(jscale[i].window_base + jscale[i].window_size == nitems);
	}
	return false;
}

int
gpujoin_process_task(GpuTask *gtask, CUmodule cuda_module)
{
	GpuJoinTask	   *pgjoin = (GpuJoinTask *) gtask;

	/* Ensure inner hash/heap buffe is loaded */
	gpujoinLoadInnerBuffer(pgjoin->task.gcontext,
						   pgjoin->gj_sstate);
	/* Terminator task skips jobs */
	if (pgjoin->is_terminator)
		goto out;

	/* Attach destination buffer on demand. */
	if (!pgjoin->pds_dst)
	{
		pgstrom_data_store *pds;
		kern_data_store	   *kds_head = pgjoin->kds_dst_head;

		Assert(kds_head->format == KDS_FORMAT_ROW);
		pds = dmaBufferAlloc(gtask->gcontext, kds_head->length);
		if (!pds)
			return 1;	/* out of resource */

		pg_atomic_init_u32(&pds->refcnt, 1);
		pds->nblocks_uncached = 0;
		pds->ntasks_running = 0;
		memcpy(&pds->kds, kds_head, offsetof(kern_data_store,
											 colmeta[kds_head->ncols]));
		pgjoin->pds_dst = pds;
	}

	do {
		if (!gpujoin_process_kernel(pgjoin, cuda_module))
			return 100001;	/* out of resource; 100ms delay */
		if (pgjoin->task.kerror.errcode != StromError_Success)
			break;			/* deliver the error status */
	} while (gpujoin_try_rerun_kernel(pgjoin, cuda_module));

	/*
	 * Note that inner hash/heap buffer shall be detached on backend side
	 * if CPU fallback is required.
	 */
out:
	if (!pgjoin->task.cpu_fallback)
	{
		gpujoinPutInnerBuffer(pgjoin->gj_sstate,
							  gpujoin_outerjoin_kicker, pgjoin);
		pgjoin->gj_sstate = NULL;
	}
	return 0;
}

/* ================================================================
 *
 * Routines to preload inner relations (heap/hash)
 *
 * ================================================================
 */

/*
 * add_extra_randomness
 *
 * BUG#211 - In case when we have to split inner relations virtually,
 * extra randomness is significant to avoid singularity. In theorem,
 * rowid of KDS (assigned sequentially on insertion) is independent
 * concept from the join key. However, people usually insert tuples
 * according to the key value (referenced by join) sequentially.
 * It eventually leads unexpected results - A particular number of
 * outer rows generates unexpected number of results rows. Even if
 * CPU reduced inner_size according to the run-time statistics, retry
 * shall be repeated until the virtual inner relation boundary goes
 * across the problematic key value.
 * This extra randomness makes distribution of the join keys flatten.
 * Because rowid of KDS items are randomized, we can expect reduction
 * of inner_size[] will reduce scale of the join result as expectation
 * of statistical result.
 *
 * NOTE: we may be able to add this extra randomness only when inner_size
 * is smaller than kds->nitems and not yet randomized. However, we also
 * pay attention the case when NVRTC support dynamic parallelism then
 * GPU kernel get capability to control inner_size[] inside GPU kernel.
 */
static void
add_extra_randomness(pgstrom_data_store *pds)
{
	kern_data_store	   *kds = &pds->kds;
	cl_uint				x, y, temp;

	return;		//????

	if (pds->kds.format == KDS_FORMAT_ROW ||
		pds->kds.format == KDS_FORMAT_HASH)
	{
		cl_uint	   *row_index = KERN_DATA_STORE_ROWINDEX(kds);
		cl_uint		nitems = pds->kds.nitems;

		for (x=0; x < nitems; x++)
		{
			y = rand() % nitems;
			if (x == y)
				continue;

			if (pds->kds.format == KDS_FORMAT_HASH)
			{
				kern_hashitem  *khitem_x = KERN_DATA_STORE_HASHITEM(kds, x);
				kern_hashitem  *khitem_y = KERN_DATA_STORE_HASHITEM(kds, y);
				Assert(khitem_x->rowid == x);
				Assert(khitem_y->rowid == y);
				khitem_x->rowid = y;	/* swap */
				khitem_y->rowid = x;	/* swap */
			}
			temp = row_index[x];
			row_index[x] = row_index[y];
			row_index[y] = temp;
		}
	}
	else
		elog(ERROR, "Unexpected data chunk format: %u", kds->format);
}

/*
 * calculation of the hash-value
 */
static pg_crc32
get_tuple_hashvalue(innerState *istate,
					bool is_inner_hashkeys,
					TupleTableSlot *slot,
					bool *p_is_null_keys)
{
	ExprContext	   *econtext = istate->econtext;
	pg_crc32		hash;
	List		   *hash_keys_list;
	ListCell	   *lc1;
	ListCell	   *lc2;
	ListCell	   *lc3;
	ListCell	   *lc4;
	bool			is_null_keys = true;

	if (is_inner_hashkeys)
	{
		hash_keys_list = istate->hash_inner_keys;
		econtext->ecxt_innertuple = slot;
	}
	else
	{
		hash_keys_list = istate->hash_outer_keys;
		econtext->ecxt_scantuple = slot;
	}

	/* calculation of a hash value of this entry */
	INIT_LEGACY_CRC32(hash);
	forfour (lc1, hash_keys_list,
			 lc2, istate->hash_keylen,
			 lc3, istate->hash_keybyval,
			 lc4, istate->hash_keytype)
	{
		ExprState  *clause = lfirst(lc1);
		int			keylen = lfirst_int(lc2);
		bool		keybyval = lfirst_int(lc3);
		Oid			keytype = lfirst_oid(lc4);
		Datum		value;
		bool		isnull;

		value = ExecEvalExpr(clause, istate->econtext, &isnull, NULL);
		if (isnull)
			continue;
		is_null_keys = false;	/* key is non-NULL valid */

		/* fixup host representation to special internal format. */
		if (keytype == NUMERICOID)
		{
			kern_context	dummy;
			pg_numeric_t	temp;

			/*
			 * FIXME: If NUMERIC value is out of range, we cannot execute
			 * GpuJoin in the kernel space, so needs a fallback routine.
			 */
			temp = pg_numeric_from_varlena(&dummy, (struct varlena *)
										   DatumGetPointer(value));
			COMP_LEGACY_CRC32(hash, &temp.value, sizeof(temp.value));
		}
		else if (keytype == BPCHAROID)
		{
			/*
			 * whitespace is the tail end of CHAR(n) data shall be ignored
			 * when we calculate hash-value, to match same text exactly.
			 */
			cl_char	   *s = VARDATA_ANY(value);
			cl_int		i, len = VARSIZE_ANY_EXHDR(value);

			for (i = len - 1; i >= 0 && s[i] == ' '; i--)
				;
			COMP_LEGACY_CRC32(hash, VARDATA_ANY(value), i+1);
		}
		else if (keybyval)
			COMP_LEGACY_CRC32(hash, &value, keylen);
		else if (keylen > 0)
			COMP_LEGACY_CRC32(hash, DatumGetPointer(value), keylen);
		else
			COMP_LEGACY_CRC32(hash,
							  VARDATA_ANY(value),
							  VARSIZE_ANY_EXHDR(value));
	}
	FIN_LEGACY_CRC32(hash);

	*p_is_null_keys = is_null_keys;

	return hash;
}

/*
 * gpujoin_inner_hash_preload
 *
 * Preload inner relation to the data store with hash-format, for hash-
 * join execution.
 */
static pgstrom_data_store *
gpujoin_inner_hash_preload(GpuJoinState *gjs, innerState *istate)
{
	PlanState		   *scan_ps = istate->state;
	TupleTableSlot	   *scan_slot;
	TupleDesc			scan_desc;
	pgstrom_data_store *pds_hash = NULL;
	pg_crc32			hash;
	bool				is_null_keys;
	Size				length;

	for (;;)
	{
		scan_slot = ExecProcNode(istate->state);
		if (TupIsNull(scan_slot))
			break;

		(void)ExecFetchSlotTuple(scan_slot);
		hash = get_tuple_hashvalue(istate, true, scan_slot,
								   &is_null_keys);
		/*
		 * If join keys are NULLs, it is obvious that inner tuple shall not
		 * match with outer tuples. Unless it is not referenced in outer join,
		 * we don't need to keep this tuple in the 
		 */
		if (is_null_keys && (istate->join_type == JOIN_INNER ||
							 istate->join_type == JOIN_LEFT))
			continue;

		scan_desc = scan_slot->tts_tupleDescriptor;
		if (!pds_hash)
		{
			pds_hash = PDS_create_hash(gjs->gts.gcontext,
									   scan_desc,
									   Max(istate->ichunk_size,
										   pgstrom_chunk_size() / 4));

		}

		while (!PDS_insert_hashitem(pds_hash, scan_slot, hash))
		{
			pds_hash = PDS_expand_size(gjs->gts.gcontext,
									   pds_hash,
									   pds_hash->kds.length * 2);
		}
	}
	/* a dummy empty hash table if no rows read */
	if (!pds_hash)
	{
		scan_slot = scan_ps->ps_ResultTupleSlot;
		scan_desc = scan_slot->tts_tupleDescriptor;
		length = KDS_CALCULATE_HASH_LENGTH(scan_desc->natts, 0, 0);
		pds_hash = PDS_create_hash(gjs->gts.gcontext,
								   scan_desc,
								   length);
	}
	/* add extra randomness for better key distribution */
	add_extra_randomness(pds_hash);
	PDS_build_hashtable(pds_hash);

	return pds_hash;
}

/*
 * gpujoin_inner_heap_preload
 *
 * Preload inner relation to the data store with row-format, for nested-
 * loop execution.
 */
static pgstrom_data_store *
gpujoin_inner_heap_preload(GpuJoinState *gjs, innerState *istate)
{
	PlanState	   *scan_ps = istate->state;
	TupleTableSlot *scan_slot;
	TupleDesc		scan_desc;
	Size			length;
	pgstrom_data_store *pds_heap = NULL;

	for (;;)
	{
		scan_slot = ExecProcNode(scan_ps);
		if (TupIsNull(scan_slot))
			break;

		scan_desc = scan_slot->tts_tupleDescriptor;
		if (!pds_heap)
		{
			length = Max(istate->ichunk_size,
						 pgstrom_chunk_size() / 4);
			pds_heap = PDS_create_row(gjs->gts.gcontext,
									  scan_desc,
									  Max(istate->ichunk_size,
										  pgstrom_chunk_size() / 4));
		}
		(void)ExecFetchSlotTuple(scan_slot);
		while (!PDS_insert_tuple(istate->pds_in, scan_slot))
		{
			pds_heap = PDS_expand_size(gjs->gts.gcontext,
									   pds_heap,
									   pds_heap->kds.length * 2);
		}
	}
	/* if no rows were read, create a small empty PDS */
	if (!pds_heap)
	{
		scan_slot = scan_ps->ps_ResultTupleSlot;
		scan_desc = scan_slot->tts_tupleDescriptor;
		length = STROMALIGN(offsetof(kern_data_store,
									 colmeta[scan_desc->natts]));
		pds_heap = PDS_create_row(gjs->gts.gcontext,
								  scan_desc,
								  length);
	}
	/* add extra randomness for better key distribution */
	add_extra_randomness(istate->pds_in);
	/* shrink unnecessary hole */
	PDS_shrink_size(istate->pds_in);

	return pds_heap;
}

/*
 * gpujoin_inner_preload
 *
 * It preload inner relation to the GPU DMA buffer once, even if larger
 * than device memory. If size is over the capacity, inner chunks are
 * splitted into multiple portions.
 */
static bool
__gpujoin_inner_preload(GpuJoinState *gjs)
{
	GpuJoinSharedState *gj_sstate = gjs->gj_sstate;
	int			i, num_rels = gjs->num_rels;
	Size		inner_usage = gj_sstate->total_length;
	Size		ojmap_usage = 0;

	Assert(!IsParallelWorker());

	/*
	 * Load inner relations
	 */
	for (i=0; i < num_rels; i++)
	{
		innerState *istate = &gjs->inners[i];

		if (!istate->pds_in)
			istate->pds_in = (istate->hash_inner_keys != NIL
							  ? gpujoin_inner_hash_preload(gjs, istate)
							  : gpujoin_inner_heap_preload(gjs, istate));
		gj_sstate->kern.chunks[i].chunk_offset = inner_usage;
		inner_usage += STROMALIGN(istate->pds_in->kds.length);

		if (!istate->hash_outer_keys)
			gj_sstate->kern.chunks[i].is_nestloop = true;

		if (istate->join_type == JOIN_RIGHT ||
			istate->join_type == JOIN_FULL)
		{
			gj_sstate->kern.chunks[i].right_outer = true;
			gj_sstate->kern.chunks[i].ojmap_offset = ojmap_usage;
			ojmap_usage += STROMALIGN(istate->pds_in->kds.nitems);
		}

		if (istate->join_type == JOIN_LEFT ||
			istate->join_type == JOIN_FULL)
		{
			gj_sstate->kern.chunks[i].left_outer = true;
		}
	}
	gj_sstate->total_length = inner_usage;

	/*
	 * NOTE: Special optimization case. In case when any chunk has no items,
	 * and all deeper level is inner join, it is obvious no tuples shall be
	 * produced in this GpuJoin. We can omit outer relation load that shall
	 * be eventually dropped.
	 */
	for (i=num_rels; i > 0; i--)
	{
		innerState	   *istate = &gjs->inners[i-1];

		/* outer join can produce something from empty */
		if (istate->join_type != JOIN_INNER)
			break;
		if (istate->pds_in->kds.nitems == 0)
			return false;
	}

	/* allocation of outer-join map if any */
	if (ojmap_usage > 0)
	{
		gj_sstate->kern.ojmap_length = ojmap_usage;
		gj_sstate->h_ojmaps = dmaBufferAlloc(gjs->gts.gcontext,
											 (numDevAttrs + 1) * ojmap_usage);
		if (!gj_sstate->h_ojmaps)
			elog(ERROR, "out of dma buffer");
	}
	/* PDS gets referenced by GpuJoinSharedState also */
	for (i=0; i < num_rels; i++)
		gj_sstate->inner_chunks[i] = PDS_retain(gjs->inners[i].pds_in);

	return true;
}

static bool
gpujoin_inner_preload(GpuJoinState *gjs)
{
	GpuJoinSharedState *gj_sstate = gjs->gj_sstate;
	uint32		preload_done;

	preload_done = pg_atomic_read_u32(&gj_sstate->preload_done);
	if (preload_done == 0)
	{
		if (!IsParallelWorker())
		{
			/* master process is responsible for inner preloading */
			if (__gpujoin_inner_preload(gjs))
				preload_done = 1;	/* valid inner buffer was loaded */
			else
				preload_done = 2;	/* no need to run GpuJoin actually */
			pg_atomic_write_u32(&gj_sstate->preload_done, preload_done);

			/* wake up parallel workers, if any */
			if (gjs->gts.pcxt)
			{
				ParallelContext *pcxt = gjs->gts.pcxt;
				pid_t		pid;
				int			i;

				for (i=0; i < pcxt->nworkers_launched; i++)
				{
					if (GetBackgroundWorkerPid(pcxt->worker[i].bgwhandle,
											   &pid) == BGWH_STARTED)
						ProcSendSignal(pid);
				}
			}
		}
		else
		{
			/* wait for the completion of inner preload by master */
			for (;;)
			{
				CHECK_FOR_INTERRUPTS();

				preload_done = pg_atomic_read_u32(&gj_sstate->preload_done);
				if (preload_done != 0)
					break;

				WaitLatch(&MyProc->procLatch, WL_LATCH_SET, -1);
				ResetLatch(&MyProc->procLatch);
			}
		}
	}
	Assert(preload_done > 0);
	return (preload_done == 1 ? true : false);
}

/*
 * createGpuJoinSharedState
 *
 * It construct an empty inner multi-relations buffer. It can be shared with
 * multiple backends, and referenced by CPU/GPU.
 */
static GpuJoinSharedState *
createGpuJoinSharedState(GpuJoinState *gjs)
{
	GpuContext	   *gcontext = gjs->gts.gcontext;
	ParallelContext *pcxt = gjs->gts.pcxt;
	GpuJoinSharedState *gj_sstate;
	cl_int			num_rels = gjs->num_rels;
	cl_int			pg_nworkers = (!pcxt ? 1 : pcxt->nworkers + 1);
	Size			head_length;
	Size			required;
	char		   *pos;

	/* calculate total length and allocate */
	head_length = STROMALIGN(offsetof(GpuJoinSharedState,
									  kern.chunks[num_rels]));
	required = head_length
		+ MAXALIGN(sizeof(pgstrom_data_store *) * num_rels)/* inner_chunks */
		+ MAXALIGN(sizeof(size_t) * (num_rels+1))		/* inner_nitems */
		+ MAXALIGN(sizeof(size_t) * (num_rels+1))		/* right_nitems */
		+ MAXALIGN(sizeof(cl_double) * (num_rels+1))	/* row_dist_score */
		+ MAXALIGN(sizeof(cl_int) * (numDevAttrs+1))	/* nr_tasks */
		+ MAXALIGN(sizeof(cl_bool) * pg_nworkers)		/* is_loaded */
		+ MAXALIGN(sizeof(CUdeviceptr) * numDevAttrs)	/* m_kmrels */
		+ MAXALIGN(sizeof(CUdeviceptr) * numDevAttrs);	/* m_ojmaps */

	gj_sstate= dmaBufferAlloc(gcontext, required);
	memset(gj_sstate, 0, required);
	pos = (char *)gj_sstate + head_length;

	pg_atomic_init_u32(&gj_sstate->preload_done, 0);
	gj_sstate->head_length = head_length;
	gj_sstate->total_length = head_length;
	gj_sstate->inner_chunks = (pgstrom_data_store **) pos;
	pos += MAXALIGN(sizeof(pgstrom_data_store *) * gjs->num_rels);

	/* run-time statistics */
	SpinLockInit(&gj_sstate->lock);
	gj_sstate->inner_nitems = (size_t *) pos;
	pos += MAXALIGN(sizeof(size_t) * (num_rels+1));
	gj_sstate->right_nitems = (size_t *) pos;
	pos += MAXALIGN(sizeof(size_t) * (num_rels+1));
	gj_sstate->row_dist_score = (cl_double *) pos;
	pos += MAXALIGN(sizeof(cl_double) * (num_rels+1));

	/* multi-processes coordination */
	pthreadMutexInit(&gj_sstate->mutex);
	gj_sstate->nr_tasks = (cl_int *) pos;
	pos += MAXALIGN(sizeof(int) * (numDevAttrs + 1));
	gj_sstate->is_loaded = (cl_bool *) pos;
	pos += MAXALIGN(sizeof(cl_bool) * pg_nworkers);
	gj_sstate->m_kmrels = (CUdeviceptr *) pos;
	pos += MAXALIGN(sizeof(CUdeviceptr) * numDevAttrs);
	gj_sstate->m_ojmaps = (CUdeviceptr *) pos;
	pos += MAXALIGN(sizeof(CUdeviceptr) * numDevAttrs);
	gj_sstate->h_ojmaps = NULL;	/* to be set on preload */

	/* kern_multirels */
	memcpy(gj_sstate->kern.pg_crc32_table,
		   pg_crc32_table,
		   sizeof(cl_uint) * 256);
	gj_sstate->kern.nrels = gjs->num_rels;
	gj_sstate->kern.ojmap_length = 0;

	return gj_sstate;
}

/*
 * gpujoinGetInnerBuffer
 */
static GpuJoinSharedState *
gpujoinGetInnerBuffer(GpuContext *gcontext, GpuJoinSharedState *gj_sstate)
{
	int		dindex = gcontext->gpuserv_id;

	pthreadMutexLock(&gj_sstate->mutex);
	Assert(gj_sstate->nr_tasks[dindex] >= 0);
	gj_sstate->nr_tasks[dindex]++;
	pthreadMutexUnlock(&gj_sstate->mutex);

	return gj_sstate;
}

/*
 * gpujoinPutInnerBuffer
 */
static void
gpujoinPutInnerBuffer(GpuJoinSharedState *gj_sstate,
					  void (*outerjoin_kicker)(GpuJoinSharedState *gj_sstate,
											   void *private),
					  void *private)
{
	bool	kick_outer_join = false;
	int		i, dindex;

	dindex = (gpuserv_cuda_dindex < 0 ? numDevAttrs : gpuserv_cuda_dindex);
	Assert(dindex >= 0 && dindex <= numDevAttrs);
	pthreadMutexLock(&gj_sstate->mutex);
	Assert(gj_sstate->nr_tasks[dindex] > 0);
	if (--gj_sstate->nr_tasks[dindex] == 0 &&	/* last task on the device? */
		gj_sstate->outer_scan_done &&	/* no more task will be produced? */
		gj_sstate->h_ojmaps != NULL)	/* outer join may happen? */
	{
		if (!gj_sstate->outer_join_kicked)
		{
			for (i=0; i <= numDevAttrs; i++)
			{
				if (gj_sstate->nr_tasks[i] > 0)
					goto no_outer_join;
			}
			kick_outer_join = true;
			gj_sstate->outer_join_kicked = true;
		}
	no_outer_join:
		/*
		 * In case when GpuJoin task is the last task on the current GPU
		 * device (so CPU fallback case is an exception), OUTER JOIN map
		 * must be written back for the colocation.
		 * If and when only single GPU device is installed, and no CPU
		 * fallback has happen yet, we can omit this step, because we
		 * already have OUTER JOIN map on the device with no other source.
		 */
		if (IsGpuServerProcess() &&
			(numDevAttrs > 1 || gj_sstate->had_cpu_fallback))
		{
			CUresult	rc;
			CUdeviceptr	m_ojmaps = gj_sstate->m_ojmaps[dindex];
			cl_bool	   *h_ojmaps = (gj_sstate->h_ojmaps +
									gj_sstate->kern.ojmap_length * dindex);

			rc = cuMemcpyDtoH(h_ojmaps,
							  m_ojmaps,
							  gj_sstate->kern.ojmap_length);
			if (rc != CUDA_SUCCESS)
			{
				pthreadMutexUnlock(&gj_sstate->mutex);
				werror("failed on cuMemcpyDtoH: %s", errorText(rc));
			}
		}
	}
	pthreadMutexUnlock(&gj_sstate->mutex);

	/*
	 * Clone GpuJoin task, then launch OUTER JOIN
	 */
	if (kick_outer_join)
		outerjoin_kicker(gj_sstate, private);
}

/*
 * gpujoinLoadInnerBuffer
 */
static bool
__gpujoinLoadInnerBuffer(GpuContext *gcontext, GpuJoinSharedState *gj_sstate)
{
	CUdeviceptr	m_kmrels = 0UL;
	CUdeviceptr	m_ojmaps = 0UL;
	CUresult	rc;
	int			i, dindex = gcontext->gpuserv_id;
	Size		offset;
	Size		length;

	/* device memory allocation */
	length = gj_sstate->total_length + gj_sstate->kern.ojmap_length;
	rc = gpuMemAlloc(gcontext, &m_kmrels, length);
	if (rc != CUDA_SUCCESS)
	{
		if (rc == CUDA_ERROR_OUT_OF_MEMORY)
			return false;
		werror("failed on gpuMemAlloc: %s", errorText(rc));
	}
	if (gj_sstate->kern.ojmap_length > 0)
		m_ojmaps = m_kmrels + gj_sstate->kern.ojmap_length;

	/* DMA Send: kernel inner hash/heap buffer */
	rc = cuMemcpyHtoD(m_kmrels, &gj_sstate->kern, gj_sstate->head_length);
	if (rc != CUDA_SUCCESS)
		werror("failed on cuMemcpyHtoD: %s", errorText(rc));

	for (i=0; i < gj_sstate->kern.nrels; i++)
	{
		pgstrom_data_store *pds = gj_sstate->inner_chunks[i];

		offset = gj_sstate->kern.chunks[i].chunk_offset;
		rc = cuMemcpyHtoD(m_kmrels + offset, &pds->kds,
						  pds->kds.length);
		if (rc != CUDA_SUCCESS)
			werror("failed on cuMemcpyHtoD: %s", errorText(rc));
	}

	/* Zero clear of outer-join map, if any */
	if (m_ojmaps != 0UL)
	{
		rc = cuMemsetD32(m_ojmaps, 0, gj_sstate->kern.ojmap_length / 4);
		if (rc != CUDA_SUCCESS)
			werror("failed on cuMemcpyHtoD: %s", errorText(rc));
	}
	gj_sstate->m_kmrels[dindex]	= m_kmrels;
    gj_sstate->m_ojmaps[dindex]	= m_ojmaps;

	return true;
}

static bool
gpujoinLoadInnerBuffer(GpuContext *gcontext, GpuJoinSharedState *gj_sstate)
{
	int			pg_worker = gcontext->shgcon->pg_worker_index;
	int			dindex = gcontext->gpuserv_id;
	bool		retval = true;
	CUresult	rc;

	Assert(dindex == gpuserv_cuda_dindex);
	pthreadMutexLock(&gj_sstate->mutex);
	if (!gj_sstate->is_loaded[pg_worker])
	{
		/* no need to attach device memory twice */
		gj_sstate->is_loaded[pg_worker] = true;

		STROM_TRY();
		{
			if (gj_sstate->m_kmrels[dindex] == 0UL)
				retval = __gpujoinLoadInnerBuffer(gcontext, gj_sstate);
			else
			{
				/*
				 * Other concurrent session which shares the same GPU device
				 * might set up its device memory already. Just retain them.
				 */
				rc = gpuMemRetain(gcontext, gj_sstate->m_kmrels[dindex]);
				if (rc != CUDA_SUCCESS)
					werror("failed on gpuMemRetain: %s", errorText(rc));
			}
		}
		STROM_CATCH();
		{
			pthreadMutexUnlock(&gj_sstate->mutex);
			STROM_RE_THROW();
		}
		STROM_END_TRY();
	}
	pthreadMutexUnlock(&gj_sstate->mutex);
	return retval;
}

/*
 * releaseGpuJoinSharedState
 */
static void
releaseGpuJoinSharedState(GpuJoinState *gjs, bool is_rescan)
{
	GpuContext	   *gcontext = gjs->gts.gcontext;
	SharedGpuContext *shgcon = gcontext->shgcon;
	GpuJoinSharedState *gj_sstate = gjs->gj_sstate;
	CUdeviceptr		deviceptr = 0UL;
	CUresult		rc;
	int				i, pg_worker = shgcon->pg_worker_index;

	pthreadMutexLock(&gj_sstate->mutex);
	if (gj_sstate->is_loaded[pg_worker])
		deviceptr = gj_sstate->m_kmrels[gcontext->gpuserv_id];
	pthreadMutexUnlock(&gj_sstate->mutex);

	if (deviceptr != 0UL)
	{
		rc = gpuMemFree(gcontext, deviceptr);
		if (rc != CUDA_SUCCESS)
			elog(WARNING, "failed on gpuMemFree: %s", errorText(rc));
	}

	if (IsParallelWorker())
		return;
	/*
	 * At this point, all the backend workers (if any) already finished
	 * ExecEnd() methods, due to SynchronizeGpuContext().
	 * So, we can reset/release buffers safely.
	 */
	for (i=0; i < gj_sstate->kern.nrels; i++)
	{
		if (gj_sstate->inner_chunks[i])
			PDS_release(gj_sstate->inner_chunks[i]);
		gj_sstate->inner_chunks[i] = NULL;
	}

	if (gj_sstate->h_ojmaps)
		dmaBufferFree(gj_sstate->h_ojmaps);

	if (!is_rescan)
		dmaBufferFree(gj_sstate);
	else
	{
		ParallelContext *pcxt = gjs->gts.pcxt;
		int		pg_nworkers = (pcxt ? pcxt->nworkers + 1 : 1);

		pg_atomic_init_u32(&gj_sstate->preload_done, 0);
		gj_sstate->total_length = gj_sstate->head_length;
		memset(gj_sstate->nr_tasks, 0, sizeof(int) * (numDevAttrs + 1));
		memset(gj_sstate->is_loaded, 0, sizeof(bool) * pg_nworkers);
		memset(gj_sstate->m_kmrels, 0, sizeof(CUdeviceptr) * numDevAttrs);
		memset(gj_sstate->m_ojmaps, 0, sizeof(CUdeviceptr) * numDevAttrs);
		gj_sstate->h_ojmaps = NULL;
	}
}

/*
 * pgstrom_init_gpujoin
 *
 * Entrypoint of GpuJoin
 */
void
pgstrom_init_gpujoin(void)
{
	/* turn on/off gpunestloop */
	DefineCustomBoolVariable("pg_strom.enable_gpunestloop",
							 "Enables the use of GpuNestLoop logic",
							 NULL,
							 &enable_gpunestloop,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* turn on/off gpuhashjoin */
	DefineCustomBoolVariable("pg_strom.enable_gpuhashjoin",
							 "Enables the use of GpuHashJoin logic",
							 NULL,
							 &enable_gpuhashjoin,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* setup path methods */
	gpujoin_path_methods.CustomName				= "GpuJoin";
	gpujoin_path_methods.PlanCustomPath			= PlanGpuJoinPath;

	/* setup plan methods */
	gpujoin_plan_methods.CustomName				= "GpuJoin";
	gpujoin_plan_methods.CreateCustomScanState	= gpujoin_create_scan_state;
	RegisterCustomScanMethods(&gpujoin_plan_methods);

	/* setup exec methods */
	gpujoin_exec_methods.CustomName				= "GpuJoin";
	gpujoin_exec_methods.BeginCustomScan		= ExecInitGpuJoin;
	gpujoin_exec_methods.ExecCustomScan			= ExecGpuJoin;
	gpujoin_exec_methods.EndCustomScan			= ExecEndGpuJoin;
	gpujoin_exec_methods.ReScanCustomScan		= ExecReScanGpuJoin;
	gpujoin_exec_methods.MarkPosCustomScan		= NULL;
	gpujoin_exec_methods.RestrPosCustomScan		= NULL;
	gpujoin_exec_methods.EstimateDSMCustomScan  = ExecGpuJoinEstimateDSM;
	gpujoin_exec_methods.InitializeDSMCustomScan = ExecGpuJoinInitDSM;
	gpujoin_exec_methods.InitializeWorkerCustomScan = ExecGpuJoinInitWorker;
	gpujoin_exec_methods.ExplainCustomScan		= ExplainGpuJoin;

	/* hook registration */
	set_join_pathlist_next = set_join_pathlist_hook;
	set_join_pathlist_hook = gpujoin_add_join_path;
}

/**************************************************************************
 * 
 * Copyright 2007 VMware, Inc.
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/

/* Author:
 *    Keith Whitwell <keithw@vmware.com>
 */

#include "draw/draw_context.h"
#include "util/os_time.h"
#include "pipe/p_defines.h"
#include "util/u_memory.h"
#include "sp_context.h"
#include "sp_query.h"
#include "sp_state.h"

struct softpipe_query {
   unsigned type;
   uint64_t start;
   uint64_t end;
   struct pipe_query_data_so_statistics so;
   struct pipe_query_data_pipeline_statistics stats;
};


static struct softpipe_query *softpipe_query( struct pipe_query *p )
{
   return (struct softpipe_query *)p;
}

static struct pipe_query *
softpipe_create_query(struct pipe_context *pipe, 
		      unsigned type,
		      unsigned index)
{
   struct softpipe_query* sq;

   assert(type == PIPE_QUERY_OCCLUSION_COUNTER ||
          type == PIPE_QUERY_OCCLUSION_PREDICATE ||
          type == PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE ||
          type == PIPE_QUERY_TIME_ELAPSED ||
          type == PIPE_QUERY_SO_STATISTICS ||
          type == PIPE_QUERY_SO_OVERFLOW_PREDICATE ||
          type == PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE ||
          type == PIPE_QUERY_PRIMITIVES_EMITTED ||
          type == PIPE_QUERY_PRIMITIVES_GENERATED || 
          type == PIPE_QUERY_PIPELINE_STATISTICS ||
          type == PIPE_QUERY_GPU_FINISHED ||
          type == PIPE_QUERY_TIMESTAMP ||
          type == PIPE_QUERY_TIMESTAMP_DISJOINT);
   sq = CALLOC_STRUCT( softpipe_query );
   sq->type = type;

   return (struct pipe_query *)sq;
}


static void
softpipe_destroy_query(struct pipe_context *pipe, struct pipe_query *q)
{
   FREE(q);
}


static boolean
softpipe_begin_query(struct pipe_context *pipe, struct pipe_query *q)
{
   struct softpipe_context *softpipe = softpipe_context( pipe );
   assert(0);
   return true;
}


static bool
softpipe_end_query(struct pipe_context *pipe, struct pipe_query *q)
{
   struct softpipe_context *softpipe = softpipe_context( pipe );
   assert(0);
   return true;
}


/**
 * Called by rendering function to check rendering is conditional.
 * \return TRUE if we should render, FALSE if we should skip rendering
 */
boolean
softpipe_check_render_cond(struct softpipe_context *sp)
{
  return TRUE;
}


static void
softpipe_set_active_query_state(struct pipe_context *pipe, boolean enable)
{
}


void softpipe_init_query_funcs(struct softpipe_context *softpipe )
{
   softpipe->pipe.create_query = softpipe_create_query;
   softpipe->pipe.destroy_query = softpipe_destroy_query;
   softpipe->pipe.begin_query = softpipe_begin_query;
   softpipe->pipe.end_query = softpipe_end_query;
   //softpipe->pipe.get_query_result = softpipe_get_query_result;
   softpipe->pipe.set_active_query_state = softpipe_set_active_query_state;
}



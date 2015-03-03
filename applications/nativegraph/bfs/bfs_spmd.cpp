////////////////////////////////////////////////////////////////////////
// This file is part of Grappa, a system for scaling irregular
// applications on commodity clusters. 

// Copyright (C) 2010-2014 University of Washington and Battelle
// Memorial Institute. University of Washington authorizes use of this
// Grappa software.

// Grappa is free software: you can redistribute it and/or modify it
// under the terms of the Affero General Public License as published
// by Affero, Inc., either version 1 of the License, or (at your
// option) any later version.

// Grappa is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// Affero General Public License for more details.

// You should have received a copy of the Affero General Public
// License along with this program. If not, you may obtain one from
// http://www.affero.org/oagpl.html.
////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////
/// This mini-app demonstrates a breadth-first-search of the built-in 
/// graph data structure. This implement's Graph500's BFS benchmark:
/// - Uses the Graph500 Specification Kronecker graph generator with
///   numVertices = 2^scale (--scale specified on command-line)
/// - Uses the builtin hybrid compressed-sparse-row graph format
/// - Computes the 'parent' tree given a root, and does this a number 
///   of times (specified by --nbfs).
////////////////////////////////////////////////////////////////////////

#include "common.hpp"

GRAPPA_DECLARE_METRIC(SummarizingMetric<double>, bfs_mteps);
GRAPPA_DECLARE_METRIC(SummarizingMetric<double>, total_time);
GRAPPA_DECLARE_METRIC(SimpleMetric<int64_t>, bfs_nedge);
GRAPPA_DECLARE_METRIC(SimpleMetric<double>, graph_create_time);
GRAPPA_DECLARE_METRIC(SimpleMetric<double>, verify_time);

//int64_t *frontier_base, *f_head, *f_tail, *f_level;

struct Frontier {
  int64_t *base, *head, *tail, *level;
  int64_t sz;
  
  void init(size_t sz) {
    this->sz = sz;
    base = head = tail = level = locale_alloc<int64_t>(sz);
  }
  
  void clear() {
    head = tail = level = (base);
  }
  
  void push(int64_t v) { *tail++ = v; assert(tail < base+sz); }
  int64_t pop() { return *head++;     CHECK_LE(head, level); }
  
  int64_t next_size() { return tail - level; }
  void next_level() { level = tail; }
  bool level_empty() { return head >= level; }
};

Frontier frontier;

GlobalCompletionEvent joiner;

void bfs(GlobalAddress<G> g, int nbfs, TupleGraph tg) {
  bool verified = false;
  
  // initialize frontier on each core
  call_on_all_cores([g]{
    //      frontier.init(g->nv / cores() * 2);
    frontier.init(g->nv / cores() * 8);
  });
  
  // do BFS from multiple different roots and average their times
  for (int root_idx = 0; root_idx < nbfs; root_idx++) {
  
    // intialize parent to -1
    forall(g, [](G::Vertex& v){ v->init(); });
    call_on_all_cores([]{ frontier.clear(); });
    
    int64_t root = choose_root(g);
    VLOG(1) << "root => " << root;

    // setup 'root' as the parent of itself
    delegate::call(g->vs+root, [=](G::Vertex& v){ v->parent = root; });
    
    // intialize frontier with root
    frontier.push(root);
    
    Metrics::start_tracing();
    double t = walltime();

    // use 'SPMD' mode, this matches the style of the MPI reference code
    // (but of course the asynchronous message passing is implicit)
    on_all_cores([=]{
      int64_t level = 0;
              
      auto vs = g->vs;
      
      int64_t next_level_total;
      
      do {
        if (mycore() == 0) VLOG(1) << "level " << level;
        // setup frontier to pop from the next level
        frontier.next_level();
        
        // enroll one 'task' on each core to make sure no one falls through
        joiner.enroll();
        barrier();
        
        // process all the vertices in the frontier on this core
        while ( !frontier.level_empty() ) {
          auto i = frontier.pop();
          
          // visit all the adjacencies of the vertex
          forall<async,&joiner>(adj(g,vs+i), [i](G::Edge& e){
            auto j = e.id;
            // at the core where the vertex is...
            if( e.ga.core() == Grappa::mycore() ) {
                    G::Vertex& v = *(e.ga.pointer());
              // note: no synchronization needed because 'call' is 
              // guaranteed to be executed atomically because it 
              // does no blocking operations
              if (v->parent == -1) {
                // claim parenthood
                v->parent = i;
                // add this vertex to the frontier for the next level
                frontier.push(j);
              }
            } else {
                    delegate::call<async,&joiner>(e.ga, [i,j](G::Vertex& v){
              // note: no synchronization needed because 'call' is 
              // guaranteed to be executed atomically because it 
              // does no blocking operations
              if (v->parent == -1) {
                // claim parenthood
                v->parent = i;
                // add this vertex to the frontier for the next level
                frontier.push(j);
              }
            });
            }
            });
        }
        
        joiner.complete();
        joiner.wait();
        
        // find the total number of vertices in the next level of frontier
        // (MPI-style 'allreduce' shares the total with all cores)
        next_level_total = allreduce<int64_t, collective_add>(frontier.next_size());
        level++;
        
        // keep going until nothing was added to the frontier
      } while (next_level_total > 0); 

    }); // end of 'SPMD' region
    
    double this_total_time = walltime() - t;

    Metrics::stop_tracing();
    LOG(INFO) << "(root=" << root << ", time=" << this_total_time << ")";
    total_time += this_total_time;
    
    if (!verified) {
      // only verify the first one to save time
      t = walltime();
      bfs_nedge = verify(tg, g, root);
      verify_time = (walltime()-t);
      LOG(INFO) << verify_time;
      verified = true;
    }
    
    bfs_mteps += bfs_nedge / this_total_time / 1.0e6;
  }
}

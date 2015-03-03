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
// http://www.affero.org/oagl.html.
////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////
/// Demonstrates using the GraphLab API to implement BFS
////////////////////////////////////////////////////////////////////////
#include <Grappa.hpp>
#include "graphlab.hpp"

DEFINE_bool( metrics, false, "Dump metrics");

DEFINE_bool( max_degree_source, false, "Start from maximum degree vertex");
DEFINE_int32(scale, 10, "Log2 number of vertices.");
DEFINE_int32(edgefactor, 16, "Average number of edges per vertex.");

DEFINE_int32(trials, 3, "Number of timed trials to run and average over.");

DEFINE_string(path, "", "Path to graph source file.");
DEFINE_string(format, "bintsv4", "Format of graph source file.");

GRAPPA_DEFINE_METRIC(SimpleMetric<double>, tuple_time, 0);
GRAPPA_DEFINE_METRIC(SimpleMetric<double>, construction_time, 0);
GRAPPA_DEFINE_METRIC(SummarizingMetric<double>, total_time, 0);

struct BFSVertexData : public GraphlabVertexData< BFSVertexData > {
  VertexID parent;
  BFSVertexData(): parent(-1) {}
};

using G = Graph<BFSVertexData,Empty>;


template< typename G >
inline VertexID choose_root(GlobalAddress<G> g) {
  VertexID root;
  do {
    root = random() % g->nv;
  } while (call(g->vs+root,[](typename G::Vertex& v){ return v.nadj; }) == 0);
  return root;
}

struct BFS : public GraphlabVertexProgram<G,bool> {
  BFS(const Vertex& v) {}
  
  bool gather_edges(Vertex& v) const { return false; }
  bool gather(Vertex& src, Edge& e) const { return false; }
  
  void apply(Vertex& v, bool ignore) { }
  
  bool scatter_edges(Vertex& v) const { return true; }
  
  bool scatter(const Edge& e, Vertex& target) const {
    if (target->parent == -1) {
      target->parent = e.id;
      target->activate();
    }
    return false;
  }
};

using MaxDegree = CmpElement<VertexID,int64_t>;
Reducer<MaxDegree,ReducerType::Max> max_degree;

int main(int argc, char* argv[]) {
  init(&argc, &argv);
  run([]{
    double t;
    
    TupleGraph tg;
    
    GRAPPA_TIME_REGION(tuple_time) {
      if (FLAGS_path.empty()) {
        int64_t NE = (1L << FLAGS_scale) * FLAGS_edgefactor;
        tg = TupleGraph::Kronecker(FLAGS_scale, NE, 111, 222);
      } else {
        LOG(INFO) << "loading " << FLAGS_path;
        tg = TupleGraph::Load(FLAGS_path, FLAGS_format);
        LOG(INFO) << "done! loaded " << tg.nedge << " edges";
      }
    }
    
    t = walltime();
    
    auto g = G::create(tg); // undirected
        
    tg.destroy();
    
    construction_time = (walltime()-t);
    LOG(INFO) << construction_time;
    
    VertexID root;
    if (FLAGS_max_degree_source) {
      forall(g, [](VertexID i, G::Vertex& v){
        max_degree << MaxDegree(i, v.nadj);
      });
      root = static_cast<MaxDegree>(max_degree).idx();
    } else {
      root = choose_root(g);
    }
    
    LOG(INFO) << "BFS on root:" << root;
    
    for (int i = 0; i < FLAGS_trials; i++) {
      if (FLAGS_trials > 1) LOG(INFO) << "trial " << i;
      
      // re-initialize
      forall(g, [](G::Vertex& v){ new (&v.data) BFSVertexData(); });
      
      Metrics::start_tracing();
      GRAPPA_TIME_REGION(total_time) {
        activate(g->vs+root);
        NaiveGraphlabEngine<G,BFS>::run_sync(g);
      }
      Metrics::stop_tracing();
      
      if (i == 0) Metrics::reset_all_cores(); // don't count the first one
    }
    
    LOG(INFO) << "-- BFS done";
    
    if (FLAGS_metrics) Metrics::merge_and_print();
    else {
      std::cerr << total_time << "\n"
                << iteration_time << "\n";
    }
    Metrics::merge_and_dump_to_file();
    
    g->destroy();
  });
  finalize();
}

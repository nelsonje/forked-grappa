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

#include <Grappa.hpp>
#include <FullEmpty.hpp>
#include <array/GlobalArray.hpp>

using namespace Grappa;

DEFINE_uint64(m, 4, "number of rows in array");
DEFINE_uint64(n, 4, "number of columns in array");
DEFINE_uint64(iterations, 1, "number of iterations");
DEFINE_string(pattern, "border", "what pattern of kernel should we run?");

#define ARRAY(i,j) (local[(j)+((i)*dim2_percore)])

//#define ARRAY(i,j) (local[1+j+i*(dim2_percore)])

double * local = NULL;
int dim1_size = 0;
int dim2_size = 0;
int dim2_percore = 0;

GlobalArray< double, int, Distribution::Local, Distribution::Block > ga;
GlobalArray< FullEmpty< double >, int, Distribution::Local, Distribution::Block > leftsa;
FullEmpty<double> * lefts = NULL;

double iter_time = 0.0;

int main( int argc, char * argv[] ) {
  init( &argc, &argv );
  run([]{
      LOG(INFO) << "Grappa pipeline stencil execution on 2D ("
                << FLAGS_m << "x" << FLAGS_n
                << ") grid";
      
      ga.allocate( FLAGS_m, FLAGS_n );
      leftsa.allocate( FLAGS_m, Grappa::cores() );
      on_all_cores( [] {
          lefts = new FullEmpty<double>[FLAGS_n];
        } );

      double avgtime = 0.0;
      double mintime = 366.0*24.0*3600.0;
      double maxtime = 0.0;

      // initialize
      LOG(INFO) << "Initializing....";
      forall( ga, [] (int i, int j, double& d) {
          if( i == 0 ) {
            d = j;
          } else if ( j == 0 ) {
            d = i;
          } else {
            d = 0.0;
          }
          // LOG(INFO) << "Initx: ga(" << i << "," << j << ")=" << d;
        });
        on_all_cores( [] {
            for( int i = 0; i < FLAGS_n; ++i ) {
              if( (Grappa::mycore() == 0) || 
                  (Grappa::mycore() == 1 && ga.dim2_percore == 1 ) ) {
                lefts[i].writeXF(i);
              } else {
                if( i == 0 ) {
                  lefts[i].writeXF( ga.local_chunk[0] - 1 ); // one to the left of our first element
                } else {
                  lefts[i].reset();
                }
              }
            }
          } );
      
      LOG(INFO) << "Running " << FLAGS_iterations << " iterations....";
      for( int iter = 0; iter < FLAGS_iterations; ++iter ) {
        on_all_cores( [] {
            for( int i = 1; i < FLAGS_n; ++i ) {
              if( ! ((Grappa::mycore() == 0) || 
                     (Grappa::mycore() == 1 && ga.dim2_percore == 1 )) ) {
                lefts[i].reset();
              }
            }
          } );
        // forall( leftsa, [] (int i, int j, FullEmpty<double>& d) {
        //     if( j == 0 || j == 1 ) {
        //       d.writeXF( i );
        //     } else if ( i == 0 ) {
        //       d.writeXF( j );
        //     } else {
        //       d.reset();
        //     }
        //   } );
        
        // execute kernel
        VLOG(2) << "Starting iteration " << iter;
        double start = Grappa::walltime();

        if( FLAGS_pattern == "border" ) {

          finish( [] {
              on_all_cores( [] {
                  local = ga.local_chunk;
                  dim1_size = ga.dim1_size;
                  dim2_size = ga.dim2_size;
                  dim2_percore = ga.dim2_percore;
                  int first_j = Grappa::mycore() * dim2_percore;

                  iter_time = Grappa::walltime();
                  for( int i = 1; i < dim1_size; ++i ) {

                    // prepare to iterate over this segment      
                    double left = readFF( &lefts[i] );
                    double diag = readFF( &lefts[i-1] );
                    double up = 0.0;
                    double current = 0.0;

                    for( int j = 0; j < dim2_percore; ++j ) {
                      int actual_j = j + first_j;
                      if( actual_j > 0 ) {
                        // compute this cell's value
                        up = local[ (i-1)*dim2_percore + j ];
                        current = up + left - diag;

                        // update for next iteration
                        diag = up;
                        left = current;

                        // write value
                        local[ (i)*dim2_percore + j ] = current;
                      }
                    }

                    // if we're at the end of a segment, write to corresponding full bit
                    if( Grappa::mycore()+1 < Grappa::cores() ) {
                      delegate::call<async>( Grappa::mycore()+1,
                                             [=] () {
                                               writeXF( &lefts[i], current );
                                             } );
                    }

                  }
                  iter_time = Grappa::walltime() - iter_time;
                  iter_time = reduce<double,collective_min<double>>( &iter_time );

                } );
            } );

          // forall( ga, [] (int i, int j, double& d) {
          //     LOG(INFO) << "Done: ga(" << i << "," << j << ")=" << ARRAY(i,j);
          //   } );
                  
        } else {
          LOG(FATAL) << "unknown kernel pattern " << FLAGS_pattern << "!";
        }
        double end = Grappa::walltime();

        if( iter > 0 || FLAGS_iterations == 1 ) { // skip the first iteration
          //double time = end - start;
          double time = iter_time;
          avgtime += time;
          mintime = std::min( mintime, time );
          maxtime = std::max( maxtime, time );
        }
        
        on_all_cores( [] {
            VLOG(2) << "done with this iteration";
          } );

        // copy top right corner value to bottom left corner to create dependency
        VLOG(2) << "Adding end-to-end dependence for iteration " << iter;
        int last_m = FLAGS_m-1;
        int last_n = FLAGS_n-1;
        double val = delegate::read( &ga[ FLAGS_m-1 ][ FLAGS_n-1 ] );
        delegate::write( &ga[0][0], -1.0 * val );
        delegate::call( 0, [val] { lefts[0].writeXF( -1.0 * val ); } );
        if( ga.dim2_percore == 1 ) delegate::call( 1, [val] { lefts[0].writeXF( -1.0 * val ); } );
        VLOG(2) << "Done with iteration " << iter;
      }
      
      avgtime /= (double) std::max( FLAGS_iterations-1, static_cast<google::uint64>(1) );
      LOG(INFO) << "Rate (MFlops/s): " << 1.0E-06 * 2 * ((double)(FLAGS_m-1)*(FLAGS_n-1)) / mintime
                << ", Avg time (s): " << avgtime
                << ", Min time (s): " << mintime
                << ", Max time (s): " << maxtime;
      
      // verify result
      VLOG(2) << "Verifying result";
      double expected_corner_val = (double) FLAGS_iterations * ( FLAGS_m + FLAGS_n - 2 );
      double actual_corner_val = delegate::read( &ga[ FLAGS_m-1 ][ FLAGS_n-1 ] );
      CHECK_DOUBLE_EQ( actual_corner_val, expected_corner_val );

      on_all_cores( [] {
          if( lefts ) delete [] lefts;
        } );
      leftsa.deallocate( );
      ga.deallocate( );
      
      LOG(INFO) << "Done.";
    });
  finalize();
  return 0;
}

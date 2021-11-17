/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
   Copyright (c) 2015-2020 The plumed team
   (see the PEOPLE file at the root of the distribution for a list of names)
  
   See http://www.plumed.org for more information.
  
   This file is part of plumed, version 2.

   plumed is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by 
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.
  
   plumed is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.
  
   You should have received a copy of the GNU Lesser General Public License
   along with plumed.  If not, see <http://www.gnu.org/licenses/>.
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */
#include "EvaluateGridFunction.h" 
#include "core/PlumedMain.h"
#include "core/ActionSet.h"

namespace PLMD {
namespace gridtools {

void EvaluateGridFunction::registerKeywords( Keywords& keys ){
  keys.add("compulsory","INTERPOLATION_TYPE","spline","the method to use for interpolation.  Can be spline or floor.");
  keys.addFlag("ZERO_OUTSIDE_GRID_RANGE",false,"if we are asked to evaluate the function for a number that is outside the range of the grid set it to zero");
}

void EvaluateGridFunction::setupGridObject( Value* values, GridCoordinatesObject& gridobject ) { 
  ActionWithValue* act=values->getPntrToAction();
  std::vector<unsigned> ind( values->getRank() ), nbin( values->getRank() ); std::string gtype;
  std::vector<double> spacing( values->getRank() ), xx( values->getRank() ); std::vector<bool> pbc( values->getRank() );
  std::vector<std::string> argn( values->getRank() ), min( values->getRank() ), max( values->getRank() );
  act->getInfoForGridHeader( gtype, argn, min, max, nbin, spacing, pbc, false );
  gridobject.setup( "flat", pbc, 0, 0.0 );
}

void EvaluateGridFunction::setupGridBounds( Value* values, GridCoordinatesObject& gridobject ) {
  unsigned dimension = values->getRank();
  std::vector<std::string> argn( dimension ), min( dimension ), max( dimension ); std::string gtype;
  std::vector<unsigned> nbin( dimension ); std::vector<double> spacing( dimension ); std::vector<bool> ipbc( dimension );
  values->getPntrToAction()->getInfoForGridHeader( gtype, argn, min, max, nbin, spacing, ipbc, false );
  gridobject.setBounds( min, max, nbin, spacing );
}

std::vector<bool> EvaluateGridFunction::getPbc() const {
  std::vector<bool> ipbc( gridobject.getDimension() ); 
  for(unsigned i=0; i<ipbc.size(); ++i) ipbc[i] = gridobject.isPeriodic(i);
  return ipbc;
}

void EvaluateGridFunction::read( ActionWithArguments* action ) {
  if( !action->getPntrToArgument(0)->isTimeSeries() ) {
      if( action->getPntrToArgument(0)->getRank()==0 || !action->getPntrToArgument(0)->hasDerivatives() ) action->error("should have one grid as input to this action");
  }
  // Now use this information to create a gridobject
  setupGridObject( action->getPntrToArgument(0), gridobject );
  // if( gtype=="fibonacci" ) action->error("cannot interpolate on fibonacci sphere");
  parseFlag(action,"ZERO_OUTSIDE_GRID_RANGE",set_zero_outside_range);
  if( set_zero_outside_range ) action->log.printf("  function is zero outside grid range \n");
  // Get the type of interpolation that we are doing
  std::string itype; parse(action,"INTERPOLATION_TYPE",itype);
  if( itype=="spline" ) { 
      interpolation_type=spline;
      spline_interpolator=Tools::make_unique<Interpolator>( action->getPntrToArgument(0), gridobject );
  } else if( itype=="linear" ) {
    interpolation_type=linear;  
  } else if( itype=="floor" ) {
    interpolation_type=floor;
    for(unsigned j=0;j<action->getPntrToArgument(0)->getRank();++j) {
       if( !action->getPntrToArgument(0)->isTimeSeries() && gridobject.isPeriodic(j) ) action->error("floor interepolation doesn't work with periodic variables");
    }
  } else action->error("type " + itype + " of interpolation is not defined");
  action->log.printf("  generating off grid points using %s interpolation \n", itype.c_str() );
}

void EvaluateGridFunction::setup( const ActionWithArguments* action ) {
  setupGridBounds( action->getPntrToArgument(0), gridobject );
}

void EvaluateGridFunction::calc( const ActionWithArguments* action, const std::vector<double>& args, std::vector<double>& vals, Matrix<double>& derivatives ) const {
  if( set_zero_outside_range && !gridobject.inbounds( args ) ) { vals[0]=0.0; return; }
  unsigned dimension = gridobject.getDimension(); plumed_dbg_assert( args.size()==dimension && vals.size()==1 ); 
  if( interpolation_type==spline ) {
      std::vector<double> der( dimension );
      vals[0] =  spline_interpolator->splineInterpolation( args, der );
      for(unsigned j=0; j<dimension; ++j) derivatives(0,j) = der[j];
  } else if( interpolation_type==linear ) {
      Value* values=action->getPntrToArgument(0); std::vector<double> xfloor(dimension);
      std::vector<unsigned> indices(dimension), nindices(dimension), ind(dimension); 
      gridobject.getIndices( args, indices ); unsigned nn=gridobject.getIndex(args);
      gridobject.getGridPointCoordinates( nn, nindices, xfloor ); 
      double y1 = values->get(nn); vals[0] = y1;
      for(unsigned i=0; i<args.size(); ++i) {
          int x0=1; if(nindices[i]==indices[i]) x0=0;
          double ddx=gridobject.getGridSpacing()[i];
          double X = fabs((args[i]-xfloor[i])/ddx-(double)x0);
          for(unsigned j=0; j<args.size();++j) ind[j] = indices[j];
          if( gridobject.isPeriodic(i) && (ind[i]+1)==gridobject.getNbin(false)[i] ) ind[i]=0;
          else ind[i] = ind[i] + 1;
          vals[0] += ( values->get( gridobject.getIndex(ind) ) - y1 )*X;
          derivatives(0,i) = ( values->get( gridobject.getIndex(ind) ) - y1 ) / ddx;
      }
  } else if( interpolation_type==floor ) {
      Value* values=action->getPntrToArgument(0); std::vector<unsigned> indices(dimension); 
      gridobject.getIndices( args, indices ); unsigned nn = gridobject.getIndex(indices);
      if( values->isTimeSeries() && !gridobject.inbounds(args) ) nn = gridobject.getNbin(false)[0]-1;
      if( values->isTimeSeries() && nn==values->getShape()[0] ) {
          vals[0] = values->get( nn - 1 ); 
      } else {
          plumed_dbg_assert( nn<values->getNumberOfValues() );
          vals[0] = values->get( nn ); 
      }
      if( !values->isTimeSeries() ) {
          for(unsigned j=0; j<dimension; ++j) derivatives(0,j) = values->getGridDerivative( nn, j );
      }
  } else plumed_error();
}

void EvaluateGridFunction::applyForce( const ActionWithArguments* action, const std::vector<double>& args, const double& force, std::vector<double>& forcesToApply ) const {
  unsigned dimension = gridobject.getDimension();
  if( interpolation_type==spline ) {
      action->error("can't apply forces on values interpolated using splines");
  } else if( interpolation_type==linear ) {
      Value* values=action->getPntrToArgument(0); std::vector<double> xfloor(dimension);
      std::vector<unsigned> indices(dimension), nindices(dimension), ind(dimension);
      gridobject.getIndices( args, indices ); unsigned nn=gridobject.getIndex(args);
      gridobject.getGridPointCoordinates( nn, nindices, xfloor );
      for(unsigned i=0; i<args.size(); ++i) {
          int x0=1; if(nindices[i]==indices[i]) x0=0;
          double ddx=gridobject.getGridSpacing()[i];
          double X = fabs((args[i]-xfloor[i])/ddx-(double)x0);
          for(unsigned j=0; j<args.size();++j) ind[j] = indices[j];
          if( gridobject.isPeriodic(i) && (ind[i]+1)==gridobject.getNbin(false)[i] ) ind[i]=0;
          else ind[i] = ind[i] + 1;
          forcesToApply[nn] += force*(1-X); forcesToApply[gridobject.getIndex(ind)] += X*force;  
      }
  } else if( interpolation_type==floor ) {
      Value* values=action->getPntrToArgument(0); std::vector<unsigned> indices(dimension);
      gridobject.getIndices( args, indices ); unsigned nn = gridobject.getIndex(indices);
      if( values->isTimeSeries() && !gridobject.inbounds(args) ) nn = gridobject.getNbin(false)[0]-1;
      if( values->isTimeSeries() && nn==values->getShape()[0] ) forcesToApply[nn-1] += force; 
      else forcesToApply[nn] += force; 
  } else plumed_error();

}

}
}

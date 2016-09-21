
#include <Utility.H>
#include <Geometry.H>
#include <MultiFab.H>
#include <PhysBCFunct.H>
#include <PArray.H>
#include <ParmParse.H>
#include <ParallelDescriptor.H>
#include <writePlotFile.H>

#include "myfunc_F.H"

Real compute_dt (Real dx)
{
    return 0.9*dx*dx / (2.0*BL_SPACEDIM);
}

void advance (MultiFab& old_phi, MultiFab& new_phi, PArray<MultiFab>& flux,
	      Real time, Real dt, const Geometry& geom, PhysBCFunct& physbcf)
{
    // Fill the ghost cells of each grid from the other grids
    old_phi.FillBoundary(geom.periodicity());

    // Fill physical boundaries
    physbcf.FillBoundary(old_phi, time);
    
    int Ncomp = old_phi.nComp();
    int ng_p = old_phi.nGrow();
    int ng_f = 0;
    
    const Real* dx = geom.CellSize();

    //
    // Note that this simple example is not optimized.
    // The following two MFIter loops could be merged
    // and we do not have to use flux MultiFab.
    // 

    // Compute fluxes one grid at a time
    for ( MFIter mfi(old_phi); mfi.isValid(); ++mfi )
    {
	const Box& bx = mfi.validbox();
	
	compute_flux(old_phi[mfi].dataPtr(),
		     &ng_p,
		     flux[0][mfi].dataPtr(),
		     flux[1][mfi].dataPtr(),
#if (BL_SPACEDIM == 3)   
		     flux[2][mfi].dataPtr(),
#endif
		     &ng_f, bx.loVect(), bx.hiVect(), &dx[0]);
    }
    
    // Advance the solution one grid at a time
    for ( MFIter mfi(old_phi); mfi.isValid(); ++mfi )
    {
	const Box& bx = mfi.validbox();
	
	update_phi(old_phi[mfi].dataPtr(),
		   new_phi[mfi].dataPtr(),
		   &ng_p,
		   flux[0][mfi].dataPtr(),
		   flux[1][mfi].dataPtr(),
#if (BL_SPACEDIM == 3)   
		   flux[2][mfi].dataPtr(),
#endif
		   &ng_f, bx.loVect(), bx.hiVect(), &dx[0] , &dt);
    }
}

void main_main ()
{
    // What time is it now?  We'll use this to compute total run time.
    Real strt_time = ParallelDescriptor::second();
    
    std::cout << std::setprecision(15);
    
    int n_cell, max_grid_size, nsteps, plot_int, is_per[BL_SPACEDIM];

    // inputs parameters
    {
	// ParmParse is way of reading inputs from the inputs file
	ParmParse pp;

	// We need to get n_cell from the inputs file - this is the number of cells on each side of 
	//   a square (or cubic) domain.
	pp.get("n_cell",n_cell);
	
	// Default nsteps to 0, allow us to set it to something else in the inputs file
	pp.get("max_grid_size",max_grid_size);

	// Default plot_int to 1, allow us to set it to something else in the inputs file
	//  If plot_int < 0 then no plot files will be written
	plot_int = 1;
	pp.query("plot_int",plot_int);

	// Default nsteps to 0, allow us to set it to something else in the inputs file
	nsteps = 0;
	pp.query("nsteps",nsteps);
    }

    // make BoxArray and Geometry
    BoxArray grids;
    Geometry geom;
    {
	IntVect dom_lo(IntVect(D_DECL(0,0,0)));
	IntVect dom_hi(IntVect(D_DECL(n_cell-1, n_cell-1, n_cell-1)));
	Box domain(dom_lo, dom_hi);

	// Initialize the boxarray "grids" from the single box "bx"
	grids.define(domain);
	// Break up boxarray "grids" into chunks no larger than "max_grid_size" along a direction
	grids.maxSize(max_grid_size);

	// This defines the physical size of the box.  Right now the box is [-1,1] in each direction.
	RealBox real_box;
	for (int n = 0; n < BL_SPACEDIM; n++) {
	    real_box.setLo(n,-1.0);
	    real_box.setHi(n, 1.0);
	}

	// This says we are using Cartesian coordinates
	int coord = 0;
	
	// This sets the boundary conditions to be doubly or triply periodic
	int is_per[BL_SPACEDIM];
	for (int i = 0; i < BL_SPACEDIM; i++) {
	    is_per[i] = 1;
	}

	// This defines a Geometry object
	geom.define(domain,&real_box,coord,is_per);
    }

    // define physical boundary filling object
    PhysBCFunct physbcf;
    {
	// Boundary conditions
	int lo_bc[BL_SPACEDIM];
	int hi_bc[BL_SPACEDIM];
	for (int i = 0; i < BL_SPACEDIM; ++i) {
	    lo_bc[i] = hi_bc[i] = INT_DIR;   // periodic boundaries are interior boundaries
	}
	BCRec bcr(lo_bc, hi_bc);

	physbcf.define(geom, bcr, BndryFunctBase(phifill)); // phifill is a fortran function
    }

    // Nghost = number of ghost cells for each array 
    int Nghost = 1;
    
    // Ncomp = number of components for each array
    int Ncomp  = 1;

    // time = starting time in the simulation
    Real time = 0.0;
    
    PArray<MultiFab> phi(2, PArrayManage);
    phi.set(0, new MultiFab(grids, Ncomp, Nghost));
    phi.set(1, new MultiFab(grids, Ncomp, Nghost));
    // Initialize both to zero (just because)
    phi[0].setVal(0.0);
    phi[1].setVal(0.0);

    // Initialize phi[init_index] by calling a Fortran routine.
    // MFIter = MultiFab Iterator
    int init_index = 0;
    for ( MFIter mfi(phi[init_index]); mfi.isValid(); ++mfi )
    {
	const Box& bx = mfi.validbox();

	init_phi(phi[init_index][mfi].dataPtr(),
		 bx.loVect(), bx.hiVect(), &Nghost,
		 geom.CellSize(), geom.ProbLo(), geom.ProbHi());
    }

    // Call the compute_dt routine to return a time step which we will pass to advance
    Real dt = compute_dt(geom.CellSize(0));

    // Write a plotfile of the initial data if plot_int > 0 (plot_int was defined in the inputs file)
    if (plot_int > 0)
    {
	int n = 0;
	const std::string& pltfile = BoxLib::Concatenate("plt",n,5);
	writePlotFile(pltfile, phi[init_index], geom);
    }

    // build the flux multifabs
    PArray<MultiFab> flux(BL_SPACEDIM, PArrayManage);
    for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
	// flux(dir) has one component, zero ghost cells, and is nodal in direction dir
	BoxArray edge_grids = grids;
	edge_grids.surroundingNodes(dir);
	flux.set(dir, new MultiFab(edge_grids, 1, 0));
    }

    int old_index = init_index;
    for (int n = 1; n <= nsteps; n++, old_index = 1 - old_index)
    {
	int new_index = 1 - old_index;
	
	// new_phi = old_phi + dt * (something)
	advance(phi[old_index], phi[new_index], flux, time, dt, geom, physbcf); 
	
	// Tell the I/O Processor to write out which step we're doing
	if (ParallelDescriptor::IOProcessor())
	    std::cout << "Advanced step " << n << std::endl;
	
	// Write a plotfile of the current data (plot_int was defined in the inputs file)
	if (plot_int > 0 && n%plot_int == 0)
	{
	    const std::string& pltfile = BoxLib::Concatenate("plt",n,5);
	    writePlotFile(pltfile, phi[new_index], geom);
	}
    }
    
    // Call the timer again and compute the maximum difference between the start time and stop time
    //   over all processors
    Real stop_time = ParallelDescriptor::second() - strt_time;
    const int IOProc = ParallelDescriptor::IOProcessorNumber();
    ParallelDescriptor::ReduceRealMax(stop_time,IOProc);
    
    // Tell the I/O Processor to write out the "run time"
    if (ParallelDescriptor::IOProcessor()) {
	std::cout << "Run time = " << stop_time << std::endl;
    }
}


int main (int argc, char* argv[])
{
    BoxLib::Initialize(argc,argv);

    main_main();

    BoxLib::Finalize();
    return 0;
}


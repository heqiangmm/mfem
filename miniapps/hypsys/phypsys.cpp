#include "lib/lib.hpp"
#include "fe_evol/plib.hpp"
#include "apps/lib.hpp"

int main(int argc, char *argv[])
{
   MPI_Session mpi(argc, argv);
   const int myid = mpi.WorldRank();

   Configuration config;
   config.ProblemNum = 0;
   config.ConfigNum = 1;
   const char *MeshFile = "data/unstr.mesh";
   int refinements = 1;
   int prefinements = 0;
   config.order = 3;
   config.tFinal = 1.;
   config.dt = 0.001;
   config.odeSolverType = 3;
   config.VisSteps = 100;

   EvolutionScheme scheme = Galerkin;

   config.precision = 8;
   cout.precision(config.precision);

   OptionsParser args(argc, argv);
   args.AddOption(&config.ProblemNum, "-p", "--problem",
                  "Hyperbolic system of equations to solve.");
   args.AddOption(&config.ConfigNum, "-c", "--configuration",
                  "Problem setup to use.");
   args.AddOption(&MeshFile, "-m", "--mesh",
                  "Mesh file to use.");
   args.AddOption(&refinements, "-r", "--refine",
                  "Number of times to refine the mesh uniformly in serial.");
   args.AddOption(&prefinements, "-pr", "--parallel-refine",
                  "Number of times to refine the mesh uniformly in parallel.");
   args.AddOption(&config.order, "-o", "--order",
                  "Order (polynomial degree) of the finite element space.");
   args.AddOption(&config.tFinal, "-tf", "--t-final",
                  "Final time; start time is 0.");
   args.AddOption(&config.dt, "-dt", "--time-step",
                  "Time step.");
   args.AddOption(&config.odeSolverType, "-s", "--ode-solver",
                  "ODE solver: 1 - Forward Euler,\n\t"
                  "            2 - RK2 SSP, 3 - RK3 SSP.");
   args.AddOption(&config.VisSteps, "-vs", "--visualization-steps",
                  "Visualize every n-th timestep.");
   args.AddOption((int *)(&scheme), "-e", "--EvolutionScheme",
                  "Scheme: 0 - Galerkin Finite Element Approximation,\n\t"
                  "        1 - Monolithic Convex Limiting.");

   args.Parse();
   if (!args.Good())
   {
      if (myid == 0)
      {
         args.PrintUsage(cout);
      }
      return -1;
   }
   if (myid == 0)
   {
      args.PrintOptions(cout);
   }

   ODESolver *odeSolver = NULL;
   switch (config.odeSolverType)
   {
      case 1: odeSolver = new ForwardEulerSolver; break;
      case 2: odeSolver = new RK2Solver(1.0); break;
      case 3: odeSolver = new RK3SSPSolver; break;
      default:
         cout << "Unknown ODE solver type: " << config.odeSolverType << endl;
         return -1;
   }

   // Read the serial mesh from the given mesh file on all processors.
   Mesh *mesh = new Mesh(MeshFile, 1, 1);
   const int dim = mesh->Dimension();
   for (int lev = 0; lev < refinements; lev++)
   {
      mesh->UniformRefinement();
   }
   mesh->GetBoundingBox(config.bbMin, config.bbMax, max(config.order, 1));

   // Parallel partitioning of the mesh.
   ParMesh pmesh(MPI_COMM_WORLD, *mesh);
   delete mesh;
   for (int lev = 0; lev < prefinements; lev++)
   {
      pmesh.UniformRefinement();
   }

   if (pmesh.NURBSext)
   {
      pmesh.SetCurvature(max(config.order, 1));
   }
   MPI_Comm comm = pmesh.GetComm();

   int NumEq; // number of scalar unknowns, e.g. 3 for SWE in 2D.
   switch (config.ProblemNum)
   {
      case 0:
      case 1:
      case 2: NumEq = 1; break;
      case 3: NumEq = 1 + dim; break;
      case 4: NumEq = 2 + dim; break;
      default:
         cout << "Unknown hyperbolic system: " << config.ProblemNum << endl;
         delete odeSolver;
         return -1;
   }

   // Create Bernstein Finite Element Space.
   const int btype = BasisType::Positive;
   L2_FECollection fec(config.order, dim, btype);
   ParFiniteElementSpace pfes(&pmesh, &fec);
   ParFiniteElementSpace vfes(&pmesh, &fec, NumEq, Ordering::byNODES);

   Array<int> offsets(NumEq + 1);
   for (int k = 0; k <= NumEq; k++) { offsets[k] = k * pfes.GetNDofs(); }
   BlockVector u_block(offsets);

   const int ProblemSize = vfes.GlobalVSize();
   if (myid == 0)
   {
      cout << "Number of unknowns: " << ProblemSize << endl;
   }

   // The min/max bounds are represented as H1 functions of the same order
   // as the solution, thus having 1:1 dof correspondence inside each element.
   H1_FECollection fecBounds(max(config.order, 1), dim,
                             BasisType::GaussLobatto);
   ParFiniteElementSpace pfesBounds(&pmesh, &fecBounds);
   ParDofInfo pdofs(&pfes, &pfesBounds);

   bool NodalQuadRule = false;
   if (scheme == MonolithicConvexLimiting)
   {
      NodalQuadRule = true;
   }

   HyperbolicSystem *hyp;
   switch (config.ProblemNum)
   {
      case 0: { hyp = new Advection(&vfes, u_block, config, NodalQuadRule, pdofs); break; }
      case 1: { hyp = new Burgers(&vfes, u_block, config); break; }
      case 2: { hyp = new KPP(&vfes, u_block, config); break; }
      case 3: { hyp = new ShallowWater(&vfes, u_block, config); break; }
      case 4: { hyp = new Euler(&vfes, u_block, config); break; }
      default:
         return -1;
   }

   if (config.odeSolverType != 1 && hyp->SteadyState)
   {
      MFEM_WARNING("Better use forward Euler pseudo time stepping for steady state simulations.");
   }

   ParGridFunction u(&vfes, u_block);
   u = hyp->u0;

   // uk is used for visualization with GLVis.
   ParGridFunction uk(&pfes, u_block.GetBlock(0));
   if (hyp->FileOutput)
   {
      ofstream omesh("grid.mesh");
      omesh.precision(config.precision);
      pmesh.PrintAsOne(omesh);
      ofstream osol("initial.gf");
      osol.precision(config.precision);
      uk.SaveAsOne(osol);
   }

   socketstream sout;
   char vishost[] = "localhost";
   int visport = 19916;
   {
      // Make sure all MPI ranks have sent their 'v' solution before initiating
      // another set of GLVis connections (one from each rank):
      MPI_Barrier(comm);
      ParVisualizeField(sout, vishost, visport, hyp->ProblemName, uk,
                        hyp->valuerange);
   }

   FE_Evolution *evol;
   switch (scheme)
   {
      case Galerkin: { evol = new ParGalerkinEvolution(&vfes, hyp, pdofs); break; }
      case MonolithicConvexLimiting: { evol = new ParMCL_Evolution(&vfes, hyp, pdofs); break; }
      default:
         MFEM_ABORT("Unknown evolution scheme");
   }

   double InitialMass, MassMPI = evol->LumpedMassMat * u;
   MPI_Allreduce(&MassMPI, &InitialMass, 1, MPI_DOUBLE, MPI_SUM, comm);

   odeSolver->Init(*evol);
   if (hyp->SteadyState)
   {
      evol->uOld.SetSize(ProblemSize);
      evol->uOld = 0.;
   }

   double dt, res, t = 0., tol = 1.e-12;
   bool done = t >= config.tFinal;
   tic_toc.Clear();
   tic_toc.Start();
   if (myid == 0)
   {
      cout << "Preprocessing done. Entering time stepping loop.\n";
   }

   for (int ti = 0; !done;)
   {
      dt = min(config.dt, config.tFinal - t);
      odeSolver->Step(u, t, dt);
      ti++;

      done = (t >= config.tFinal - 1.e-8 * config.dt);

      if (hyp->SteadyState)
      {
         res = evol->ConvergenceCheck(dt, tol, u);
         if (res < tol)
         {
            done = true;
            u = evol->uOld;
         }
      }

      if (done || ti % config.VisSteps == 0)
      {
         if (myid == 0)
         {
            if (hyp->SteadyState)
            {
               cout << "time step: " << ti << ", time: " << t << ", residual: " << res << endl;
            }
            else
            {
               cout << "time step: " << ti << ", time: " << t << endl;
            }
         }

         ParVisualizeField(sout, vishost, visport, hyp->ProblemName, uk,
                           hyp->valuerange);
      }
   }

   tic_toc.Stop();
   if (myid == 0)
   {
      cout << "Time stepping loop done in " << tic_toc.RealTime() << " seconds.\n\n";
   }

   double FinalMass, DomainSize, DomainSizeMPI = evol->LumpedMassMat.Sum();
   MPI_Allreduce(&DomainSizeMPI, &DomainSize, 1, MPI_DOUBLE, MPI_SUM, comm);

   if (hyp->SolutionKnown)
   {
      Array<double> errors;
      hyp->ComputeErrors(errors, u, DomainSize, t);
      if (myid == 0)
      {
         cout << "L1 error:                    " << errors[0] << endl;
         if (hyp->FileOutput)
         {
            hyp->WriteErrors(errors);
         }
      }
   }

   MassMPI = evol->LumpedMassMat * u;
   MPI_Allreduce(&MassMPI, &FinalMass, 1, MPI_DOUBLE, MPI_SUM, comm);

   if (myid == 0)
   {
      cout << "Difference in solution mass: "
           << abs(InitialMass - FinalMass) / DomainSize << "\n\n";
   }

   if (hyp->FileOutput)
   {
      ofstream osol("ultimate.gf");
      osol.precision(config.precision);
      uk.SaveAsOne(osol);
   }

   delete evol;
   delete hyp;
   delete odeSolver;
   return 0;
}
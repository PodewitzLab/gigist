#include "Action_GIGIST.h"
#include "GIGIST_six_corr.h"
#include <iostream>
#include <iomanip>

/**
 * Standard constructor
 */
Action_GIGist::Action_GIGist() :
#ifdef CUDA
NBindex_c_(nullptr),
molecule_c_(nullptr),
paramsLJ_c_(nullptr),
result_w_c_(nullptr),
result_s_c_(nullptr),
result_O_c_(nullptr),
result_N_c_(nullptr),
#endif
list_(nullptr),
top_(nullptr),
dict_(DataDictionary()),
datafile_(nullptr),
dxfile_(nullptr),
febissWaterfile_(nullptr),
wrongNumberOfAtoms_(false)
{}

/**
 * The help function.
 */
void Action_GIGist::Help() const {
  mprintf("     Usage:\n"
          "    griddim [dimx dimy dimz]   Defines the dimension of the grid.\n"
          "    <gridcntr [x y z]>         Defines the center of the grid, default [0 0 0].\n"
          "    <temp 300>                 Defines the temperature of the simulation.\n"
          "    <gridspacn 0.5>            Defines the grid spacing\n"
          "    <refdens 0.0329>           Defines the reference density for the water model.\n"
          "    <febiss 104.57>            Activates FEBISS placement with given ideal water angle (only available for water)\n"
          "    <out \"out.dat\">          Defines the name of the output file.\n"
          "    <dx>                       Set to write out dx files. Population is always written.\n"
          "    <solventStart [n]>         Sets the first solvent as the nth molecule (necessary for CHCl3).\n"

          "  The griddimensions must be set in integer values and have to be larger than 0.\n"
          "  The greatest advantage, stems from the fact that this code is parallelized\n"
          "  on the GPU.\n\n"

          "  The code is meant to run on the GPU. Therefore, the CPU implementation of GIST\n"
          "  in this code is probably slower than the original GIST implementation.\n\n"

          "  When using this GIST implementation please cite:\n"
          "#    Johannes Kraml, Anna S. Kamenik, Franz Waibl, Michael Schauperl, Klaus R. Liedl, JCTC (2019)\n"
          "#    Steven Ramsey, Crystal Nguyen, Romelia Salomon-Ferrer, Ross C. Walker, Michael K. Gilson, and Tom Kurtzman\n"
          "#      J. Comp. Chem. 37 (21) 2016\n"
          "#    Crystal Nguyen, Michael K. Gilson, and Tom Young, arXiv:1108.4876v1 (2011)\n"
          "#    Crystal N. Nguyen, Tom Kurtzman Young, and Michael K. Gilson,\n"
          "#      J. Chem. Phys. 137, 044101 (2012)\n"
          "#    Lazaridis, J. Phys. Chem. B 102, 3531–3541 (1998)\n");
}

Action_GIGist::~Action_GIGist() {
  // The GPU memory should already be freed, but just in case...
  #ifdef CUDA
  freeGPUMemory();
  #endif
}

/*****
 * @brief Calculate the start of the grid.
 * 
 * Calculates the start of the grid, using the center, stepsize in each
 * dimension and the voxel Size.
 */
void Action_GIGist::calcGridStart() noexcept
{
  info_.grid.start.SetVec(info_.grid.center[0] - (info_.grid.dimensions[0] * 0.5) * info_.grid.voxelSize, 
                          info_.grid.center[1] - (info_.grid.dimensions[1] * 0.5) * info_.grid.voxelSize,
                          info_.grid.center[2] - (info_.grid.dimensions[2] * 0.5) * info_.grid.voxelSize);
}

/*****
 * @brief Calculate the end of the grid.
 * 
 * Calculates the end of the grid, using the center, stepsize in each
 * dimension and the voxel Size.
 */
void Action_GIGist::calcGridEnd() noexcept
{
  info_.grid.end.SetVec(info_.grid.center[0] + info_.grid.dimensions[0] * info_.grid.voxelSize, 
                        info_.grid.center[1] + info_.grid.dimensions[1] * info_.grid.voxelSize,
                        info_.grid.center[2] + info_.grid.dimensions[2] * info_.grid.voxelSize);
}

/*****
 * @brief Get the info of the system.
 * 
 * A helper function that takes the system specific information
 * that is supplied by the user.
 */
void Action_GIGist::getSystemInfo(ArgList &argList)
{
  info_.system.temperature = argList.getKeyDouble("temp", 300.0);
  info_.system.rho0 = argList.getKeyDouble("refdens", 0.0329);
  info_.system.nFrames = 0;
}

/*****
 * @brief Get settings for the GIST calculation.
 * 
 * This function takes the settings supplied by the user and sets the 
 * appropriate values.
 */
void Action_GIGist::getGistSettings(ArgList &argList)
{
  info_.gist.solventStart = argList.getKeyInt("solventStart", -1);
  info_.gist.neighborCutoff = argList.getKeyDouble("neighbour", 3.5);
  info_.gist.neighborCutoff *= info_.gist.neighborCutoff;
  info_.gist.calcEnergy = !(argList.hasKey("skipE"));
  info_.gist.writeDx = argList.hasKey("dx");
  info_.gist.doorder = argList.hasKey("doorder");
  info_.gist.useCOM = argList.hasKey("com");
  info_.gist.febiss = argList.hasKey("febiss");
  info_.gist.idealWaterAngle_ = argList.getKeyDouble("febiss_angle", 104.57);
}

/*****
 * @brief Grid Calculation.
 * 
 * Builds the grid structure from the settings of the user and then creates
 * the logical concepts.
 */
bool Action_GIGist::buildGrid(ArgList &argList)
{
  info_.grid.voxelSize = argList.getKeyDouble("gridspacn", 0.5);
  info_.grid.voxelVolume = info_.grid.voxelSize * info_.grid.voxelSize * info_.grid.voxelSize;

  if (argList.Contains("griddim")) {
    ArgList dimArgs = argList.GetNstringKey("griddim", 3);
    info_.grid.dimensions[0] = dimArgs.getNextInteger(-1.0);
    info_.grid.dimensions[1] = dimArgs.getNextInteger(-1.0);
    info_.grid.dimensions[2] = dimArgs.getNextInteger(-1.0);
    if ( (info_.grid.dimensions[0] <= 0) || 
          (info_.grid.dimensions[1] <= 0) || 
          (info_.grid.dimensions[2] <= 0) ) {
      mprinterr("Error: griddimension must be positive integers (non zero).\n\n");
      return false;
    }
    info_.grid.nVoxels = info_.grid.dimensions[0] * info_.grid.dimensions[1] * info_.grid.dimensions[2];
  } else {
    mprinterr("Error: Dimensions must be set!\n\n");
    return false;
  }

  double x = 0, y = 0, z = 0;
  if (argList.Contains("gridcntr")) {
    ArgList cntrArgs = argList.GetNstringKey("gridcntr", 3);
    x = cntrArgs.getNextDouble(-1);
    y = cntrArgs.getNextDouble(-1);
    z = cntrArgs.getNextDouble(-1);
  } else {
    mprintf("Warning: No grid center specified, defaulting to origin!\n\n");
  }
  info_.grid.center.SetVec(x, y, z);

  calcGridStart();
  calcGridEnd();

  return true;
}

/***
 * Document in header FILE!
 * 
 */
bool Action_GIGist::analyzeInfo(ArgList &argList)
{
  getSystemInfo(argList);
  getGistSettings(argList);
  bool ret{ buildGrid(argList) };
  #ifdef CUDA
  if (info_.gist.doorder && !info_.gist.calcEnergy) {
    mprinterr("Error: For CUDA code, if energy is not calculated, order parameter cannot be calculated.");
    ret = false;
  }
  #endif
  return ret;
}

/*****
 * @brief Print citation and info.
 */
void Action_GIGist::printCitationInfo() const noexcept
{
  mprintf("Center: %g %g %g, Dimensions %d %d %d\n"
    "  When using this GIST implementation please cite:\n"
    "#    Johannes Kraml, Anna S. Kamenik, Franz Waibl, Michael Schauperl, Klaus R. Liedl, JCTC (2019)\n"
    "#    Steven Ramsey, Crystal Nguyen, Romelia Salomon-Ferrer, Ross C. Walker, Michael K. Gilson, and Tom Kurtzman\n"
    "#      J. Comp. Chem. 37 (21) 2016\n"
    "#    Crystal Nguyen, Michael K. Gilson, and Tom Young, arXiv:1108.4876v1 (2011)\n"
    "#    Crystal N. Nguyen, Tom Kurtzman Young, and Michael K. Gilson,\n"
    "#      J. Chem. Phys. 137, 044101 (2012)\n"
    "#    Lazaridis, J. Phys. Chem. B 102, 3531–3541 (1998)\n",
    info_.grid.center[0],
    info_.grid.center[1],
    info_.grid.center[2],
    static_cast<int>( info_.grid.dimensions[0] ),
    static_cast<int>( info_.grid.dimensions[1] ),
    static_cast<int>( info_.grid.dimensions[2] ) 
  );
}

/*****
 * @brief Prepare the variables for the calculation on the GPU.
 * 
 * The different variables are set and prepared (i.e., memory allocated), so
 * that the calculation on the GPU can be performed without any problems.
 */
bool Action_GIGist::prepareGPUCalc(ActionSetup &setup) {
#ifdef CUDA
  if (info_.gist.calcEnergy) {
    NonbondParmType nb{ setup.Top().Nonbond() };
    NBIndex_ = nb.NBindex();
    numberAtomTypes_ = nb.Ntypes();
    for (unsigned int i = 0; i < nb.NBarray().size(); ++i) {
      lJParamsA_.push_back( (float) nb.NBarray().at(i).A() );
      lJParamsB_.push_back( (float) nb.NBarray().at(i).B() );
    }

    try {
      allocateCuda_GIGIST(((void**)&NBindex_c_), NBIndex_.size() * sizeof(int));
      allocateCuda_GIGIST((void**)&result_w_c_, info_.system.numberAtoms * sizeof(float));
      allocateCuda_GIGIST((void**)&result_s_c_, info_.system.numberAtoms * sizeof(float));
      allocateCuda_GIGIST((void**)&result_O_c_, info_.system.numberAtoms * 4 * sizeof(int));
      allocateCuda_GIGIST((void**)&result_N_c_, info_.system.numberAtoms * sizeof(int));
    } catch (CudaException &e) {
      mprinterr("Error: Could not allocate memory on GPU!\n");
      freeGPUMemory();
      return false;
    }
    try {
      copyToGPU();
    } catch (CudaException &e) {
      return false;
    }
  }
#endif
  return true;
}

/*****
 * @brief Resize the vectors needed for the calculation.
 */
void Action_GIGist::resizeVectors()
{
  if (info_.gist.febiss) {
    hVectors_.resize( info_.grid.nVoxels );
  }
  centersAndRotations_.resize(info_.grid.nVoxels, info_.system.numberSolvent * info_.system.nFrames);
}

/*****
 * @brief Create the needed datafiles and datasets.
 * 
 * This function creates the neceesary datasets, as well as datafiles. For the
 * datasets, there are multiple different datasets, basically for the energies,
 * the entropies, the dipole moments, neighbors, etc., as well as the densities
 * for each atom of the solvent.
 * 
 * @param argList The argument list that was given by the user
 * @param actionInit The action initialization object
 */
void Action_GIGist::createDatasets(ArgList &argList, ActionInit &actionInit)
{
  std::string outfilename{argList.GetStringKey("out", "out.dat")};
  datafile_ = actionInit.DFL().AddCpptrajFile( outfilename, "GIST output" );

  std::string dsname{ actionInit.DSL().GenerateDefaultName("GIST") };
  result_ = std::vector<DataSet_3D *>(dict_.size());
  for (unsigned int i = 0; i < dict_.size(); ++i) {
    result_.at(i) = (DataSet_3D*)actionInit.DSL().AddSet(DataSet::GRID_FLT, MetaData(dsname, dict_.getElement(i)));
    result_.at(i)->Allocate_N_C_D(
      info_.grid.dimensions[0],
      info_.grid.dimensions[1],
      info_.grid.dimensions[2],
      info_.grid.center,
      info_.grid.voxelSize
    );

    if (
        ( info_.gist.writeDx &&
          dict_.getElement(i).compare("Eww") != 0 && 
          dict_.getElement(i).compare("Esw") != 0 &&
          dict_.getElement(i).compare("dipole_xtemp") != 0 && 
          dict_.getElement(i).compare("dipole_ytemp") != 0 &&
          dict_.getElement(i).compare("dipole_ztemp") != 0 &&
          dict_.getElement(i).compare("order") != 0 && 
          dict_.getElement(i).compare("neighbour") != 0 ) ||
        i == 0 
       ) 
    {
      DataFile *file = actionInit.DFL().AddDataFile(dict_.getElement(i) + ".dx");
      file->AddDataSet(result_.at(i));
    }
  }
  if (info_.gist.febiss) {
    this->febissWaterfile_ = actionInit.DFL().AddCpptrajFile( "febiss-waters.pdb", "GIST output");
  }
}

/*****
 * @brief Adds atom type information into the different vectors.
 * 
 * This function adds the charges, atom types, masses and to which molecule a
 * given atom belongs to the appropriate vectors.
 * 
 * @param atom The atom for which the information should be extracted
 */
void Action_GIGist::addAtomType(const Atom &atom)
{
  molecule_.push_back(atom.MolNum());
  charges_.push_back(atom.Charge());
  atomTypes_.push_back(atom.TypeIndex());
  masses_.push_back(atom.Mass());
}


/*****
 * @brief From a given molecule get all atom information needed.
 * 
 * This function finds all the different parameters needed for the GIST
 * calculation. Predominatntly, this function adds the different solvent
 * specific informations about the atom. But also atom type information is
 * added.
 * 
 * @param setup The setup object, where setup stuff is safed.
 * @param mol The molecule for which the atoms should be added
 * @param firstRound A few informations are only needed for the first round,
 *                so that some things can be skipped during the other rounds.
 */
void Action_GIGist::setAtomInformation(
  const ActionSetup &setup,
  const Molecule& mol,
  bool firstRound
)
{
  int nAtoms{ mol.NumAtoms() };

  for (int i = 0; i < nAtoms; ++i) {
    addAtomType(setup.Top()[mol.MolUnit().Front() + i]);
    // Check if the molecule is a solvent, either by the topology parameters or because force was set.
    if ( (mol.IsSolvent() && info_.gist.solventStart == -1) 
            || (( info_.gist.solventStart > -1 ) 
                        && 
                ( setup.Top()[mol.MolUnit().Front()].MolNum() >= info_.gist.solventStart ))
    ) {

      std::string aName{ setup.Top()[mol.MolUnit().Front() + i].ElementName() };
      
      // Check if dictionary already holds an entry for the atoms name, if not add it to
      // the dictionary, if yes, add 1 to the correct solvent atom counter.
      if (! (dict_.contains(aName)) ) {
        dict_.add(aName);
        solventAtomCounter_.push_back(1);
      } else if (firstRound) {
        solventAtomCounter_.at(dict_.getIndex(aName) - result_.size()) += 1;
      }
      // Check for the centerSolventAtom (which in this easy approximation is either C or O)
      if ( weight(aName) < weight(info_.gist.centerAtom) ) {
        info_.gist.centerAtom = setup.Top()[mol.MolUnit().Front() + i].ElementName();
        info_.gist.centerIdx = i; // Assumes the same order of atoms.
        info_.gist.centerType = setup.Top()[mol.MolUnit().Front() + i].TypeIndex();
      }
      // Set solvent to true
      solvent_[mol.MolUnit().Front() + i] = true;
    } else {
      solvent_[mol.MolUnit().Front() + i] = false;
    }
  }
}

/*****
 * @brief Goes over all molecules and sets the appropriate information for this
 * molecule.
 * 
 * Compared to the atom function, this function does not really change much by
 * itself, but simply goes over all the different molecules and hands the
 * different molecules to the atom function.
 * 
 * @param setup The action setup object for setting up the GIST calculation
 */
void Action_GIGist::setMoleculeInformation(ActionSetup &setup)
{
  bool firstRound{ true };

  // Save different values, which depend on the molecules and/or atoms.
  for (auto mol = setup.Top().MolStart(); 
       mol != setup.Top().MolEnd(); ++mol) {
    setAtomInformation(setup, *(mol.base()), firstRound);
    if ((mol->IsSolvent() && info_.gist.solventStart == -1) || 
          (
            ( info_.gist.solventStart > -1 ) 
            && 
            ( setup.Top()[mol->MolUnit().Front()].MolNum() >= info_.gist.solventStart )
          )
    ) {
      firstRound = false;
    }
  }
}


/*****
 * @brief Prepares the density grids
 * 
 * Before this was changed, only water could be analyzed, so this stuff did
 * not need to be dynamically managed. Since I changed that, this must also be
 * changed, so that this is now dynamically allocated.
 */
void Action_GIGist::prepDensityGrids()
{
  // Add results for the different solvent atoms.
  for (unsigned int i = 0; i < (dict_.size() - result_.size()); ++i) {
    resultV_.push_back(
      std::vector<double>(
        info_.grid.dimensions[0] *
        info_.grid.dimensions[1] *
        info_.grid.dimensions[2]
      )
    );
  }
}

/*****
 * @brief Prepares the quaternion calculation
 * 
 * This function analyzes the molecule and picks appropriate atoms for the 
 * quaternion construction needed for the GIST calculations.
 * 
 * @param frame The frame is needed, because for the calculation the
 *              coordinates are needed.
 */
void Action_GIGist::prepQuaternion(ActionFrame &frame)
{
  for (Topology::mol_iterator mol = top_->MolStart(); mol < top_->MolEnd(); ++mol) {
      int moleculeLength = mol->MolUnit().Back() - mol->MolUnit().Front() + 1;
      if (moleculeLength < 3)
         continue;
      if ((mol->IsSolvent() && info_.gist.solventStart == -1 ) || 
        (
          ( info_.gist.solventStart > -1 ) &&
          ( top_->operator[](mol->MolUnit().Front()).MolNum() >= info_.gist.solventStart )
          )
      ) {
        quat_indices_ = calcQuaternionIndices(mol->MolUnit().Front(), mol->MolUnit().Back(), frame.Frm().XYZ(mol->MolUnit().Front()));
        break;
      }
    }
}


/**
 * Initialize the GIST calculation by setting up the users input.
 * @param argList: The argument list of the user.
 * @param actionInit: The action initialization object.
 * @return: Action::OK on success and Action::ERR on error.
 */
Action::RetType Action_GIGist::Init(ArgList &argList, ActionInit &actionInit, int test) {
#if defined MPI
  if (actionInit.TrajComm().Size() > 1) {
    mprinterr("Error: GIST cannot yet be used with MPI parallelization.\n"
              "       Maximum allowed processes is 1, you used %d.\n",
              actionInit.TrajComm().Size());
    return Action::ERR;
  }
#endif
  
  // Get Infos
  if (! analyzeInfo(argList) ) {
    return Action::ERR;
  }
  
  // Imaging
  image_.InitImaging( true );
  resizeVectors();
  createDatasets(argList, actionInit);
  printCitationInfo();
  
  return Action::OK;
}

/**
 * Setup for the GIST calculation. Does everything involving the Topology file.
 * @param setup: The setup object of the cpptraj code libraries.
 * @return: Action::OK on success, Action::ERR otherwise.
 */
Action::RetType Action_GIGist::Setup(ActionSetup &setup) {
  solventAtomCounter_ = std::vector<int>();
  // Setup imaging and topology parsing.
  image_.SetupImaging( setup.CoordInfo().TrajBox().HasBox() );

  // Save topology and topology related values
  top_             = setup.TopAddress();
  info_.system.numberAtoms = setup.Top().Natom();
  info_.system.numberSolvent   = setup.Top().Nsolvent();
  //solvent_ = std::make_unique<bool []>(info_.system.numberAtoms);
  solvent_ = std::unique_ptr<bool []>(new bool[info_.system.numberAtoms]);

  setMoleculeInformation(setup);

  prepDensityGrids();

  if (!prepareGPUCalc(setup)) {
    return Action::ERR;
  }

  return Action::OK;
}



Action_GIGist::TestObj Action_GIGist::calcBoxParameters(const ActionFrame &frame)
{
  // Setting up Image type here, don't know why this is necessary at all...
  if (image_.ImagingEnabled()) {
      image_.SetImageType( frame.Frm().BoxCrd().Is_X_Aligned_Ortho() );
  }
  Matrix_3x3 ucell_m{}, recip_m{};
  std::unique_ptr<float[]> recip;
  std::unique_ptr<float[]> ucell;;
  int boxinfo{};
  // Check Boxinfo and write the necessary data into recip, ucell and boxinfo.
  switch(image_.ImagingType()) {
    case ImageOption::NONORTHO:
      recip = std::unique_ptr<float[]>(new float[9]);
      ucell = std::unique_ptr<float[]>(new float[9]);
      ucell_m = frame.Frm().BoxCrd().UnitCell();
      recip_m = frame.Frm().BoxCrd().FracCell();
      //frame.Frm().BoxCrd().ToRecip(ucell_m, recip_m);
      for (int i = 0; i < 9; ++i) {
        ucell[i] = static_cast<float>( ucell_m.Dptr()[i] );
        recip[i] = static_cast<float>( recip_m.Dptr()[i] );
      }
      boxinfo = 2;
      break;
    case ImageOption::ORTHO:
      recip = std::unique_ptr<float[]>(new float[9]);
      for (int i = 0; i < 3; ++i) {
        recip[i] = static_cast<float>( frame.Frm().BoxCrd().XyzPtr()[i] );
      }
      ucell = nullptr;
      boxinfo = 1;
      break;
    case ImageOption::NO_IMAGE:
      recip = nullptr;
      ucell = nullptr;
      boxinfo = 0;
      break;
    default:
      throw "Error: Unexpected box information found.";
  }
  TestObj test;
  test.recip.swap(recip);
  test.ucell.swap(ucell);
  test.boxinfo = boxinfo;

  return test;
}

void Action_GIGist::calcHVectors(
  int voxel,
  int headAtomIndex,
  const std::vector<Vec3> &molAtomCoords)
{
  Vec3 X;
  Vec3 Y;
  bool setX = false;
  bool setY = false;
  if (info_.gist.febiss){
    for (unsigned int i = 0; i < molAtomCoords.size(); ++i) {
      if ((int)i != headAtomIndex) {
        if (setX && !setY) {
          Y.SetVec(molAtomCoords.at(i)[0] - molAtomCoords.at(headAtomIndex)[0], 
                    molAtomCoords.at(i)[1] - molAtomCoords.at(headAtomIndex)[1], 
                    molAtomCoords.at(i)[2] - molAtomCoords.at(headAtomIndex)[2]);
          hVectors_.at(voxel).push_back(Y);
          Y.Normalize();
          setY = true;
        }
        if (!setX) {
          X.SetVec(molAtomCoords.at(i)[0] - molAtomCoords.at(headAtomIndex)[0], 
                    molAtomCoords.at(i)[1] - molAtomCoords.at(headAtomIndex)[1], 
                    molAtomCoords.at(i)[2] - molAtomCoords.at(headAtomIndex)[2]);
          hVectors_.at(voxel).push_back(X);
          X.Normalize();
          setX = true;
        }
        if (setX && setY) {
          break;
        }
      }
    }
  }
}

std::tuple<Vec3, int> Action_GIGist::prepCom(const Molecule& mol, const ActionFrame& frame) {
  int mol_begin{ mol.MolUnit().Front() };
  int mol_end{ mol.MolUnit().Back() };
  Vec3 com{ calcCenterOfMass(mol_begin, mol_end, frame.Frm().XYZ(mol_begin)) };
  return { com, bin(mol_begin, mol_end, com, frame) };
}

std::tuple<std::vector<DOUBLE_O_FLOAT>, 
      std::vector<DOUBLE_O_FLOAT>,
      std::vector<int>,
      std::vector<int>
> Action_GIGist::calcGPUEnergy(const ActionFrame &frame) 
{
  #ifdef CUDA
  tEnergy_.Start();
  std::vector<DOUBLE_O_FLOAT> eww_result;
  std::vector<DOUBLE_O_FLOAT> esw_result;
  std::vector<int> result_o( 4 * info_.system.numberAtoms );
  std::vector<int> result_n( info_.system.numberAtoms );
  if (info_.gist.calcEnergy){
    auto boxParams{ calcBoxParameters(frame) };

    // std::vector<int> result_o{ std::vector<int>(4 * info_.system.numberAtoms) };
    // std::vector<int> result_n{ std::vector<int>(info_.system.numberAtoms) };
    // TODO: Switch things around a bit and move the back copying to the end of the calculation.
    //       Then the time needed to go over all waters and the calculations that come with that can
    //       be hidden quite nicely behind the interaction energy calculation.
    // Must create arrays from the vectors, does that by getting the address of the first element of the vector.
    auto e_result{ 
      doActionCudaEnergy_GIGIST(
        frame.Frm().xAddress(),
        NBindex_c_,
        numberAtomTypes_,
        paramsLJ_c_,
        molecule_c_,
        boxParams.boxinfo,
        boxParams.recip.get(),
        boxParams.ucell.get(),
        info_.system.numberAtoms,
        info_.gist.centerType,
        info_.gist.neighborCutoff,
        &(result_o[0]),
        &(result_n[0]),
        result_w_c_,
        result_s_c_,
        result_O_c_,
        result_N_c_,
        info_.gist.doorder) };
    eww_result = std::move(e_result.eww);
    esw_result = std::move(e_result.esw);

    std::vector<std::vector<int>> order_indices{};

    if (info_.gist.doorder) {
      int counter{ 0 };
      for (int i = 0; i < (4 * info_.system.numberAtoms); i += 4) {
        ++counter;
        std::vector<int> temp{};
        for (unsigned int j = 0; j < 4; ++j) {
          temp.push_back(result_o.at(i + j));
        }
        order_indices.push_back(temp);
      }
    }

    tEnergy_.Stop();
  }
  return { eww_result, esw_result, result_o, result_n };
  #else
  return {std::vector<DOUBLE_O_FLOAT>(), 
    std::vector<DOUBLE_O_FLOAT>(),
    std::vector<int>(),
    std::vector<int>()};
  #endif
}



/**
 * Starts the calculation of GIST. Can use either CUDA, OPENMP or single thread code.
 * This function is actually way too long. Refactoring of this code might help with
 * readability.
 * @param frameNum: The number of the frame.
 * @param frame: The frame itself.
 * @return: Action::ERR on error, Action::OK if everything ran smoothly.
 */
Action::RetType Action_GIGist::DoAction(int frameNum, ActionFrame &frame) {

  info_.system.nFrames++;
  std::vector<DOUBLE_O_FLOAT> eww_result{};
  std::vector<DOUBLE_O_FLOAT> esw_result{};
  std::vector<std::vector<int> > order_indices{};

  if (info_.gist.febiss && info_.system.nFrames == 1) {
    this->writeOutSolute(frame);
  }

  if (info_.gist.useCOM && info_.system.nFrames == 1) 
  {
    prepQuaternion(frame);
  }

  // CUDA necessary information
  #ifdef CUDA

  auto energyResults = calcGPUEnergy(frame);

  eww_result = std::move(std::get<0>(energyResults));
  esw_result = std::move(std::get<1>(energyResults));
  #endif

  std::vector<bool> onGrid(info_.system.numberAtoms, false);
  /*for (unsigned int i = 0; i < onGrid.size(); ++i) {
    onGrid.at(i) = false;
  }*/

  #if defined _OPENMP && defined CUDA
  tHead_.Start();
  #pragma omp parallel for
  #endif
  for (Topology::mol_iterator mol = top_->MolStart(); mol < top_->MolEnd(); ++mol) {
    if ((mol->IsSolvent() && info_.gist.solventStart == -1) || 
      (
        ( info_.gist.solventStart > -1 ) &&
        ( top_->operator[](mol->MolUnit().Front()).MolNum() >= info_.gist.solventStart )
      )
    ) {
      int headAtomIndex{ -1 };
      // Keep voxel at -1 if it is not possible to put it on the grid
      int voxel{ -1 };
      std::vector<Vec3> molAtomCoords{};
      Vec3 com{ 0, 0, 0 };
      Vec3 coord{ 0, 0, 0 };

      // If center of mass should be used, use this part.
      if (info_.gist.useCOM) {
        auto test = prepCom(*mol, frame);
        com = std::get<0>(test);
        coord = com;
        voxel = std::get<1>(test);
      }
      

      #if !defined _OPENMP && !defined CUDA
      tHead_.Start();
      #endif
      for (int atom1 = mol->MolUnit().Front(); atom1 < mol->MolUnit().Back(); ++atom1) {
        bool first{ true };
        if (solvent_[atom1]) { // Do we need that?
          // Save coords for later use.
          const double *vec = frame.Frm().XYZ(atom1);
          molAtomCoords.push_back(Vec3(vec));
          // Check if atom is "Head" atom of the solvent
          // Could probably save some time here by writing head atom indices into an array.
          // TODO: When assuming fixed atom position in topology, should be very easy.
          if ( !info_.gist.useCOM && std::string((*top_)[atom1].ElementName()).compare(info_.gist.centerAtom) == 0 && first ) {
            // Try to bin atom1 onto the grid. If it is possible, get the index and keep working,
            // if not, calculate the energies between all atoms to this point.
            voxel = bin(mol->MolUnit().Front(), mol->MolUnit().Back(), vec, frame);
            coord = vec;
            headAtomIndex = atom1 - mol->MolUnit().Front();
            first = false;
          } else {
            size_t bin_i{}, bin_j{}, bin_k{};
            if ( result_.at(dict_.getIndex("population"))->Bin().Calc(vec[0], vec[1], vec[2], bin_i, bin_j, bin_k) 
                    /*&& bin_i < dimensions_[0] && bin_j < dimensions_[1] && bin_k < dimensions_[2]*/) {
              std::string aName{ top_->operator[](atom1).ElementName() };
              long voxTemp{ result_.at(dict_.getIndex("population"))->CalcIndex(bin_i, bin_j, bin_k) };
              #ifdef _OPENMP
              #pragma omp critical
              {
              #endif
              try{
                resultV_.at(dict_.getIndex(aName) - result_.size()).at(voxTemp) += 1.0;
              } catch(std::out_of_range e)
              {
                std::cout << std::setprecision(30) << (size_t)((vec[0] + 35.0f) / 0.5f) << ", " << vec[1] << ", " << vec[2] << '\n';
                std::cout << result_.at(dict_.getIndex("population"))->Bin().Calc(vec[0], vec[1], vec[2], bin_i, bin_j, bin_k) << '\n';
                std::cout << bin_i << " " << bin_j << " " << bin_k << '\n';
                std::cout << voxTemp << '\n';
                throw std::out_of_range("");
              }
              #ifdef _OPENMP
              }
              #endif
            }
          }
        }
      }
      #if !defined _OPENMP && !defined CUDA
      tHead_.Stop();
      #endif

      

      if (voxel != -1) {

        calcHVectors(voxel, headAtomIndex, molAtomCoords);

        #if !defined _OPENMP
        tRot_.Start();
        #endif
        
        Quaternion<DOUBLE_O_FLOAT> quat{};
        
        // Create Quaternion for the rotation from the new coordintate system to the lab coordinate system.
        if (!info_.gist.useCOM) {
          quat = calcQuaternion(molAtomCoords, molAtomCoords.at(headAtomIndex), headAtomIndex);
        } else {
          // -1 Will never evaluate to true, so in the funciton it will have no consequence.
          quat = calcQuaternion(molAtomCoords, com, quat_indices_);
        }
        //if (quat.initialized())
        //{
          #ifdef _OPENMP
          #pragma omp critical
          {
          #endif
          centersAndRotations_.push_back(voxel, {coord, quat, info_.system.nFrames});
          #ifdef _OPENMP
          }
          #endif
        //}
        

        #if !defined _OPENMP
        tRot_.Stop();
        #endif
        
  // If energies are already here, calculate the energies right away.
  #ifdef CUDA
        /* 
        * Calculation of the order parameters
        * Following formula:
        * q = 1 - 3/8 * SUM[a>b]( cos(Thet[a,b]) + 1/3 )**2
        * This, however, only makes sense for water, so please do not
        * use it for any other solvent.
        */
        if (info_.gist.doorder) {
          double sum{ 0 };
          Vec3 cent{ frame.Frm().xAddress() + (mol->MolUnit().Front() + headAtomIndex) * 3 };
          std::vector<Vec3> vectors{};
          switch(image_.ImagingType()) {
            case ImageOption::NONORTHO:
            case ImageOption::ORTHO:
              {
                Matrix_3x3 ucell, recip;
                ucell = frame.Frm().BoxCrd().UnitCell();
                recip = frame.Frm().BoxCrd().FracCell();
                //frame.Frm().BoxCrd().ToRecip(ucell, recip);
                Vec3 vec(frame.Frm().xAddress() + (order_indices.at(mol->MolUnit().Front() + headAtomIndex).at(0) * 3));
                vectors.push_back( MinImagedVec(vec, cent, ucell, recip));
                vec = Vec3(frame.Frm().xAddress() + (order_indices.at(mol->MolUnit().Front() + headAtomIndex).at(1) * 3));
                vectors.push_back( MinImagedVec(vec, cent, ucell, recip));
                vec = Vec3(frame.Frm().xAddress() + (order_indices.at(mol->MolUnit().Front() + headAtomIndex).at(2) * 3));
                vectors.push_back( MinImagedVec(vec, cent, ucell, recip));
                vec = Vec3(frame.Frm().xAddress() + (order_indices.at(mol->MolUnit().Front() + headAtomIndex).at(3) * 3));
                vectors.push_back( MinImagedVec(vec, cent, ucell, recip));
              }
              break;
            default:
              vectors.push_back( Vec3( frame.Frm().xAddress() + (order_indices.at(mol->MolUnit().Front() + headAtomIndex).at(0) * 3) ) - cent );
              vectors.push_back( Vec3( frame.Frm().xAddress() + (order_indices.at(mol->MolUnit().Front() + headAtomIndex).at(1) * 3) ) - cent );
              vectors.push_back( Vec3( frame.Frm().xAddress() + (order_indices.at(mol->MolUnit().Front() + headAtomIndex).at(2) * 3) ) - cent );
              vectors.push_back( Vec3( frame.Frm().xAddress() + (order_indices.at(mol->MolUnit().Front() + headAtomIndex).at(3) * 3) ) - cent );
          }
          
          for (int i = 0; i < 3; ++i) {
            for (int j = i + 1; j < 4; ++j) {
              double cosThet{ (vectors.at(i) * vectors.at(j)) / sqrt(vectors.at(i).Magnitude2() * vectors.at(j).Magnitude2()) };
              sum += (cosThet + 1.0/3) * (cosThet + 1.0/3);
            }
          }
          #ifdef _OPENMP
          #pragma omp critical
          {
          #endif
          result_.at(dict_.getIndex("order"))->UpdateVoxel(voxel, 1.0 - (3.0/8.0) * sum);
          #ifdef _OPENMP
          }
          #endif
        }
        #ifdef _OPENMP
        #pragma omp critical
        {
        #endif
        result_.at(dict_.getIndex("neighbour"))->UpdateVoxel(voxel, std::get<3>(energyResults).at(mol->MolUnit().Front() + headAtomIndex));
        #ifdef _OPENMP
        }
        #endif
        // End of calculation of the order parameters

        #ifndef _OPENMP
        tEadd_.Start();
        #endif
        #ifdef _OPENMP
        #pragma omp critical
        {
        #endif
        // There is absolutely nothing to check here, as the solute can not be in place here.
        for (int atom = mol->MolUnit().Front(); atom < mol->MolUnit().Back(); ++atom) {
          // Just adds up all the interaction energies for this voxel.
          result_.at(dict_.getIndex("Eww"))->UpdateVoxel(voxel, static_cast<double>(eww_result.at(atom)));
          result_.at(dict_.getIndex("Esw"))->UpdateVoxel(voxel, static_cast<double>(esw_result.at(atom)));
        }
        #ifdef _OPENMP
        }
        #endif
        #ifndef _OPENMP
        tEadd_.Stop();
        #endif
  #endif
      }

      // If CUDA is used, energy calculations are already done.
  #ifndef CUDA
      if (voxel != -1 ) {
        std::vector<Vec3> nearestWaters(4);
        // Use HUGE distances at the beginning. This is defined as 3.40282347e+38F.
        double distances[4]{HUGE, HUGE, HUGE, HUGE};
        // Needs to be fixed, one does not need to calculate all interactions each time.
        for (int atom1 = mol->MolUnit().Front(); atom1 < mol->MolUnit().Back(); ++atom1) {
          double eww{ 0 };
          double esw{ 0 };
  // OPENMP only over the inner loop

          #pragma omp parallel for
          for (unsigned int atom2 = 0; atom2 < info_.system.numberAtoms; ++atom2) {
            if ( (*top_)[atom1].MolNum() != (*top_)[atom2].MolNum() ) {
              tEadd_.Start();
              double r_2{ calcDistanceSqrd(frame, atom1, atom2) };
              double energy{ calcEnergy(r_2, atom1, atom2) };
              tEadd_.Stop();
              if (solvent_[atom2]) {
                #pragma omp atomic
                eww += energy;
              } else {
                #pragma omp atomic
                esw += energy;
              }
              if (atomTypes_.at(atom1) == info_.gist.centerType &&
                  atomTypes_.at(atom2) == info_.gist.centerType) {
                if (r_2 < distances[0]) {
                  distances[3] = distances[2];
                  distances[2] = distances[1];
                  distances[1] = distances[0];
                  distances[0] = r_2;
                  nearestWaters.at(3) = nearestWaters.at(2);
                  nearestWaters.at(2) = nearestWaters.at(1);
                  nearestWaters.at(1) = nearestWaters.at(0);
                  nearestWaters.at(0) = Vec3(frame.Frm().XYZ(atom2)) - Vec3(frame.Frm().XYZ(atom1));
                } else if (r_2 < distances[1]) {
                  distances[3] = distances[2];
                  distances[2] = distances[1];
                  distances[1] = r_2;
                  nearestWaters.at(3) = nearestWaters.at(2);
                  nearestWaters.at(2) = nearestWaters.at(1);
                  nearestWaters.at(1) = Vec3(frame.Frm().XYZ(atom2)) - Vec3(frame.Frm().XYZ(atom1));
                } else if (r_2 < distances[2]) {
                  distances[3] = distances[2];
                  distances[2] = r_2;
                  nearestWaters.at(3) = nearestWaters.at(2);
                  nearestWaters.at(2) = Vec3(frame.Frm().XYZ(atom2)) - Vec3(frame.Frm().XYZ(atom1));
                } else if (r_2 < distances[3]) {
                  distances[3] = r_2;
                  nearestWaters.at(3) = Vec3(frame.Frm().XYZ(atom2)) - Vec3(frame.Frm().XYZ(atom1));
                }
                if (r_2 < info_.gist.neighborCutoff) {
                  #ifdef _OPENMP
                  #pragma omp critical
                  {
                  #endif
                  result_.at(dict_.getIndex("neighbour"))->UpdateVoxel(voxel, 1);
                  #ifdef _OPENMP
                  }
                  #endif
                }
              }
            }
          }
          double sum{ 0 };
          for (int i = 0; i < 3; ++i) {
            for (int j = i + 1; j < 4; ++j) {
              double cosThet{ (nearestWaters.at(i) * nearestWaters.at(j)) / 
                                sqrt(nearestWaters.at(i).Magnitude2() * nearestWaters.at(j).Magnitude2()) };
              sum += (cosThet + 1.0/3) * (cosThet + 1.0/3);
            }
          }
          #ifdef _OPENMP
          #pragma omp critical
          {
          #endif
          result_.at(dict_.getIndex("order"))->UpdateVoxel(voxel, 1.0 - (3.0/8.0) * sum);
          eww /= 2.0;
          result_.at(dict_.getIndex("Eww"))->UpdateVoxel(voxel, eww);
          result_.at(dict_.getIndex("Esw"))->UpdateVoxel(voxel, esw);
          #ifdef _OPENMP
          }
          #endif
        }
      }
#endif
    }
  }
  
  #if defined _OPENMP && defined CUDA
  tHead_.Stop();
  #endif

  return Action::OK;
}

/**
 * Post Processing is done here.
 */
void Action_GIGist::Print() {
  /* This is not called for two reasons
   * 1) The RAM on the GPU is far less than the main memory
   * 2) It does not speed up the calculation significantly enough
   * However, this can be changed if wished for (It is not yet stable enough to be used)
   * Tests are ongoing
   */
  #ifdef CUDA_UPDATED
  std::vector<std::vector<float> > dTSTest = doActionCudaEntropy(waterCoordinates_, info_.grid,dimension[0], info_.grid,dimension[1],
                                                              info_.grid,dimension[2], quaternions_, info_.system.temperature, info_.system.rho0, info_.system.nFrames);
  #endif
  mprintf("Processed %d frames.\nMoving on to entropy calculation.\n", info_.system.nFrames);
  ProgressBar progBarEntropy(info_.grid.nVoxels);

  
#ifdef _OPENMP
  int curVox{ 0 };
  
#endif
    int concerningNeighbors{ 0 };
  #pragma omp parallel for
  for (int voxel = 0; voxel < info_.grid.nVoxels; ++voxel) {
    // If _OPENMP is defined, the progress bar has to be updated critically,
    // to ensure the right addition.
#ifndef _OPENMP
    progBarEntropy.Update( voxel );
#else
    #pragma omp critical
    progBarEntropy.Update( curVox++ );
#endif
    double dTSorient_norm   { 0.0 };
    double dTStrans_norm    { 0.0 };
    double dTSsix_norm      { 0.0 };
    double dTSorient_dens   { 0.0 };
    double dTStrans_dens    { 0.0 };
    double dTSsix_dens      { 0.0 };
    double Esw_norm         { 0.0 };
    double Esw_dens         { 0.0 };
    double Eww_norm         { 0.0 };
    double Eww_dens         { 0.0 };
    double order_norm       { 0.0 };
    double neighbour_dens   { 0.0 };
    double neighbour_norm   { 0.0 };
    // Only calculate if there is actually water molecules at that position.
    if (result_.at(dict_.getIndex("population"))->operator[](voxel) > 0) {
      
      double pop = result_.at(dict_.getIndex("population"))->operator[](voxel);
      // Used for calcualtion of the Entropy on the GPU
      #ifdef CUDA_UPDATED
      dTSorient_norm          = dTSTest.at(1).at(voxel);
      dTStrans_norm           = dTSTest.at(0).at(voxel);
      dTSsix_norm             = dTSTest.at(2).at(voxel);
      dTSorient_dens          = dTSorient_norm * pop / (this->nFrames_ * this->voxelVolume_);
      dTStrans_dens           = dTStrans_norm * pop / (this->nFrames_ * this->voxelVolume_);
      dTSsix_dens             = dTSsix_norm * pop / (this->nFrames_ * this->voxelVolume_);
      #else
      std::array<double, 2> dTSorient = calcOrientEntropy(voxel);
      dTSorient_norm          = dTSorient.at(0);
      dTSorient_dens          = dTSorient.at(1);
      auto ret = calcTransEntropy(voxel);
      concerningNeighbors += std::get<1>(ret);
      std::array<double, 4> dTS = std::move(std::get<0>(ret));
      dTStrans_norm           = dTS.at(0);
      dTStrans_dens           = dTS.at(1);
      dTSsix_norm             = dTS.at(2);
      dTSsix_dens             = dTS.at(3);
      #endif
      
      Esw_norm = result_.at(dict_.getIndex("Esw"))->operator[](voxel) / pop;
      Esw_dens = result_.at(dict_.getIndex("Esw"))->operator[](voxel) / (info_.system.nFrames * info_.grid.voxelVolume);
      Eww_norm = result_.at(dict_.getIndex("Eww"))->operator[](voxel) / pop;
      Eww_dens = result_.at(dict_.getIndex("Eww"))->operator[](voxel) / (info_.system.nFrames * info_.grid.voxelVolume);
      order_norm = result_.at(dict_.getIndex("order"))->operator[](voxel) / pop;
      neighbour_norm = result_.at(dict_.getIndex("neighbour"))->operator[](voxel) / pop;
      neighbour_dens = result_.at(dict_.getIndex("neighbour"))->operator[](voxel) / (info_.system.nFrames * info_.grid.voxelVolume);
    }

    
    // Calculate the final dipole values. The temporary data grid has to be used, as data
    // already saved cannot be updated.
    double DPX{ result_.at(dict_.getIndex("dipole_xtemp"))->operator[](voxel) / (DEBYE * info_.system.nFrames * info_.grid.voxelVolume) };
    double DPY{ result_.at(dict_.getIndex("dipole_ytemp"))->operator[](voxel) / (DEBYE * info_.system.nFrames * info_.grid.voxelVolume) };
    double DPZ{ result_.at(dict_.getIndex("dipole_ztemp"))->operator[](voxel) / (DEBYE * info_.system.nFrames * info_.grid.voxelVolume) };
    double DPG{ sqrt( DPX * DPX + DPY * DPY + DPZ * DPZ ) };
    result_.at(dict_.getIndex("dTStrans_norm"))->UpdateVoxel(voxel, dTStrans_norm);
    result_.at(dict_.getIndex("dTStrans_dens"))->UpdateVoxel(voxel, dTStrans_dens);
    result_.at(dict_.getIndex("dTSorient_norm"))->UpdateVoxel(voxel, dTSorient_norm);
    result_.at(dict_.getIndex("dTSorient_dens"))->UpdateVoxel(voxel, dTSorient_dens);
    result_.at(dict_.getIndex("dTSsix_norm"))->UpdateVoxel(voxel, dTSsix_norm);
    result_.at(dict_.getIndex("dTSsix_dens"))->UpdateVoxel(voxel, dTSsix_dens);
    result_.at(dict_.getIndex("order_norm"))->UpdateVoxel(voxel, order_norm);
    result_.at(dict_.getIndex("neighbour_norm"))->UpdateVoxel(voxel, neighbour_norm);
    result_.at(dict_.getIndex("neighbour_dens"))->UpdateVoxel(voxel, neighbour_dens);

    
    result_.at(dict_.getIndex("Esw_norm"))->UpdateVoxel(voxel, Esw_norm);
    result_.at(dict_.getIndex("Esw_dens"))->UpdateVoxel(voxel, Esw_dens);
    result_.at(dict_.getIndex("Eww_norm"))->UpdateVoxel(voxel, Eww_norm);
    result_.at(dict_.getIndex("Eww_dens"))->UpdateVoxel(voxel, Eww_dens);
    // Maybe there is a better way, I have to look that
    result_.at(dict_.getIndex("dipole_x"))->UpdateVoxel(voxel, DPX);
    result_.at(dict_.getIndex("dipole_y"))->UpdateVoxel(voxel, DPY);
    result_.at(dict_.getIndex("dipole_z"))->UpdateVoxel(voxel, DPZ);
    result_.at(dict_.getIndex("dipole_g"))->UpdateVoxel(voxel, DPG);
    for (unsigned int i = 0; i < resultV_.size(); ++i) {
      resultV_.at(i).at(voxel) /= (info_.system.nFrames * info_.grid.voxelVolume * info_.system.rho0 * solventAtomCounter_.at(i));
    }
  }

  if (info_.gist.febiss) {
    if (info_.gist.centerAtom == "O" && solventAtomCounter_.size() == 2) {
      placeFebissWaters();
    } else {
      mprinterr("Error: FEBISS only works with water as solvent so far.\n");
    }
  }

  mprintf("Number of possible failures in Nearest-Neighbor search:\n");
  mprintf("Trans: %d (%.1f%); Six: %d (%.1f%); Total searches: %d;\n",
          info_.gist.nearestNeighborTransFailures,
          (double) info_.gist.nearestNeighborTransFailures / info_.gist.nearestNeighborTotal * 100.0,
          info_.gist.nearestNeighborSixFailures,
          (double) info_.gist.nearestNeighborSixFailures / info_.gist.nearestNeighborTotal * 100.0,
          info_.gist.nearestNeighborTotal);

  mprintf("Percent of concerning Neighbors:\n");
  mprintf("%d\n", concerningNeighbors );

  mprintf("Writing output:\n");
  this->datafile_->Printf("GIST calculation output. rho0 = %g, n_frames = %d\n", info_.system.rho0, info_.system.nFrames);
  this->datafile_->Printf("   voxel        x          y          z         population     dTSt_d(kcal/mol)  dTSt_n(kcal/mol)"
                          "  dTSo_d(kcal/mol)  dTSo_n(kcal/mol)  dTSs_d(kcal/mol)  dTSs_n(kcal/mol)   "
                          "Esw_d(kcal/mol)   Esw_n(kcal/mol)   Eww_d(kcal/mol)   Eww_n(kcal/mol)    dipoleX    "
                          "dipoleY    dipoleZ    dipole    neighbour_d    neighbour_n    order_n  ");
  // Moved the densities to the back of the output file, so that the energies are always
  // at the same positions.
  for (unsigned int i = result_.size(); i < dict_.size(); ++i) {
    datafile_->Printf("  g_%s  ", dict_.getElement(i).c_str());
  }
  datafile_->Printf("\n");

  // Final output, the DX files are done automatically by cpptraj
  // so only the standard GIST-format is done here
  ProgressBar progBarIO(info_.grid.nVoxels);
  for (int voxel = 0; voxel < info_.grid.nVoxels; ++voxel) {
    progBarIO.Update( voxel );
    size_t i{}, j{}, k{};
    result_.at(dict_.getIndex("population"))->ReverseIndex(voxel, i, j, k);
    Vec3 coords{ result_.at(dict_.getIndex("population"))->Bin().Center(i, j, k) };
    datafile_->Printf("%d %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g", 
                            voxel, coords[0], coords[1], coords[2],
                            result_.at(dict_.getIndex("population"))->operator[](voxel),
                            result_.at(dict_.getIndex("dTStrans_dens"))->operator[](voxel),
                            result_.at(dict_.getIndex("dTStrans_norm"))->operator[](voxel),
                            result_.at(dict_.getIndex("dTSorient_dens"))->operator[](voxel),
                            result_.at(dict_.getIndex("dTSorient_norm"))->operator[](voxel),
                            result_.at(dict_.getIndex("dTSsix_dens"))->operator[](voxel),
                            result_.at(dict_.getIndex("dTSsix_norm"))->operator[](voxel),
                            result_.at(dict_.getIndex("Esw_dens"))->operator[](voxel),
                            result_.at(dict_.getIndex("Esw_norm"))->operator[](voxel),
                            result_.at(dict_.getIndex("Eww_dens"))->operator[](voxel),
                            result_.at(dict_.getIndex("Eww_norm"))->operator[](voxel),
                            result_.at(dict_.getIndex("dipole_x"))->operator[](voxel),
                            result_.at(dict_.getIndex("dipole_y"))->operator[](voxel),
                            result_.at(dict_.getIndex("dipole_z"))->operator[](voxel),
                            result_.at(dict_.getIndex("dipole_g"))->operator[](voxel),
                            result_.at(dict_.getIndex("neighbour_dens"))->operator[](voxel),
                            result_.at(dict_.getIndex("neighbour_norm"))->operator[](voxel),
                            result_.at(dict_.getIndex("order_norm"))->operator[](voxel)
                            );
    for (unsigned int i = 0; i < resultV_.size(); ++i) {
      datafile_->Printf(" %g", resultV_.at(i).at(voxel));
    }
    datafile_->Printf("\n");
  }
  // The atom densities of the solvent compared to the reference density.
  if (info_.gist.writeDx) {
    for (int i = 0; i < static_cast<int>( resultV_.size() ); ++i) {
      writeDxFile("g_" + dict_.getElement(result_.size() + i) + ".dx", resultV_.at(i));
    }
  }


  mprintf("Timings:\n"
          " Find Head Atom:   %8.3f\n"
          " Add up Energy:    %8.3f\n"
          " Calculate Dipole: %8.3f\n"
          " Calculate Quat:   %8.3f\n"
          " Calculate Energy: %8.3f\n\n",
          tHead_.Total(),
          tEadd_.Total(),
          tDipole_.Total(),
          tRot_.Total(),
          tEnergy_.Total());
  if (wrongNumberOfAtoms_)
  {
    mprintf("Warning: It seems you are having multiple solvents in your system.");
  }
  #ifdef CUDA
  freeGPUMemory();
  #endif
}

/**
 * Calculate the Van der Waals and electrostatic energy.
 * @param r_2: The squared distance between atom 1 and atom 2.
 * @param a1: The first atom.
 * @param a2: The second atom.
 * @return: The interaction energy between the two atoms.
 */
double Action_GIGist::calcEnergy(double r_2, int a1, int a2) {
  r_2 = 1 / r_2;
  return calcElectrostaticEnergy(r_2, a1, a2) + calcVdWEnergy(r_2, a1, a2);
}

/**
 * Calculate the squared distance between two atoms.
 * @param frm: The frame for which to calculate the distance.
 * @param a1: The first atom for the calculation.
 * @param a2: The second atom for the calculation.
 * @return: The squared distance between the two atoms.
 */
double Action_GIGist::calcDistanceSqrd(const ActionFrame &frm, int a1, int a2) {
    Matrix_3x3 ucell{}, recip{};
    double dist{ 0.0 };
    Vec3 vec1{frm.Frm().XYZ(a1)};
    Vec3 vec2{frm.Frm().XYZ(a2)};
    switch( image_.ImagingType() ) {
        case ImageOption::NONORTHO:
            ucell = frm.Frm().BoxCrd().UnitCell();
            recip = frm.Frm().BoxCrd().FracCell();
            dist = DIST2_ImageNonOrtho(vec1, vec2, ucell, recip);
            break;
        case ImageOption::ORTHO:
            dist = DIST2_ImageOrtho(vec1, vec2, frm.Frm().BoxCrd());
            break;
        case ImageOption::NO_IMAGE:
            dist = DIST2_NoImage(vec1, vec2);
            break;
        default:
            throw BoxInfoException();
    }
    return dist;
}

/**
 * Calculate the electrostatic energy between two atoms, as
 * follows from:
 * E(el) = q1 * q2 / r
 * @param r_2_i: The inverse of the squared distance between the atoms.
 * @param a1: The atom index of atom 1.
 * @param a2: The atom index of atom 2.
 * @return: The electrostatic energy.
 */
double Action_GIGist::calcElectrostaticEnergy(double r_2_i, int a1, int a2) {
    //double q1 = top_->operator[](a1).Charge();
    //double q2 = top_->operator[](a2).Charge();
    double q1{ charges_.at(a1) };
    double q2{ charges_.at(a2) };
    return q1 * Constants::ELECTOAMBER * q2 * Constants::ELECTOAMBER * sqrt(r_2_i);
}

/**
 * Calculate the van der Waals interaction energy between
 * two different atoms, as follows:
 * E(vdw) = A / (r ** 12) - B / (r ** 6)
 * Be aware that the inverse is used, as to calculate faster.
 * @param r_2_i: The inverse of the squared distance between the two atoms.
 * @param a1: The atom index of atom1.
 * @param a2: The atom index of atom2.
 * @return: The VdW interaction energy.
 */
double Action_GIGist::calcVdWEnergy(double r_2_i, int a1, int a2) {
    // Attention, both r_6 and r_12 are actually inverted. This is very ok, and makes the calculation faster.
    // However, it is not noted, thus it could be missleading
    double r_6{ r_2_i * r_2_i * r_2_i };
    double r_12{ r_6 * r_6 };
    NonbondType const &params = top_->GetLJparam(a1, a2);
    return params.A() * r_12 - params.B() * r_6;
}

/**
 * Calculate the orientational entropy of the water atoms
 * in a given voxel.
 * @param voxel: The index of the voxel.
 * @return: The entropy of the water molecules in that voxel.
 */
std::array<double, 2> Action_GIGist::calcOrientEntropy(int voxel) {
  std::array<double, 2> ret{};
  int nwtotal = this->result_.at(this->dict_.getIndex("population"))->operator[](voxel);
  if(nwtotal < 2) {
    return ret;
  }
  double dTSo_n{ 0.0 };
  int water_count{ 0 };
  for (const VecAndQuat& quat : centersAndRotations_.at(voxel)) {
    double NNr{ HUGE };
    for (const VecAndQuat& quat2 : centersAndRotations_.at(voxel)) {
      if (&quat == &quat2) {
        continue;
      }

      if ( std::get<1>(quat).initialized() && std::get<1>(quat2).initialized() )
      {
         double rR{ std::get<1>(quat).distance(std::get<1>(quat2)) };
         if ( (rR < NNr) ) {
           NNr = rR;
         }
       }
    }
    if (NNr < HUGE) {
      ++water_count;
      /* dTSo_n += log(NNr * NNr * NNr / (3.0 * Constants::TWOPI)); */
      dTSo_n += log((NNr - sin(NNr)) / Constants::PI);
    }
  }
  dTSo_n += water_count * log(water_count);
  dTSo_n = Constants::GASK_KCAL * info_.system.temperature * (dTSo_n / water_count + Constants::EULER_MASC);
  ret.at(0) = dTSo_n;
  ret.at(1) = dTSo_n * water_count / (info_.system.nFrames * info_.grid.voxelVolume);
  return ret;
}

/**
 * Calculate the translational entropy.
 * @param voxel: The voxel for which to calculate the translational entropy.
 * @return: A vector type object, holding the values for the translational
 *          entropy, as well as the six integral entropy.
 */
std::pair<std::array<double, 4>, int> Action_GIGist::calcTransEntropy(int voxel) {
  // Will hold dTStrans (norm, dens) and dTSsix (norm, dens)
  std::array<double, 4> ret{};
  if ( voxelIsAtGridBorder(voxel) ) {
    return { ret, 0 };
  }
  int concerningNeighbors{ 0 };
  // dTStrans uses all solvents => use nwtotal
  // dTSsix does not use ions => count separately
  int nwtotal = (*this->result_.at(this->dict_.getIndex("population")))[voxel];
  int nw_six{ 0 };
  for (const VecAndQuat& quat : centersAndRotations_.at(voxel)) {
    if ( std::get<1>(quat).initialized() ) {
        // the current molecule has rotational degrees of freedom, i.e., it's not an ion.
        ++nw_six;
    }
    std::tuple<double, double, int> distances{ sixEntropyNearestNeighbor( quat, voxel, 0 ) };
    double NNd = std::get<0>( distances );
    double NNs = std::get<1>( distances );
    if ( std::abs( std::get<2>( distances ) ) <= 3 ) {
        concerningNeighbors++;
    }
    /* if ( std::abs( std::get<2>(distances) ) == 0 ) { */
    /*     mprintf("Something might be wrong!\n"); */
    /* } */
    NNd = sqrt(NNd);
    if (NNd <= 0) {
        throw "Error: 2 molecules seem to be at the same place";
    }
    updateNNFailureCount(NNd*NNd, NNs);
    if (NNd < HUGE){
      // For both, the number of frames is used as the number of measurements.
      // The third power of NNd has to be taken, since NNd is only power 1.
      ret.at(0) += log(NNd * NNd * NNd * info_.system.nFrames * 4 * Constants::PI * info_.system.rho0 / 3.0);
      // NNs is used to the power of 6, since it is already power of 2, only the third power
      // has to be calculated.
      if ( std::get<1>(quat).initialized() ) {
        double sixVol = NNs * NNs * NNs * info_.system.nFrames * Constants::PI * info_.system.rho0 / 48.0;
        sixVol /= sixVolumeCorrFactor(NNs);
        ret.at(2) += log(sixVol);
      }
    }
  }
  if (ret.at(0) != 0) {
    double dTSt_n{ Constants::GASK_KCAL * info_.system.temperature * (ret.at(0) / nwtotal + Constants::EULER_MASC) };
    ret.at(0) = dTSt_n;
    ret.at(1) = dTSt_n * nwtotal / (info_.system.nFrames * info_.grid.voxelVolume);
  }
  if (ret.at(2) != 0) {
    double dTSs_n{ Constants::GASK_KCAL * info_.system.temperature * (ret.at(2) / nw_six + Constants::EULER_MASC) };
    ret.at(2) = dTSs_n;
    ret.at(3) = dTSs_n * nw_six / (info_.system.nFrames * info_.grid.voxelVolume);
  }
  return { ret, concerningNeighbors };
}


std::tuple<double, double, int> Action_GIGist::sixEntropyNearestNeighbor(
    const VecAndQuat& quat,
    int voxel,
    int n_layers,
    double NNd,
    double NNs)
{
  const std::array<int, 3> griddims{ info_.grid.dimensions };
  const std::array<int, 3> step{ griddims[2] * griddims[1], griddims[2], 1 };
  const std::array<int, 3> xyz{ getVoxelVec(voxel) };
  std::pair<int, int> nnFrames;
  for (int x = xyz[0] - n_layers; x <= xyz[0] + n_layers; ++x) {
    if ( x < 0 || x >= griddims[0] ) { continue; }
    bool x_is_border{ x == xyz[0] - n_layers || x == xyz[0] + n_layers };
    for (int y = xyz[1] - n_layers; y <= xyz[1] + n_layers; ++y) {
      if ( y < 0 || y >= griddims[1] ) { continue; }
      bool y_is_border{ y == xyz[1] - n_layers || y == xyz[1] + n_layers };
      for (int z = xyz[2] - n_layers; z <= xyz[2] + n_layers; ++z) {
        if ( z < 0 || z >= griddims[2] ) { continue; }
        bool z_is_border{ z == xyz[2] - n_layers || z == xyz[2] + n_layers };
        if ( !(x_is_border || y_is_border || z_is_border) ) { continue; }
        int voxel2{ x * step[0] + y * step[1] + z * step[2] };
        nnFrames = calcTransEntropyDist(voxel2, quat, NNd, NNs);
      }
    }
  }
  double save_dist{ info_.grid.voxelSize * n_layers };
  save_dist *= save_dist;
  if (std::get<1>(quat).initialized() && NNs > save_dist) {
    return sixEntropyNearestNeighbor(quat, voxel, n_layers + 1, NNd, NNs);
  }
  int dist = std::abs(nnFrames.second - nnFrames.first);
  return {NNd, NNs, dist};
}

double Action_GIGist::sixVolumeCorrFactor(double NNs) const
{
    double dbl_index = NNs / SIX_CORR_SPACING;
    int index = std::max(0, std::min(
        static_cast<int>(SIX_CORR.size() - 2),
        static_cast<int>(dbl_index)));
    double dx = dbl_index - index;
    double interp = (1-dx) * SIX_CORR[index] + dx * SIX_CORR[index+1];
    return interp;
}

std::array<int, 3> Action_GIGist::getVoxelVec(int voxel) const
{
  return {
    voxel / ( info_.grid.dimensions[2] * info_.grid.dimensions[1] ),
    (voxel / info_.grid.dimensions[2] ) % info_.grid.dimensions[1],
    voxel % info_.grid.dimensions[2],
  };
}

bool Action_GIGist::voxelIsAtGridBorder(int voxel) const
{
  std::array<int, 3> xyz{ getVoxelVec(voxel) };
  std::array<int, 3> dim{ info_.grid.dimensions };
  return ( !(
     xyz[0] > 0 && xyz[0] < dim[0] - 1 &&
     xyz[1] > 0 && xyz[1] < dim[1] - 1 &&
     xyz[2] > 0 && xyz[2] < dim[2] - 1
  ) );
}

/**
 * Calculates the distance between the different atoms in the voxels.
 * Both, for the distance in space, as well as the angular distance.
 * @param voxel1: The first of the two voxels.
 * @param voxel2: The voxel to compare voxel1 to.
 * @param n0: The water molecule of the first voxel.
 * @param NNd: The lowest distance in space. If the calculated one
 *                is smaller, saves it here.
 * @param NNs: The lowest distance in angular space. If the calculated
 *                one is smaller, saves it here.
 */
std::pair<int, int> Action_GIGist::calcTransEntropyDist(int voxel2, const VecAndQuat& quat, double &NNd, double &NNs)
{
  std::pair<int, int> frames{ std::get<2>(quat), 0 };
  for (const VecAndQuat& quat2 : centersAndRotations_.at(voxel2)) {
    if (&quat == &quat2){
      continue;
    }
    /* double dd{ (std::get<0>(quat) - std::get<0>(quat2)).Magnitude2() }; */
    /* if (dd < NNd) { */
      /* NNd = dd; */
    /* } */
    if (std::get<1>(quat).initialized() && std::get<1>(quat2).initialized())
    {
      double dd{ (std::get<0>(quat) - std::get<0>(quat2)).Magnitude2() };
      if (dd < NNd) {
      NNd = dd;
      }
      if (dd < NNs) {
        double rR{ std::get<1>( quat ).distance( std::get<1>(quat2) ) };
        double ds{ rR * rR + dd };
        if (ds < NNs) {
            NNs = ds;
            frames.second = std::get<2>( quat2 );
        }
      }
    }
  }
  return frames;
} 

/**
 * updates nearestNeighborTransFailures, nearestNeighborSixFailures, and nearestNeighborTotal
 * NNd_sqr and NNs_sqr should be squared nearest neighbor estimates.
 * if they are smaller than the grid spacing, they are guaranteed to be the smallest.
 * Otherwise increment failure counts.
 **/
void Action_GIGist::updateNNFailureCount(double NNd_sqr, double NNs_sqr) {
    double save_dist = info_.grid.voxelSize;
    save_dist *= save_dist;
    if (NNd_sqr > save_dist) {
        ++info_.gist.nearestNeighborTransFailures;
    }
    if (NNs_sqr > save_dist) {
        ++info_.gist.nearestNeighborSixFailures;
    }
    ++info_.gist.nearestNeighborTotal;
}

/**
 * A weighting for the different elements.
 * @param atom: A string holding the element symbol.
 * @return a weight for that particular element.
 **/
int Action_GIGist::weight(std::string atom) {
  if (atom.compare("S") == 0) {
    return 0;
  }
  if (atom.compare("C") == 0) {
    return 1;
  }
  if (atom.compare("O") == 0) {
    return 2;
  }
  if (atom.compare("") == 0) {
    return 10000;
  }
  return 1000;
}

/**
 * Writes a dx file. The dx file is the same file as the cpptraj dx file, however,
 * this is under my complete control, cpptraj is not.
 * Still for most calculations, the cpptraj tool is used.
 * @param name: A string holding the name of the written file.
 * @param data: The data to write to the dx file.
 */
void Action_GIGist::writeDxFile(std::string name, const std::vector<double> &data) {
  std::ofstream file{};
  file.open(name.c_str());
  Vec3 griddim{ info_.grid.dimensions.data() };
  Vec3 origin{ info_.grid.center - griddim * (0.5 * info_.grid.voxelSize) };
  file << "object 1 class gridpositions counts " << info_.grid.dimensions[0] << " " << info_.grid.dimensions[1] << " " << info_.grid.dimensions[2] << "\n";
  file << "origin " << origin[0] << " " << origin[1] << " " << origin[2] << "\n";
  file << "delta " << info_.grid.voxelSize << " 0 0\n";
  file << "delta 0 " << info_.grid.voxelSize << " 0\n";
  file << "delta 0 0 " << info_.grid.voxelSize << "\n";
  file << "object 2 class gridconnections counts " << info_.grid.dimensions[0] << " " << info_.grid.dimensions[1] << " " << info_.grid.dimensions[2] << "\n";
  file << "object 3 class array type double rank 0 items " << info_.grid.nVoxels << " data follows" << "\n";
  int i{ 0 };
  while ( (i + 3) < static_cast<int>( info_.grid.nVoxels  )) {
    file << data.at(i) << " " << data.at(i + 1) << " " << data.at(i + 2) << "\n";
    i +=3;
  }
  while (i < static_cast<int>( info_.grid.nVoxels )) {
    file << data.at(i) << " ";
    i++;
  }
  file << std::endl;
}

/**
 * Calculate the center of mass for a set of atoms. These atoms do not necessarily need
 * to belong to the same molecule, but in this case do.
 * @param atom_begin: The first atom in the set.
 * @param atom_end: The index of the last atom in the set.
 * @param coords: The current coordinates, on which processing occurs.
 * @return A vector, of class Vec3, holding the center of mass.
 */
Vec3 Action_GIGist::calcCenterOfMass(int atom_begin, int atom_end, const double *coords) const {
  double mass{ 0.0 };
  double x{ 0.0 }, y{ 0.0 }, z{ 0.0 };
  for (int i = 0; i < (atom_end - atom_begin); ++i) {
    double currentMass{masses_.at(i + atom_begin)};
    x += coords[i * 3    ] * currentMass;
    y += coords[i * 3 + 1] * currentMass;
    z += coords[i * 3 + 2] * currentMass;
    mass += currentMass;
  }
  return Vec3{x / mass, y / mass, z / mass};
}

/**
 * A function to bin a certain vector to a grid. This still does more, will be fixed.
 * @param begin: The first atom in the molecule.
 * @param end: The last atom in the molecule.
 * @param vec: The vector to be binned.
 * @param frame: The current frame.
 * @return The voxel this frame was binned into. If binning was not succesfull, returns -1.
 */
int Action_GIGist::bin(int begin, int end, const Vec3 &vec, const ActionFrame &frame) {
  size_t bin_i{}, bin_j{}, bin_k{};
  // This is set to -1, if binning is not possible, the function will return a nonsensical value of -1, which can be tested.
  int voxel{ -1 };
  if (result_.at(dict_.getIndex("population"))->Bin().Calc(vec[0], vec[1], vec[2], bin_i, bin_j, bin_k)
      /*&& bin_i < dimensions_[0] && bin_j < dimensions_[1] && bin_k < dimensions_[2]*/)
  {
    voxel = result_.at(dict_.getIndex("population"))->CalcIndex(bin_i, bin_j, bin_k);
    
    

    #ifdef _OPENMP
    #pragma omp critical
    {
    #endif
    
      result_.at(dict_.getIndex("population"))->UpdateVoxel(voxel, 1.0);
    if (!info_.gist.useCOM) {
      resultV_.at(dict_.getIndex(info_.gist.centerAtom) - result_.size()).at(voxel) += 1.0;
    }
    #ifdef _OPENMP
    }
    #endif

    calcDipole(begin, end, voxel, frame);

  }
  return voxel;
}

/**
 * Calculates the total dipole for a given set of atoms.
 * @param begin: The index of the first atom of the set.
 * @param end: The index of the last atom of the set.
 * @param voxel: The voxel in which the values should be binned
 * @param frame: The current frame.
 * @return Nothing at the moment
 */
void Action_GIGist::calcDipole(int begin, int end, int voxel, const ActionFrame &frame) {
  #if !defined _OPENMP && !defined CUDA
    tDipole_.Start();
#endif
    double DPX{ 0 };
    double DPY{ 0 };
    double DPZ{ 0 };
    for (int atoms = begin; atoms < end; ++atoms)
    {
      const double *XYZ = frame.Frm().XYZ(atoms);
      double charge{ charges_.at(atoms) };
      DPX += charge * XYZ[0];
      DPY += charge * XYZ[1];
      DPZ += charge * XYZ[2];
    }

    #ifdef _OPENMP
    #pragma omp critical
    {
    #endif
    result_.at(dict_.getIndex("dipole_xtemp"))->UpdateVoxel(voxel, DPX);
    result_.at(dict_.getIndex("dipole_ytemp"))->UpdateVoxel(voxel, DPY);
    result_.at(dict_.getIndex("dipole_ztemp"))->UpdateVoxel(voxel, DPZ);
    #ifdef _OPENMP
    }
    #endif
#if !defined _OPENMP && !defined CUDA
    tDipole_.Stop();
#endif
}

std::vector<int> Action_GIGist::calcQuaternionIndices(int begin, int end, const double * molAtomCoords)
{
  std::vector<int> indices;
  Vec3 com {calcCenterOfMass( begin, end, molAtomCoords )};
  Vec3 X{ 0, 0, 0};
  for (int i {0}; i < (end - begin); i++)
  {
    if (top_->operator[](i).Element() == Atom::HYDROGEN){
      continue;
    }
    Vec3 coord{&molAtomCoords[i * 3]};
    if ( (coord - com).Length() > 0.2)
    {
      // Return if enough atoms are found
      if (indices.size() >= 2)
      {
        return indices;
      }
      else if (indices.size() == 0)
      {
        indices.push_back(i);
        X = coord - com;
      }
      else
      {
        double angleCos{ (X * (coord - com)) / (X.Length() * (coord - com).Length()) };
        if ( angleCos <= 0.95 && angleCos >= -0.95 )
        {
          indices.push_back(i);
          return indices;
        }
      }
    }
  }
  if (indices.size() < 2) {
    for (int i {0}; i < (end - begin); i++)
    {
      if (top_->operator[](i).Element() == Atom::HYDROGEN){
        Vec3 coord{&molAtomCoords[i * 3]};
        if ( (coord - com).Length() > 0.2)
        {
          // Return if enough atoms are found
          if (indices.size() >= 2)
          {
            return indices;
          }
          else if (indices.size() == 0)
          {
            indices.push_back(i);
            X = coord - com;
          }
          else {
            double angleCos{ (X * (coord - com)) / (X.Length() * (coord - com).Length()) };
            if ( angleCos <= 0.8 && angleCos >= -0.8 )
            {
                indices.push_back(i);
                return indices;
            }
          }
        }
      }
    }
  }
  return indices;
}


Quaternion<DOUBLE_O_FLOAT> Action_GIGist::calcQuaternion(const std::vector<Vec3> &molAtomCoords, const Vec3 &center, std::vector<int> indices)
{
  if (static_cast<int>(molAtomCoords.size()) < indices.at(0) || 
          static_cast<int>(molAtomCoords.size()) < indices.at(1))
  {
    wrongNumberOfAtoms_ = true;
    return Quaternion<DOUBLE_O_FLOAT> {};
  }

  Vec3 X = molAtomCoords.at(indices.at(0)) - center;
  Vec3 Y = molAtomCoords.at(indices.at(1)) - center;

  // Create Quaternion for the rotation from the new coordintate system to the lab coordinate system.
   Quaternion<DOUBLE_O_FLOAT> quat(X, Y);
   // The Quaternion would create the rotation of the lab coordinate system onto the
   // calculated solvent coordinate system. The invers quaternion is exactly the rotation of
   // the solvent coordinate system onto the lab coordinate system.
   quat.invert();
   return quat;
}


/**
 * Calculate the quaternion as a rotation when a certain center is given
 * and a set of atomic coordinates are supplied. If the center coordinates
 * are actually one of the atoms, headAtomIndex should evaluate to that
 * atom, if this is not done, unexpexted behaviour might occur.
 * If the center is set to something other than an atomic position,
 * headAtomIndex should evaluate to a nonsensical number (preferrably
 * a negative value).
 * @param molAtomCoords: The set of atomic cooordinates, saved as a vector
 *                           of Vec3 objects.
 * @param center: The center coordinates.
 * @param headAtomIndex: The index of the head atom, when counting the first
 *                           atom as 0, as indices naturally do.
 * @return: A quaternion holding the rotational value.
 * FIXME: Decision for the different X and Y coordinates has to be done at the beginning.
 */
Quaternion<DOUBLE_O_FLOAT> Action_GIGist::calcQuaternion(const std::vector<Vec3> &molAtomCoords, const Vec3 &center, int headAtomIndex) {
  Vec3 X{};
  Vec3 Y{};
  bool setX{false};
  bool setY{false};    
  for (unsigned int i = 0; i < molAtomCoords.size(); ++i) {
    if ((int)i != headAtomIndex) {
      if (setX && !setY) {
        Y.SetVec(molAtomCoords.at(i)[0] - center[0], 
                  molAtomCoords.at(i)[1] - center[1], 
                  molAtomCoords.at(i)[2] - center[2]);
        Y.Normalize();
        
      }
      if (!setX) {
        X.SetVec(molAtomCoords.at(i)[0] - center[0], 
                  molAtomCoords.at(i)[1] - center[1], 
                  molAtomCoords.at(i)[2] - center[2]);
        if (!(X.Length() < 0.001)) {
          X.Normalize();
          setX = true;
        }
      }
      if (setX && setY) {
        break;
      }
    }
  }

  if (X.Length() <= 0.1 || Y.Length() <= 0.1)
  {
    return Quaternion<DOUBLE_O_FLOAT>{};
  }

   // Create Quaternion for the rotation from the new coordintate system to the lab coordinate system.
   Quaternion<DOUBLE_O_FLOAT> quat(X, Y);
   // The Quaternion would create the rotation of the lab coordinate system onto the
   // calculated solvent coordinate system. The invers quaternion is exactly the rotation of
   // the solvent coordinate system onto the lab coordinate system.
   quat.invert();
   return quat;
}

/**
 * Checks for two numbers being almost equal. Also takes care of problems arising due to values close to zero.
 * This comaprison is implemented as suggested by 
 * https://www.learncpp.com/cpp-tutorial/relational-operators-and-floating-point-comparisons/
 * @param input: The input number that should be compared.
 * @param control: The control number to which input should be almost equal.
 * @return true if they are almost equal, false otherwise.
 */
bool Action_GIGist::almostEqual(double input, double control) 
{
  double abs_inp{std::fabs(input)};
  double abs_cont{std::fabs(control)};
  double abs_diff{std::abs(input - control)};

  // Check if the absolute error is already smaller than epsilon (errors close to 0)
  if (abs_diff < __DBL_EPSILON__) {
    return true;
  }

  // Fall back to Knuth's algorithm
  return abs_diff <= ( (abs_inp < abs_cont) ? abs_cont : abs_inp) * __DBL_EPSILON__;
}



// Functions used when CUDA is specified.
#ifdef CUDA

/**
 * Frees all the Memory on the GPU.
 */
void Action_GIGist::freeGPUMemory(void) {
  freeCuda_GIGIST(NBindex_c_);
  freeCuda_GIGIST(molecule_c_);
  freeCuda_GIGIST(paramsLJ_c_);
  freeCuda_GIGIST(result_w_c_);
  freeCuda_GIGIST(result_s_c_);
  freeCuda_GIGIST(result_O_c_);
  freeCuda_GIGIST(result_N_c_);
  NBindex_c_   = nullptr;
  molecule_c_  = nullptr;
  paramsLJ_c_  = nullptr;
  result_w_c_= nullptr;
  result_s_c_= nullptr;
  result_O_c_  = nullptr;
  result_N_c_  = nullptr;
}

/**
 * Copies data from the CPU to the GPU.
 * @throws: CudaException
 */
void Action_GIGist::copyToGPU(void) {
  try {
    copyMemoryToDevice_GIGIST(&(NBIndex_[0]), NBindex_c_, NBIndex_.size() * sizeof(int));
    copyMemoryToDeviceStruct_GIGIST(&(charges_[0]), &(atomTypes_[0]), solvent_.get(), &(molecule_[0]), info_.system.numberAtoms, &(molecule_c_),
                              &(lJParamsA_[0]), &(lJParamsB_[0]), lJParamsA_.size(), &(paramsLJ_c_));
  } catch (CudaException &ce) {
    freeGPUMemory();
    mprinterr("Error: Could not copy data to the device.\n");
    throw ce;
  } catch (std::exception &e) {
    freeGPUMemory();
    throw e;
  }
}
#endif

/**
 * @brief main function for FEBISS placement
 *
 */
void Action_GIGist::placeFebissWaters(void) {
  mprintf("Transfering data for FEBISS placement\n");
  determineGridShells();
  /* calculate delta G and read density data */
  std::vector<double> deltaG;
  std::vector<double> relPop;
  for (int voxel = 0; voxel < info_.grid.nVoxels; ++voxel) {
    double dTSt = result_.at(dict_.getIndex("dTStrans_norm"))->operator[](voxel);
    double dTSo = result_.at(dict_.getIndex("dTSorient_norm"))->operator[](voxel);
    double esw = result_.at(dict_.getIndex("Esw_norm"))->operator[](voxel);
    double eww = result_.at(dict_.getIndex("Eww_norm"))->operator[](voxel);
    double value = esw + eww - dTSo - dTSt;
    deltaG.push_back(value);
    relPop.push_back(resultV_.at(info_.gist.centerIdx).at(voxel));
  }
  /* Place water to recover 95% of the original density */
  int waterToPosition = static_cast<int>(round(info_.system.numberSolvent * 0.95 / 3));
  mprintf("Placing %d FEBISS waters\n", waterToPosition);
  ProgressBar progBarFebiss(waterToPosition);
  /* cycle to position all waters */
  for (int i = 0; i < waterToPosition; ++i) {
    progBarFebiss.Update(i);
    double densityValueOld = 0.0;
    /* get data of current highest density voxel */
    std::vector<double>::iterator maxDensityIterator;
    maxDensityIterator = std::max_element(relPop.begin(), relPop.end());
    double densityValue = *(maxDensityIterator);
    int index = maxDensityIterator - relPop.begin();
    Vec3 voxelCoords = coordsFromIndex(index);
    /**
     * bin hvectors to grid
     * Currently this grid is hardcoded. It has 21 voxels in x, y, z direction.
     * The spacing is 0.1 A, so it stretches out exactly 1 A in each direction
     * and is centered on the placed oxygen. This grid allows for convenient and
     * fast binning of the relative vectors of the H atoms during the simulation
     * which have been stored in hVectors_ as a std::vector of Vec3
     */
    std::vector<std::vector<std::vector<int>>> hGrid;
    int hDim = 21;
    setGridToZero(hGrid, hDim);
    std::vector<int> binnedHContainer;
    /* cycle relative H vectors */
    for (unsigned int j = 0; j < hVectors_[index].size(); ++j) {
      /* cycle x, y, z */
      for (int k = 0; k < 3; ++k) {
        double tmpBin = hVectors_[index][j][k];
        tmpBin *= 10;
        tmpBin = std::round(tmpBin);
        tmpBin += 10;
        /* sometimes bond lengths can be longer than 1 A and exactly along the
         * basis vectors, this ensures that the bin is inside the grid */
        if (tmpBin < 0)
          tmpBin = 0;
        else if (tmpBin > 20)
          tmpBin = 20;
        binnedHContainer.push_back(static_cast<int>(tmpBin));
      }
    }
    /* insert data into grid */
    for (unsigned int j = 0; j < binnedHContainer.size(); j += 3) {
      int x = binnedHContainer[j];
      int y = binnedHContainer[j + 1];
      int z = binnedHContainer[j + 2];
      hGrid[x][y][z] += 1;
    }
    /* determine maxima in grid */
    auto maximum1 = findHMaximum(hGrid, hDim);
    deleteAroundFirstH(hGrid, hDim, maximum1);
    auto maximum2 = findHMaximum(hGrid, hDim, maximum1);
    Vec3 h1 = coordsFromHGridPos(maximum1);
    Vec3 h2 = coordsFromHGridPos(maximum2);
    /* increase included shells until enough density to subtract */
    int shellNum = 0;
    int maxShellNum = shellcontainerKeys_.size() - 1;
    /* not enough density and not reached limit */
    while (densityValue < 1 / (info_.grid.voxelVolume * info_.system.rho0) &&
           shellNum < maxShellNum
    ) {
      densityValueOld = densityValue;
      ++shellNum;
      /* new density by having additional watershell */
      densityValue = addWaterShell(densityValue, relPop, index, shellNum);
    }
    /* determine density weighted delta G with now reached density */
    double weightedDeltaG = assignDensityWeightedDeltaG(
        index, shellNum, densityValue, densityValueOld, relPop, deltaG);
    int atomNumber = 3 * i + info_.system.numberSoluteAtoms; // running index in pdb file
    /* write new water to pdb */
    writeFebissPdb(atomNumber, voxelCoords, h1, h2, weightedDeltaG);
    /* subtract density in included shells */
    subtractWater(relPop, index, shellNum, densityValue, densityValueOld);
  } // cycle of placed water molecules
}

/**
 * @brief writes solute of given frame into pdb
 *
 * @argument actionFrame frame from which solute is written
 */
void Action_GIGist::writeOutSolute(ActionFrame& frame) {
  std::vector<Vec3> soluteCoords;
  std::vector<std::string> soluteEle;
  for (Topology::mol_iterator mol = top_->MolStart();
       mol < top_->MolEnd(); ++mol) {
    if (!mol->IsSolvent()) {
      for (int atom = mol->MolUnit().Front(); atom < mol->MolUnit().Back(); ++atom) {
        info_.system.numberSoluteAtoms++;
        const double *vec = frame.Frm().XYZ(atom);
        soluteCoords.push_back(Vec3(vec));
        soluteEle.push_back(top_->operator[](atom).ElementName());
      }
    }
  }
  for (unsigned int i = 0; i < soluteCoords.size(); ++i) {
    auto name = soluteEle[i].c_str();
    febissWaterfile_->Printf(
      "ATOM  %5d  %3s SOL     1    %8.3f%8.3f%8.3f%6.2f%7.2f          %2s\n",
      i + 1,
      name,
      soluteCoords[i][0],
      soluteCoords[i][1],
      soluteCoords[i][2],
      1.0,
      0.0,
      name
    );
  }
}

/**
 * @brief calculates distance of all voxels to the grid center and groups
 *        them intp shells of identical distances
 *
 * a map stores lists of indices with their identical squared distance as key
 * the indices are stores as difference to the center index to be applicable for
 * all voxels later without knowing the center index
 * a list contains all keys in ascending order to systematically grow included
 * shells later in the algorithm
 */
void Action_GIGist::determineGridShells(void) {
  /* determine center index */
  size_t centeri, centerj, centerk;
  result_.at(dict_.getIndex("population"))->
      Bin().Calc(info_.grid.center[0], info_.grid.center[1], info_.grid.center[2], centeri, centerj, centerk);
  int centerIndex = result_.at(dict_.getIndex("population"))
      ->CalcIndex(centeri, centerj, centerk);
  /* do not use center_ because it does not align with a voxel but lies between voxels */
  /* however the first shell must be solely the voxel itself -> use coords from center voxel */
  Vec3 centerCoords = coordsFromIndex(centerIndex);
  for (int vox = 0; vox < info_.grid.nVoxels; ++vox) {
    /* determine squared distance */
    Vec3 coords = coordsFromIndex(vox);
    Vec3 difference = coords - centerCoords;
    double distSquared = difference[0] * difference[0] +
                         difference[1] * difference[1] +
                         difference[2] * difference[2];
    /* find function of map returns last memory address of map if not found */
    /* if is entered if distance already present as key in map -> can be added */
    if (shellcontainer_.find(distSquared) != shellcontainer_.end()) {
      shellcontainer_[distSquared].push_back(vox-centerIndex);
    } else {
      /* create new entry in map */
      std::vector<int> indexDifference;
      indexDifference.push_back(vox-centerIndex);
      shellcontainer_.insert(std::make_pair(distSquared, indexDifference));
    }
  }
  /* create list to store ascending keys */
  shellcontainerKeys_.reserve(shellcontainer_.size());
  std::map<double, std::vector<int>>::iterator it = shellcontainer_.begin();
  while(it != shellcontainer_.end()) {
    shellcontainerKeys_.push_back(it->first);
    it++;
  }
}

/**
 * @brief own utility function to get coords from grid index
 *
 * @argument index The index in the GIST grid
 * @return Vec3 coords at the voxel
 */
Vec3 Action_GIGist::coordsFromIndex(const int index) {
  size_t i, j, k;
  result_.at(dict_.getIndex("population"))->ReverseIndex(index, i, j, k);
  Vec3 coords = info_.grid.start;
  /* the + 0.5 * size is necessary because of cpptraj's interprets start as corner of grid */
  /* and voxel coordinates are given for the center of the voxel -> hence the shift of half spacing */
  coords[0] += (i + 0.5) * info_.grid.voxelSize;
  coords[1] += (j + 0.5) * info_.grid.voxelSize;
  coords[2] += (k + 0.5) * info_.grid.voxelSize;
  return coords;
}

/**
 * @brief sets the grid for the H atoms to zero
 *
 * @argument grid
 * @argument dim The dimension of the grid
 */
void Action_GIGist::setGridToZero(std::vector<std::vector<std::vector<int>>>& grid, const int dim) const {
  grid.resize(dim);
  for (int i = 0; i < dim; ++i) {
    grid[i].resize(dim);
    for (int j = 0; j < dim; ++j) {
      grid[i][j].resize(dim);
    }
  }
}

/**
 * @brief Find highest value with possible consideration of first maximum
 *
 * @argument grid
 * @argument dim The dimension of the grid
 * @argument firstMaximum Optional first maximum
 *
 * @return std::tuple<int, int, int, int> maximum in the grid
 */
std::tuple<int, int, int, int> Action_GIGist::findHMaximum(std::vector<std::vector<std::vector<int>>>& grid, const int dim, std::tuple<int, int, int, int> firstMaximum) const {
  std::tuple<int, int, int, int> maximum = std::make_tuple(0, 0, 0, 0);
  /* default for firstMaximum is 0,
   * so this bool is False is no firstMaximum is given */
  bool considerOtherMaximum = std::get<0>(firstMaximum) != 0;
  for (int i = 0; i < dim; ++i) {
    for (int j = 0; j < dim; ++j) {
      for (int k = 0; k < dim; ++k) {
        /* if bigger value check if angle in range if firstMaximum is given */
        if (grid[i][j][k] > std::get<0>(maximum)) {
          auto possibleMaximum = std::make_tuple(grid[i][j][k], i, j, k);
          if (considerOtherMaximum) {
            double angle = this->calcAngleBetweenHGridPos(possibleMaximum, firstMaximum);
            if (info_.gist.idealWaterAngle_ - 5 < angle &&
                angle < info_.gist.idealWaterAngle_ + 5) {
              maximum = possibleMaximum;
            }
          } else {
            maximum = possibleMaximum;
          }
        }
        /* if equal and already firstMaximum, take better angle */
        else if (considerOtherMaximum && grid[i][j][k] == std::get<0>(maximum)) {
          double angle = calcAngleBetweenHGridPos(maximum, firstMaximum);
          auto possibleMaximum = std::make_tuple(grid[i][j][k], i, j, k);
          double newAngle = this->calcAngleBetweenHGridPos(possibleMaximum, firstMaximum);
          if (std::fabs(newAngle - info_.gist.idealWaterAngle_)
              < std::fabs(angle - info_.gist.idealWaterAngle_))
            maximum = possibleMaximum;
        }
      }
    }
  }
  return maximum;
}

/**
 * @brief calculate angle in degrees between two hGrid points
 *
 * @argument a Point in the grid
 * @argument b Point in the grid
 *
 * @return double angle in degrees
 */
double Action_GIGist::calcAngleBetweenHGridPos(const std::tuple<int, int, int, int>& a, const std::tuple<int, int, int, int>& b) const {
  double xa = (std::get<1>(a) - 10) * 0.1;
  double ya = (std::get<2>(a) - 10) * 0.1;
  double za = (std::get<3>(a) - 10) * 0.1;
  double xb = (std::get<1>(b) - 10) * 0.1;
  double yb = (std::get<2>(b) - 10) * 0.1;
  double zb = (std::get<3>(b) - 10) * 0.1;
  double dotProduct = (xa * xb + ya * yb + za * zb) / std::sqrt( (xa * xa + ya * ya + za * za) * (xb * xb + yb * yb + zb * zb) );
  double angle =  180 * acos(dotProduct) / Constants::PI;
  return angle;
}

/**
 * @brief sets hGrid points within 0.5 A of given maximum to 0
 *
 * @argument grid The grid storing the values
 * @argument dim The dimension of the grid
 * @argument maximum The point around which points will be deleted
 */
void Action_GIGist::deleteAroundFirstH(std::vector<std::vector<std::vector<int>>>& grid, const int dim, const std::tuple<int, int, int, int>& maximum) const {
  double destroyDistance = 0.5;
  double destroyDistanceSq = destroyDistance * destroyDistance;
  for (int i = 0; i < dim; ++i) {
    for (int j = 0; j < dim; ++j) {
      for (int k = 0; k < dim; ++k) {
        std::tuple<int, int, int, int> position = std::make_tuple(0, i, j, k);
        double distance = calcDistSqBetweenHGridPos(position, maximum);
        if (distance <= destroyDistanceSq)
          grid[i][j][k] = 0;
      }
    }
  }
}

/**
 * @brief calculate squared distance between two hGrid points
 *
 * @argument a Point in the grid
 * @argument b Point in the grid
 *
 * @return double squared distance
 */
double Action_GIGist::calcDistSqBetweenHGridPos(const std::tuple<int, int, int, int>& a, const std::tuple<int, int, int, int>& b) const {
  double xa = (std::get<1>(a) - 10) * 0.1;
  double ya = (std::get<2>(a) - 10) * 0.1;
  double za = (std::get<3>(a) - 10) * 0.1;
  double xb = (std::get<1>(b) - 10) * 0.1;
  double yb = (std::get<2>(b) - 10) * 0.1;
  double zb = (std::get<3>(b) - 10) * 0.1;
  double distance = (xa - xb) * (xa - xb) + (ya - yb) * (ya - yb) + (za - zb) * (za - zb);
  return distance;
}

/**
 * @brief gives cartesian coordinates of point in hGrid
 *
 * @argument pos Point in grid
 *
 * @return Vec3 coordinates of the point
 */
Vec3 Action_GIGist::coordsFromHGridPos(const std::tuple<int, int, int, int>& pos) const {
  double x = (std::get<1>(pos) - 10) * 0.1;
  double y = (std::get<2>(pos) - 10) * 0.1;
  double z = (std::get<3>(pos) - 10) * 0.1;
  return Vec3(x, y, z);
}

/**
 * @brief weights Delta G of GIST with the water density that was subtracted
 *        to place the water molecule
 *
 * @argument index The index where the oxygen is placed
 * @argument shellNum The number of shells around the placed oxygen to add to the density
 * @argument densityValue The value after the last shell was added
 * @argument densityValueOld The value before the last shell was added
 * @argument relPop The list of the population values
 * @argument deltaG The list of all DeltaG values
 *
 * @return double density weighted Delta G
 */
double Action_GIGist::assignDensityWeightedDeltaG(
  int index,
  int shellNum,
  double densityValue,
  double densityValueOld,
  const std::vector<double>& relPop,
  const std::vector<double>& deltaG
) {
  double value = 0.0; // value to be returned
  /* cycle through all shells but the last one */
  for (int i = 0; i < shellNum; ++i) {
    /* current shell */
    auto shell = std::make_shared<std::vector<int>>(
        shellcontainer_[shellcontainerKeys_[i]]);
    /* cycle through current shell */
    for (unsigned int j = 0; j < (*shell).size(); ++j) {
      /* get index and check if inside the grid */
      int tmpIndex = index + (*shell)[j];
      if (0 < tmpIndex && tmpIndex < static_cast<int>(info_.grid.nVoxels))
        /* add density weighted delta G to value */
        value += relPop[tmpIndex] * deltaG[tmpIndex];
    }
  }
  double last_shell = densityValue - densityValueOld; // density of last shell
  /* Get percentage of how much of the last shell shall be accounted for */
  double percentage = 1.0;
  if (last_shell != 0.0)
    percentage -= (densityValue - 1 / (info_.grid.voxelVolume * info_.system.rho0)) / last_shell;
  /* identical to above but only last shell and percentage */
  auto outerShell = std::make_shared<std::vector<int>>(
      shellcontainer_[shellcontainerKeys_[shellNum]]);
  for (unsigned int i = 0; i < (*outerShell).size(); ++i) {
    int tmpIndex = index + (*outerShell)[i];
    if (0 < tmpIndex && tmpIndex < static_cast<int>(info_.grid.nVoxels))
      value += percentage * relPop[tmpIndex] * deltaG[tmpIndex];
  }
  return value * info_.grid.voxelVolume * info_.system.rho0;
}

/**
 * @brief adds density of additional water shell to the densityValue
 *
 * @argument densityValue The value after the last shell was added
 * @argument relPop The list of the population values
 * @argument index The index of the GIST grid where the oxygen will be placed
 * @argument shellNum The number of the new shells to be added
 *
 * @return double density value with the new water shell
 */
double Action_GIGist::addWaterShell(double& densityValue, const std::vector<double>& relPop, const int index, const int shellNum) {
  /* get shell from map */
  auto newShell = std::make_shared<std::vector<int>>(
      shellcontainer_[shellcontainerKeys_[shellNum]]
  );
  for (unsigned int i = 0; i < (*newShell).size(); ++i) {
    int tmpIndex = index + (*newShell)[i];
    if (0 < tmpIndex && tmpIndex < static_cast<int>(info_.grid.nVoxels))
      densityValue += relPop[tmpIndex];
  }
  return densityValue;
}

/**
 * @brief subtract density from all voxels that belonged to the included shells
 *
 * @argument relPop The list of the population values
 * @argument index The index where the oxygen is placed
 * @argument shellNum The number of shells around the placed oxygen that were included
 * @argument densityValue The value after the last shell was added
 * @argument densityValueOld The value before the last shell was added
 */
void Action_GIGist::subtractWater
(
  std::vector<double>& relPop,
  int index,
  int shellNum,
  double densityValue,
  double densityValueOld
) 
{
  /* cycle through all but the last shell */
  for (int i = 0; i < shellNum; ++i) {
    auto shell = std::make_shared<std::vector<int>>(
        shellcontainer_[shellcontainerKeys_[i]]);
    for (unsigned int j = 0; j < (*shell).size(); ++j) {
      int tmpIndex = index + (*shell)[j];
      if (0 < tmpIndex && tmpIndex < static_cast<int>(info_.grid.nVoxels))
        /* remove all population from the voxel in the GIST grid */
        relPop[tmpIndex] = 0.0;
    }
  }
  /* since density of one water is overshot, the density must not be deleted
   * completely in the last shell, first determine percentage */
  double last_shell = densityValue - densityValueOld; // density in last shell
  double percentage = 1.0;
  if (last_shell != 0.0)
    percentage -= (densityValue - 1 / (info_.grid.voxelVolume * info_.system.rho0)) / last_shell;
  /* identical to before but only last shell and percentage */
  auto outerShell = std::make_shared<std::vector<int>>(
      shellcontainer_[shellcontainerKeys_[shellNum]]);
  for (unsigned int i = 0; i < (*outerShell).size(); ++i) {
    int tmpIndex = index + (*outerShell)[i];
    if (0 < tmpIndex && tmpIndex < static_cast<int>(info_.grid.nVoxels))
      relPop[tmpIndex] -= percentage * relPop[tmpIndex];
  }
}

/**
 * @brief writes placed water into pdb
 *
 * @argument atomNumber The running index in the pdb file
 * @argument voxelCoords The coordinates for the oxygen
 * @argument h1 coordinates of one hydrogen relative to the oxygen
 * @argument h2 coordinates of the other hydrogen relative to the oxygen
 * @argument deltaG density weighted Delta G to be included as b-factor
 */
void Action_GIGist::writeFebissPdb
(
  const int atomNumber,
  const Vec3& voxelCoords,
  const Vec3& h1,
  const Vec3& h2,
  const double deltaG
) {
  /* get absolute coordinates of hydrogens */
  Vec3 h1Coords = h1 + voxelCoords;
  Vec3 h2Coords = h2 + voxelCoords;

  febissWaterfile_->Printf(
    "HETATM%5d    O FEB     1    %8.3f%8.3f%8.3f%6.2f%7.2f           O  \n",
    atomNumber+1,
    voxelCoords[0],
    voxelCoords[1],
    voxelCoords[2],
    1.00,
    deltaG
  );

  febissWaterfile_->Printf(
    "HETATM%5d    H FEB     1    %8.3f%8.3f%8.3f%6.2f%7.2f           H  \n",
    atomNumber+2,
    h1Coords[0],
    h1Coords[1],
    h1Coords[2],
    1.00,
    deltaG
  );

  febissWaterfile_->Printf(
    "HETATM%5d    H FEB     1    %8.3f%8.3f%8.3f%6.2f%7.2f           H  \n",
    atomNumber+3,
    h2Coords[0],
    h2Coords[1],
    h2Coords[2],
    1.00,
    deltaG
  );
}
